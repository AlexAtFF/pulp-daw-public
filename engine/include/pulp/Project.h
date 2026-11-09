#pragma once
// The project document: what gets edited, saved and compiled into a RenderState.
// Only control threads touch it. Continuous values (knobs) live in shared Param objects
// so the audio thread reads them directly; structural data is recompiled and swapped in.
#include "pulp/Engine.h"
#include "pulp/Stream.h"
#include <functional>
#include <map>

namespace pulp {

constexpr int kProjectVersion = 2;      // see docs/PROJECT_FORMAT.md for what changed in each version

struct Note {
    int tick = 0;                 // start, in ticks from the pattern start
    int length = kTicksPerStep;
    int key = kRootKey;
    float velocity = kDefaultVelocity;
    float pan = 0.0f;             // -1..1
    float release = 0.5f;         // 0..1
    float modX = 0.5f;            // 0..1 (filter cutoff in native instruments)
    float modY = 0.5f;            // 0..1 (resonance)
    float fine = 0.0f;            // cents, -1200..1200
    int color = 0;                // 0..15, note colour group / MIDI channel
    bool slide = false, porta = false, muted = false;
    bool selected = false;        // interface state, not saved
    int end() const { return tick + length; }
};

struct Channel {
    int id = 0;
    std::string name;
    uint32 color = 0xff5f6b73;
    std::shared_ptr<ChannelLive> live;
    int mixerTrack = 0;           // 0 = master
    std::vector<int> layerChildren;   // channel ids that also play this channel's notes (layer channels)
    std::string group;                // channel group name ("" = ungrouped)
};

struct PatternMarker {
    int tick = 0;
    std::string name;
};

struct Pattern {
    int id = 0;
    std::string name;
    uint32 color = 0xff4f7ea8;
    int steps = 16;               // step-sequencer length
    float swing = 0.0f;           // 0..1
    std::vector<PatternMarker> markers;          // piano-roll time markers
    std::map<int, std::vector<Note>> notes;      // by channel id, kept sorted by tick
    std::map<std::string, std::vector<AutoEvent>> events;     // event automation, by parameter address, sorted by tick
    std::vector<Note>& notesFor(int channelId) { return notes[channelId]; }
    const std::vector<Note>* find(int channelId) const { auto it = notes.find(channelId); return it == notes.end() ? nullptr : &it->second; }
    bool empty() const { for (auto& kv : notes) if (!kv.second.empty()) return false; return true; }
};

struct Clip {
    enum Type { PatternClip = 0, AudioClip = 1, AutomationClip = 2 };
    int type = PatternClip;
    int track = 0;                // playlist track, 0-based
    int64 start = 0, length = 0;  // song ticks
    int64 offset = 0;             // start offset into the source
    int patternId = 0;
    bool muted = false;
    int group = 0;                // clips sharing a non-zero group move and select together
    // ---- audio clips only ----
    int audioId = 0;              // which entry of Project::audios
    int mixerTrack = 0;           // the mixer track it plays through
    float gain = 1.0f;
    int64 fadeIn = 0, fadeOut = 0;        // ticks
    bool reverse = false;
    int64 sourceOffsetFrames = 0; // frames of the file skipped before the clip's own offset (recording latency)
    int stretchMode = 0;          // stretch::Mode: 0 resample, 1 rhythmic, 2 tonal
    bool sync = false;            // follow the project tempo (needs the audio's own tempo)
    float timeMul = 1.0f;         // length multiplier when not synced (2 = twice as long)
    float pitchCents = 0.0f;      // pitch shift
    // ---- automation clips only ----
    int autoId = 0;               // which entry of Project::autos
    // ---- audio clips again ----
    int takeGroup = 0;            // takes recorded over the same stretch of song share a group
    int take = 0;                 // 1-based take number within the group
    bool selected = false;        // interface state, not saved
    int64 end() const { return start + length; }
};

struct PlaylistTrack {
    std::string name;
    uint32 color = 0xff3a4046;
    bool mute = false, solo = false, locked = false;
    float height = 1.0f;
    bool grouped = false;         // grouped with the track above: follows that track's mute and solo
    bool collapsed = false;       // on the top track of a group: the grouped tracks under it are not shown
};

// One audio file used by audio clips. Short files are held in memory (sample); long ones
// are played from disk (stream).
struct AudioItem {
    int id = 0;
    std::string name;
    std::string path;             // as loaded: factory:, relative to the project, or absolute
    SamplePtr sample;
    std::shared_ptr<AudioStream> stream;
    double bpm = 0.0;             // the audio's own tempo (detected or typed in); 0 = not known
    bool valid() const { return sample != nullptr || stream != nullptr; }
    int64 frames() const { return sample ? sample->frames() : (stream ? stream->frames : 0); }
    double rate() const { return sample ? sample->sampleRate : (stream ? stream->sampleRate : 48000.0); }
    int channels() const { return sample ? sample->channels : (stream ? stream->channels : 0); }
    float peak() const { return sample ? sample->peak() : (stream ? stream->peak : 0.0f); }
    double seconds() const { return (double)frames() / rate(); }
    SamplePtr inMemory() const { return sample ? sample : (stream ? SamplePtr(stream->loadAll()) : nullptr); }
    // Lowest and highest sample in file frames [from, to), for drawing.
    void minMax(int64 from, int64 to, float& mn, float& mx) const;
};

struct TimeMarker {
    int64 tick = 0;
    std::string name;
};

void sortNotes(std::vector<Note>& notes);

// A connection from one mixer track to another.
struct Route {
    int to = 0;
    std::shared_ptr<Param> level;     // send level, 0..1 (1 = unity)
    bool sidechain = false;           // true: only feeds the target's sidechain input
};

struct MixerTrack {
    std::string name;
    uint32 color = 0xff3f464d;
    int dock = 0;                     // -1 left dock, 0 middle, 1 right dock
    std::shared_ptr<TrackLive> live;
    std::shared_ptr<Plugin> slots[kNumSlots];
    std::vector<Route> routes;
    const Route* routeTo(int to) const { for (auto& r : routes) if (r.to == to) return &r; return nullptr; }
    // recording
    int input = 0;                    // 0 none, 1 = input 1 (mono), 2 = input 2 (mono), 3 = inputs 1+2 (stereo)
    bool monitor = false;             // hear the input through this track's effects
    int output = 0;                   // hardware output pair it also plays on: 0 none, 1 = outputs 1+2, 2 = outputs 3+4 ...
};

class Project {
public:
    Project();
    std::shared_ptr<GlobalLive> global;
    std::string name = "untitled";
    std::string projectDir;       // folder that relative sample paths resolve against
    int timeSigNum = 4, timeSigDen = 4;
    std::vector<Channel> channels;
    std::vector<Pattern> patterns;
    std::vector<Clip> clips;
    std::vector<AudioItem> audios;
    std::vector<AutoClip> autos;            // the curves behind automation clips
    std::vector<ParamLink> links;           // controllers linked to parameters, at most one per parameter
    std::vector<PlaylistTrack> tracks;      // always kNumPlaylistTracks long
    std::vector<MixerTrack> mixer;          // always kNumMixerTracks long: 0 master, 1..125 inserts, 126 current
    int selectedMixerTrack = 0;             // the "current" track listens to this one; interface state
    std::vector<TimeMarker> markers;
    int currentPattern = 0;       // index into patterns
    bool songMode = false;
    int64 loopStart = 0, loopEnd = 0;
    int patLoopStart = 0, patLoopEnd = 0;   // piano-roll loop selection; interface state, not saved
    double sampleRate = 48000.0;

    // ---- editing helpers ----
    Channel& addChannel(std::shared_ptr<Plugin> instrument, const std::string& name);
    Channel& addSamplerChannel(const std::string& samplePath, const std::string& name);
    void removeChannel(int index);
    Pattern& addPattern(const std::string& name = {});
    Pattern& pattern() { return patterns[(size_t)clampT(currentPattern, 0, (int)patterns.size() - 1)]; }
    int channelIndex(int id) const;
    int patternIndex(int id) const;
    Pattern* patternById(int id);
    const Pattern* patternById(int id) const;

    // Step-sequencer view of a pattern: a step is a note at the root key starting on that step.
    // A note belongs to the step its start falls in (the step graph can shift it within the step).
    static bool hasStep(const std::vector<Note>& notes, int step);
    static Note* stepNote(std::vector<Note>& notes, int step);
    static bool isStepSequence(const std::vector<Note>& notes);   // false when it needs the piano roll to show it
    static void setStep(std::vector<Note>& notes, int step, bool on);

    int stepsPerBar() const { return timeSigNum * 16 / std::max(1, timeSigDen); }
    int ticksPerBar() const { return stepsPerBar() * kTicksPerStep; }
    int patternLengthTicks(const Pattern& p) const;
    int64 songLengthTicks() const;
    bool trackAudible(int track) const;             // mute, solo and track groups taken together
    int trackGroupHead(int track) const;            // the top track of this track's group (itself if not grouped)
    bool trackIsGroupHead(int track) const;         // not grouped itself, and the track under it is
    bool trackHidden(int track) const;              // a grouped track whose group is collapsed (it still plays)
    Clip& addPatternClip(int patternIndex, int track, int64 startTick);
    // A note played at a song position (note.tick counts from the start of the song) goes into the pattern
    // that plays there: the current pattern, if one of its clips covers the position; otherwise the pattern
    // takePatternId (a take begun earlier in the same recording), whose clip is stretched to reach; otherwise
    // a new pattern in a new clip on a free track, which becomes the current pattern. A clip grows by whole
    // bars to hold the note while its track is free there. Returns the id of the pattern written to.
    int recordSongNote(int channelId, Note note, int takePatternId = 0);
    int addAudio(const std::string& path, std::string* error = nullptr);       // returns the audio id, or 0
    int addAudio(SamplePtr sample, const std::string& name);                   // for audio that is already in memory
    const AudioItem* audioById(int id) const;
    Clip& addAudioClip(int audioId, int track, int64 startTick, int mixerTrack = 0);
    // ---- automation ----
    // Finds a parameter by address. keep, when given, receives something that keeps the parameter alive.
    Param* findParam(const std::string& address, std::shared_ptr<void>* keep = nullptr) const;
    std::string addressOf(const Param* p) const;            // "" when the parameter is not part of the project
    std::string paramLabel(const std::string& address) const;      // for people: "Insert 3 - Squeeze - Threshold"
    AutoClip* autoById(int id);
    const AutoClip* autoById(int id) const;
    // Makes a curve for the parameter (two points at its present value) and puts a clip of it on the playlist.
    Clip& addAutomationClip(const std::string& target, int track, int64 startTick, int64 length);
    int makeAutoClipUnique(int clipIndex);                  // gives the clip its own copy of the curve; returns the new curve's id
    ParamLink* linkFor(const std::string& target);
    void setLink(const ParamLink& link);                    // replaces any link the parameter already has
    bool removeLink(const std::string& target);
    bool isAutomated(const std::string& address) const;     // by a clip, by events in any pattern, or by a link
    // ---- stretching ----
    double clipLengthRatio(const Clip& c) const;    // length of what the clip plays / length of its file
    bool clipNeedsProcessing(const Clip& c) const;
    int64 fullClipLength(const Clip& c) const;      // ticks the whole file takes with the clip's settings
    // Changes a clip's stretch settings; its length and offset are rescaled so it covers the same part of the file.
    void setClipStretch(int clipIndex, int mode, bool sync, float timeMul, float pitchCents);
    double detectAudioTempo(int audioId);           // analyses the file, stores the result in the item and returns it
    void prefetchAudio(double tick) const;          // have streamed clips ready to play from this position
    // ---- takes ----
    int splitClip(int clipIndex, int64 tick);       // cuts a clip in two; returns the index of the second part, or -1
    void useTake(int clipIndex);                    // this take plays over its stretch of song; the group's other takes are cut there and muted
    bool recordingLinear = false;                   // while a take is recorded without a loop, the song does not wrap at its end; not saved
    int64 secondsToTicks(double seconds) const { return (int64)std::llround(seconds * (double)global->tempo.get() * kPPQ / 60.0); }
    int makeClipUnique(int clipIndex);              // gives the clip its own copy of the pattern; returns the new pattern index

    // ---- mixer ----
    static std::string mixerTrackName(int index);
    bool canRoute(int from, int to) const;          // false for loops, the master as a source, the current track, itself
    bool setRoute(int from, int to, bool on, bool sidechain = false);
    bool setSlot(int track, int slot, std::shared_ptr<Plugin> plugin);     // null clears it
    int firstFreeMixerTrack() const;                // an insert no channel uses yet, or -1
    // Runs a change that moves parameters to other addresses (an effect to another slot, a mixer track to
    // another place), then points automation clips, events and links at wherever each parameter went.
    void keepingAddresses(const std::function<void()>& change);
    // Two insert tracks change places, with everything on them and everything that refers to them: channels,
    // audio clips, routes and sends, sidechains, automation and links. False for the master and the current track.
    bool swapMixerTracks(int a, int b);
    bool swapSlots(int track, int a, int b);        // two effect slots of one track change places, automation following

    // ---- runtime ----
    void prepare(double sr);                        // (re)prepare every plugin; not while attached to a running engine
    std::shared_ptr<RenderState> compile() const;

    // ---- persistence ----
    Json toJson() const;
    bool fromJson(const Json& j, std::string* error = nullptr);
    bool save(const std::string& path, std::string* error = nullptr);     // samples inside the project folder are stored by relative path
    bool load(const std::string& path, std::string* error = nullptr);
    std::vector<std::string> fileRefs() const;                              // every non-factory file the project uses
    int flushEditors(const std::string& dir);                               // writes audio that editors hold only in memory into dir/edits; returns how many files
    Json toPortableJson() const;                                            // like toJson, with every file reference made absolute (for backups)
    bool pack(const std::string& zipPath, std::string* error = nullptr) const;     // project plus its audio in one archive
    static bool unpack(const std::string& zipPath, const std::string& destDir, std::string& projectFile, std::string* error = nullptr);
    static bool migrate(Json& doc, std::string* error = nullptr);          // upgrades an older document in place

private:
    void initMixer();
    void compileMixer(RenderState& st, const std::vector<int>& extraActive) const;
    Json mixerToJson() const;
    void mixerFromJson(const Json& j);
    bool loadAudioItem(AudioItem& a, std::string* error) const;
    void compileAutomation(RenderState& st) const;
    int nextChannelId_ = 1, nextPatternId_ = 1, nextAudioId_ = 1, nextAutoId_ = 1;
    mutable std::map<std::string, std::shared_ptr<LinkStateRt>> linkStateCache_;
    // Delay lines and send ramps are kept between recompiles so editing does not click.
    mutable std::map<std::pair<int, int>, std::shared_ptr<EdgeRt>> edgeCache_;
    mutable std::map<int, std::shared_ptr<PdcDelay>> channelLineCache_;
    mutable std::map<std::pair<int, int>, std::shared_ptr<PdcDelay>> slotDryCache_;
};

} // namespace pulp
