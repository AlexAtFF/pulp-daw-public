#pragma once
// The running program's state: the project, the engine, the audio device, and the
// edit / publish cycle that keeps the audio thread in step with the document.
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "pulp/Audio.h"
#include "pulp/Engine.h"
#include "pulp/Project.h"
#include <map>
#include <set>

class Session : private juce::AudioIODeviceCallback, private juce::Timer {
public:
    struct Listener {
        virtual ~Listener() = default;
        virtual void projectChanged() = 0;      // structure or selection changed: rebuild and repaint
    };

    Session();
    ~Session() override;

    // noAudio: run the engine from a silent background thread instead of a sound card
    // (used by automated interface tests so nothing is played).
    void start(bool noAudio);
    void shutdown();

    pulp::Project project;
    pulp::Engine engine;
    juce::AudioDeviceManager devices;

    // ---- files ----
    juce::File projectFile;                 // empty until the project has been saved or opened
    bool dirty = false;                     // changed since the last save
    juce::String projectTitle() const;
    void newProject();
    bool openProject(const juce::File& file);       // .pulp, or a packed .pulpz (unpacked beside it first)
    bool saveProject(const juce::File& file);
    bool saveNewVersion();                          // song.pulp -> song_2.pulp -> song_3.pulp
    bool packProject(const juce::File& zip);
    juce::StringArray recentFiles() const;
    void noteRecent(const juce::File& f);

    // ---- undo history ----
    struct HistoryEntry { juce::String name; std::string json; };
    std::vector<HistoryEntry> history;      // history[historyPos] is the state now
    int historyPos = -1;
    void undoToggle();                      // Ctrl+Z: undo the last step, or redo it if it was just undone
    void undoStep();                        // Ctrl+Alt+Z: one step further back
    void redoStep();
    void jumpToHistory(int index);
    void resetHistory(const juce::String& name);

    // ---- autosave and crash recovery ----
    static juce::File dataDir();            // settings, autosaves, crash log
    static juce::File dataDirOverride;      // set by --data-dir (tests)
    bool crashedLastTime = false;           // the previous run did not close cleanly
    juce::Array<juce::File> listBackups() const;    // newest first
    bool autosaveNow();
    bool restoreBackup(const juce::File& backup);
    void beginRunLock();
    void endRunLock();
    int autosaveSeconds = 120;

    // ---- export ----
    struct ExportOptions {
        bool song = true;
        double sampleRate = 48000.0;
        int bits = 24;                      // 16, 24, or 32 (float)
        int tail = 0;                       // pulp::RenderOptions::Tail
        bool dither = false;
        bool stems = false;                 // "split mixer tracks": one file for each mixer track that has channels or audio on it
        int mp3Kbps = 192;
        float oggQuality = 0.6f;
    };
    static juce::StringArray exportFormats();       // file endings this build can write: wav, flac, and mp3 / ogg when their codecs are in
    // Renders on a background thread. progress(0..1) and done(error or empty) arrive on the message thread.
    void exportWav(const juce::File& file, const ExportOptions& opt, std::function<void(double)> progress, std::function<void(juce::String)> done);
    void cancelExport();
    bool exporting() const { return exportThread_ != nullptr && exportThread_->isThreadRunning(); }

    // ---- recording audio ----
    int countInBars = 0;                    // 0, 1 or 2 bars of metronome before recording or playing starts
    int recordOffsetFrames = 0;             // extra latency correction on top of what the driver reports
    bool anyInputArmed() const;             // some mixer track has a hardware input selected
    bool audioRecording() const { return recordThread_ != nullptr; }
    juce::File recordingsDir() const;       // "recordings" beside the project file (or in the data folder until it is saved)
    int recordingLatencyFrames() const;     // engine input delay + driver input and output latency + the correction
    bool beginAudioRecording();
    void finishAudioRecording();
    bool testInput = false;                 // silent test mode: feed a test tone into input 1
    int testLoopFrames = 0;                 // silent test mode: feed the output back into input 1 this many frames late (a cable)
    // What the record button does beyond recording (the record button's right-click menu).
    struct RecordOptions {
        bool punch = false;                 // only record inside the playlist's loop region
        bool loopTakes = true;              // with a loop region: keep looping and stack every pass as a take
        bool waitForInput = false;          // start when the input gets loud or a note is played
        bool replaceNotes = false;          // note recording wipes the notes it passes over (off: overdub)
    } recOpt;
    void loadRecordOptions();
    void saveRecordOptions();
    // Latency calibration: plays three quiet noise bursts and listens for them on the inputs (about 1.2 s).
    // done(frames, quality): frames is the measured round trip, or -1 if the bursts did not come back.
    bool calibrate(std::function<void(int, float)> done);
    bool calibrating() const { return calibration_ != nullptr; }
    int calibratedLatency() const;          // measured round trip for the device as it is set up now, or -1
    void storeCalibration(int frames);      // -1 forgets it
    // Adds a file to the playlist as an audio clip; listens for its tempo and offers to fit it. Returns the clip index or -1.
    int addAudioFile(const juce::File& f, int track, pulp::int64 tick);
    std::function<void(const juce::String& name, double bpm, std::function<void(bool)> answer)> onAskStretch;
    void seek(double tick);                 // moves the song position (and gets streamed audio ready there)
    juce::File editsDir() const;            // where audio made in an editor is kept

    // ---- editing ----
    void commit(const juce::String& what);  // a finished edit: publish to the engine and tell the interface
    void publish();                         // push the document to the engine without ending the edit (during drags)
    void notify();                          // selection-only change
    void addListener(Listener* l) { listeners_.add(l); }
    void removeListener(Listener* l) { listeners_.remove(l); }

    // ---- channels ----
    int currentChannel = 0;                 // index of the channel the editors follow
    std::set<int> selectedChannels;         // channel ids
    pulp::Channel* channel(int index) { return index >= 0 && index < (int)project.channels.size() ? &project.channels[(size_t)index] : nullptr; }
    pulp::Channel* current() { return channel(currentChannel); }
    void selectChannel(int index, bool addToSelection = false);
    int addInstrumentChannel(const std::string& typeId, int preset = -1);
    // From the Browser: a preset goes onto the selected channel when that holds this instrument, otherwise
    // (and for the instrument itself) a new channel is made.
    int useInstrument(const std::string& typeId, int preset);
    std::function<void(const std::string& typeId, int preset)> onUseEffect;    // the same for an effect, on the selected mixer track
    // The pad drum sampler and the loop slicer (SessionBeats.cpp).
    bool setPadSample(int index, int pad, const juce::File& file, bool addLayer);
    int loadPadKit(int index, const juce::Array<juce::File>& files);      // returns how many pads were filled
    int loadPadKit(int index, const juce::File& folder);
    int addChopChannel(const juce::File& loop);
    int writeSliceNotes(int index);                                        // one note per slice into the current pattern
    // Whole MIDI files (SessionImport.cpp). Import adds a channel per track and one pattern holding all of it;
    // it returns a description of what was made, or nothing on failure.
    juce::String importMidiSong(const juce::File& file);
    void newFromTemplate(int index);                                       // pulp::projectTemplates(); -1 is the demo song
    bool exportMidiSong(const juce::File& file);                           // the song in song mode, else the current pattern
    juce::StringArray midiOutputNames() const;                            // the MIDI outputs of this PC, in the order MIDI Out channels number them
    juce::String midiOutLog() const;                                       // what MIDI Out channels have sent (for scripts)
    juce::StringArray soundFontPresets(const juce::File& sf2);             // "bank:program  name", sorted
    int addSoundFontChannel(const juce::File& sf2, int listIndex);         // a preset as a Timbre channel
    int addSampleChannel(const juce::File& file);                    // -1 if the file cannot be read
    bool setChannelSample(int index, const juce::File& file);
    void replaceChannelInstrument(int index, const std::string& typeId);
    void deleteChannel(int index);
    void cloneChannel(int index);
    void moveChannel(int index, int delta);
    static std::string projectPath(const juce::File& file);          // "factory:..." for factory files, else the full path
    juce::File resolvePath(const std::string& path) const;
    static bool isAudioFile(const juce::File& f);
    static juce::String audioFileEndings();          // "wav;flac;aif;aiff" and, when built in, ogg and mp3
    static juce::String audioFilePattern();          // the same as a file chooser wants it: "*.wav;*.flac;..."
    static bool isInstrumentFile(const juce::File& f);              // a .pulpinst multisample instrument
    int addInstrumentFileChannel(const juce::File& file);           // a Timbre channel playing that instrument; -1 if it cannot be read
    // Builds a multisample instrument from recordings of single notes (one file is enough) on a new Timbre
    // channel, or on channel `index` if that is a Timbre. Returns the channel index or -1.
    int buildInstrumentChannel(const juce::StringArray& audioFiles, int index = -1);
    juce::String lastBuildReport;                                    // what was heard in each file, from the last build

    // ---- patterns ----
    void selectPattern(int index);
    void newPattern();
    void setTimeSignature(int beats, int unit);     // beats in a bar, and the note that counts as a beat (4 or 8)
    void clonePattern();
    void deletePattern();

    // ---- transport ----
    void togglePlay();
    void stop();
    void setSongMode(bool song);
    void setTempo(double bpm);
    void tapTempo();

    // ---- preview and live notes ----
    bool previewFile(const juce::File& file);
    void stopPreview() { engine.preview.stop(); }
    // Notes from the typing keyboard, the on-screen keyboards or a MIDI keyboard: they play the
    // current channel and, while recording, are written into the current pattern.
    // sound: play it now (false when the caller already did). atTick: when it was played, if that is known.
    void liveNoteOn(int key, float velocity, bool sound = true, double atTick = -1.0);
    void liveNoteOff(int key, bool sound = true, double atTick = -1.0);

    // ---- automation and controllers ----
    int createAutomationClip(pulp::Param* p);           // a clip for this control on the first free playlist track; returns its index or -1
    void paramTouched(pulp::Param* p, int phase);       // every control reports: 0 grabbed, 1 moved, 2 let go
    struct ControllerSource { int track, slot, output; juce::String name; };
    std::vector<ControllerSource> controllerSources() const;   // every controller output in the mixer
    void setLink(const pulp::ParamLink& link);
    void removeLink(const std::string& address);
    void beginMidiLearn(pulp::Param* p);                // the next hardware control that moves is linked to it
    void cancelMidiLearn();
    bool midiLearning() const { return learnArmed_.load(); }
    const std::string& learnAddress() const { return learnAddress_; }
    std::string eventTarget;                            // the control shown in the event editor
    std::function<void()> onShowEvents;                 // open the event editor
    std::function<void(const std::string& address)> onShowLink;    // open the link window for a control
    void finishAutomationRecording();                   // turns recorded movements into events or a clip

    // ---- MIDI devices ----
    struct MidiDevice { juce::String name; bool notes, controllers, open; };
    std::vector<MidiDevice> midiDevices() const;
    void setMidiDeviceRole(int index, bool notes, bool controllers);
    void refreshMidiDevices();
    void handleMidi(const juce::MidiMessage& m, bool notes = true, bool controllers = true);     // any thread
    juce::String lastMidiText;                          // the last message received, for the settings page
    bool recording = false;                 // record armed
    bool stepRecording = false;             // step mode: each note lands at the step cursor, which then moves on
    int stepCursor = 0;                     // ticks
    bool typingKeyboard = false;
    int typingOctave = 5;
    int inputSnap = 0;                      // snap of the piano roll when it is open (0 = use the main snap)
    void setMetronome(bool on) { engine.setMetronome(on); notify(); }

    // ---- snap ----
    pulp::Param snap;                       // the main snap selector in the toolbar
    static const std::vector<std::pair<const char*, int>>& snapChoices();   // name, ticks (0 = line, 1 = none)
    int snapTicks(double pixelsPerTick = 0.1) const;    // resolves "line" from the zoom level
    static int lineSnap(double pixelsPerTick, int ticksPerBar);

    // ---- device ----
    juce::String deviceSummary() const;
    juce::String describeDevices();
    // Opens a device. Empty name picks the type's default; 0 keeps the device's rate / buffer size.
    juce::String openDevice(const juce::String& type, const juce::String& name, double sampleRate = 0, int bufferSize = 0);
    int xruns() const { return xruns_.load(); }
    bool audioRunning() const { return running_.load(); }
    bool usingNullDevice() const { return null_ != nullptr; }
    static juce::File settingsFile();
    void saveDeviceSettings();
    // Named sections of the settings file. Writes are skipped during scripted runs so tests
    // never change the user's own settings.
    static bool persistSettings;
    static std::unique_ptr<juce::XmlElement> loadSettings(const juce::String& tag);
    static void saveSettings(std::unique_ptr<juce::XmlElement> section);

    void newDefaultProject();
    static juce::File factoryDir();
    std::function<void(const juce::String&)> onMessage;     // user-facing errors
    std::function<void(int page)> onShowSettings;
    std::function<void(const juce::File&)> onOpenSoundFont;  // show a SoundFont's presets to choose from           // open the settings window at a page (2 = plugins)

private:
    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void timerCallback() override;
    void chooseFirstRunDevice();
    void message(const juce::String& m) { if (onMessage) onMessage(m); }

    class NullDevice;
    std::unique_ptr<NullDevice> null_;
    juce::ListenerList<Listener> listeners_;
    std::atomic<int> xruns_{0};
    std::atomic<bool> running_{false};
    double deviceRate_ = 48000.0;
    juce::Array<double> taps_;
    bool started_ = false;
    struct Held { int key; double startTick; float velocity; int channelId; };
    std::vector<Held> held_;
    int takePattern_ = 0;           // the pattern that notes recorded in song mode began in this pass; 0 between passes
    void restore(const std::string& json);
    void recordParamValue(pulp::Param* p);
    void startMidi(bool openDevices);
    void stopMidi();
    void loadMidiSettings();
    void saveMidiSettings();
    class MidiPort;
    class MidiOutPump;
    std::shared_ptr<MidiOutPump> midiOut_;
    void refreshMidiOuts();
    std::vector<std::shared_ptr<MidiPort>> midiPorts_;
    std::map<juce::String, std::pair<bool, bool>> midiRoles_;       // by device name: plays notes, acts as controller
    juce::MidiDeviceListConnection midiWatch_;
    bool midiDisabled_ = false;
    std::atomic<bool> learnArmed_{false};
    std::string learnAddress_;
    struct AutoRec { std::string address; std::vector<pulp::AutoEvent> events; };
    std::map<pulp::Param*, AutoRec> autoRec_;
    std::map<pulp::Param*, float> midiLinkSeen_;
    juce::String calibrationKey() const;
    void loadCalibrations();
    void pumpEditors();
    std::map<juce::String, int> calibrations_;
    class CalibrationPoll;
    std::unique_ptr<juce::Timer> calibration_;
    bool recPunch_ = false;
    pulp::int64 recPunchIn_ = 0, recPunchOut_ = 0;
    float compiledTempo_ = 0.0f, seenTempo_ = 0.0f;
    int tempoStable_ = 0;
    double replaceLast_ = -1.0;
    std::vector<std::pair<int, int>> replaceKept_;      // (tick, key) of notes recorded in this pass
    bool lastWasUndo_ = false;
    juce::uint32 lastAutosave_ = 0;
    bool dirtySinceAutosave_ = false;
    class ExportThread;
    class RecordThread;
    std::unique_ptr<juce::Thread> recordThread_;
    std::unique_ptr<juce::Thread> exportThread_;
    std::atomic<bool> exportCancel_{false};
};
