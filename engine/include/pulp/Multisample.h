#pragma once
// The multisample engine: turns recordings of single notes into a playable instrument.
// Given one or more notes it finds each one's pitch, trims the silence around it, decides
// whether the sound holds (strings, organ) or dies away (piano, plucks), finds a seamless
// sustain loop for sounds that hold, lays the notes out across the keyboard, and matches their
// levels. Timbre is the instrument that plays the result.
#include "pulp/Audio.h"
#include "pulp/Dsp.h"
#include "pulp/Json.h"
#include "pulp/Plugin.h"
#include "pulp/Trio.h"

namespace pulp {

std::shared_ptr<SampleData> readAiffMemory(const void* data, size_t size, std::string* error = nullptr);

namespace ms {

// ---- listening to one note ----
struct Pitch {
    double hz = 0.0;
    double midi = 0.0;          // fractional key number (69 = 440 Hz)
    double clarity = 0.0;       // 0..1: how periodic the note is. Below about 0.5 the pitch is a guess.
    int key() const { return (int)std::lround(midi); }
    double cents() const { return (midi - std::lround(midi)) * 100.0; }
};
Pitch detectPitch(const SampleData& s);

struct Range { int64 start = 0, end = 0; };
// Where the sound is: from just before it begins to where it has died away.
Range findSound(const SampleData& s, float belowPeakDb = 54.0f);
// True when the level holds for as long as the note lasts (bowed, blown, sung); false when it decays.
bool holds(const SampleData& s, Range sound);

struct Loop {
    bool found = false;
    int64 start = 0, end = 0;   // frames; playback jumps from end back to start
    int64 crossfade = 0;        // frames blended before the loop end so the jump cannot be heard
    double match = 0.0;         // how alike the two ends are, 0..1
};
// A stretch in the steady part of a held note that can repeat for ever.
Loop findLoop(const SampleData& s, Range sound, double hz);
// Returns a copy of the audio with the crossfade written in, ready to loop from loop.end to loop.start.
std::shared_ptr<SampleData> bakeLoop(const SampleData& s, const Loop& loop);

// A note name in a file name: "Violin_A#3.wav", "piano-Gb2", "C4", or a bare MIDI number such as "_060_".
// middleC is the octave number the source uses for MIDI 60 (4 in most libraries). Returns -1 when there is none.
int noteFromName(const std::string& fileName, int middleC = 4);
std::string noteName(int key);      // PULP's own naming, where key 60 is C5 (as in the reference program)

// ---- the instrument ----
struct Zone {
    SamplePtr sample;           // trimmed, level left as recorded, loop crossfade written in
    std::string file;           // where it is stored (project-style path), "" while it only exists in memory
    int root = 60;              // the key it was played on
    float cents = 0.0f;         // how far the recording is from that key's true pitch
    int loKey = 0, hiKey = 127;
    int loVel = 0, hiVel = 127;
    int group = 0, groupSize = 1, turn = 0;     // round robin: zones of one group take turns
    bool loop = false;
    int64 loopStart = 0, loopEnd = 0;
    float gain = 1.0f;          // evens out the level between zones
};

struct Instrument {
    std::string name;
    std::string category;
    bool sustain = false;       // a sound that holds: its notes loop until released
    std::vector<Zone> zones;
    std::string credit;         // who recorded it and under which licence (shown in the Browser)
    Json params;                // Timbre settings that suit it (envelope, filter ...), by parameter id
    const Zone* pick(int key, int velocity, uint32 turn) const;     // audio thread: the zone for a note
};
using InstrumentPtr = std::shared_ptr<const Instrument>;

// One recording handed to the builder.
struct Note {
    SamplePtr sample;
    std::string file;           // its name, for reading a note out of and for the report
    int key = -1;               // what the source says it is (-1: work it out by listening)
    int layer = 0;              // velocity layer: 0 is the softest
    int take = 0;               // round-robin take
};
struct BuildOptions {
    int sustain = -1;           // -1 decide by listening, 0 never loop, 1 loop when a loop can be found
    bool trim = true;
    bool matchLevels = true;
    int middleC = 4;            // for note names in file names
};
struct BuildReport {
    struct Line { std::string file; int key = 0; double cents = 0, clarity = 0; bool looped = false; double loopMatch = 0; std::string note; };
    std::vector<Line> lines;
    std::string text() const;
};
// Builds an instrument from recordings of single notes. One note is enough.
std::shared_ptr<Instrument> build(const std::vector<Note>& notes, const std::string& name, const BuildOptions& opt = {}, BuildReport* report = nullptr);

// Instrument files (.pulpinst): a JSON description beside its samples.
// save writes every zone's audio as FLAC (or WAV if asked) into the folder of `path` and the description to `path`.
bool saveInstrument(const Instrument& inst, const std::string& path, bool flac = true, std::string* error = nullptr);
std::shared_ptr<Instrument> loadInstrument(const std::string& path, std::string* error = nullptr);
// An SFZ instrument (a text file that maps samples to keys). loadInstrument reads these too, by the .sfz ending.
std::shared_ptr<Instrument> loadSfz(const std::string& path, std::string* error = nullptr);
Json zonesToJson(const Instrument& inst);                                   // zones by reference to their files
std::shared_ptr<Instrument> instrumentFromJson(const Json& j, const std::string& baseDir, std::string* error = nullptr);

} // namespace ms

// Timbre: the multisample instrument. Plays an ms::Instrument with an amplitude envelope, a filter
// with its own envelope, an LFO and a pitch envelope.
class Timbre : public pulp::Instrument {
public:
    Timbre();
    const char* typeId() const override { return "multi"; }
    const char* displayName() const override { return "Timbre"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    Json saveExtra() const override;
    void loadExtra(const Json& j, const std::string& projectDir) override;
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;

    // ---- control thread ----
    void setInstrument(ms::InstrumentPtr inst, bool applyItsSettings = true);
    ms::InstrumentPtr instrument() const { return inst_.get(); }
    bool loadFile(const std::string& path, const std::string& projectDir = {}, std::string* error = nullptr);     // a .pulpinst
    // Builds an instrument from audio files (one note each) and takes it up. The report says what was heard in each.
    bool buildFromFiles(const std::vector<std::string>& paths, const std::string& projectDir = {}, ms::BuildReport* report = nullptr, std::string* error = nullptr);
    std::string instrumentFile() const { return file_; }

    Param *pAttack, *pDecay, *pSustain, *pRelease, *pCutoff, *pReso, *pEnvAmt, *pFAttack, *pFDecay, *pKeyTrack,
          *pLfoRate, *pLfoDelay, *pVibrato, *pTremolo, *pLfoFilter, *pPitchEnv, *pPitchTime, *pVelSens, *pLoop, *pTune, *pTranspose, *pStart, *pVolume;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    struct Voice {
        bool active = false;
        uint32 id = 0, age = 0;
        int key = 60, group = 0;
        const ms::Zone* zone = nullptr;
        double pos = 0.0;
        float semis = 0.0f;             // key - root - cents, fixed for the note
        float gainL = 1.0f, gainR = 1.0f;
        float bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;
        float modX = 0.5f, modY = 0.5f, relScale = 1.0f;
        Adsr amp, filt;
        dsp::Svf svfL, svfR;
        double lfoPhase = 0.0;
        float lfoFade = 0.0f;           // 0..1 as the LFO comes in after its delay
        float pitchEnv = 0.0f;          // semitones still to fall away
        int64 played = 0;               // samples since the note began
        float kill = 1.0f, killStep = 0.0f;
        bool looping = false;
    };
    static constexpr int kVoices = 40;
    void startVoice(const NoteEvent& e);
    void renderVoice(Voice& v, float* L, float* R, int n);

    RtShared<ms::Instrument> inst_;
    const ms::Instrument* cur_ = nullptr;
    const ms::Instrument* last_ = nullptr;
    std::string file_;                  // the .pulpinst this came from, if any
    Voice voices_[kVoices];
    uint32 ageCounter_ = 0, turn_ = 0;
    double sr_ = 48000.0;
    float bend_ = 0.0f;
    std::atomic<int> voicesActive_{0};
};

} // namespace pulp
