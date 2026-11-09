#include "MainComponent.h"
#include "PluginHost.h"
#include "Shortcuts.h"

namespace {
// The reference program's two-row typing keyboard: Z is C, the Q row is an octave higher.
struct TypingKey { int code; int semis; };
const TypingKey kTyping[] = {
    {'Z', 0}, {'S', 1}, {'X', 2}, {'D', 3}, {'C', 4}, {'V', 5}, {'G', 6}, {'B', 7}, {'H', 8}, {'N', 9}, {'J', 10}, {'M', 11},
    {',', 12}, {'L', 13}, {'.', 14}, {';', 15}, {'/', 16},
    {'Q', 12}, {'2', 13}, {'W', 14}, {'3', 15}, {'E', 16}, {'R', 17}, {'5', 18}, {'T', 19}, {'6', 20}, {'Y', 21}, {'7', 22}, {'U', 23},
    {'I', 24}, {'9', 25}, {'O', 26}, {'0', 27}, {'P', 28}};
const int kNumTyping = (int)(sizeof kTyping / sizeof kTyping[0]);
}

int MainComponent::typingKeyOffset(int keyCode) {
    for (auto& k : kTyping) if (k.code == keyCode) return k.semis;
    return -1;
}

MainComponent::MainComponent(Session& s) : session_(s), toolbar_(s), browser_(s) {
    setOpaque(true);
    addAndMakeVisible(toolbar_);
    addAndMakeVisible(browser_);
    setWantsKeyboardFocus(true);
    getProperties().set("workTop", Toolbar::kHeight);
    getProperties().set("workLeft", kBrowserW);

    rack_ = new ChannelRack(s);
    chanEditor_ = new ChannelEditor(s);
    settings_ = new SettingsPages(s);
    events_ = new EventEditor(s);
    link_ = new LinkEditor(s);
    piano_ = new PianoRoll(s);
    playlist_ = new Playlist(s);
    mixer_ = new MixerView(s);
    auto* mixWin = addWindow("mixer", "Mixer", mixer_, {kBrowserW + 16, Toolbar::kHeight + 300, 1000, 470}, false);
    mixWin->setMinimumSize(560, 300);
    mixer_->onOpenEffect = [this](std::shared_ptr<pulp::Plugin> p, const juce::String& title) { openEffect(std::move(p), title); };
    auto* plWin = addWindow("playlist", "Playlist", playlist_, {kBrowserW + 630, Toolbar::kHeight + 14, 600, 440}, true);
    plWin->setMinimumSize(480, 220);
    auto* rackWin = addWindow("rack", "Channel rack", rack_, {kBrowserW + 16, Toolbar::kHeight + 14, 600, 300}, true);
    rackWin->setMinimumSize(380, 140);
    auto* pianoWin = addWindow("piano", "Piano roll", piano_, {kBrowserW + 16, Toolbar::kHeight + 324, 1010, 450}, false);
    pianoWin->setMinimumSize(520, 300);
    auto* chanWin = addWindow("chan", "Channel settings", chanEditor_, {kBrowserW + 632, Toolbar::kHeight + 14, 400, 620}, false);
    chanWin->setMinimumSize(300, 240);
    auto* setWin = addWindow("settings", "Settings", settings_, {kBrowserW + 120, Toolbar::kHeight + 90, 520, 498}, false);
    setWin->setMinimumSize(420, 400);
    auto* evWin = addWindow("events", "Events", events_, {kBrowserW + 200, Toolbar::kHeight + 240, 760, 300}, false);
    evWin->setMinimumSize(360, 180);
    events_->onTitle = [this](const juce::String& t) { window("events")->setTitle(t); };
    auto* linkWin = addWindow("link", "Link to controller", link_, {kBrowserW + 300, Toolbar::kHeight + 160, 520, 230}, false);
    linkWin->setMinimumSize(420, 220);
    link_->onTitle = [this](const juce::String& t) { window("link")->setTitle(t); };

    // What every knob, fader and switch offers on right-click, and what happens when one is moved.
    ParamUi::valid = [this](pulp::Param* p) { return !session_.project.addressOf(p).empty(); };
    ParamUi::touched = [this](pulp::Param* p, int phase) { session_.paramTouched(p, phase); };
    ParamUi::addMenuItems = [this](pulp::Param* p, juce::PopupMenu& m, int id) {
        const std::string address = session_.project.addressOf(p);
        if (address.empty()) return;
        m.addItem(id, "Create automation clip");
        m.addItem(id + 1, "Link to controller...");
        m.addItem(id + 2, "Edit events");
        if (session_.project.linkFor(address)) m.addItem(id + 3, "Remove the link");
    };
    ParamUi::menuChosen = [this](pulp::Param* p, int item) {
        const std::string address = session_.project.addressOf(p);
        if (address.empty()) return;
        if (item == 0) { if (session_.createAutomationClip(p) >= 0) window("playlist")->showAndRaise(); }
        else if (item == 1) showLink(address);
        else if (item == 2) showEvents(address);
        else if (item == 3) session_.removeLink(address);
    };

    auto* expWin = addWindow("export", "Export", makeExportPanel(), {kBrowserW + 220, Toolbar::kHeight + 120, 420, 330}, false);
    expWin->setMinimumSize(380, 300);
    toolbar_.addMenuButton("FILE", "File menu: new, open, save, versions, backups, export", [this](juce::Component* c) { fileMenu(c); });
    toolbar_.addMenuButton("EDIT", "Edit menu: undo, redo and the history of changes", [this](juce::Component* c) { editMenu(c); });

    rack_->onOpenChannel = [this](int index) { openChannel(index); };
    session_.onShowSettings = [this](int page) { showSettingsPage(page); };
    // an effect or one of its presets chosen in the Browser: onto the selected mixer track
    session_.onUseEffect = [this](const std::string& typeId, int preset) {
        auto& proj = session_.project;
        int track = proj.selectedMixerTrack;
        if (track < 0 || track > pulp::kNumInserts) track = pulp::kMasterTrack;
        auto& t = proj.mixer[(size_t)track];
        int slot = -1;
        // a preset goes onto an effect of this kind that is already there
        if (preset >= 0) for (int i = 0; i < pulp::kNumSlots && slot < 0; ++i) if (t.slots[i] && typeId == t.slots[i]->typeId()) slot = i;
        if (slot < 0) {
            for (int i = 0; i < pulp::kNumSlots && slot < 0; ++i) if (!t.slots[i]) slot = i;
            if (slot < 0) { showMessage("Every slot of that mixer track is in use."); return; }
            if (!mixer_->setEffect(track, slot, typeId)) { showMessage("That effect could not be loaded."); return; }
        }
        if (preset >= 0) {
            const auto presets = t.slots[slot]->factoryPresets();
            if (preset < (int)presets.size()) { t.slots[slot]->loadState(presets[(size_t)preset].state, proj.projectDir); session_.commit("Choose preset"); }
        }
        window("mixer")->showAndRaise();
        openEffect(t.slots[slot], juce::String(t.slots[slot]->displayName()) + "  -  " + juce::String(pulp::Project::mixerTrackName(track)) + " slot " + juce::String(slot + 1));
    };
    session_.onOpenSoundFont = [this](const juce::File& f) { window("rack")->showAndRaise(); rack_->soundFontMenu(f); };
    // hosted plugins: their windows hand back the keys they do not use, and offer PULP's own view of the plugin
    auto& host = PluginHost::get();
    host.onKey = [this](const juce::KeyPress& k) { return handleKey(k); };
    host.onEdited = [this](const juce::String& what) { session_.commit("Plugin: " + what); };
    host.onListChanged = [this] { browser_.refresh(); settings_->plugins().listChanged(); };
    host.onShowParameters = [this](std::shared_ptr<pulp::Plugin> p) {
        for (int i = 0; i < (int)session_.project.channels.size(); ++i)
            if (session_.project.channels[(size_t)i].live->plugin == p) { session_.selectChannel(i); window("chan")->showAndRaise(); return; }
        openEffectPanel(p, p->displayName());
    };
    rack_->onOpenPianoRoll = [this](int index) { session_.selectChannel(index); window("piano")->showAndRaise(); };
    chanEditor_->onTitle = [this](const juce::String& t) { window("chan")->setTitle(t); };
    playlist_->onOpenPattern = [this](int) { window("rack")->showAndRaise(); };

    toolbar_.addWindowButton("PLAYLIST", "Playlist (F5)", [this] { window("playlist")->toggle(); }, [this] { return window("playlist")->isVisible(); });
    toolbar_.addWindowButton("RACK", "Channel rack (F6)", [this] { window("rack")->toggle(); }, [this] { return window("rack")->isVisible(); });
    toolbar_.addWindowButton("PIANO", "Piano roll (F7)", [this] { window("piano")->toggle(); }, [this] { return window("piano")->isVisible(); });
    toolbar_.addWindowButton("MIXER", "Mixer (F9)", [this] { window("mixer")->toggle(); }, [this] { return window("mixer")->isVisible(); });
    toolbar_.addWindowButton("BROWSER", "Browser (Alt+F8)", [this] { toggleBrowser(); }, [this] { return browserVisible_; });
    toolbar_.addWindowButton("AUDIO", "Audio settings (F10)", [this] { window("settings")->toggle(); }, [this] { return window("settings")->isVisible(); });

    session_.onMessage = [this](const juce::String& m) { showMessage(m); };
    // questions are only asked when a person is at the controls (scripted runs never get dialogs)
    if (Session::persistSettings)
        session_.onAskStretch = [this](const juce::String& name, double bpm, std::function<void(bool)> answer) {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                             .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                             .withTitle("Fit to the tempo?")
                                             .withMessage(name + " sounds like " + juce::String(bpm, 1) + " BPM. The project is at " + juce::String(session_.project.global->tempo.get(), 1)
                                                          + " BPM.\n\nStretch it so that it follows the project tempo? You can change this later from the clip's menu (Stretch and pitch).")
                                             .withButton("Stretch to the tempo").withButton("Leave it as it is")
                                             .withAssociatedComponent(this),
                                         [answer](int r) { answer(r == 1); });
        };
    session_.addListener(this);
    registerShortcuts();
    setSize(1500, 860);
    loadLayout();
}

// The windows' places and which of them are showing are remembered between runs (layout.xml beside the settings).
// Scripted runs start from the standard layout unless they ask otherwise (--layout).
bool MainComponent::keepLayout = true;

void MainComponent::saveLayout() const {
    if (!keepLayout) return;
    juce::XmlElement root("LAYOUT");
    root.setAttribute("browser", browserVisible_);
    for (int i = 0; i < windows_.size(); ++i) {
        auto* e = root.createNewChildElement("WINDOW");
        e->setAttribute("id", windowIds_[i]);
        e->setAttribute("bounds", windows_[i]->getBounds().toString());
        e->setAttribute("visible", windows_[i]->isVisible());
    }
    Session::dataDir().createDirectory();
    root.writeTo(Session::dataDir().getChildFile("layout.xml"));
}

void MainComponent::loadLayout() {
    if (!keepLayout) return;
    auto root = juce::parseXML(Session::dataDir().getChildFile("layout.xml"));
    if (!root) return;
    browserVisible_ = root->getBoolAttribute("browser", true);
    for (auto* e : root->getChildWithTagNameIterator("WINDOW")) {
        auto* w = window(e->getStringAttribute("id"));
        if (!w) continue;
        auto b = juce::Rectangle<int>::fromString(e->getStringAttribute("bounds"));
        // a window is never restored to where it cannot be reached
        if (b.getWidth() >= 200 && b.getHeight() >= 100) w->setBounds(b.withPosition(juce::jlimit(-b.getWidth() + 80, 4000, b.getX()), juce::jlimit(Toolbar::kHeight, 3000, b.getY())));
        // the settings, export and editing windows always start closed
        const auto id = e->getStringAttribute("id");
        if (id == "rack" || id == "playlist" || id == "piano" || id == "mixer" || id == "chan") w->setVisible(e->getBoolAttribute("visible", w->isVisible()));
    }
    resized();
}

// Every program-wide command and its standard key (the reference program's defaults).
void MainComponent::registerShortcuts() {
    using K = juce::KeyPress;
    const auto ctrl = juce::ModifierKeys(juce::ModifierKeys::ctrlModifier), alt = juce::ModifierKeys(juce::ModifierKeys::altModifier), none = juce::ModifierKeys();
    const auto ctrlShift = juce::ModifierKeys(juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier);
    const auto ctrlAlt = juce::ModifierKeys(juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier);
    auto& sc = Shortcuts::get();
    sc.clear();
    sc.add("play", "Transport", "Play / stop", K(K::spaceKey, none, 0), [this] { session_.togglePlay(); });
    sc.add("pause", "Transport", "Pause", K(K::spaceKey, ctrl, 0), [this] { session_.engine.pause(); });
    sc.add("songmode", "Transport", "Pattern / song mode", K('L', none, 0), [this] { session_.setSongMode(!session_.project.songMode); });
    sc.add("record", "Transport", "Record on / off", K('R', none, 0), [this] { session_.recording = !session_.recording; session_.notify(); });
    sc.add("metronome", "Transport", "Metronome on / off", K('M', ctrl, 0), [this] { session_.setMetronome(!session_.engine.metronome()); });
    sc.add("typing", "Transport", "Typing keyboard plays notes", K('T', ctrl, 0), [this] {
        session_.typingKeyboard = !session_.typingKeyboard;
        if (!session_.typingKeyboard) pollTypingKeys();
        session_.notify();
    });
    sc.add("playlist", "Windows", "Playlist", K(K::F5Key, none, 0), [this] { window("playlist")->toggle(); });
    sc.add("rack", "Windows", "Channel rack", K(K::F6Key, none, 0), [this] { window("rack")->toggle(); });
    sc.add("piano", "Windows", "Piano roll", K(K::F7Key, none, 0), [this] { window("piano")->toggle(); });
    sc.add("picker", "Windows", "Plugin picker (add an instrument)", K(K::F8Key, none, 0), [this] { window("rack")->showAndRaise(); rack_->addChannelMenu(rack_); });
    sc.add("browser", "Windows", "Browser", K(K::F8Key, alt, 0), [this] { toggleBrowser(); });
    sc.add("mixer", "Windows", "Mixer", K(K::F9Key, none, 0), [this] { window("mixer")->toggle(); });
    sc.add("settings", "Windows", "Settings", K(K::F10Key, none, 0), [this] { window("settings")->toggle(); });
    sc.add("newpattern", "Patterns", "New pattern", K(K::F4Key, none, 0), [this] { session_.newPattern(); });
    sc.add("nextpattern", "Patterns", "Next pattern", K(K::numberPadAdd, none, 0), [this] { session_.selectPattern(session_.project.currentPattern + 1); });
    sc.add("prevpattern", "Patterns", "Previous pattern", K(K::numberPadSubtract, none, 0), [this] { session_.selectPattern(session_.project.currentPattern - 1); });
    // a little further into the manual's list
    sc.add("panic", "Transport", "Stop all sound", K('H', ctrl, 0), [this] { session_.engine.panic(); });
    sc.add("waitinput", "Transport", "Wait for input before recording, on / off", K('I', ctrl, 0), [this] { session_.recOpt.waitForInput = !session_.recOpt.waitForInput; session_.saveRecordOptions(); session_.notify(); });
    sc.add("precount", "Transport", "Count-in before recording, on / off", K('P', ctrl, 0), [this] { session_.countInBars = session_.countInBars > 0 ? 0 : 1; session_.notify(); });
    sc.add("blend", "Transport", "Recorded notes blend with / replace what is there", K('B', ctrl, 0), [this] { session_.recOpt.replaceNotes = !session_.recOpt.replaceNotes; session_.saveRecordOptions(); session_.notify(); });
    sc.add("stepedit", "Transport", "Step recording, on / off", K('E', ctrl, 0), [this] { session_.stepRecording = !session_.stepRecording; session_.notify(); });
    auto moveBars = [this](int bars) {
        const double bar = (double)session_.project.ticksPerBar();
        const double bars0 = session_.engine.positionTicks() / bar;
        // forward from the start of this bar; back from its start, or to its start when part-way through
        const double at = ((bars > 0 ? std::floor(bars0 + 1.0e-6) : std::ceil(bars0 - 1.0e-6)) + bars) * bar;
        session_.seek(juce::jmax(0.0, at));
    };
    sc.add("nextbar", "Transport", "Next bar", K(K::numberPadMultiply, none, 0), [moveBars] { moveBars(1); });
    sc.add("prevbar", "Transport", "Previous bar", K(K::numberPadDivide, none, 0), [moveBars] { moveBars(-1); });
    sc.add("forward", "Transport", "Fast forward (four bars)", K(K::numberPad0, none, 0), [moveBars] { moveBars(4); });
    sc.add("closeall", "Windows", "Close all windows", K(K::F12Key, none, 0), [this] { for (auto* w : windows_) w->setVisible(false); for (auto* w : effectWindows_) w->setVisible(false); });
    sc.add("cycle", "Windows", "Bring the next window to the front", K(K::tabKey, none, 0), [this] {
        // the visible windows in their fixed order, starting after the one that has the focus
        juce::Array<ToolWindow*> shown;
        for (auto* w : windows_) if (w->isVisible()) shown.add(w);
        if (shown.isEmpty()) return;
        int cur = -1;
        for (int i = 0; i < shown.size(); ++i) if (shown[i]->hasKeyboardFocus(true)) cur = i;
        shown[(cur + 1) % shown.size()]->showAndRaise();
    });
    sc.add("closewindow", "Windows", "Close the window in front", K(K::escapeKey, none, 0), [this] {
        for (auto* w : effectWindows_) if (w->isVisible() && w->hasKeyboardFocus(true)) { w->setVisible(false); return; }
        for (auto* w : windows_) if (w->isVisible() && w->hasKeyboardFocus(true)) { w->setVisible(false); return; }
    });
    sc.add("gotoplaylist", "Windows", "Go to the Playlist", K(K::returnKey, ctrl, 0), [this] { window("playlist")->showAndRaise(); });
    sc.add("arrange", "Windows", "Arrange the windows as they first were", K('H', ctrlShift, 0), [this] { arrangeWindows(); });
    sc.add("help", "Windows", "Help: take the tour", K(K::F1Key, none, 0), [this] { startTour(); });
    for (int i = 1; i <= 9; ++i)
        sc.add("pattern" + juce::String(i), "Patterns", "Select pattern " + juce::String(i), K(K::numberPad1 + (i - 1), none, 0), [this, i] { session_.selectPattern(i - 1); });
    sc.add("emptypattern", "Patterns", "Next empty pattern", K(K::F4Key, ctrl, 0), [this] { session_.newPattern(); });
    sc.add("renamepattern", "Patterns", "Rename the pattern", K(K::F2Key, none, 0), [this] {
        askForText("Rename pattern", "Pattern name", session_.project.pattern().name, this, [this](juce::String n) {
            if (n.trim().isEmpty()) return;
            session_.project.pattern().name = n.trim().toStdString();
            session_.commit("Rename pattern");
        });
    });
    sc.add("exportmidi", "File", "Export a MIDI file", K('M', ctrlShift, 0), [this] { fileExportMidi(); });
    sc.add("exportwindow", "File", "Export audio in another format (the export window)", K('R', ctrlShift, 0), [this] { fileExport(); });
    sc.add("save", "File", "Save", K('S', ctrl, 0), [this] { fileSave({}); });
    sc.add("saveas", "File", "Save as", K('S', ctrlShift, 0), [this] { fileSaveAs({}); });
    sc.add("savenew", "File", "Save new version", K('N', ctrl, 0), [this] { fileSaveNewVersion(); });
    sc.add("open", "File", "Open", K('O', ctrl, 0), [this] { fileOpen(); });
    sc.add("export", "File", "Export audio", K('R', ctrl, 0), [this] { fileExport(); });
    sc.add("undo", "Edit", "Undo / redo the last change", K('Z', ctrl, 0), [this] { session_.undoToggle(); });
    sc.add("undostep", "Edit", "Step back through the history", K('Z', ctrlAlt, 0), [this] { session_.undoStep(); });
    sc.add("redostep", "Edit", "Step forward through the history", K('Z', ctrlShift, 0), [this] { session_.redoStep(); });
    sc.load();
}

MainComponent::~MainComponent() {
    saveLayout();
    session_.onMessage = nullptr;
    session_.onAskStretch = nullptr;
    session_.onShowSettings = nullptr;
    session_.onUseEffect = nullptr;
    session_.onOpenSoundFont = nullptr;
    PluginHost::get().onKey = nullptr;
    PluginHost::get().onEdited = nullptr;
    PluginHost::get().onListChanged = nullptr;
    PluginHost::get().onShowParameters = nullptr;
    PluginHost::get().closeWindows();
    ParamUi::valid = nullptr;
    ParamUi::touched = nullptr;
    ParamUi::addMenuItems = nullptr;
    ParamUi::menuChosen = nullptr;
    session_.removeListener(this);
    Shortcuts::get().clear();
    effectWindows_.clear();
    windows_.clear();
}

void MainComponent::openEffect(std::shared_ptr<pulp::Plugin> plugin, const juce::String& title) {
    if (PluginHost::isHosted(plugin.get())) PluginHost::get().showEditor(std::move(plugin), title);
    else openEffectPanel(std::move(plugin), title);
}

// A channel's settings; a hosted instrument shows its own window (its PARAMETERS button leads to the settings).
void MainComponent::openChannel(int index) {
    auto* ch = session_.channel(index);
    if (ch && PluginHost::isHosted(ch->live->plugin.get()))
        PluginHost::get().showEditor(ch->live->plugin, juce::String(ch->name) + "  -  " + ch->live->plugin->displayName());
    else window("chan")->showAndRaise();
}

void MainComponent::openEffectPanel(std::shared_ptr<pulp::Plugin> plugin, const juce::String& title) {
    if (!plugin) return;
    for (auto* w : effectWindows_)
        if (static_cast<PluginEditor*>(w->content())->plugin() == plugin.get()) { w->setTitle(title); w->showAndRaise(); return; }
    auto* ed = new PluginEditor(session_, plugin);
    ed->typeId = plugin->typeId();
    for (int t = 0; t < (int)session_.project.mixer.size(); ++t)
        for (int sl = 0; sl < pulp::kNumSlots; ++sl)
            if (session_.project.mixer[(size_t)t].slots[sl] == plugin) { ed->track = t; ed->slot = sl; }
    auto* w = effectWindows_.add(new ToolWindow(title, ed, true));
    w->setBroughtToFrontOnMouseClick(true);
    w->onKey = [this](const juce::KeyPress& k) { return handleKey(k); };
    w->onKeyState = [this](bool) { return pollTypingKeys(); };
    w->setMinimumSize(300, 160);
    addChildComponent(w);
    const int width = std::string(plugin->typeId()) == "editor" ? 680 : 420;
    const int height = juce::jmin(getHeight() - Toolbar::kHeight - 30, ed->preferredHeight(width - 8) + ToolWindow::kTitle + 8);
    const int n = effectWindows_.size();
    w->setBounds(juce::jmax(0, getWidth() - width - 20 - (n % 6) * 24), Toolbar::kHeight + 20 + (n % 6) * 24, width, height);
    w->showAndRaise();
}

juce::Array<juce::Component*> MainComponent::customViews() const {
    juce::Array<juce::Component*> out;
    for (auto* w : effectWindows_) if (auto* v = static_cast<PluginEditor*>(w->content())->customView()) out.add(v);
    return out;
}

juce::Array<AnalyserView*> MainComponent::analysers() const {
    juce::Array<AnalyserView*> out;
    for (auto* w : effectWindows_) if (auto* v = static_cast<PluginEditor*>(w->content())->analyserView()) out.add(v);
    return out;
}

void MainComponent::showLink(const std::string& address) {
    link_->setTarget(address);
    window("link")->showAndRaise();
}

void MainComponent::showEvents(const std::string& address) {
    session_.eventTarget = address;
    session_.notify();
    window("events")->showAndRaise();
}

void MainComponent::showSettingsPage(int page) {
    settings_->showPage(page);
    window("settings")->showAndRaise();
}

bool MainComponent::spliceCommand(const juce::StringArray& args, const juce::File& baseDir, std::function<void(const juce::String&)> log) {
    SpliceView* v = nullptr;
    for (auto* w : effectWindows_) if (auto* sv = static_cast<PluginEditor*>(w->content())->spliceView()) v = sv;
    if (!v) return false;
    auto file = [&](const juce::String& p) { return juce::File::isAbsolutePath(p) ? juce::File(p) : baseDir.getChildFile(p); };
    const auto c = args[1];
    if (c == "sel" && args.size() >= 4) { v->setSelection(args[2].getDoubleValue(), args[3].getDoubleValue()); return true; }
    if (c == "op" && args.size() >= 3) return v->op(args[2]);
    if (c == "load" && args.size() >= 3) return v->loadFile(file(args[2]));
    if (c == "save" && args.size() >= 3) return v->saveAs(file(args[2]));
    if (c == "toplaylist") return v->toPlaylist() >= 0;
    if (c == "info") { log(v->describe()); return true; }
    return false;
}

// Puts every window back where a new installation has it.
void MainComponent::arrangeWindows() {
    for (int i = 0; i < windows_.size() && i < defaultBounds_.size(); ++i) {
        windows_[i]->setBounds(defaultBounds_[i]);
        windows_[i]->setVisible(defaultVisible_[i]);
    }
    if (!browserVisible_) toggleBrowser();
}

void MainComponent::fileExportMidi() {
    auto chooser = std::make_shared<juce::FileChooser>("Export a MIDI file", juce::File(), "*.mid;*.midi");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                         [this, chooser](const juce::FileChooser& fc) {
        if (fc.getResult() == juce::File()) return;
        if (session_.exportMidiSong(fc.getResult().withFileExtension("mid"))) showMessage("Wrote " + fc.getResult().withFileExtension("mid").getFileName());
    });
}

ToolWindow* MainComponent::addWindow(const juce::String& id, const juce::String& title, juce::Component* content, juce::Rectangle<int> bounds, bool visible) {
    auto* w = windows_.add(new ToolWindow(title, content, true));
    windowIds_.add(id);
    defaultBounds_.add(bounds);
    defaultVisible_.add(visible);
    w->setBroughtToFrontOnMouseClick(true);
    w->onKey = [this](const juce::KeyPress& k) { return handleKey(k); };
    w->onKeyState = [this](bool) { return pollTypingKeys(); };
    addChildComponent(w);
    w->setBounds(bounds);
    w->setVisible(visible);
    return w;
}

ToolWindow* MainComponent::window(const juce::String& name) {
    const int i = windowIds_.indexOf(name);
    return i >= 0 ? windows_[i] : nullptr;
}

void MainComponent::projectChanged() {
    // close editors whose effect is no longer in the mixer
    // Undo, redo and opening a backup rebuild the project, so the effect an editor showed is a new object:
    // the editor is opened again, in the same place, on whatever effect of the same kind now sits in that slot.
    struct Reopen { std::shared_ptr<pulp::Plugin> plugin; juce::String title; juce::Rectangle<int> bounds; };
    std::vector<Reopen> reopen;
    for (int i = effectWindows_.size(); --i >= 0;) {
        auto* ed = static_cast<PluginEditor*>(effectWindows_[i]->content());
        auto* plug = ed->plugin();
        bool found = false;
        for (int t = 0; t < (int)session_.project.mixer.size(); ++t)
            for (int sl = 0; sl < pulp::kNumSlots; ++sl)
                if (session_.project.mixer[(size_t)t].slots[sl].get() == plug) { found = true; ed->track = t; ed->slot = sl; }
        if (found) continue;
        if (ed->track >= 0 && ed->track < (int)session_.project.mixer.size()) {
            auto now = session_.project.mixer[(size_t)ed->track].slots[ed->slot];
            if (now && std::string(now->typeId()) == ed->typeId) reopen.push_back({now, effectWindows_[i]->title(), effectWindows_[i]->getBounds()});
        }
        effectWindows_.remove(i);
    }
    for (auto it = reopen.rbegin(); it != reopen.rend(); ++it) {
        openEffectPanel(it->plugin, it->title);
        if (auto* w = effectWindows_.getLast()) w->setBounds(it->bounds);
    }
    PluginHost::get().closeEditorsNotIn(session_.project);
    if (auto* ch = session_.current())
        window("piano")->setTitle("Piano roll  -  " + juce::String(ch->name) + "  -  " + juce::String(session_.project.pattern().name));
    if (onTitle) onTitle("PULP  -  " + session_.projectTitle());
    repaint();
}

void MainComponent::paint(juce::Graphics& g) { g.fillAll(theme::bg); }

void MainComponent::resized() {
    auto r = getLocalBounds();
    toolbar_.setBounds(r.removeFromTop(Toolbar::kHeight));
    browser_.setVisible(browserVisible_);
    if (browserVisible_) browser_.setBounds(r.removeFromLeft(kBrowserW));
    if (tour_) tour_->setBounds(getLocalBounds());
    getProperties().set("workLeft", browserVisible_ ? kBrowserW : 0);
}

void MainComponent::startTour() {
    auto win = [this](const char* name) { return [this, name] { return window(name)->getBounds(); }; };
    auto show = [this](const char* name) { return [this, name] { window(name)->showAndRaise(); }; };
    std::vector<TourOverlay::Step> steps;
    steps.push_back({"Welcome to PULP",
                     "PULP makes music out of patterns. A pattern is a short loop: a drum beat, a bass line, a tune. You make patterns, then lay them end to end to build a song.\n\n"
                     "This tour takes a minute and shows where everything is. Right arrow or NEXT goes on; Escape leaves.", {}, {}});
    steps.push_back({"The Channel Rack: your instruments",
                     "Each row is one instrument. Click the squares in a row to place hits, sixteen to a bar. Click a row's name to open its settings; "
                     "the + at the bottom adds an instrument (drum pads, synths, sampled pianos and strings, your own recordings).", win("rack"), show("rack")});
    steps.push_back({"Play, stop and tempo",
                     "Space plays and stops. PAT plays the pattern you are working on, round and round; SONG plays the whole arrangement. "
                     "The round button arms recording. Drag the tempo up or down to change the speed.", [this] { return toolbar_.getBounds(); }, {}});
    steps.push_back({"The Browser: sounds",
                     "Drum hits, loops and instruments live here. Click a sound to hear it; drag it onto the Channel Rack to use it. "
                     "\"My folders\" > \"Add a folder\" brings in your own samples.", [this] { return browser_.getBounds(); }, [this] { if (!browserVisible_) toggleBrowser(); }});
    steps.push_back({"The Piano Roll: melodies and chords",
                     "For anything with pitch. Click to draw a note, drag to move it, drag its right edge to make it longer, right-click to delete. "
                     "Up is higher, right is later. F7 opens it for the selected instrument.", win("piano"), show("piano")});
    steps.push_back({"The Playlist: the song",
                     "Paint patterns onto the tracks to arrange a song: the beat here, the bass coming in there. Recorded audio (your voice) lands here too. "
                     "Switch to SONG to hear it play from left to right. F5 opens it.", win("playlist"), [this] { window("piano")->setVisible(false); window("playlist")->showAndRaise(); }});
    steps.push_back({"The Mixer: levels and effects",
                     "Every sound passes through here. Faders set how loud each part is; the slots on the right hold effects (reverb, delay, compression, EQ). "
                     "To record your voice, pick the microphone in a track's IN box, arm recording and press play. F9 opens it.", win("mixer"), show("mixer")});
    steps.push_back({"Keeping your work",
                     "Ctrl+S saves. PULP also saves a backup every two minutes and offers it back if anything goes wrong. Ctrl+R exports the song as a WAV, FLAC or MP3 file to share.\n\n"
                     "Ctrl+Z undoes; the EDIT menu shows the whole history. The bar at the top says what the thing under the mouse does.", [this] { return toolbar_.getBounds(); }, [this] { window("mixer")->setVisible(false); }});
    steps.push_back({"Where to start",
                     "File > \"Open the demo song\" shows a finished song to take apart. File > \"New from template\" starts you with a drum kit, a bass and chords for a style.\n\n"
                     "Or just click some squares in the Channel Rack and press Space. File > \"Take the tour\" shows this again.", {}, {}});
    tour_ = std::make_unique<TourOverlay>(std::move(steps));
    tour_->onClose = [this](bool finished) { juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this), finished] { if (safe) safe->endTour(finished); }); };
    addAndMakeVisible(*tour_);
    tour_->setBounds(getLocalBounds());
    tour_->go(0);
    tour_->grabKeyboardFocus();
}

void MainComponent::endTour(bool) {
    tour_.reset();
    // seen once is enough, however it ended
    if (Session::persistSettings || Session::dataDirOverride != juce::File()) {
        Session::dataDir().createDirectory();
        Session::dataDir().getChildFile("tour.seen").replaceWithText("The first-run tour has been shown. Delete this file to see it again at the next start.\n");
    }
    grabKeyboardFocus();
}

void MainComponent::checkFirstRun() {
    if (Session::persistSettings && !Session::dataDir().getChildFile("tour.seen").existsAsFile()) startTour();
}

void MainComponent::toggleBrowser() {
    browserVisible_ = !browserVisible_;
    resized();
}

void MainComponent::showMessage(const juce::String& text) {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "PULP", text, "OK", this);
}

juce::String MainComponent::hintFor(juce::Component* under) {
    juce::String hint;
    for (auto* c = under; c != nullptr; c = c->getParentComponent()) {
        if (auto* t = dynamic_cast<juce::TooltipClient*>(c)) {
            hint = t->getTooltip();
            if (hint.isNotEmpty()) break;
        }
        if (auto* tv = dynamic_cast<juce::TreeView*>(c)) {
            const auto p = tv->getMouseXYRelative();
            if (auto* item = tv->getItemAt(p.y)) hint = item->getTooltip();
            if (hint.isNotEmpty()) break;
        }
    }
    return hint;
}

void MainComponent::uiTick() {
    // Hint bar: describe whatever the mouse is over.
    toolbar_.setHint(hintFor(juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse()));
    if (session_.typingKeyboard) pollTypingKeys();      // catches releases that arrive while another window has focus
}

// Typing keyboard: play the current channel from the computer keys while it is switched on.
bool MainComponent::pollTypingKeys() {
    const bool active = session_.typingKeyboard && dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) == nullptr;
    const auto mods = juce::ModifierKeys::getCurrentModifiers();
    bool used = false;
    for (int i = 0; i < kNumTyping; ++i) {
        const bool down = active && !mods.isCtrlDown() && !mods.isAltDown() && juce::KeyPress::isKeyCurrentlyDown(kTyping[i].code);
        if (down && typingDown_[i] == 0) {
            const int key = juce::jlimit(0, 127, session_.typingOctave * 12 + kTyping[i].semis);
            typingDown_[i] = key + 1;
            session_.liveNoteOn(key, pulp::kDefaultVelocity);
            used = true;
        } else if (!down && typingDown_[i] != 0) {
            session_.liveNoteOff(typingDown_[i] - 1);
            typingDown_[i] = 0;
            used = true;
        }
    }
    return used;
}

bool MainComponent::keyStateChanged(bool) { return pollTypingKeys(); }

bool MainComponent::keyPressed(const juce::KeyPress& k) { return handleKey(k); }

bool MainComponent::handleKey(const juce::KeyPress& k) {
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();
    const bool plain = !mods.isAnyModifierKeyDown();
    const bool ctrlOnly = mods.isCtrlDown() && !mods.isShiftDown() && !mods.isAltDown();

    // while the typing keyboard is on, its keys play notes and nothing else
    if (session_.typingKeyboard && plain && typingKeyOffset(code) >= 0) return true;

    // the program-wide commands, each on the key it has been given (Settings > SHORTCUTS)
    juce::ignoreUnused(ctrlOnly);
    if (Shortcuts::get().handle(k)) return true;
    // channel keys work when the rack or the bare workspace has the focus
    if (window("rack")->isVisible() && (window("rack")->hasKeyboardFocus(true) || hasKeyboardFocus(false)) && rack_->handleKey(k)) return true;
    return false;
}

juce::Component* MainComponent::findTarget(const juce::String& name) {
    if (name == "main") return this;
    if (name == "toolbar") return &toolbar_;
    if (name == "browser") return &browser_;
    if (name == "fx" && effectWindows_.size() > 0) return effectWindows_.getLast();
    if (name.endsWith("win")) return window(name.dropLastCharacters(3));
    if (auto* w = window(name)) return w->content();
    return nullptr;
}
