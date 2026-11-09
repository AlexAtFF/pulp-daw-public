#pragma once
// Third-party plugins. A VST3 instrument or effect sits behind the same interface as a built-in
// one (pulp::Plugin), so the engine, the mixer, automation and project files treat it like any
// other. Scanning happens in a separate process: a plugin that crashes while being looked at
// takes that process down, not the program, and is put on a blacklist.
#include <juce_audio_processors/juce_audio_processors.h>
#include "Session.h"

// One hosted plugin instance.
class HostedPlugin : public pulp::Plugin, private juce::AudioProcessorParameter::Listener, private juce::AudioPlayHead {
public:
    HostedPlugin(std::unique_ptr<juce::AudioPluginInstance> instance, const juce::PluginDescription& description, std::string typeId);
    ~HostedPlugin() override;
    const char* typeId() const override { return typeId_.c_str(); }
    const char* displayName() const override { return name_.c_str(); }
    bool isInstrument() const override { return instrument_; }
    void setOffline(bool offline) override { instance_->setNonRealtime(offline); }     // exports tell the plugin they are not in real time
    bool wantsSidechain() const override { return sidechain_; }      // the plugin has a second input: the mixer's sidechain feeds it
    void prepare(double sampleRate) override;
    void process(pulp::ProcessContext& ctx) override;
    void reset() override;
    int latencySamples() const override { return latency_.load(std::memory_order_relaxed); }
    double tailSeconds() const override { return tail_; }
    pulp::Json saveExtra() const override;
    void loadExtra(const pulp::Json& j, const std::string& projectDir) override;
    void loadState(const pulp::Json& j, const std::string& projectDir = {}) override;
    std::vector<pulp::ParamGroup> paramGroups() const override;

    juce::AudioPluginInstance& instance() { return *instance_; }
    const juce::PluginDescription& description() const { return description_; }
    pulp::Param* lastTouched() const;               // the control most recently moved in the plugin's own window
    int totalParameters() const { return instance_->getParameters().size(); }
    juce::String valueText(int index) const;        // the plugin's own wording for a parameter's value
    void simulateTouch(int index, float value);     // what the plugin does when a control in its window is dragged (scripts)
    static constexpr int kMaxParams = 256;          // how many of the plugin's parameters are exposed to automation

private:
    void parameterValueChanged(int index, float value) override;
    void parameterGestureChanged(int index, bool starting) override;
    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override;

    std::unique_ptr<juce::AudioPluginInstance> instance_;
    juce::PluginDescription description_;
    std::string typeId_, name_;
    bool instrument_ = false, prepared_ = false, sidechain_ = false;
    bool loadedParams_ = false, monoIn_ = false;
    double sr_ = 0.0, tail_ = 0.0;
    std::atomic<int> latency_{0};
    juce::Array<juce::AudioProcessorParameter*> hosted_;
    std::vector<int> slotOf_;                           // plugin parameter index -> index in hosted_, or -1
    std::unique_ptr<std::atomic<float>[]> sent_;        // what each parameter was last told to be
    std::unique_ptr<std::atomic<bool>[]> held_;         // being dragged in the plugin's own window
    std::atomic<int> lastTouched_{-1};
    juce::AudioBuffer<float> work_;
    juce::MidiBuffer midi_;
    struct Sounding { pulp::uint32 id; int key, channel; };
    Sounding notes_[128];
    int numNotes_ = 0;
    float lastBend_ = 0.0f;
    // what the plugin is told about the song
    double posTempo_ = 120.0, posTick_ = 0.0;
    bool posPlaying_ = false;
    pulp::int64 posSamples_ = 0;
    std::shared_ptr<std::atomic<bool>> alive_;
};

class PluginHost {
public:
    static PluginHost& get();
    void start();                                   // loads the list of known plugins and tells the engine how to make them
    void shutdown();
    void closeWindows();

    struct Entry { std::string typeId; juce::String name, maker, file; bool instrument; };
    std::vector<Entry> plugins() const;             // every scanned plugin, by name
    std::string findByName(const juce::String& part) const;       // type id of the first plugin whose name contains this
    std::shared_ptr<pulp::Plugin> create(const std::string& typeId);
    static bool isHosted(const pulp::Plugin* p) { return dynamic_cast<const HostedPlugin*>(p) != nullptr; }

    // While a project is rebuilt from a snapshot (undo, redo), the instances it already holds are handed
    // back for the same plugin type and not made afresh: no reload, and their windows stay open.
    void beginReuse(const pulp::Project& project);
    void endReuse();

    // Scanning. Each file is examined by a child process (this program run with --scan-vst3).
    juce::StringArray defaultFiles() const;         // every .vst3 in the usual folders and the folders that were added
    void scan(const juce::StringArray& files, bool rescanKnown, std::function<void(const juce::String&)> progress,
              std::function<void(int found, int failed)> done);
    bool scanning() const { return scanThread_ != nullptr && scanThread_->isThreadRunning(); }
    bool everScanned() const { return scanned_; }
    juce::StringArray blacklist() const { return blacklist_; }
    void clearBlacklist() { blacklist_.clear(); save(); }
    void forget(const std::string& typeId);         // take one plugin off the list
    juce::StringArray folders;                      // extra folders to look in
    void save();
    static int runScanChild(const juce::String& file, const juce::File& out);      // what the child process does

    // The plugin's own window.
    void showEditor(std::shared_ptr<pulp::Plugin> plugin, const juce::String& title);
    void closeEditorsFor(pulp::Plugin* plugin);
    void closeEditorsNotIn(const pulp::Project& project);
    juce::String describeEditors() const;
    bool captureWindow(int index, const juce::File& png) const;      // a picture of a plugin window as Windows draws it
    std::function<void()> onListChanged;
    std::function<void(std::shared_ptr<pulp::Plugin>)> onShowParameters;      // the PARAMETERS button in a plugin window
    std::function<bool(const juce::KeyPress&)> onKey;                         // keys the plugin did not want
    std::function<void(const juce::String& what)> onEdited;                   // a control in a plugin's window was let go: an undo step

private:
    PluginHost() = default;
    void load();
    static juce::File listFile();
    struct Juce { juce::AudioPluginFormatManager formats; juce::KnownPluginList known; };
    std::unique_ptr<Juce> j_;
    juce::StringArray blacklist_;
    bool scanned_ = false;
    std::vector<std::shared_ptr<pulp::Plugin>> reuse_;
    class ScanThread;
    std::unique_ptr<juce::Thread> scanThread_;
    class Window;
    juce::OwnedArray<juce::DocumentWindow> windows_;
};
