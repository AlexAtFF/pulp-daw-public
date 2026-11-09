#pragma once
// The real-time engine: transport, sequencer, channels and the mix to the output.
// One Engine serves live playback (driven by the sound card callback) or an offline
// render (driven by a loop). Both run exactly the same code in the same 64-sample quanta,
// so an export is the same audio as playback.
#include "pulp/Audio.h"
#include "pulp/Automation.h"
#include "pulp/Mixer.h"
#include "pulp/Plugin.h"
#include "pulp/Stream.h"

namespace pulp {

// ---- compiled, immutable sequence data handed to the audio thread ----
struct NoteRt {
    int tick = 0, length = kTicksPerStep;
    short key = kRootKey;
    unsigned char flags = 0, color = 0;
    float velocity = kDefaultVelocity, pan = 0, modX = 0.5f, modY = 0.5f, fine = 0, release = 0.5f;
};
enum : unsigned char { kNoteSlide = 1, kNotePorta = 2 };

struct PatternRt {
    int lengthTicks = 16 * kTicksPerStep;
    std::vector<std::vector<NoteRt>> channelNotes;   // indexed like RenderState::channels, sorted by tick
};
struct ClipRt {
    int64 start = 0, length = 0, offset = 0;
    int pattern = 0;
};

// Project-wide live values.
struct GlobalLive {
    GlobalLive();
    Param tempo, masterVolume, masterPitch;
};

// The runtime side of one Channel Rack channel: its instrument, the channel settings
// (volume, pan, pitch, polyphony, cut groups, time, arpeggiator, echo, tracking) and the
// note scheduler. Shared between the project (control thread) and the engine.
class ChannelLive {
public:
    explicit ChannelLive(std::shared_ptr<Plugin> instrument);
    const std::shared_ptr<Plugin> plugin;
    Param volume, pan, mute, pitch, pitchRange;
    Param poly, mono, porta, portaTime, cut, cutBy;
    Param gate, shift, swingMix;
    Param arpDir, arpRange, arpTime, arpGate, arpChord, arpRepeat;
    Param echoCount, echoTime, echoFeed, echoPan, echoPitch, echoModX, echoModY;
    Param velTrack, keyPan, keyModX;
    std::vector<Param*> stripParams();
    Param* stripParam(const std::string& id);
    static float volumeToGain(float v);

    // ---- audio thread only ----
    struct Sched { int64 clock; NoteEvent ev; };
    static constexpr int kMaxSched = 2048;
    void reset();           // drop every scheduled note and ramp; not while audio runs
    void rtBegin(double samplesPerTick, double sampleRate) { spt_ = samplesPerTick; sr_ = sampleRate; }
    void rtSchedule(int64 clock, const NoteEvent& ev);
    void rtNote(int64 onClock, int64 offClock, const NoteRt& n);     // from the sequencer
    void rtLiveOn(int64 clock, int key, float velocity);             // from a keyboard
    void rtLiveOff(int64 clock, int key);
    void rtReleaseAll(int64 clock, bool dropPending);
    void rtRescale(int64 now, double factor);
    void rtPrepare(int64 qStart);                                    // run arpeggiator, echo and voice rules for this quantum
    int rtOnsets(int64 qStart, int64* clocks, int max) const;        // note-ons due this quantum (for cut groups)
    void rtProcess(int64 qStart, double tempo, double tick, bool playing, float masterPitchSemis);
    float buf[2][kQuantum];
    std::atomic<float> peak{0.0f};
    std::atomic<int> activity{0};        // counts note-ons, for the channel activity light
private:
    static constexpr int64 kOpen = INT64_MAX;
    struct In { int64 on, off; NoteRt n; int liveKey; };
    struct Held { NoteRt n; int64 on, off; int liveKey; };
    uint32 emit(int64 on, int64 off, const NoteRt& n);
    void scheduleOff(uint32 id, int64 clock, int echoes);
    void runArp(int64 qEnd);

    Sched sched_[kMaxSched];
    int numSched_ = 0;
    In in_[256];
    int numIn_ = 0;
    Held held_[32];
    int numHeld_ = 0;
    int64 arpNext_ = -1;
    int arpStep_ = 0;
    uint32 arpRand_ = 0x2545F491u;
    NoteEvent events_[kMaxEventsPerQuantum];
    uint32 nextId_ = 1;
    uint32 recent_[64] = {};
    int recentW_ = 0;
    float lastKey_ = -1.0f;
    uint32 liveOut_[128] = {};
    unsigned char liveEchoes_[128] = {};
    Ramp gainL_, gainR_;
    bool primed_ = false;
    double spt_ = 1.0, sr_ = 48000.0;
};

// Plays one audio file through the output for browser previews.
class PreviewPlayer {
public:
    void play(SamplePtr s, float gain);     // control thread
    void stop();
    bool isPlaying() const { return playing_.load(std::memory_order_relaxed); }
    void rtRender(float* l, float* r, int n, double sampleRate);
private:
    RtShared<SampleData> sample_;
    std::atomic<int> trigger_{0};
    std::atomic<float> gain_{0.8f};
    std::atomic<bool> playing_{false};
    int seen_ = 0;
    const SampleData* cur_ = nullptr;
    double pos_ = 0.0;
    float fade_ = 1.0f;
    bool stopping_ = false;
};

// One connection into a mixer track: its send gain (ramped) and, when needed, a delay that
// lines it up with the track's slowest input.
struct EdgeRt {
    Ramp gain;
    bool primed = false;
    std::unique_ptr<PdcDelay> delay;
};
struct TrackInRt {
    int from = 0;                       // position in RenderState::tracks
    std::shared_ptr<Param> level;       // null = unity
    int delay = 0;
    bool sidechain = false;             // feeds the track's sidechain input instead of its audio
    std::shared_ptr<EdgeRt> rt;
};
struct TrackRt {
    int index = 0;                      // mixer track number
    std::shared_ptr<TrackLive> live;
    std::shared_ptr<Plugin> slots[kNumSlots];
    std::shared_ptr<PdcDelay> slotDry[kNumSlots];
    int slotLatency[kNumSlots] = {};
    std::vector<TrackInRt> inputs;
    std::vector<int> channels;          // channel indices routed here
    int channelDelay = 0;
    std::shared_ptr<PdcDelay> channelLine;
    bool usesPre = false;               // audio clips or a live input feed this track
    int input = 0;                      // hardware input: 0 none, 1 in 1 mono, 2 in 2 mono, 3 stereo
    int hwOut = 0;                      // hardware output pair this track also plays on: 0 none, 1 = outputs 1+2, 2 = outputs 3+4 ...
    bool monitor = false;
};

// An audio clip on the playlist, ready for the audio thread.
struct AudioClipRt {
    int64 start = 0, length = 0, offset = 0;    // ticks
    int64 offsetFrames = 0;                     // file frames skipped at the head
    SamplePtr sample;                           // audio held in memory, or
    std::shared_ptr<AudioStream> stream;        // audio played from disk
    int64 frames = 0;                           // length of the source
    double rate = 48000.0;                      // its sample rate
    float gain = 1.0f;
    int64 fadeIn = 0, fadeOut = 0;              // ticks
    bool reverse = false;
    int trackPos = -1;                          // position in RenderState::tracks
};

// A run of recorded input: where it starts in the take and where in the song it belongs.
struct RecSegment {
    int64 frame = 0;
    double tick = 0.0;
};

// Where one take goes in the playlist: a clip that starts on a whole tick and skips the part
// of the file that is only latency, so the take plays back where it was performed.
struct TakePiece {
    int64 startTick = 0, lengthTicks = 0;
    int64 sourceOffsetFrames = 0;
};
// Turns the engine's record segments into clips. totalFrames is the length of the recorded file,
// latencyFrames the whole round trip (Engine::kInputDelay + driver or calibrated latency).
// With punchOut > punchIn each piece is cut to the punch range.
std::vector<TakePiece> planTakes(const std::vector<RecSegment>& segments, int64 totalFrames, int latencyFrames,
                                 double samplesPerTick, double punchIn = 0.0, double punchOut = 0.0);

struct RenderState {
    std::shared_ptr<GlobalLive> global;
    std::vector<TrackRt> tracks;        // active mixer tracks in processing order
    std::vector<int> levelStart;        // tracks[levelStart[k] .. levelStart[k+1]) do not depend on each other
    int masterPos = -1;                 // position of the master track in tracks
    int latency = 0;                    // samples the whole mix is late (plugin delay compensation)
    std::vector<std::shared_ptr<ChannelLive>> channels;
    std::vector<std::vector<int>> layerTargets;      // per channel: channels that also receive its notes
    std::vector<std::shared_ptr<const PatternRt>> patterns;
    std::vector<ClipRt> clips;          // song mode, sorted by start
    std::vector<AudioClipRt> audioClips;
    int currentPattern = 0;
    bool songMode = false;
    int64 songLengthTicks = 16 * kTicksPerStep * 4;
    int64 loopStart = 0, loopEnd = 0;   // song-mode loop region; loopEnd <= loopStart means the whole song
    int patLoopStart = 0, patLoopEnd = 0;   // pattern-mode loop region (a selected time range in the piano roll)
    int beatTicks = kPPQ, beatsPerBar = 4;  // for the metronome
    std::vector<AutoTargetRt> autoTargets;  // automation clips and pattern events, per parameter
    std::vector<LinkRt> links;              // controllers linked to parameters
};

class Engine {
public:
    Engine();
    ~Engine();
    void prepare(double sampleRate);
    double sampleRate() const { return sr_; }

    // ---- control threads ----
    void setState(std::shared_ptr<const RenderState> s);
    void collectGarbage() { state_.collect(); }
    // With a count-in, the metronome plays that long before the position moves off its start.
    // With waitForInput, playback starts when the hardware input gets loud or a note is played.
    void play(double countInTicks = 0.0, bool waitForInput = false);
    bool waitingForInput() const { return waitingPub_.load(std::memory_order_relaxed); }
    void setInputThreshold(float gain) { inputThreshold_.store(gain); }
    void stop();                        // a second stop while stopped rewinds and cuts all sound
    void pause();
    void seek(double tick);
    void setLooping(bool on) { looping_.store(on); }
    void setMetronome(bool on) { metronome_.store(on); }    // live only; never part of a project or an export
    enum { ClickBeep, ClickTick, ClickBlock, ClickBell, NumClickSounds };
    void setMetronomeSound(int sound) { clickSound_.store(clampT(sound, 0, (int)NumClickSounds - 1)); }
    int metronomeSound() const { return clickSound_.load(); }
    // An offline render reads streamed audio from disk on the spot; live playback never waits for the disk.
    void setOffline(bool on) { offline_.store(on); }
    int64 streamMisses() const { return streamMisses_.load(std::memory_order_relaxed); }
    // ---- recording ----
    // The engine keeps the last few seconds of the hardware input in a ring; a recorder thread drains it.
    static constexpr int kInputDelay = kQuantum;       // samples the input is held before the engine sees it
    void startRecording();
    // Punch in / out: only input between these song positions is recorded. out <= in switches it off.
    void setPunch(double inTick, double outTick) { punchIn_.store(inTick); punchOut_.store(outTick); }
    // Each unbroken run of recorded input, in order: a new one starts at every loop pass and punch-in.
    bool recordSegment(RecSegment& s) { return recSegs_.pop(s); }
    // Latency calibration: plays three quiet noise bursts and captures the input meanwhile (about 1.2 s).
    // Read the capture with recordRead and hand it to measureRoundTrip.
    void startCalibration();
    bool calibrating() const { return calibratingPub_.load(std::memory_order_relaxed); }
    static std::vector<float> calibrationSignal(double sampleRate);
    // Samples between a burst leaving the engine and coming back into a recording (the whole offset a take
    // has to be shifted by). Returns -1 when the bursts were not found. quality is 0..1.
    static int measureRoundTrip(const float* recordedMono, int n, double sampleRate, float* quality = nullptr);
    void stopRecording() { recording_.store(false); }
    bool isRecording() const { return recording_.load(std::memory_order_relaxed); }
    int recordRead(float* interleavedStereo, int maxFrames);    // control thread
    double recordStartTick() const { return recStartTick_.load(); }   // song position of the first recorded frame
    bool recordStarted() const { return recStartTick_.load() > -1.0e17; }
    int64 recordDropped() const { return recDropped_.load(); }
    float inputPeak(int ch) const { return inputPeak_[ch].load(std::memory_order_relaxed); }
    void resetInputPeak() { inputPeak_[0].store(0); inputPeak_[1].store(0); }

    // Worker threads for the mixer graph. Call while no audio is running. 1 = everything on the audio thread.
    void setThreads(int n);
    int threads() const { return pool_ ? pool_->threads() : 1; }
    int latencySamples() const { return latencyPub_.load(std::memory_order_relaxed); }
    bool metronome() const { return metronome_.load(); }
    // ---- automation and controllers ----
    // A hardware controller moved: value 0..1. channel 0..15. Any thread.
    void setCc(int channel, int cc, float value);
    float ccValue(int channel, int cc) const;           // -1 until the control has been moved; channel -1 = any
    // While a control is being dragged, automation and links leave it alone.
    void setTouched(Param* p) { touched_.store(p); }
    // In an offline render: the sample at which the transport reached the end of the song, or -1.
    int64 endedAtClock() const { return endedAt_.load(std::memory_order_relaxed); }
    void noteOn(int channel, int key, float velocity);
    void noteOff(int channel, int key);
    void panic();
    PreviewPlayer preview;

    bool isPlaying() const { return playingPub_.load(std::memory_order_relaxed); }
    double positionTicks() const { return posPub_.load(std::memory_order_relaxed); }
    int64 clock() const { return clockPub_.load(std::memory_order_relaxed); }
    float masterPeak(int ch) const { return masterPeak_[ch].load(std::memory_order_relaxed); }
    void resetMasterPeak() { masterPeak_[0].store(0); masterPeak_[1].store(0); }
    int scopeRead(float* dst, int n) const;     // most recent n mono samples of the output
    int activeVoices() const { return voicesPub_.load(std::memory_order_relaxed); }
    float cpuLoad() const { return cpuPub_.load(std::memory_order_relaxed); }
    void reportCallbackLoad(float fractionOfBudget);

    // ---- audio thread ----
    // out has numOut channels of n frames. in may be null.
    void process(const float* const* in, int numIn, float* const* out, int numOut, int n);

private:
    struct Command {
        enum Type { Play, Stop, Pause, Seek, NoteOn, NoteOff, Panic } type = Play;
        int a = 0, b = 0;
        float f = 0;
        double d = 0;
    };
    void post(const Command& c);
    void renderQuantum();
    void handleCommands(const RenderState* st);
    void sequence(const RenderState& st, int64 qStart);
    void emitNotes(const RenderState& st, int64 segStart, int64 segEnd, double loopEnd);
    void loopRange(const RenderState& st, double& ls, double& le) const;
    int64 clockOfTick(double tick) const { return anchorClock_ + (int64)std::llround((tick - anchorTick_) * spt_); }
    double tickAtClock(int64 c) const { return anchorTick_ + (double)(c - anchorClock_) / spt_; }
    void releaseAll(const RenderState* st, bool dropPending);
    void killAll(const RenderState* st);

    double sr_ = 48000.0;
    RtShared<RenderState> state_;
    std::mutex cmdMutex_;
    SpscQueue<Command, 1024> cmds_;
    std::atomic<bool> looping_{true};
    std::atomic<bool> metronome_{false};

    // audio thread
    struct Click { int offset; bool accent; };
    Click clicks_[8];
    int numClicks_ = 0;
    double clickPhase_ = 0.0, clickHz_ = 1000.0;
    float clickEnv_ = 0.0f;
    void renderClicks();
    void mixTrack(const RenderState& st, int pos, double tempo, double tick);
    void renderAudioClips(const RenderState& st, int64 segStart, int64 segEnd, int64 qStart);
    // hardware input, delayed by exactly kInputDelay samples
    static constexpr int kInRing = 16384;
    float inRing_[2][kInRing];
    int inW_ = 0, inR_ = 0;
    float inQ_[2][kQuantum];
    std::atomic<float> inputPeak_[2];
    // recording
    static constexpr int kRecRing = 1 << 19;        // stereo frames (about 5 s at 96 kHz)
    std::unique_ptr<float[]> recRing_;
    std::atomic<int64> recW_{0}, recR_{0};
    std::atomic<bool> recording_{false};
    std::atomic<double> recStartTick_{-1.0e18};
    std::atomic<int64> recDropped_{0};
    std::atomic<int64> recStartW_{0};               // ring position where the current take began
    SpscQueue<RecSegment, 256> recSegs_;
    std::atomic<double> punchIn_{0.0}, punchOut_{0.0};
    int64 recBase_ = -1;                            // ring position of this quantum's first recorded frame, or -1
    bool recActive_ = false;
    bool waiting_ = false;                          // armed, waiting for the input to get loud
    std::atomic<bool> waitingPub_{false};
    std::atomic<float> inputThreshold_{0.02f};
    void startPlaying(double countInTicks);
    void markLoopPass(int64 clockAtWrap, int64 qStart, double tick);
    // calibration
    std::vector<float> calibSig_;
    std::atomic<bool> calibReq_{false}, calibratingPub_{false};
    int calibPos_ = 0, calibLeft_ = 0;
    void applyAutomation(const RenderState& st, double tick);
    void applyLinks(const RenderState& st);
    void syncTempo(const RenderState* st);
    std::atomic<float> cc_[17][128];                // last value per channel and controller; row 16 is "any channel"
    std::atomic<Param*> touched_{nullptr};
    std::atomic<int64> endedAt_{-1};
    bool chase_ = false;                            // the transport jumped: set every automated control for the new position
    std::atomic<bool> offline_{false};
    std::atomic<int64> streamMisses_{0};
    std::atomic<int> clickSound_{0};
    int clickKind_ = 0;
    int64 clickAge_ = 0;
    uint32 clickNoise_ = 0x1234567u;
    double countInUntil_ = -1.0e18;                 // metronome is forced on before this tick
    std::unique_ptr<WorkerPool> pool_;
    std::atomic<int> latencyPub_{0};
    int64 clock_ = 0;
    bool playing_ = false;
    int64 anchorClock_ = 0;
    double anchorTick_ = 0.0;
    double spt_ = 1.0;                  // samples per tick
    double tempo_ = 0.0;
    bool lastSongMode_ = false, modeKnown_ = false;
    float qOut_[2][kQuantum];
    static constexpr int kMaxOutPairs = 4;          // outputs 1 to 8
    float qOutX_[kMaxOutPairs - 1][2][kQuantum];    // output pairs after the first, fed by tracks with a hardware output chosen
    bool anyHwOut_ = false;
    int outAvail_ = 0, outRead_ = 0;
    Ramp masterGain_;
    bool masterPrimed_ = false;

    std::atomic<bool> playingPub_{false};
    std::atomic<double> posPub_{0.0};
    std::atomic<int64> clockPub_{0};
    std::atomic<float> masterPeak_[2];
    std::atomic<int> voicesPub_{0};
    std::atomic<float> cpuPub_{0.0f};
    static constexpr int kScope = 4096;
    float scope_[kScope] = {};
    std::atomic<int> scopeW_{0};
};

} // namespace pulp
