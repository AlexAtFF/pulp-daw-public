#include "Browser.h"
#include "pulp/Multisample.h"
#include "pulp/Stretch.h"
#include "pulp/Sampler.h"
#include "PluginHost.h"

namespace {

enum class Kind { Section, Folder, File, Plugin, Action };

class Item : public juce::TreeViewItem {
public:
    Item(Browser& b, Kind k, juce::String label) : b_(b), kind(k), label_(std::move(label)) {}
    Browser& b_;
    Kind kind;
    juce::String label_;
    juce::File file;
    std::string pluginId;
    bool effect = false;                        // a plugin item that is an effect (goes to the mixer, not the rack)
    int preset = -1;                            // a plugin item that is one of the plugin's presets
    std::function<void()> action;
    std::function<void(Item&)> populate;        // sections fill themselves when opened
    bool userRoot = false;                      // a folder the user added (can be removed)

    // a built-in plugin opens to show its presets (a hosted one keeps its presets in its own window)
    bool hasPresets() const { return kind == Kind::Plugin && preset < 0 && pluginId.rfind("vst3:", 0) != 0; }
    bool mightContainSubItems() override { return kind == Kind::Section || kind == Kind::Folder || hasPresets(); }
    int getItemHeight() const override { return 20; }
    juce::String getUniqueName() const override { return label_ + "|" + file.getFullPathName() + "|" + juce::String(pluginId) + "|" + juce::String(preset); }
    bool canBeSelected() const override { return kind != Kind::Section; }

    juce::String getTooltip() override {
        switch (kind) {
            case Kind::File:
                if (file.hasFileExtension("mid;midi")) return file.getFileName() + " (MIDI file): double-click or drag to the Channel Rack to bring it in as channels and a pattern";
                if (file.hasFileExtension("sf2")) return file.getFileName() + " (SoundFont): double-click or drag to the Channel Rack to choose one of its presets as an instrument";
                if (Session::isInstrumentFile(file)) return file.getFileNameWithoutExtension() + " (instrument): click to hear a note, double-click or drag to the Channel Rack to play it";
                return file.getFileName() + ": click to hear it, double-click or drag to the Channel Rack to use it, right-click for more";
            case Kind::Plugin:
                if (preset >= 0 && effect) return label_ + " (preset): double-click to use it on the selected mixer track (on the effect of this kind there, or in a free slot)";
                if (preset >= 0) return label_ + " (preset): double-click to use it on the selected channel if that is this instrument, or in a new channel";
                if (effect) return label_ + " (effect): double-click to put it in a free slot of the selected mixer track; open it to see its presets";
                return label_ + ": double-click or drag to the Channel Rack to add it as a channel; open it to see its presets";
            case Kind::Folder: return file.getFullPathName();
            case Kind::Action: return label_;
            default: return {};
        }
    }

    void paintItem(juce::Graphics& g, int w, int h) override {
        if (isSelected()) { g.setColour(theme::accent.withAlpha(0.25f)); g.fillRect(0, 0, w, h); }
        juce::Colour c = theme::text;
        bool bold = false;
        switch (kind) {
            case Kind::Section: c = theme::accent; bold = true; break;
            case Kind::Folder: c = theme::text; break;
            case Kind::File: c = b_.isFavourite(file) ? theme::warn : (Session::isInstrumentFile(file) ? theme::good.brighter(0.2f) : theme::accent2.brighter(0.2f)); break;
            case Kind::Plugin: c = preset >= 0 ? theme::text : (effect ? theme::accent2.brighter(0.2f) : theme::good); break;
            case Kind::Action: c = theme::textDim; break;
        }
        // a small glyph for the kind of thing this is
        g.setColour(c.withAlpha(0.8f));
        const juce::Rectangle<float> ic(2.0f, (float)h * 0.5f - 5.0f, 10.0f, 10.0f);
        if (kind == Kind::Folder || kind == Kind::Section) g.fillRoundedRectangle(ic.withTrimmedTop(2.0f), 1.5f);
        else if (kind == Kind::File) { for (int i = 0; i < 4; ++i) { const float hh = 3.0f + (float)((i * 5) % 7); g.fillRect(ic.getX() + i * 2.6f, ic.getCentreY() - hh * 0.5f, 1.6f, hh); } }
        else if (kind == Kind::Plugin) g.fillEllipse(ic.reduced(1.0f));
        else { g.fillRect(ic.getX() + 4.0f, ic.getY(), 2.0f, 10.0f); g.fillRect(ic.getX(), ic.getY() + 4.0f, 10.0f, 2.0f); }
        g.setColour(c);
        g.setFont(theme::font(12.5f, bold));
        g.drawText(label_, 16, 0, w - 18, h, juce::Justification::centredLeft, true);
    }

    void itemOpennessChanged(bool open) override {
        if (!open || getNumSubItems() > 0) return;
        if (kind == Kind::Folder) {
            juce::Array<juce::File> dirs, files;
            for (const auto& e : juce::RangedDirectoryIterator(file, false, "*", juce::File::findFilesAndDirectories)) {
                const auto f = e.getFile();
                if (f.isDirectory()) {
                    // an instrument's folder shows as the instrument, not as a folder full of its samples
                    const auto inst = f.getChildFile(f.getFileName() + ".pulpinst");
                    if (inst.existsAsFile()) files.add(inst); else dirs.add(f);
                }
                else if (Session::isAudioFile(f) || Session::isInstrumentFile(f) || f.hasFileExtension("mid;midi;sf2")) files.add(f);
            }
            dirs.sort(); files.sort();
            for (auto& d : dirs) { auto* it = new Item(b_, Kind::Folder, d.getFileName()); it->file = d; addSubItem(it); }
            for (auto& f : files) { auto* it = new Item(b_, Kind::File, f.getFileNameWithoutExtension()); it->file = f; addSubItem(it); }
        } else if (hasPresets()) {
            // the presets come from the plugin itself
            if (auto p = pulp::PluginRegistry::global().create(pluginId)) {
                int i = 0;
                for (auto& pr : p->factoryPresets()) {
                    auto* it = new Item(b_, Kind::Plugin, pr.name);
                    it->pluginId = pluginId;
                    it->effect = effect;
                    it->preset = i++;
                    addSubItem(it);
                }
            }
        } else if (populate) populate(*this);
    }

    void itemSelectionChanged(bool selected) override {
        if (selected && kind == Kind::File) b_.preview(file);
    }

    void itemClicked(const juce::MouseEvent& e) override {
        if (kind == Kind::Action && !e.mods.isPopupMenu()) { if (action) action(); return; }
        if (kind == Kind::Section || kind == Kind::Folder) {
            if (e.mods.isPopupMenu() && userRoot) {
                juce::PopupMenu m;
                m.addItem(1, "Remove this folder from the browser");
                m.addItem(2, "Show in Explorer");
                const auto f = file;
                auto* b = &b_;
                m.showMenuAsync(juce::PopupMenu::Options(), [b, f](int r) { if (r == 1) b->removeFolder(f); else if (r == 2) f.revealToUser(); });
            } else if (!e.mods.isPopupMenu()) setOpen(!isOpen());
            return;
        }
        if (kind == Kind::File) {
            if (e.mods.isPopupMenu()) {
                juce::PopupMenu m;
                const bool fav = b_.isFavourite(file);
                m.addItem(1, "Open in a new channel");
                m.addItem(2, "Replace the selected channel's sample");
                m.addItem(5, "Slice it in a new channel (Chop)", Session::isAudioFile(file));
                m.addSeparator();
                m.addItem(3, fav ? "Remove from favourites" : "Add to favourites");
                m.addItem(4, "Show in Explorer");
                const auto f = file;
                auto* b = &b_;
                m.showMenuAsync(juce::PopupMenu::Options(), [b, f](int r) {
                    if (r == 1) b->openFile(f);
                    else if (r == 2) { auto& s = b->session(); if (!s.setChannelSample(s.currentChannel, f)) b->openFile(f); }
                    else if (r == 3) b->toggleFavourite(f);
                    else if (r == 4) f.revealToUser();
                    else if (r == 5) b->session().addChopChannel(f);
                });
            } else b_.preview(file);
        }
    }

    void itemDoubleClicked(const juce::MouseEvent&) override {
        if (kind == Kind::File) b_.openFile(file);
        else if (kind == Kind::Plugin && effect) { if (b_.session().onUseEffect) b_.session().onUseEffect(pluginId, preset); }
        else if (kind == Kind::Plugin) b_.session().useInstrument(pluginId, preset);
    }

    juce::var getDragSourceDescription() override {
        if (kind == Kind::File) return "file:" + file.getFullPathName();
        if (kind == Kind::Plugin && !effect) return "plugin:" + juce::String(pluginId) + (preset >= 0 ? "#" + juce::String(preset) : juce::String());
        return {};
    }
};

void findFiles(const juce::File& dir, const juce::String& needle, juce::Array<juce::File>& out, int& budget) {
    for (const auto& e : juce::RangedDirectoryIterator(dir, true, Session::audioFilePattern() + ";*.pulpinst;*.sfz", juce::File::findFiles)) {
        if (--budget <= 0 || out.size() >= 300) return;
        if (e.getFile().getFileName().containsIgnoreCase(needle)) out.add(e.getFile());
    }
}

} // namespace

Browser::Browser(Session& s) : s_(s) {
    setOpaque(true);
    loadSettings();
    search_.setTextToShowWhenEmpty("Search sounds", theme::textDim);
    search_.setFont(theme::font(13.0f));
    search_.addListener(this);
    search_.setEscapeAndReturnKeysConsumed(false);
    addAndMakeVisible(search_);
    tree_.setRootItemVisible(false);
    tree_.setIndentSize(14);
    tree_.setDefaultOpenness(false);
    tree_.setColour(juce::TreeView::backgroundColourId, theme::panelLo);
    tree_.setColour(juce::TreeView::selectedItemBackgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(tree_);
    wave_.emptyText = "";
    wave_.setTooltip("Preview of the last sound you clicked. Click to hear it again.");
    wave_.onClick = [this] { if (auto smp = wave_.sample()) s_.engine.preview.play(smp, 0.7f); };
    addAndMakeVisible(wave_);
    sync_.text = "SYNC";
    sync_.setTooltip("Preview in time: when a sound has a steady beat, play its preview stretched to the project tempo");
    sync_.lit = syncPreview_;
    sync_.onClick = [this] { syncPreview_ = !syncPreview_; sync_.setLit(syncPreview_); saveSettings(); };
    addAndMakeVisible(sync_);
    s_.addListener(this);
    refresh();
}

Browser::~Browser() {
    s_.removeListener(this);
    tree_.setRootItem(nullptr);
}

void Browser::loadSettings() {
    if (auto x = Session::loadSettings("BROWSER")) {
        for (auto* e : x->getChildWithTagNameIterator("FOLDER")) folders_.addIfNotAlreadyThere(e->getStringAttribute("path"));
        for (auto* e : x->getChildWithTagNameIterator("FAVOURITE")) favourites_.addIfNotAlreadyThere(e->getStringAttribute("path"));
        syncPreview_ = x->getBoolAttribute("syncPreview", false);
    }
}

void Browser::saveSettings() {
    auto x = std::make_unique<juce::XmlElement>("BROWSER");
    for (auto& f : folders_) x->createNewChildElement("FOLDER")->setAttribute("path", f);
    for (auto& f : favourites_) x->createNewChildElement("FAVOURITE")->setAttribute("path", f);
    x->setAttribute("syncPreview", syncPreview_);
    Session::saveSettings(std::move(x));
}

void Browser::refresh() {
    std::unique_ptr<juce::XmlElement> open;
    if (root_) open = tree_.getOpennessState(true);
    tree_.setRootItem(nullptr);
    auto root = std::make_unique<Item>(*this, Kind::Section, "root");
    const auto needle = searchText();

    if (needle.isNotEmpty()) {
        auto* res = new Item(*this, Kind::Section, "Search results");
        juce::Array<juce::File> found;
        int budget = 40000;
        findFiles(Session::factoryDir(), needle, found, budget);
        for (auto& f : folders_) findFiles(juce::File(f), needle, found, budget);
        for (auto& f : found) { auto* it = new Item(*this, Kind::File, f.getFileNameWithoutExtension()); it->file = f; res->addSubItem(it); }
        if (found.isEmpty()) res->addSubItem(new Item(*this, Kind::Action, "Nothing matches \"" + needle + "\""));
        root->addSubItem(res);
        res->setOpen(true);
    } else {
        auto* factory = new Item(*this, Kind::Folder, "Factory library");
        factory->file = Session::factoryDir();
        root->addSubItem(factory);

        auto* proj = new Item(*this, Kind::Section, "Current project");
        juce::StringArray seen;
        for (auto& ch : s_.project.channels)
            if (auto* smp = dynamic_cast<pulp::Sampler*>(ch.live->plugin.get())) {
                const auto path = smp->samplePath();
                if (path.empty()) continue;
                const auto f = s_.resolvePath(path);
                if (seen.contains(f.getFullPathName())) continue;
                seen.add(f.getFullPathName());
                auto* it = new Item(*this, Kind::File, f.getFileNameWithoutExtension());
                it->file = f;
                proj->addSubItem(it);
            }
        // the undo history, newest last; click a step to go back to it
        auto* hist = new Item(*this, Kind::Section, "History");
        const int n = (int)s_.history.size();
        for (int i = juce::jmax(0, n - 60); i < n; ++i) {
            auto* it = new Item(*this, Kind::Action, juce::String(i == s_.historyPos ? "> " : "   ") + juce::String(i + 1) + ". " + s_.history[(size_t)i].name);
            it->action = [this, i] { jumpToHistory(i); };
            hist->addSubItem(it);
        }
        proj->addSubItem(hist);
        root->addSubItem(proj);

        auto* mine = new Item(*this, Kind::Section, "My folders");
        for (auto& f : folders_) {
            auto* it = new Item(*this, Kind::Folder, juce::File(f).getFileName());
            it->file = juce::File(f);
            it->userRoot = true;
            mine->addSubItem(it);
        }
        auto* add = new Item(*this, Kind::Action, "Add a folder...");
        add->action = [this] { addFolder(); };
        mine->addSubItem(add);
        root->addSubItem(mine);

        auto* favs = new Item(*this, Kind::Section, "Favourites");
        for (auto& f : favourites_) { auto* it = new Item(*this, Kind::File, juce::File(f).getFileNameWithoutExtension()); it->file = juce::File(f); favs->addSubItem(it); }
        root->addSubItem(favs);

        auto* plugins = new Item(*this, Kind::Section, "Plugin database");
        auto* inst = new Item(*this, Kind::Section, "Instruments");
        for (auto& t : pulp::PluginRegistry::global().types())
            if (t.instrument) { auto* it = new Item(*this, Kind::Plugin, t.name); it->pluginId = t.id; inst->addSubItem(it); }
        plugins->addSubItem(inst);
        {
            // the effects, by kind, as the mixer's slot menu has them
            auto* fx = new Item(*this, Kind::Section, "Effects");
            juce::StringArray cats;
            for (auto& t : pulp::PluginRegistry::global().types()) if (!t.instrument) cats.addIfNotAlreadyThere(t.category);
            for (auto& cat : cats) {
                auto* sec = new Item(*this, Kind::Section, cat);
                for (auto& t : pulp::PluginRegistry::global().types())
                    if (!t.instrument && juce::String(t.category) == cat) { auto* it = new Item(*this, Kind::Plugin, t.name); it->pluginId = t.id; it->effect = true; sec->addSubItem(it); }
                fx->addSubItem(sec);
            }
            auto* vstFx = new Item(*this, Kind::Section, "VST3 effects");
            for (auto& e : PluginHost::get().plugins())
                if (!e.instrument) { auto* it = new Item(*this, Kind::Plugin, e.name); it->pluginId = e.typeId; it->effect = true; vstFx->addSubItem(it); }
            if (vstFx->getNumSubItems() > 0) fx->addSubItem(vstFx); else delete vstFx;
            plugins->addSubItem(fx);
        }
        {
            // instruments from other makers (effects are added from a mixer slot's menu)
            auto* vst = new Item(*this, Kind::Section, "VST3 instruments");
            for (auto& e : PluginHost::get().plugins())
                if (e.instrument) { auto* it = new Item(*this, Kind::Plugin, e.name); it->pluginId = e.typeId; vst->addSubItem(it); }
            auto* find = new Item(*this, Kind::Action, "Find VST3 plugins...");
            find->action = [this] { if (s_.onShowSettings) s_.onShowSettings(2); };
            vst->addSubItem(find);
            plugins->addSubItem(vst);
        }
        root->addSubItem(plugins);

        if (!open) { factory->setOpen(true); mine->setOpen(true); }
    }
    root_ = std::move(root);
    tree_.setRootItem(root_.get());
    if (open && needle.isEmpty()) tree_.restoreOpennessState(*open, false);
    repaint();
}

void Browser::projectChanged() {
    juce::String sig;
    for (auto& ch : s_.project.channels)
        if (auto* smp = dynamic_cast<pulp::Sampler*>(ch.live->plugin.get())) sig << smp->samplePath() << "|";
    sig << "h" << (int)s_.history.size() << "@" << s_.historyPos;
    if (sig != projectSignature_) { projectSignature_ = sig; if (searchText().isEmpty()) refresh(); }
}

void Browser::textEditorTextChanged(juce::TextEditor&) { refresh(); }

void Browser::preview(const juce::File& f) {
    if (Session::isInstrumentFile(f)) {
        // an instrument: play the recording nearest the middle of the keyboard
        std::string err;
        auto inst = pulp::ms::loadInstrument(f.getFullPathName().toStdString(), &err);
        if (!inst) { info_ = "Cannot read " + f.getFileName(); wave_.setSample(nullptr); repaint(); return; }
        const pulp::ms::Zone* best = nullptr;
        for (auto& z : inst->zones) if (!best || std::abs(z.root - 60) < std::abs(best->root - 60)) best = &z;
        wave_.setSample(best ? best->sample : nullptr);
        info_ = juce::String(inst->name) + "   instrument, " + juce::String((int)inst->zones.size()) + " recordings" + (inst->credit.empty() ? juce::String() : "   " + juce::String(inst->credit));
        if (best) s_.engine.preview.play(best->sample, 0.7f * best->gain);
        repaint();
        return;
    }
    if (!Session::isAudioFile(f)) return;
    auto sample = pulp::SampleCache::global().load(Session::projectPath(f), s_.project.projectDir);
    if (!sample) { info_ = "Cannot read " + f.getFileName(); wave_.setSample(nullptr); repaint(); return; }
    wave_.setSample(sample);
    info_ = f.getFileName() + "   " + juce::String((double)sample->frames() / sample->sampleRate, 2) + " s  "
          + juce::String(sample->sampleRate / 1000.0, 1) + " kHz  " + (sample->channels > 1 ? "stereo" : "mono");
    // in sync with the tempo: a loop with a steady beat is stretched to the project tempo before it plays
    const double seconds = (double)sample->frames() / sample->sampleRate;
    if (syncPreview_ && seconds >= 1.5 && seconds <= 60.0) {
        const double bpm = pulp::stretch::detectTempo(*sample), tempo = s_.project.global->tempo.get();
        if (bpm > 0.0) {
            info_ << "  " << juce::String(bpm, 1) << " BPM";
            if (std::fabs(bpm - tempo) > 0.5) { s_.engine.preview.play(pulp::stretch::rhythmic(*sample, bpm / tempo), 0.7f); repaint(); return; }
        }
    }
    s_.engine.preview.play(sample, 0.7f);
    repaint();
}

void Browser::openFile(const juce::File& f) {
    s_.stopPreview();
    if (f.hasFileExtension("sf2")) { if (s_.onOpenSoundFont) s_.onOpenSoundFont(f); }
    else if (f.hasFileExtension("mid;midi")) { const auto what = s_.importMidiSong(f); if (what.isNotEmpty() && s_.onMessage) s_.onMessage(what); }
    else if (Session::isInstrumentFile(f)) s_.addInstrumentFileChannel(f);
    else s_.addSampleChannel(f);
}

bool Browser::openPath(const juce::StringArray& labels) {
    juce::TreeViewItem* at = root_.get();
    for (auto& label : labels) {
        if (!at) return false;
        at->setOpen(true);
        juce::TreeViewItem* next = nullptr;
        for (int i = 0; i < at->getNumSubItems() && !next; ++i)
            if (auto* it = dynamic_cast<Item*>(at->getSubItem(i))) if (it->label_.equalsIgnoreCase(label)) next = it;
        at = next;
    }
    if (!at) return false;
    at->setOpen(true);
    return true;
}

void Browser::addFolder() {
    auto chooser = std::make_shared<juce::FileChooser>("Add a folder of sounds to the browser", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this, chooser](const juce::FileChooser& fc) {
        const auto f = fc.getResult();
        if (f.isDirectory()) { folders_.addIfNotAlreadyThere(f.getFullPathName()); saveSettings(); refresh(); }
    });
}

void Browser::removeFolder(const juce::File& f) {
    folders_.removeString(f.getFullPathName());
    saveSettings();
    refresh();
}

void Browser::toggleFavourite(const juce::File& f) {
    const auto p = f.getFullPathName();
    if (favourites_.contains(p)) favourites_.removeString(p);
    else favourites_.add(p);
    saveSettings();
    refresh();
}

void Browser::resized() {
    auto r = getLocalBounds().reduced(4);
    r.removeFromTop(20);
    sync_.setBounds(getWidth() - 52, 3, 46, 19);
    search_.setBounds(r.removeFromTop(24));
    r.removeFromTop(4);
    auto bottom = r.removeFromBottom(84);
    wave_.setBounds(bottom.removeFromBottom(60));
    tree_.setBounds(r.withTrimmedBottom(2));
}

void Browser::paint(juce::Graphics& g) {
    g.fillAll(theme::panel);
    g.setColour(theme::text);
    g.setFont(theme::font(13.0f, true));
    g.drawText("BROWSER", 8, 4, getWidth() - 16, 18, juce::Justification::centredLeft, false);
    g.setColour(theme::textDim);
    g.setFont(theme::font(11.0f));
    g.drawFittedText(info_.isEmpty() ? juce::String("Click a sound to hear it") : info_, 6, getHeight() - 86, getWidth() - 12, 20, juce::Justification::centredLeft, 1, 0.75f);
    g.setColour(theme::line);
    g.drawVerticalLine(getWidth() - 1, 0.0f, (float)getHeight());
}
