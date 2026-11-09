#pragma once
// The workspace: toolbar on top, the Browser docked on the left, floating tool windows
// over the rest, and the global shortcuts.
#include "AudioSettings.h"
#include "AutomationViews.h"
#include "Browser.h"
#include "ChannelEditor.h"
#include "ChannelRack.h"
#include "MixerView.h"
#include "PianoRoll.h"
#include "PluginEditor.h"
#include "Playlist.h"
#include "Session.h"
#include "ToolWindow.h"
#include "Toolbar.h"
#include "Tour.h"

class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      private Session::Listener,
                      private UiTicker::Client {
public:
    explicit MainComponent(Session& s);
    ~MainComponent() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& k) override;
    bool keyStateChanged(bool isDown) override;
    bool handleKey(const juce::KeyPress& k);      // global shortcuts; also used by scripts
    void registerShortcuts();
    void arrangeWindows();
    void fileExportMidi();
    static bool keepLayout;
    void saveLayout() const;        // where the windows are and which are showing, kept between runs
    void loadLayout();
    // The first-run tour (Tour.h). startTour shows it; it is shown by itself the first time the program runs.
    void startTour();
    void endTour(bool finished);
    void checkFirstRun();
    TourOverlay* tour() const { return tour_.get(); }

    // Components that scripts and screenshots can address by name.
    juce::Component* findTarget(const juce::String& name);
    ToolWindow* window(const juce::String& name);
    Session& session() { return session_; }
    ChannelRack& rack() { return *rack_; }
    Browser& browser() { return browser_; }
    PianoRoll& pianoRoll() { return *piano_; }
    Playlist& playlist() { return *playlist_; }
    MixerView& mixer() { return *mixer_; }
    EventEditor& eventEditor() { return *events_; }
    void showLink(const std::string& address);         // the "Link to controller" window for a control
    void showEvents(const std::string& address);       // the event editor for a control
    void showSettingsPage(int page);
    void openEffect(std::shared_ptr<pulp::Plugin> plugin, const juce::String& title);      // a hosted plugin opens its own window
    void openEffectPanel(std::shared_ptr<pulp::Plugin> plugin, const juce::String& title); // PULP's panel of knobs
    void openChannel(int index);
    SettingsPages& settings() { return *settings_; }
    juce::Array<juce::Component*> customViews() const;     // plugins' own control surfaces that are open
    juce::Array<AnalyserView*> analysers() const;       // the analyser displays that are open
    // Script access to the newest open audio editor: sel a b | op name | load file | save file | toplaylist | info
    bool spliceCommand(const juce::StringArray& args, const juce::File& baseDir, std::function<void(const juce::String&)> log);

    // File and Edit menus (FileActions.cpp)
    void fileMenu(juce::Component* target);
    void editMenu(juce::Component* target);
    void fileNew();
    void fileOpen();
    void fileSave(std::function<void()> then);
    void fileSaveAs(std::function<void()> then);
    void fileSaveNewVersion();
    void filePack();
    void fileExport();
    void exportTo(const juce::File& file, const Session::ExportOptions& opt, std::function<void(juce::String)> done);
    void ifSafeToDiscard(std::function<void()> then);
    void checkCrashRecovery();
    std::function<void(const juce::String&)> onTitle;      // window title changed
    static juce::String hintFor(juce::Component* under);    // the hint bar text for a component
    void showMessage(const juce::String& text);

private:
    void projectChanged() override;
    void uiTick() override;
    ToolWindow* addWindow(const juce::String& id, const juce::String& title, juce::Component* content, juce::Rectangle<int> bounds, bool visible);
    void toggleBrowser();
    juce::Component* makeExportPanel();
    bool pollTypingKeys();
    static int typingKeyOffset(int keyCode);    // semitones from C for a typing-keyboard key, or -1

    Session& session_;
    Toolbar toolbar_;
    Browser browser_;
    ChannelRack* rack_ = nullptr;
    ChannelEditor* chanEditor_ = nullptr;
    std::unique_ptr<TourOverlay> tour_;
    SettingsPages* settings_ = nullptr;
    EventEditor* events_ = nullptr;
    LinkEditor* link_ = nullptr;
    PianoRoll* piano_ = nullptr;
    Playlist* playlist_ = nullptr;
    MixerView* mixer_ = nullptr;
    juce::OwnedArray<ToolWindow> effectWindows_;    // one per open effect editor
    juce::OwnedArray<ToolWindow> windows_;
    juce::StringArray windowIds_;
    juce::Array<juce::Rectangle<int>> defaultBounds_;
    juce::Array<bool> defaultVisible_;
    bool browserVisible_ = true;
    int typingDown_[64] = {};       // sounding key for each typing-keyboard key, or 0
    static constexpr int kBrowserW = 250;
};
