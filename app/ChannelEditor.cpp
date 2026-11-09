#include "ChannelEditor.h"
#include "pulp/Multisample.h"
#include "pulp/Sampler.h"
#include "PluginHost.h"
#include "BeatViews.h"
#include "pulp/Ripple.h"
#include "pulp/Sextet.h"
#include "pulp/MidiOut.h"
#include "pulp/Creative.h"
#include "pulp/Sheaf.h"

// The keyboard range with each recording's stretch of keys, for a multisample instrument.
class ChannelEditor::ZoneMap : public juce::Component, public juce::SettableTooltipClient {
public:
    pulp::ms::InstrumentPtr inst;
    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        theme::insetBox(g, r);
        if (!inst || inst->zones.empty()) {
            g.setColour(theme::textDim);
            g.setFont(theme::font(12.0f));
            g.drawText("No instrument yet: drop one or more recordings of single notes here", getLocalBounds(), juce::Justification::centred, false);
            return;
        }
        const float kw = (r.getWidth() - 2.0f) / 128.0f;
        int n = 0;
        int lastRoot = -1;
        for (auto& z : inst->zones) {
            if (z.root == lastRoot) continue;           // layers and takes share their keys: draw them once
            lastRoot = z.root;
            const juce::Rectangle<float> zr(r.getX() + 1.0f + z.loKey * kw, r.getY() + 1.0f, (z.hiKey - z.loKey + 1) * kw, r.getHeight() - 2.0f);
            g.setColour(theme::palette(n++ + 8).withAlpha(0.55f));
            g.fillRect(zr.reduced(0.5f, 0.0f));
            // the key the note was recorded on
            g.setColour(juce::Colours::white);
            g.fillRect(r.getX() + 1.0f + z.root * kw, r.getY() + 1.0f, juce::jmax(2.0f, kw), r.getHeight() - 2.0f);
            if (zr.getWidth() > 26.0f) {
                g.setColour(juce::Colours::black);
                g.setFont(theme::font(10.0f, true));
                g.drawText(juce::String(pulp::ms::noteName(z.root)) + (z.loop ? " loop" : ""), zr.withTrimmedLeft((z.root - z.loKey + 1) * kw + 2.0f), juce::Justification::centredLeft, false);
            }
        }
        g.setColour(theme::textDim.withAlpha(0.6f));
        for (int c = 0; c <= 120; c += 12) g.drawVerticalLine((int)(r.getX() + 1.0f + c * kw), r.getBottom() - 5.0f, r.getBottom() - 1.0f);
    }
};

// ------------------------------------------------------------------ WaveformView

void WaveformView::setSample(pulp::SamplePtr s) {
    sample_ = std::move(s);
    peaksWidth_ = 0;
    repaint();
}

void WaveformView::buildPeaks(int width) {
    peaksWidth_ = width;
    lo_.assign((size_t)width, 0.0f);
    hi_.assign((size_t)width, 0.0f);
    if (!sample_ || width <= 0) return;
    const auto frames = sample_->frames();
    const float* l = sample_->left();
    const float* r = sample_->right();
    for (int x = 0; x < width; ++x) {
        const auto a = frames * x / width, b = std::max<pulp::int64>(a + 1, frames * (x + 1) / width);
        const auto stride = std::max<pulp::int64>(1, (b - a) / 64);
        float mn = 0.0f, mx = 0.0f;
        for (auto i = a; i < b && i < frames; i += stride) {
            const float v = 0.5f * (l[i] + r[i]);
            mn = std::min(mn, v); mx = std::max(mx, v);
        }
        lo_[(size_t)x] = mn; hi_[(size_t)x] = mx;
    }
}

void WaveformView::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    theme::insetBox(g, r);
    const int w = getWidth() - 2;
    if (!sample_ || w <= 2) {
        g.setColour(theme::textDim);
        g.setFont(theme::font(12.0f));
        g.drawText(emptyText, getLocalBounds(), juce::Justification::centred, false);
        return;
    }
    if (peaksWidth_ != w) buildPeaks(w);
    const float mid = r.getCentreY(), h = r.getHeight() * 0.46f;
    g.setColour(theme::line.brighter(0.2f));
    g.drawHorizontalLine((int)mid, r.getX() + 1, r.getRight() - 1);
    g.setColour(colour);
    for (int x = 0; x < w; ++x) {
        const float top = mid - hi_[(size_t)x] * h, bot = mid - lo_[(size_t)x] * h;
        g.drawVerticalLine(x + 1, top, std::max(bot, top + 1.0f));
    }
    if (showTrim) {
        const float xs = 1.0f + start * (float)w, xe = 1.0f + end * (float)w;
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRect(1.0f, 1.0f, xs - 1.0f, r.getHeight() - 2.0f);
        g.fillRect(xe, 1.0f, (float)w - xe + 1.0f, r.getHeight() - 2.0f);
        g.setColour(theme::accent);
        g.drawVerticalLine((int)xs, 1.0f, r.getBottom() - 1.0f);
        g.drawVerticalLine(juce::jmin(getWidth() - 2, (int)xe), 1.0f, r.getBottom() - 1.0f);
    }
}

void WaveformView::mouseDown(const juce::MouseEvent& e) {
    dragging_ = 0;
    if (showTrim && sample_) {
        const float x = (float)(e.x - 1) / (float)juce::jmax(1, getWidth() - 2);
        if (std::fabs(x - start) * getWidth() < 7.0f) dragging_ = 1;
        else if (std::fabs(x - end) * getWidth() < 7.0f) dragging_ = 2;
    }
    if (dragging_ == 0 && onClick) onClick();
}

void WaveformView::mouseDrag(const juce::MouseEvent& e) {
    if (dragging_ == 0) return;
    const float x = juce::jlimit(0.0f, 1.0f, (float)(e.x - 1) / (float)juce::jmax(1, getWidth() - 2));
    if (dragging_ == 1) start = juce::jmin(x, end - 0.005f);
    else end = juce::jmax(x, start + 0.005f);
    repaint();
    if (onTrim) onTrim(start, end, false);
}

void WaveformView::mouseUp(const juce::MouseEvent&) {
    if (dragging_ != 0 && onTrim) onTrim(start, end, true);
    dragging_ = 0;
}

// ------------------------------------------------------------------ KeyboardStrip

juce::String KeyboardStrip::keyName(int key) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return juce::String(names[((key % 12) + 12) % 12]) + juce::String(key / 12);
}

int KeyboardStrip::keyAt(juce::Point<int> p) const {
    int whites = 0;
    for (int k = lowKey; k < lowKey + numKeys; ++k) if (!isBlack(k)) ++whites;
    const float ww = (float)getWidth() / (float)whites;
    // black keys sit on top
    if (p.y < getHeight() * 0.6f) {
        int wi = 0;
        for (int k = lowKey; k < lowKey + numKeys; ++k) {
            if (isBlack(k)) { const float x = wi * ww; if (p.x >= x - ww * 0.32f && p.x < x + ww * 0.32f) return k; }
            else ++wi;
        }
    }
    int wi = 0;
    for (int k = lowKey; k < lowKey + numKeys; ++k) {
        if (isBlack(k)) continue;
        if (p.x >= wi * ww && p.x < (wi + 1) * ww) return k;
        ++wi;
    }
    return -1;
}

void KeyboardStrip::paint(juce::Graphics& g) {
    int whites = 0;
    for (int k = lowKey; k < lowKey + numKeys; ++k) if (!isBlack(k)) ++whites;
    const float ww = (float)getWidth() / (float)whites, h = (float)getHeight();
    int wi = 0;
    for (int k = lowKey; k < lowKey + numKeys; ++k) {
        if (isBlack(k)) continue;
        const juce::Rectangle<float> r(wi * ww, 0.0f, ww - 1.0f, h);
        g.setColour(k == down_ ? theme::accent : juce::Colour(0xffd4d8dc));
        g.fillRect(r);
        if (k % 12 == 0) {
            g.setColour(juce::Colour(0xff50565c));
            g.setFont(theme::font(9.0f));
            g.drawText(keyName(k), r.withTrimmedTop(h - 12.0f), juce::Justification::centred, false);
        }
        ++wi;
    }
    wi = 0;
    for (int k = lowKey; k < lowKey + numKeys; ++k) {
        if (!isBlack(k)) { ++wi; continue; }
        const juce::Rectangle<float> r(wi * ww - ww * 0.32f, 0.0f, ww * 0.64f, h * 0.6f);
        g.setColour(k == down_ ? theme::accent : juce::Colour(0xff1c1f22));
        g.fillRect(r);
    }
}

void KeyboardStrip::press(const juce::MouseEvent& e) {
    const int k = keyAt(e.getPosition());
    if (k == down_) return;
    if (down_ >= 0 && onNoteOff) onNoteOff(down_);
    down_ = k;
    if (k >= 0 && onNoteOn) onNoteOn(k, juce::jlimit(0.2f, 1.0f, 0.3f + 0.7f * (float)e.y / (float)juce::jmax(1, getHeight())));
    repaint();
}
void KeyboardStrip::mouseDown(const juce::MouseEvent& e) { press(e); }
void KeyboardStrip::mouseDrag(const juce::MouseEvent& e) { press(e); }
void KeyboardStrip::mouseUp(const juce::MouseEvent&) {
    if (down_ >= 0 && onNoteOff) onNoteOff(down_);
    down_ = -1;
    repaint();
}

// ------------------------------------------------------------------ ChannelEditor

class ChannelEditor::Body : public juce::Component {
public:
    juce::String info;
    int infoHeight = 18;
    void paint(juce::Graphics& g) override {
        g.fillAll(theme::panel);
        g.setColour(theme::textDim);
        g.setFont(theme::font(11.5f));
        g.drawFittedText(info, 8, 4, getWidth() - 16, infoHeight, juce::Justification::centredLeft, 2);
    }
};

ChannelEditor::ChannelEditor(Session& s) : s_(s) {
    body_ = std::make_unique<Body>();
    view_.setViewedComponent(body_.get(), false);
    view_.setScrollBarsShown(true, false);
    view_.setScrollBarThickness(10);
    addAndMakeVisible(view_);
    body_->addChildComponent(wave_);
    body_->addChildComponent(presetBtn_);
    presetBtn_.setTooltip("Presets for this instrument: click for the list, right-click for the next one");
    presetBtn_.onClick = [this] { presetMenu(); };
    presetBtn_.onRightClick = [this] {
        if (presets_.empty()) return;
        const auto it = presetIndex_.find(shownPlugin_);
        choosePreset(((it == presetIndex_.end() ? -1 : it->second) + 1) % (int)presets_.size());
    };
    zoneMap_ = std::make_unique<ZoneMap>();
    zoneMap_->setTooltip("The keyboard, with the stretch of keys each recording plays (the white line is the key it was recorded on). "
                         "Drop recordings of single notes on this window to build a new instrument from them.");
    body_->addChildComponent(*zoneMap_);
    buildBtn_.text = "BUILD FROM SAMPLES";
    buildBtn_.setTooltip("Choose one or more recordings of single notes (your own samples, or your recorded voice). PULP finds each one's pitch, "
                         "trims it, loops it if it is a held sound, and spreads them across the keyboard.");
    buildBtn_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Recordings of single notes", juce::File(), Session::audioFilePattern());
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                             [safe = juce::Component::SafePointer<ChannelEditor>(this), chooser](const juce::FileChooser& fc) {
            if (!safe || fc.getResults().isEmpty()) return;
            juce::StringArray files;
            for (auto& f : fc.getResults()) files.add(f.getFullPathName());
            safe->s_.buildInstrumentChannel(files, safe->s_.currentChannel);
        });
    };
    libraryBtn_.text = "LIBRARY";
    libraryBtn_.setTooltip("Every instrument of the factory library, by family: choose one to play it on this channel");
    libraryBtn_.onClick = [this] { libraryMenu(); };
    body_->addChildComponent(libraryBtn_);
    loadInstBtn_.text = "LOAD INSTRUMENT";
    loadInstBtn_.setTooltip("Load an instrument file (.pulpinst), for example one from the factory library");
    loadInstBtn_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Load an instrument", Session::factoryDir().getChildFile("Instruments"), "*.pulpinst");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe = juce::Component::SafePointer<ChannelEditor>(this), chooser](const juce::FileChooser& fc) {
            if (!safe || fc.getResult() == juce::File()) return;
            auto* ch = safe->s_.current();
            auto* t = ch ? dynamic_cast<pulp::Timbre*>(ch->live->plugin.get()) : nullptr;
            std::string err;
            if (!t) return;
            if (!t->loadFile(Session::projectPath(fc.getResult()), safe->s_.project.projectDir, &err)) { if (safe->s_.onMessage) safe->s_.onMessage("Could not load it: " + juce::String(err)); return; }
            ch->name = t->instrument()->name;
            safe->s_.commit("Load instrument");
        });
    };
    pluginWinBtn_.text = "OPEN PLUGIN WINDOW";
    pluginWinBtn_.setTooltip("Show this plugin's own window, with every one of its controls");
    pluginWinBtn_.onClick = [this] {
        if (auto* ch = s_.current())
            PluginHost::get().showEditor(ch->live->plugin, juce::String(ch->name) + "  -  " + ch->live->plugin->displayName());
    };
    body_->addChildComponent(pluginWinBtn_);
    wavetableBtn_.text = "LOAD WAVETABLE...";
    wavetableBtn_.setTooltip("Use a wavetable of your own: a WAV file of single cycles, 2048 samples each (or one cycle of any length). Choose \"User table\" as an oscillator's table to hear it.");
    wavetableBtn_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("A wavetable", juce::File(), Session::audioFilePattern());
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe = juce::Component::SafePointer<ChannelEditor>(this), chooser](const juce::FileChooser& fc) {
            auto* ch = safe != nullptr ? safe->s_.current() : nullptr;
            auto* ripple = ch ? dynamic_cast<pulp::Ripple*>(ch->live->plugin.get()) : nullptr;
            if (!ripple || !fc.getResult().existsAsFile()) return;
            std::string err;
            if (ripple->loadWavetable(fc.getResult().getFullPathName().toStdString(), &err)) safe->s_.commit("Load wavetable");
            else if (safe->s_.onMessage) safe->s_.onMessage("Could not load that wavetable: " + juce::String(err));
        });
    };
    body_->addChildComponent(wavetableBtn_);
    matrixBtn_.text = "EDIT ALGORITHM AS MATRIX";
    matrixBtn_.setTooltip("Copies the chosen algorithm's routing into the Matrix and Out knobs and switches Algorithm to \"Custom matrix\": the same sound, now yours to rewire.");
    matrixBtn_.onClick = [this] {
        auto* ch = s_.current();
        auto* sextet = ch ? dynamic_cast<pulp::Sextet*>(ch->live->plugin.get()) : nullptr;
        if (!sextet) return;
        const int algo = sextet->pAlgo->getInt();
        if (algo < 1) { if (s_.onMessage) s_.onMessage("Algorithm is already \"Custom matrix\": the Matrix and Out knobs are what plays."); return; }
        sextet->applyAlgorithm(algo);
        s_.commit("Algorithm to matrix");
    };
    body_->addChildComponent(matrixBtn_);
    analyseBtn_.text = "ANALYSE RECORDING...";
    analyseBtn_.setTooltip("Choose a recording of one note (or drop one on this window): Sheaf measures its harmonics over time and plays that sound at any pitch, as the \"User\" spectrum shape.");
    analyseBtn_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>("A recording of one note", juce::File(), Session::audioFilePattern());
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safe = juce::Component::SafePointer<ChannelEditor>(this), chooser](const juce::FileChooser& fc) {
            if (safe != nullptr && fc.getResult().existsAsFile()) safe->s_.setChannelSample(safe->s_.currentChannel, fc.getResult());
        });
    };
    body_->addChildComponent(analyseBtn_);
    clearAnalysisBtn_.text = "CLEAR";
    clearAnalysisBtn_.setTooltip("Forget the analysed sound: the \"User\" shape is a plain saw again.");
    clearAnalysisBtn_.onClick = [this] {
        auto* ch = s_.current();
        auto* sheaf = ch ? dynamic_cast<pulp::Sheaf*>(ch->live->plugin.get()) : nullptr;
        if (!sheaf) return;
        sheaf->clearAnalysis();
        s_.commit("Clear analysed sound");
    };
    body_->addChildComponent(clearAnalysisBtn_);
    padGrid_ = std::make_unique<PadGrid>(s_);
    padGrid_->onSelect = [this](int) { if (auto* ch = s_.current()) { showPluginSections(ch->live->plugin.get()); resized(); } };
    body_->addChildComponent(*padGrid_);
    sliceView_ = std::make_unique<SliceView>(s_);
    body_->addChildComponent(*sliceView_);
    body_->addChildComponent(buildBtn_);
    body_->addChildComponent(loadInstBtn_);
    body_->addAndMakeVisible(pluginPanel_);
    body_->addAndMakeVisible(stripPanel_);
    addAndMakeVisible(keys_);

    wave_.showTrim = true;
    wave_.setTooltip("Sample: drag the two markers to trim the start and end, click to hear it, drop a file to replace it");
    wave_.onClick = [this] { s_.engine.noteOn(s_.currentChannel, pulp::kRootKey, pulp::kDefaultVelocity); s_.engine.noteOff(s_.currentChannel, pulp::kRootKey); };
    wave_.onTrim = [this](float a, float b, bool done) {
        if (auto* ch = s_.current())
            if (auto* smp = dynamic_cast<pulp::Sampler*>(ch->live->plugin.get())) { smp->pStart->set(a); smp->pEnd->set(b); }
        if (done) s_.commit("Trim sample");
    };
    keys_.setTooltip("Preview keyboard: click a key to play the channel. Lower on the key is louder.");
    keys_.onNoteOn = [this](int k, float v) { s_.engine.noteOn(s_.currentChannel, k, v); };
    keys_.onNoteOff = [this](int k) { s_.engine.noteOff(s_.currentChannel, k); };

    auto touched = [this](pulp::Param*) { s_.publish(); wave_.repaint(); };
    auto ended = [this](pulp::Param*) { s_.commit("Change channel setting"); };
    pluginPanel_.onChange = touched; pluginPanel_.onGestureEnd = ended;
    stripPanel_.onChange = touched; stripPanel_.onGestureEnd = ended;
    s_.addListener(this);
    rebuild();
}

ChannelEditor::~ChannelEditor() { s_.removeListener(this); }

void ChannelEditor::projectChanged() {
    auto* ch = s_.current();
    const int id = ch ? ch->id : -1;
    pulp::Plugin* plug = ch ? ch->live->plugin.get() : nullptr;
    if (id != shownId_ || plug != shownPlugin_) rebuild();
    else {
        // same channel: refresh what may have changed underneath
        if (auto* smp = dynamic_cast<pulp::Sampler*>(plug)) {
            if (smp->sample() != wave_.sample()) rebuild();
            else { wave_.start = smp->pStart->get(); wave_.end = smp->pEnd->get(); wave_.repaint(); }
        }
        if (auto* t = dynamic_cast<pulp::Timbre*>(plug)) if (t->instrument() != zoneMap_->inst) { rebuild(); return; }
        // a new sound for the grains, or a new (or cleared) analysed sound: the picture and the text follow
        if (auto* motes = dynamic_cast<pulp::Motes*>(plug)) if (motes->sample() != wave_.sample()) { rebuild(); return; }
        if (auto* sheaf = dynamic_cast<pulp::Sheaf*>(plug)) if (sheaf->analysis().get() != shownAnalysis_) { rebuild(); return; }
        if (auto* kit = dynamic_cast<pulp::PadKit*>(plug)) {
            // the pad's knobs are only rebuilt when a different pad or sound is showing: this is often called
            // from inside one of those knobs (a drag that just ended), which must not be destroyed under it
            const auto title = juce::String(padGrid_->selected) + ":" + juce::String(kit->padName(padGrid_->selected)) + ":" + juce::String(kit->layerCount(padGrid_->selected));
            if (title != shownPad_) showPluginSections(plug);
            padGrid_->repaint();
        }
        if (dynamic_cast<pulp::Chop*>(plug)) sliceView_->refresh();
        if (onTitle && ch) onTitle(juce::String(ch->name) + "  -  " + plug->displayName());
        repaint();
    }
}

// The instrument's own knobs. A pad sampler shows the knobs of the pad that is selected, not all eighty.
void ChannelEditor::showPluginSections(pulp::Plugin* plug) {
    std::vector<ParamPanel::Section> sections;
    const auto groups = plug->paramGroups();
    if (auto* kit = dynamic_cast<pulp::PadKit*>(plug)) {
        const int pad = juce::jlimit(0, pulp::PadKit::kPads - 1, padGrid_->selected);
        shownPad_ = juce::String(pad) + ":" + juce::String(kit->padName(pad)) + ":" + juce::String(kit->layerCount(pad));
        ParamPanel::Section sec;
        sec.title = "Pad " + juce::String(pad + 1) + (kit->layerCount(pad) > 0 ? ": " + juce::String(kit->padName(pad)) : juce::String(" (empty)"));
        for (auto& id : groups[(size_t)pad].ids) if (auto* p = plug->param(id)) sec.params.push_back(p);
        sections.push_back(std::move(sec));
    } else if (groups.empty()) {
        ParamPanel::Section sec;
        sec.title = plug->params().empty() ? juce::String() : juce::String(plug->displayName());
        for (auto& p : plug->params()) sec.params.push_back(p.get());
        if (!sec.params.empty()) sections.push_back(std::move(sec));
    } else {
        for (auto& gr : groups) {
            ParamPanel::Section sec;
            sec.title = gr.title;
            for (auto& id : gr.ids) if (auto* p = plug->param(id)) sec.params.push_back(p);
            sections.push_back(std::move(sec));
        }
    }
    pluginPanel_.setSections(std::move(sections));
}

void ChannelEditor::rebuild() {
    auto* ch = s_.current();
    shownId_ = ch ? ch->id : -1;
    shownPlugin_ = ch ? ch->live->plugin.get() : nullptr;
    if (!ch) { pluginPanel_.setSections({}); stripPanel_.setSections({}); wave_.setVisible(false); return; }
    auto* plug = ch->live->plugin.get();

    showPluginSections(plug);

    auto& l = *ch->live;
    stripPanel_.setSections({
        {"Levels", {&l.volume, &l.pan, &l.pitch, &l.pitchRange}},
        {"Polyphony", {&l.poly, &l.mono, &l.porta, &l.portaTime}},
        {"Cut groups", {&l.cut, &l.cutBy}},
        {"Time", {&l.gate, &l.shift, &l.swingMix}},
        {"Arpeggiator", {&l.arpDir, &l.arpChord, &l.arpRange, &l.arpTime, &l.arpGate, &l.arpRepeat}},
        {"Echo delay", {&l.echoCount, &l.echoTime, &l.echoFeed, &l.echoPan, &l.echoPitch, &l.echoModX, &l.echoModY}},
        {"Tracking", {&l.velTrack, &l.keyPan, &l.keyModX}},
    });

    hasWave_ = false;
    sampleInfo_.clear();
    if (auto* smp = dynamic_cast<pulp::Sampler*>(plug)) {
        hasWave_ = true;
        auto sample = smp->sample();
        wave_.setSample(sample);
        wave_.start = smp->pStart->get();
        wave_.end = smp->pEnd->get();
        wave_.colour = juce::Colour(ch->color).brighter(0.3f);
        if (sample) {
            const auto path = juce::String(sample->path);
            sampleInfo_ = (path.isEmpty() ? juce::String("(generated)") : path.fromLastOccurrenceOf("/", false, false).fromLastOccurrenceOf("\\", false, false))
                        + "   " + juce::String((double)sample->frames() / sample->sampleRate, 2) + " s, " + juce::String(sample->sampleRate, 0) + " Hz, "
                        + (sample->channels > 1 ? "stereo" : "mono");
        } else sampleInfo_ = "No sample loaded";
    } else if (auto* t = dynamic_cast<pulp::Timbre*>(plug)) {
        const auto inst = t->instrument();
        zoneMap_->inst = inst;
        if (inst && !inst->zones.empty()) {
            int lo = 127, hi = 0, loops = 0;
            for (auto& z : inst->zones) { lo = juce::jmin(lo, z.root); hi = juce::jmax(hi, z.root); loops += z.loop ? 1 : 0; }
            sampleInfo_ = juce::String(inst->name) + ": " + juce::String((int)inst->zones.size()) + (inst->zones.size() == 1 ? " recording" : " recordings") + ", "
                        + juce::String(pulp::ms::noteName(lo)) + " to " + juce::String(pulp::ms::noteName(hi)) + ", " + (loops > 0 ? "held notes loop" : "notes ring out")
                        + (inst->credit.empty() ? juce::String() : ".  " + juce::String(inst->credit));
        } else sampleInfo_ = "Timbre plays instruments made of recorded notes. Drop recordings here, or use the buttons.";
    } else if (std::string(plug->typeId()) == "layer") {
        juce::StringArray names;
        for (int id : ch->layerChildren) { const int i = s_.project.channelIndex(id); if (i >= 0) names.add(s_.project.channels[(size_t)i].name); }
        sampleInfo_ = names.isEmpty() ? "Layer with no children. Select channels in the rack, then right-click this channel and choose Layer: set children."
                                      : "Plays: " + names.joinIntoString(", ");
    } else if (auto* hp = dynamic_cast<HostedPlugin*>(plug)) {
        sampleInfo_ = hp->description().name + " by " + hp->description().manufacturerName + ", a VST3 plugin. Its own window has every control; the knobs below are "
                    + juce::String(juce::jmin(48, (int)hp->params().size())) + " of its " + juce::String(hp->totalParameters()) + " parameters.";
    } else if (auto* motes = dynamic_cast<pulp::Motes*>(plug)) {
        // the sound the grains are taken from, drawn like a sampler's (without trim markers)
        hasWave_ = true;
        auto sound = motes->sample();
        wave_.setSample(sound);
        wave_.start = 0.0f;
        wave_.end = 1.0f;
        wave_.colour = juce::Colour(ch->color).brighter(0.3f);
        sampleInfo_ = sound ? juce::String(sound->path).fromLastOccurrenceOf("/", false, false).fromLastOccurrenceOf("\\", false, false) + "   "
                                + juce::String((double)sound->frames() / sound->sampleRate, 2) + " s: the grains are taken from this sound"
                            : juce::String("No sound loaded: drop any recording here and Motes plays it as a cloud of grains");
    } else if (auto* sheaf = dynamic_cast<pulp::Sheaf*>(plug)) {
        const auto a = sheaf->analysis();
        shownAnalysis_ = a.get();
        sampleInfo_ = a ? juce::String(a->name) + ": " + juce::String(a->partials) + " harmonics of a " + juce::String(a->hz, 1) + " Hz note measured at "
                              + juce::String(a->frames) + " moments over " + juce::String(a->seconds, 2) + " s. A spectrum whose shape is \"User\" plays it, at any pitch."
                        : juce::String("Drop a recording of one note here: Sheaf measures its harmonics and plays that sound at any pitch (spectrum shape \"User\").");
    } else if (auto* mo = dynamic_cast<pulp::MidiOut*>(plug)) {
        const auto names = s_.midiOutputNames();
        const int port = mo->port();
        sampleInfo_ = "Sends this channel's notes as MIDI to output " + juce::String(port + 1) + ": "
                    + (port < names.size() ? names[port] : (s_.usingNullDevice() ? juce::String("(silent test mode: nothing is sent)") : juce::String("nothing is connected there")))
                    + (names.size() > 1 ? ".  Outputs: " + names.joinIntoString(", ") : juce::String());
    } else if (dynamic_cast<pulp::MissingPlugin*>(plug)) {
        sampleInfo_ = "This plugin is not installed on this PC, or has not been found yet (Settings > Plugins). It makes no sound; its settings are kept and saved back untouched.";
    }
    pluginWinBtn_.setVisible(dynamic_cast<HostedPlugin*>(plug) != nullptr);
    wavetableBtn_.setVisible(dynamic_cast<pulp::Ripple*>(plug) != nullptr);
    matrixBtn_.setVisible(dynamic_cast<pulp::Sextet*>(plug) != nullptr);
    {
        auto* sh = dynamic_cast<pulp::Sheaf*>(plug);
        analyseBtn_.setVisible(sh != nullptr);
        clearAnalysisBtn_.setVisible(sh != nullptr && sh->hasAnalysis());
    }
    padGrid_->setVisible(dynamic_cast<pulp::PadKit*>(plug) != nullptr);
    sliceView_->setVisible(dynamic_cast<pulp::Chop*>(plug) != nullptr);
    sliceView_->refresh();
    keys_.lowKey = (padGrid_->isVisible() || sliceView_->isVisible()) ? 48 : 36;      // pads and slices start at C5
    keys_.repaint();
    wave_.showTrim = dynamic_cast<pulp::Sampler*>(plug) != nullptr;
    wave_.setVisible(hasWave_);
    hasZones_ = dynamic_cast<pulp::Timbre*>(plug) != nullptr;
    zoneMap_->setVisible(hasZones_);
    buildBtn_.setVisible(hasZones_);
    loadInstBtn_.setVisible(hasZones_);
    libraryBtn_.setVisible(hasZones_);
    zoneMap_->repaint();
    presets_ = plug->factoryPresets();
    presetBtn_.setVisible(!presets_.empty());
    {
        const auto it = presetIndex_.find(plug);
        presetBtn_.text = it != presetIndex_.end() && it->second < (int)presets_.size() ? "Preset: " + juce::String(presets_[(size_t)it->second].name)
                                                                                         : "Presets (" + juce::String((int)presets_.size()) + ")";
        presetBtn_.repaint();
    }
    body_->info = sampleInfo_;
    body_->infoHeight = hasWave_ ? 18 : 34;
    if (onTitle) onTitle(juce::String(ch->name) + "  -  " + plug->displayName());
    resized();
    repaint();
}

void ChannelEditor::resized() {
    auto r = getLocalBounds();
    keys_.setBounds(r.removeFromBottom(46));
    view_.setBounds(r);
    const int w = juce::jmax(200, view_.getMaximumVisibleWidth());
    int y = 4;
    if (hasWave_) { wave_.setBounds(6, y + 18, w - 12, 84); y += 108; }
    else if (hasZones_) {
        y += 40;
        zoneMap_->setBounds(6, y, w - 12, 34);
        y += 40;
        buildBtn_.setBounds(6, y, 150, 24);
        loadInstBtn_.setBounds(162, y, 130, 24);
        libraryBtn_.setBounds(298, y, 80, 24);
        y += 30;
    }
    else if (sampleInfo_.isNotEmpty()) y += 40;
    if (pluginWinBtn_.isVisible()) { pluginWinBtn_.setBounds(6, y, 170, 24); y += 30; }
    if (wavetableBtn_.isVisible()) { wavetableBtn_.setBounds(6, y + 4, 160, 24); y += 34; }
    if (matrixBtn_.isVisible()) { matrixBtn_.setBounds(6, y + 4, 200, 24); y += 34; }
    if (analyseBtn_.isVisible()) {
        analyseBtn_.setBounds(6, y + 4, 190, 24);
        if (clearAnalysisBtn_.isVisible()) clearAnalysisBtn_.setBounds(202, y + 4, 80, 24);
        y += 34;
    }
    if (padGrid_->isVisible()) { padGrid_->setBounds(0, y, w, PadGrid::kHeight); y += PadGrid::kHeight + 2; }
    if (sliceView_->isVisible()) { sliceView_->setBounds(0, y, w, SliceView::kHeight); y += SliceView::kHeight + 2; }
    if (presetBtn_.isVisible()) { presetBtn_.setBounds(6, y, juce::jmin(260, w - 12), 24); y += 30; }
    const int ph = pluginPanel_.heightFor(w);
    pluginPanel_.setBounds(0, y, w, ph);
    y += ph;
    const int sh = stripPanel_.heightFor(w);
    stripPanel_.setBounds(0, y, w, sh);
    y += sh;
    body_->setSize(w, y + 4);
}

void ChannelEditor::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
}

// The factory library as a menu: families, then instruments. Choosing one loads it into this channel's Timbre.
void ChannelEditor::libraryMenu() {
    juce::PopupMenu m;
    std::vector<juce::File> files;
    const auto root = Session::factoryDir().getChildFile("Instruments");
    juce::Array<juce::File> families;
    root.findChildFiles(families, juce::File::findDirectories, false);
    families.sort();
    for (auto& family : families) {
        juce::PopupMenu sub;
        juce::Array<juce::File> found;
        family.findChildFiles(found, juce::File::findFiles, true, "*.pulpinst");
        found.sort();
        for (auto& f : found) { sub.addItem(1 + (int)files.size(), f.getFileNameWithoutExtension()); files.push_back(f); }
        if (sub.getNumItems() > 0) m.addSubMenu(family.getFileName(), sub);
    }
    if (files.empty()) m.addItem(-1, "The factory instruments are not built yet (run build.bat library)", false);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&libraryBtn_), [safe = juce::Component::SafePointer<ChannelEditor>(this), files](int r) {
        if (safe == nullptr || r <= 0 || r > (int)files.size()) return;
        auto* ch = safe->s_.current();
        auto* t = ch ? dynamic_cast<pulp::Timbre*>(ch->live->plugin.get()) : nullptr;
        std::string err;
        if (!t) return;
        if (!t->loadFile(Session::projectPath(files[(size_t)(r - 1)]), safe->s_.project.projectDir, &err)) { if (safe->s_.onMessage) safe->s_.onMessage("Could not load it: " + juce::String(err)); return; }
        ch->name = t->instrument()->name;
        safe->s_.commit("Choose library instrument");
    });
}

void ChannelEditor::presetMenu() {
    juce::PopupMenu m;
    const auto it = presetIndex_.find(shownPlugin_);
    const int cur = it == presetIndex_.end() ? -1 : it->second;
    for (int i = 0; i < (int)presets_.size(); ++i) m.addItem(i + 1, presets_[(size_t)i].name, true, i == cur);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetBtn_),
                    [safe = juce::Component::SafePointer<ChannelEditor>(this)](int r) { if (safe && r > 0) safe->choosePreset(r - 1); });
}

void ChannelEditor::choosePreset(int index) {
    auto* ch = s_.current();
    if (!ch || index < 0 || index >= (int)presets_.size() || ch->live->plugin.get() != shownPlugin_) return;
    ch->live->plugin->loadState(presets_[(size_t)index].state, s_.project.projectDir);
    presetIndex_[shownPlugin_] = index;
    presetBtn_.text = "Preset: " + juce::String(presets_[(size_t)index].name);
    presetBtn_.repaint();
    s_.commit("Choose preset");
}

bool ChannelEditor::isInterestedInFileDrag(const juce::StringArray& files) {
    if (hasZones_) { for (auto& f : files) if (Session::isAudioFile(juce::File(f))) return true; return false; }
    return files.size() == 1 && Session::isAudioFile(juce::File(files[0])) && s_.current() != nullptr;
}
void ChannelEditor::filesDropped(const juce::StringArray& files, int, int) {
    // several files on a pad sampler are a kit
    if (padGrid_->isVisible() && files.size() > 1) {
        juce::Array<juce::File> list;
        for (auto& f : files) list.add(juce::File(f));
        s_.loadPadKit(s_.currentChannel, list);
        return;
    }
    // on a multisample instrument every dropped file is one note of a new instrument
    if (hasZones_) { s_.buildInstrumentChannel(files, s_.currentChannel); return; }
    if (!s_.setChannelSample(s_.currentChannel, juce::File(files[0])))
        if (Session::isAudioFile(juce::File(files[0]))) s_.addSampleChannel(juce::File(files[0]));
}
void ChannelEditor::itemDropped(const SourceDetails& d) { filesDropped({d.description.toString().substring(5)}, 0, 0); }
