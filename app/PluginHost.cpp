#include "PluginHost.h"
#include "Widgets.h"
#include <set>
#if JUCE_WINDOWS
 #include <windows.h>
#endif

// ------------------------------------------------------------------ HostedPlugin

HostedPlugin::HostedPlugin(std::unique_ptr<juce::AudioPluginInstance> instance, const juce::PluginDescription& description, std::string typeId)
    : instance_(std::move(instance)), description_(description), typeId_(std::move(typeId)), alive_(std::make_shared<std::atomic<bool>>(true)) {
    name_ = description.name.toStdString();
    instrument_ = description.isInstrument;
    instance_->enableAllBuses();
    // PULP's tracks are stereo: a plugin that comes up mono is asked for stereo, and stays mono if it refuses
    {
        auto layout = instance_->getBusesLayout();
        bool changed = false;
        if (layout.inputBuses.size() > 0 && layout.inputBuses.getReference(0) != juce::AudioChannelSet::stereo()) { layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo(); changed = true; }
        if (layout.outputBuses.size() > 0 && layout.outputBuses.getReference(0) != juce::AudioChannelSet::stereo()) { layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo(); changed = true; }
        if (changed && instance_->checkBusesLayoutSupported(layout)) instance_->setBusesLayout(layout);
    }
    monoIn_ = !instrument_ && instance_->getBusCount(true) > 0 && instance_->getChannelCountOfBus(true, 0) == 1;
    instance_->setPlayHead(this);
    sidechain_ = !instrument_ && instance_->getBusCount(true) > 1;
    // The plugin's parameters become ordinary parameters (0..1), so automation clips, links and the control
    // menu work on them. The list is fixed from here on, as the interface requires.
    const auto& all = instance_->getParameters();
    slotOf_.assign((size_t)all.size(), -1);
    std::set<std::string> used;
    for (int i = 0; i < all.size() && hosted_.size() < kMaxParams; ++i) {
        auto* p = all[i];
        if (!p->isAutomatable()) continue;
        pulp::ParamInfo info;
        // the plugin's own identifier for the parameter, which survives the plugin being updated
        if (auto* hp = dynamic_cast<juce::HostedAudioProcessorParameter*>(p)) info.id = "v" + hp->getParameterID().toStdString();
        if (info.id.size() < 2 || used.count(info.id)) info.id = "i" + std::to_string(i);
        used.insert(info.id);
        info.name = p->getName(48).toStdString();
        if (info.name.empty()) info.name = "Parameter " + std::to_string(i + 1);
        info.min = 0.0f; info.max = 1.0f; info.def = juce::jlimit(0.0f, 1.0f, p->getDefaultValue());
        add(info)->set(p->getValue());
        slotOf_[(size_t)i] = hosted_.size();
        hosted_.add(p);
        p->addListener(this);
    }
    const int n = juce::jmax(1, hosted_.size());
    sent_ = std::make_unique<std::atomic<float>[]>((size_t)n);
    held_ = std::make_unique<std::atomic<bool>[]>((size_t)n);
    for (int i = 0; i < n; ++i) { sent_[(size_t)i].store(i < hosted_.size() ? hosted_[i]->getValue() : 0.0f); held_[(size_t)i].store(false); }
    midi_.ensureSize(4096);
}

HostedPlugin::~HostedPlugin() {
    alive_->store(false);
    for (auto* p : hosted_) p->removeListener(this);
    // a plugin is taken apart on the interface thread, wherever the last reference was dropped
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm != nullptr && !mm->isThisTheMessageThread()) {
        auto* raw = instance_.release();
        juce::MessageManager::callAsync([raw] { delete raw; });
    } else {
        if (prepared_) instance_->releaseResources();
        instance_.reset();
    }
}

std::vector<pulp::ParamGroup> HostedPlugin::paramGroups() const {
    // the panel shows the first few dozen; the plugin's own window has them all
    pulp::ParamGroup g;
    g.title = "Parameters, 0 to 1 (the plugin's own window shows real values)";
    for (int i = 0; i < juce::jmin(48, (int)params().size()); ++i) g.ids.push_back(params()[(size_t)i]->info.id);
    return {g};
}

pulp::Param* HostedPlugin::lastTouched() const {
    const int i = lastTouched_.load();
    return i >= 0 && i < (int)params().size() ? params()[(size_t)i].get() : nullptr;
}

juce::String HostedPlugin::valueText(int index) const {
    if (index < 0 || index >= hosted_.size()) return {};
    return hosted_[index]->getCurrentValueAsText();
}

void HostedPlugin::simulateTouch(int index, float value) {
    if (index < 0 || index >= hosted_.size()) return;
    hosted_[index]->beginChangeGesture();
    hosted_[index]->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    hosted_[index]->endChangeGesture();
}

void HostedPlugin::prepare(double sampleRate) {
    // an instance that is already running at this rate (kept across an undo) carries on undisturbed
    if (prepared_ && sampleRate == sr_) return;
    sr_ = sampleRate;
    if (prepared_) instance_->releaseResources();
    instance_->setRateAndBufferSizeDetails(sampleRate, pulp::kQuantum);
    instance_->prepareToPlay(sampleRate, pulp::kQuantum);
    prepared_ = true;
    latency_.store(juce::jlimit(0, pulp::kMaxPdcSamples, instance_->getLatencySamples()));
    tail_ = juce::jlimit(0.0, 30.0, instance_->getTailLengthSeconds());
    const int chans = juce::jmax(2, instance_->getTotalNumInputChannels(), instance_->getTotalNumOutputChannels());
    work_.setSize(chans, pulp::kQuantum, false, true, false);
    numNotes_ = 0;
    lastBend_ = 0.0f;
}

void HostedPlugin::reset() {
    midi_.clear();
    numNotes_ = 0;
    if (prepared_) instance_->reset();
}

juce::Optional<juce::AudioPlayHead::PositionInfo> HostedPlugin::getPosition() const {
    PositionInfo info;
    info.setBpm(posTempo_);
    info.setIsPlaying(posPlaying_);
    info.setPpqPosition(posTick_ / (double)pulp::kPPQ);
    info.setTimeInSamples(posSamples_);
    info.setTimeInSeconds((double)posSamples_ / juce::jmax(1.0, sr_));
    info.setTimeSignature(juce::AudioPlayHead::TimeSignature{4, 4});
    return info;
}

void HostedPlugin::process(pulp::ProcessContext& c) {
    if (!prepared_) return;
    // parameters that automation, a link or a knob in PULP has moved since the last quantum
    for (int i = 0; i < hosted_.size(); ++i) {
        const float v = params()[(size_t)i]->get();
        if (v != sent_[(size_t)i].load(std::memory_order_relaxed) && !held_[(size_t)i].load(std::memory_order_relaxed)) {
            sent_[(size_t)i].store(v, std::memory_order_relaxed);
            hosted_[i]->setValue(v);
        }
    }
    posTempo_ = c.tempo;
    posTick_ = c.tickPos;
    posPlaying_ = c.playing;
    posSamples_ = (pulp::int64)(c.tickPos * 60.0 * c.sampleRate / (juce::jmax(1.0, c.tempo) * pulp::kPPQ));

    // notes become MIDI; the note's colour group is its MIDI channel
    midi_.clear();
    if (instrument_) {
        if (c.pitchBendSemis != lastBend_) {
            lastBend_ = c.pitchBendSemis;
            // a plugin's bend range is its own business: two semitones each way is the usual default
            const int wheel = juce::jlimit(0, 16383, 8192 + (int)(juce::jlimit(-1.0f, 1.0f, c.pitchBendSemis / 2.0f) * 8191.0f));
            for (int ch = 1; ch <= 16; ++ch) midi_.addEvent(juce::MidiMessage::pitchWheel(ch, wheel), 0);
        }
        auto noteOff = [&](int index, int offset) {
            midi_.addEvent(juce::MidiMessage::noteOff(notes_[index].channel, notes_[index].key), offset);
            notes_[index] = notes_[--numNotes_];
        };
        for (int e = 0; e < c.numEvents; ++e) {
            const pulp::NoteEvent& ev = c.events[e];
            const int off = juce::jlimit(0, pulp::kQuantum - 1, ev.offset);
            switch (ev.type) {
                case pulp::NoteEvent::On: {
                    const int ch = juce::jlimit(1, 16, ev.group + 1), key = juce::jlimit(0, 127, ev.key);
                    midi_.addEvent(juce::MidiMessage::noteOn(ch, key, juce::jlimit(0.004f, 1.0f, ev.velocity)), off);
                    if (numNotes_ < 128) notes_[numNotes_++] = {ev.id, key, ch};
                    break;
                }
                case pulp::NoteEvent::Off:
                case pulp::NoteEvent::KillNote:
                    for (int i = numNotes_; --i >= 0;) if (notes_[i].id == ev.id) noteOff(i, off);
                    break;
                case pulp::NoteEvent::AllOff:
                    while (numNotes_ > 0) noteOff(numNotes_ - 1, off);
                    break;
                case pulp::NoteEvent::Kill:
                    numNotes_ = 0;
                    for (int ch = 1; ch <= 16; ++ch) midi_.addEvent(juce::MidiMessage::allSoundOff(ch), off);
                    break;
                default: break;         // slide notes have no MIDI equivalent
            }
        }
    }
    const int chans = work_.getNumChannels();
    for (int ch = 0; ch < chans; ++ch) {
        if (monoIn_ && ch == 0) {
            // a mono plugin hears the sum of the two sides
            float* dst = work_.getWritePointer(0);
            for (int i = 0; i < pulp::kQuantum; ++i) dst[i] = 0.5f * (c.io[0][i] + c.io[1][i]);
        }
        else if (monoIn_) work_.clear(ch, 0, pulp::kQuantum);
        else if (!instrument_ && ch < 2) std::memcpy(work_.getWritePointer(ch), c.io[ch], sizeof(float) * (size_t)pulp::kQuantum);
        else if (sidechain_ && ch < 4 && c.sidechain[ch - 2] != nullptr) std::memcpy(work_.getWritePointer(ch), c.sidechain[ch - 2], sizeof(float) * (size_t)pulp::kQuantum);
        else work_.clear(ch, 0, pulp::kQuantum);
    }
    instance_->processBlock(work_, midi_);
    const int outs = instance_->getTotalNumOutputChannels();
    if (outs >= 1) {
        const float* l = work_.getReadPointer(0);
        const float* r = work_.getReadPointer(outs >= 2 ? 1 : 0);
        for (int i = 0; i < pulp::kQuantum; ++i) {
            // a misbehaving plugin must not poison the mix with NaNs or infinities
            const float a = l[i], b = r[i];
            c.io[0][i] = std::isfinite(a) ? a : 0.0f;
            c.io[1][i] = std::isfinite(b) ? b : 0.0f;
        }
    }
    const int lat = instance_->getLatencySamples();
    if (lat != latency_.load(std::memory_order_relaxed)) latency_.store(juce::jlimit(0, pulp::kMaxPdcSamples, lat), std::memory_order_relaxed);
}

// The plugin changed a parameter itself (its own window, a preset, its state being restored).
void HostedPlugin::parameterValueChanged(int index, float value) {
    const int i = index >= 0 && index < (int)slotOf_.size() ? slotOf_[(size_t)index] : -1;
    if (i < 0) return;
    sent_[(size_t)i].store(value, std::memory_order_relaxed);
    params()[(size_t)i]->set(value);
    if (held_[(size_t)i].load(std::memory_order_relaxed)) {
        // while a control is held in the plugin's window its movement can be recorded like any knob's
        auto alive = alive_;
        pulp::Param* p = params()[(size_t)i].get();
        juce::MessageManager::callAsync([alive, p] { if (alive->load()) ParamUi::touch(p, 1); });
    }
}

void HostedPlugin::parameterGestureChanged(int index, bool starting) {
    const int i = index >= 0 && index < (int)slotOf_.size() ? slotOf_[(size_t)index] : -1;
    if (i < 0) return;
    held_[(size_t)i].store(starting);
    lastTouched_.store(i);
    auto alive = alive_;
    pulp::Param* p = params()[(size_t)i].get();
    juce::MessageManager::callAsync([alive, p, starting] {
        if (!alive->load()) return;
        ParamUi::touch(p, starting ? 0 : 2);
        if (!starting && PluginHost::get().onEdited) PluginHost::get().onEdited(juce::String(p->info.name));
    });
}

pulp::Json HostedPlugin::saveExtra() const {
    // values PULP has set that the plugin has not been given yet (it gets them as it processes) go in first,
    // so the saved state says what the project says
    for (int i = 0; i < hosted_.size(); ++i) {
        const float v = params()[(size_t)i]->get();
        if (v != sent_[(size_t)i].load() && !held_[(size_t)i].load()) { sent_[(size_t)i].store(v); hosted_[i]->setValue(v); }
    }
    juce::MemoryBlock block;
    instance_->getStateInformation(block);
    pulp::Json j = pulp::Json::object();
    j.set("state", block.toBase64Encoding().toStdString());
    j.set("name", name_);
    j.set("maker", description_.manufacturerName.toStdString());
    j.set("plugin", description_.fileOrIdentifier.toStdString());
    return j;
}

void HostedPlugin::loadExtra(const pulp::Json& j, const std::string&) {
    const std::string state = j["state"].str("");
    if (state.empty()) return;
    juce::MemoryBlock block;
    if (!block.fromBase64Encoding(juce::String(state))) return;
    // The project's parameter values were read just before this. They are the newer word where the two
    // disagree (a value set in PULP that the plugin had not taken into its state when it was saved).
    std::vector<float> wanted((size_t)hosted_.size());
    for (int i = 0; i < hosted_.size(); ++i) wanted[(size_t)i] = params()[(size_t)i]->get();
    const bool hadValues = loadedParams_;
    instance_->setStateInformation(block.getData(), (int)block.getSize());
    for (int i = 0; i < hosted_.size(); ++i) {
        float v = hosted_[i]->getValue();
        if (hadValues && std::abs(v - wanted[(size_t)i]) > 1.0e-6f) { v = wanted[(size_t)i]; hosted_[i]->setValue(v); }
        sent_[(size_t)i].store(v);
        params()[(size_t)i]->set(v);
    }
}

// The project file lists the parameters it saved; a state with no such list (a preset) decides everything itself.
void HostedPlugin::loadState(const pulp::Json& j, const std::string& projectDir) {
    loadedParams_ = j["params"].isObject() && j["params"].size() > 0;
    pulp::Plugin::loadState(j, projectDir);
    loadedParams_ = false;
}

// ------------------------------------------------------------------ the plugin's own window

// The plugin's editor under a thin strip of PULP's own: which control was touched last (its menu creates
// an automation clip or a link, like any knob's), and a button for the list of parameters.
class PluginHost::Window : public juce::DocumentWindow {
public:
    static constexpr int kStrip = 26;

    class Content : public juce::Component, private juce::Timer {
    public:
        Content(PluginHost& host, std::shared_ptr<pulp::Plugin> plugin) : host_(host), plugin_(std::move(plugin)) {
            auto* hp = static_cast<HostedPlugin*>(plugin_.get());
            // most plugins bring their own editor; one that does not gets a plain list of its parameters
            editor_.reset(hp->instance().hasEditor() ? hp->instance().createEditorIfNeeded() : nullptr);
            if (editor_ == nullptr) editor_ = std::make_unique<juce::GenericAudioProcessorEditor>(hp->instance());
            addAndMakeVisible(*editor_);
            touched_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3d4349));
            params_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3d4349));
            touched_.setButtonText("Move a control, then click here to automate it");
            touched_.setTooltip("The control you moved last in this plugin. Click for its menu: create an automation clip, link it to a controller, edit its events.");
            params_.setButtonText("PARAMETERS");
            params_.setTooltip("Show this plugin's parameters as PULP knobs.");
            addAndMakeVisible(touched_);
            addAndMakeVisible(params_);
            touched_.onClick = [this] {
                if (auto* p = static_cast<HostedPlugin*>(plugin_.get())->lastTouched()) ParamUi::showMenu(p, &touched_, [] {});
            };
            params_.onClick = [this] { if (host_.onShowParameters) host_.onShowParameters(plugin_); };
            setSize(juce::jmax(320, editor_->getWidth()), editor_->getHeight() + kStrip);
            startTimerHz(5);
        }
        ~Content() override {
            stopTimer();
            editor_.reset();
        }
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff2b2f34)); }
        void resized() override {
            auto strip = getLocalBounds().removeFromTop(kStrip).reduced(3, 3);
            params_.setBounds(strip.removeFromRight(104));
            strip.removeFromRight(4);
            touched_.setBounds(strip);
            const juce::Rectangle<int> want(0, kStrip, getWidth(), getHeight() - kStrip);
            if (resizable() && editor_->getWidth() >= 320) { if (editor_->getBounds() != want) editor_->setBounds(want); }
            else editor_->setTopLeftPosition(juce::jmax(0, (getWidth() - editor_->getWidth()) / 2), kStrip);
        }
        // the plugin resized its own editor
        void childBoundsChanged(juce::Component* child) override {
            if (child != editor_.get()) return;
            const int w = juce::jmax(320, editor_->getWidth()), h = editor_->getHeight() + kStrip;
            if (w != getWidth() || h != getHeight()) setSize(w, h);
        }
        bool resizable() const { return editor_->isResizable(); }
        pulp::Plugin* plugin() const { return plugin_.get(); }
        juce::String touchedText() const { return touched_.getButtonText(); }
    private:
        void timerCallback() override {
            auto* hp = static_cast<HostedPlugin*>(plugin_.get());
            auto* p = hp->lastTouched();
            if (p == nullptr) return;
            int index = 0;
            for (auto& q : hp->params()) { if (q.get() == p) break; ++index; }
            const auto text = juce::String(p->info.name) + ":  " + hp->valueText(index);
            if (text != touched_.getButtonText()) touched_.setButtonText(text);
        }
        PluginHost& host_;
        std::shared_ptr<pulp::Plugin> plugin_;
        std::unique_ptr<juce::AudioProcessorEditor> editor_;
        juce::TextButton touched_, params_;
    };

    Window(PluginHost& host, std::shared_ptr<pulp::Plugin> plugin, const juce::String& title)
        : juce::DocumentWindow(title, juce::Colour(0xff2b2f34), juce::DocumentWindow::closeButton), host_(host) {
        setUsingNativeTitleBar(true);
        content_ = new Content(host, std::move(plugin));
        setContentOwned(content_, true);
        setResizable(content_->resizable(), false);
        const int n = host.windows_.size();
        centreWithSize(getWidth(), getHeight());
        setTopLeftPosition(getX() + (n % 5) * 28, juce::jmax(20, getY() + (n % 5) * 28));
        setVisible(true);
        toFront(true);
    }
    ~Window() override { clearContentComponent(); }
    void closeButtonPressed() override { host_.windows_.removeObject(this); }
    bool keyPressed(const juce::KeyPress& k) override { return (host_.onKey && host_.onKey(k)) || juce::DocumentWindow::keyPressed(k); }
    pulp::Plugin* plugin() const { return content_->plugin(); }
    juce::String touchedText() const { return content_->touchedText(); }
private:
    PluginHost& host_;
    Content* content_ = nullptr;
};

void PluginHost::showEditor(std::shared_ptr<pulp::Plugin> plugin, const juce::String& title) {
    if (!isHosted(plugin.get())) return;
    for (auto* w : windows_)
        if (static_cast<Window*>(w)->plugin() == plugin.get()) { w->setName(title); w->setVisible(true); w->toFront(true); return; }
    windows_.add(new Window(*this, std::move(plugin), title));
}

void PluginHost::closeEditorsFor(pulp::Plugin* plugin) {
    for (int i = windows_.size(); --i >= 0;)
        if (static_cast<Window*>(windows_[i])->plugin() == plugin) windows_.remove(i);
}

// A window keeps its plugin alive; when the project no longer contains the plugin, the window goes.
void PluginHost::closeEditorsNotIn(const pulp::Project& project) {
    for (int i = windows_.size(); --i >= 0;) {
        auto* plug = static_cast<Window*>(windows_[i])->plugin();
        bool found = false;
        for (auto& c : project.channels) found = found || c.live->plugin.get() == plug;
        for (auto& t : project.mixer) for (auto& sl : t.slots) found = found || sl.get() == plug;
        if (!found) windows_.remove(i);
    }
}

void PluginHost::closeWindows() { windows_.clear(); }

juce::String PluginHost::describeEditors() const {
    juce::String out = "plugin windows=" + juce::String(windows_.size());
    for (auto* w : windows_)
        out << " [" << w->getName() << " " << w->getWidth() << "x" << w->getHeight() << " touched='" << static_cast<Window*>(w)->touchedText() << "']";
    return out;
}

// The plugin draws into a native child window, which an ordinary component snapshot cannot see:
// ask Windows to render the whole window.
bool PluginHost::captureWindow(int index, const juce::File& png) const {
   #if JUCE_WINDOWS
    if (index < 0 || index >= windows_.size() || windows_[index]->getPeer() == nullptr) return false;
    HWND hwnd = (HWND)windows_[index]->getPeer()->getNativeHandle();
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return false;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    const BOOL drawn = PrintWindow(hwnd, mem, 2 /* PW_RENDERFULLCONTENT */);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    juce::Image image(juce::Image::ARGB, w, h, true);
    bool ok = false;
    {
        juce::Image::BitmapData data(image, juce::Image::BitmapData::writeOnly);
        std::vector<juce::uint8> pixels((size_t)w * (size_t)h * 4);
        ok = drawn != 0 && GetDIBits(mem, bmp, 0, (UINT)h, pixels.data(), &bi, DIB_RGB_COLORS) != 0;
        for (int y = 0; y < h && ok; ++y) {
            const juce::uint8* row = pixels.data() + (size_t)y * (size_t)w * 4;
            for (int x = 0; x < w; ++x) data.setPixelColour(x, y, juce::Colour(row[x * 4 + 2], row[x * 4 + 1], row[x * 4]));
        }
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!ok) return false;
    png.getParentDirectory().createDirectory();
    png.deleteFile();
    juce::FileOutputStream out(png);
    return out.openedOk() && juce::PNGImageFormat().writeImageToStream(image, out);
   #else
    juce::ignoreUnused(index, png);
    return false;
   #endif
}

// ------------------------------------------------------------------ PluginHost

PluginHost& PluginHost::get() { static PluginHost h; return h; }

juce::File PluginHost::listFile() { return Session::dataDir().getChildFile("plugins.xml"); }

void PluginHost::start() {
    if (j_) return;
    j_ = std::make_unique<Juce>();
    j_->formats.addFormat(std::make_unique<juce::VST3PluginFormat>());
    load();
    pulp::PluginRegistry::global().setExternalFactory([this](const std::string& id) { return create(id); });
}

void PluginHost::shutdown() {
    if (scanThread_) { scanThread_->stopThread(60000); scanThread_.reset(); }
    windows_.clear();
    reuse_.clear();
    pulp::PluginRegistry::global().setExternalFactory(nullptr);
    onListChanged = nullptr;
    onShowParameters = nullptr;
    onKey = nullptr;
    onEdited = nullptr;
    j_.reset();
}

void PluginHost::load() {
    // a scripted run with no data folder of its own starts with an empty list and leaves the user's alone
    if (!Session::persistSettings && Session::dataDirOverride == juce::File()) return;
    if (!listFile().existsAsFile()) return;
    if (auto root = juce::parseXML(listFile())) {
        if (auto* k = root->getChildByName("KNOWNPLUGINS")) j_->known.recreateFromXml(*k);
        blacklist_.clear();
        folders.clear();
        for (auto* e : root->getChildWithTagNameIterator("BLACKLISTED")) blacklist_.add(e->getStringAttribute("file"));
        for (auto* e : root->getChildWithTagNameIterator("FOLDER")) folders.add(e->getStringAttribute("path"));
        scanned_ = true;
    }
}

void PluginHost::save() {
    if (!j_ || (!Session::persistSettings && Session::dataDirOverride == juce::File())) return;
    juce::XmlElement root("PULPPLUGINS");
    root.addChildElement(j_->known.createXml().release());
    for (auto& b : blacklist_) root.createNewChildElement("BLACKLISTED")->setAttribute("file", b);
    for (auto& f : folders) root.createNewChildElement("FOLDER")->setAttribute("path", f);
    listFile().getParentDirectory().createDirectory();
    root.writeTo(listFile());
}

std::vector<PluginHost::Entry> PluginHost::plugins() const {
    std::vector<Entry> out;
    if (!j_) return out;
    for (auto& d : j_->known.getTypes())
        out.push_back({"vst3:" + d.createIdentifierString().toStdString(), d.name, d.manufacturerName, d.fileOrIdentifier, d.isInstrument});
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name.compareIgnoreCase(b.name) < 0; });
    return out;
}

std::string PluginHost::findByName(const juce::String& part) const {
    for (auto& e : plugins()) if (e.name.containsIgnoreCase(part)) return e.typeId;
    return {};
}

void PluginHost::forget(const std::string& typeId) {
    if (!j_) return;
    if (auto d = j_->known.getTypeForIdentifierString(juce::String(typeId.substr(juce::jmin((size_t)5, typeId.size()))))) j_->known.removeType(*d);
    save();
    if (onListChanged) onListChanged();
}

void PluginHost::beginReuse(const pulp::Project& project) {
    reuse_.clear();
    for (auto& c : project.channels) if (isHosted(c.live->plugin.get())) reuse_.push_back(c.live->plugin);
    for (auto& t : project.mixer) for (auto& sl : t.slots) if (isHosted(sl.get())) reuse_.push_back(sl);
}

void PluginHost::endReuse() { reuse_.clear(); }

std::shared_ptr<pulp::Plugin> PluginHost::create(const std::string& typeId) {
    if (!j_ || typeId.rfind("vst3:", 0) != 0) return nullptr;
    // plugins are made on the interface thread; a render on another thread asks it to do so and waits
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
        struct Call { PluginHost* host; const std::string* id; std::shared_ptr<pulp::Plugin> result; };
        Call call{this, &typeId, nullptr};
        juce::MessageManager::getInstance()->callFunctionOnMessageThread([](void* p) -> void* {
            auto* c = static_cast<Call*>(p);
            c->result = c->host->create(*c->id);
            return nullptr;
        }, &call);
        return call.result;
    }
    for (size_t i = 0; i < reuse_.size(); ++i)
        if (typeId == reuse_[i]->typeId()) { auto p = reuse_[i]; reuse_.erase(reuse_.begin() + (std::ptrdiff_t)i); return p; }
    const auto desc = j_->known.getTypeForIdentifierString(juce::String(typeId.substr(5)));
    if (!desc) return nullptr;
    juce::String error;
    auto instance = j_->formats.createPluginInstance(*desc, 48000.0, pulp::kQuantum, error);
    if (!instance) return nullptr;
    return std::make_shared<HostedPlugin>(std::move(instance), *desc, typeId);
}

juce::StringArray PluginHost::defaultFiles() const {
    juce::VST3PluginFormat format;
    auto paths = format.getDefaultLocationsToSearch();
    for (auto& f : folders) paths.addIfNotAlreadyThere(juce::File(f));
    return format.searchPathsForPlugins(paths, true, false);
}

// What the child process does: look inside one plugin file and write down what it holds.
int PluginHost::runScanChild(const juce::String& file, const juce::File& out) {
    if (file.contains("pulp-crash-test")) { volatile int* p = nullptr; *p = 1; }       // lets the crash handling be tested
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile(found, file);
    juce::XmlElement root("SCAN");
    for (auto* d : found) root.addChildElement(d->createXml().release());
    return root.writeTo(out) ? 0 : 2;
}

class PluginHost::ScanThread : public juce::Thread {
public:
    ScanThread(PluginHost& h, juce::StringArray files, std::function<void(const juce::String&)> progress, std::function<void(int, int)> done)
        : juce::Thread("PULP plugin scan"), host_(h), files_(std::move(files)), progress_(std::move(progress)), done_(std::move(done)) {}
    void run() override {
        const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName();
        const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory);
        int found = 0, failed = 0;
        juce::OwnedArray<juce::PluginDescription> all;
        juce::StringArray bad;
        for (int i = 0; i < files_.size() && !threadShouldExit(); ++i) {
            const auto file = files_[i];
            const auto name = juce::File::createFileWithoutCheckingPath(file).getFileName();
            auto progress = progress_;
            juce::MessageManager::callAsync([progress, name, i, n = files_.size()] { if (progress) progress("Scanning " + juce::String(i + 1) + " of " + juce::String(n) + ": " + name); });
            const auto out = temp.getNonexistentChildFile("pulp-scan", ".xml", false);
            juce::ChildProcess child;
            bool ok = child.start(juce::StringArray{exe, "--scan-vst3", file, out.getFullPathName()}, 0);
            // a plugin gets 45 seconds to show itself; one that hangs (a licence dialog, a dongle) is abandoned
            if (ok) {
                const auto t0 = juce::Time::getMillisecondCounter();
                while (child.isRunning() && juce::Time::getMillisecondCounter() - t0 < 45000 && !threadShouldExit()) wait(20);
                if (child.isRunning()) { child.kill(); ok = false; }
            }
            if (threadShouldExit()) { out.deleteFile(); break; }
            if (ok && child.getExitCode() != 0) ok = false;
            std::unique_ptr<juce::XmlElement> xml = ok ? juce::parseXML(out) : nullptr;
            out.deleteFile();
            if (!xml) { bad.add(file); ++failed; continue; }
            for (auto* e : xml->getChildIterator()) {
                auto d = std::make_unique<juce::PluginDescription>();
                if (d->loadFromXml(*e)) { all.add(d.release()); ++found; }
            }
        }
        // back on the interface thread: take the results in
        auto* host = &host_;
        auto done = done_;
        auto shared = std::make_shared<juce::OwnedArray<juce::PluginDescription>>();
        shared->swapWith(all);
        juce::MessageManager::callAsync([host, shared, bad, found, failed, done] {
            if (!host->j_) return;
            for (auto* d : *shared) host->j_->known.addType(*d);
            for (auto& b : bad) host->blacklist_.addIfNotAlreadyThere(b);
            host->scanned_ = true;
            host->save();
            if (host->onListChanged) host->onListChanged();
            if (done) done(found, failed);
        });
    }
private:
    PluginHost& host_;
    juce::StringArray files_;
    std::function<void(const juce::String&)> progress_;
    std::function<void(int, int)> done_;
};

void PluginHost::scan(const juce::StringArray& files, bool rescanKnown, std::function<void(const juce::String&)> progress, std::function<void(int, int)> done) {
    if (!j_ || scanning()) return;
    juce::StringArray todo;
    for (auto& f : files) {
        if (blacklist_.contains(f) && !rescanKnown) continue;
        bool known = false;
        for (auto& d : j_->known.getTypes()) known = known || d.fileOrIdentifier == f;
        if (known && !rescanKnown) continue;
        todo.add(f);
    }
    if (rescanKnown) for (auto& f : todo) blacklist_.removeString(f);
    scanThread_ = std::make_unique<ScanThread>(*this, todo, std::move(progress), std::move(done));
    scanThread_->startThread();
}
