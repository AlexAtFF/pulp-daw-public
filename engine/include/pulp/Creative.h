#pragma once
// Creative effects and a granular sampler:
//   Rewind     time and volume curves locked to the song position
//   Hiccup     beat-synced glitch and stutter
//   Thumb Pad  eleven effects driven by an X / Y pad
//   Platter    a record to grab and scratch
//   Motes      granular sampler (instrument)
#include "pulp/Audio.h"
#include "pulp/Dsp.h"
#include "pulp/Effects.h"
#include "pulp/Plugin.h"

namespace pulp {

namespace crt {

// A hash of three numbers: the random source of everything here that must depend on the song position
// and a seed and on nothing else.
uint32 hash32(uint32 a, uint32 b, uint32 c);
inline float hashUni(uint32 a, uint32 b, uint32 c) { return (float)(hash32(a, b, c) >> 8) * (1.0f / 16777216.0f); }   // 0..1

// Builds the interpolation table. Called from prepare() so the audio thread never does it.
void warmTables();

// Dry / wet that is exactly the dry signal when the two are equal or the mix is 0, and exactly the wet one at 1.
inline float mixDryWet(float dry, float wet, float m) { return m >= 1.0f ? wet : dry + (wet - dry) * m; }

// The most recent stretch of a stereo signal, read back at any distance behind the newest sample and at any
// speed. Reading faster than real time is band-limited (a windowed sinc that widens with the speed, up to
// four times); a read that lands on a sample at normal speed or slower returns that sample untouched.
struct Tape {
    std::vector<float> buf[2];
    int64 mask = 0;
    int64 n = 0;            // samples written since the last reset; sample k lives at k & mask
    int64 validFrom = 0;    // anything older reads as silence
    void prepare(double seconds, double sampleRate);
    void reset() { n = 0; validFrom = 0; }
    void forget() { validFrom = n; }                        // what has been recorded so far is gone
    double maxDelay() const { return (double)(mask + 1 - 96); }
    inline void push(float l, float r) {
        const size_t i = (size_t)(n & mask);
        buf[0][i] = l; buf[1][i] = r;
        ++n;
    }
    inline float at(int ch, int64 i) const { return i < validFrom ? 0.0f : buf[ch][(size_t)(i & mask)]; }
    // delay 0 is the newest sample. rate is the playback speed at this point (only its size matters).
    void read(double delay, double rate, float& l, float& r) const;
};

} // namespace crt

// ---- Rewind: time and volume curves that repeat with the song ----
class Rewind : public Plugin {
public:
    static constexpr int kMaxPoints = 32;
    static constexpr float kMaxBeats = 16.0f;
    enum Shape { Hold, Line, Smooth };
    struct Point {
        float pos = 0.0f;       // 0..1 along the pattern
        float value = 0.0f;     // time curve: beats behind now (0..16). volume curve: 0..1
        int shape = Hold;       // how the stretch after this point gets to the next one
    };
    Rewind();
    const char* typeId() const override { return "timewarp"; }
    const char* displayName() const override { return "Rewind"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    double tailSeconds() const override;        // how far back the time curve reaches, at 60 BPM
    Json saveExtra() const override;
    void loadExtra(const Json& j, const std::string& projectDir) override;
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // 0: where the pattern is (0..1), 1: how far behind now the sound is (beats), 2: the volume curve's gain.
    float meter(int i) const override;

    // Control thread. Points are sorted by position and clamped; at most kMaxPoints are kept.
    void setTimeCurve(const std::vector<Point>& pts);
    void setVolumeCurve(const std::vector<Point>& pts);
    std::vector<Point> timeCurve() const;
    std::vector<Point> volumeCurve() const;
    // A curve's value at a place in the pattern (0..1), as process() reads it. An empty curve gives def.
    static double valueAt(const std::vector<Point>& pts, double phase, float def);
    static double patternBeats(int lengthChoice);

    Param *pLength, *pTime, *pVolume, *pSmooth, *pMix;

private:
    struct Curve { int count = 0; Point pts[kMaxPoints]; };
    struct Curves { Curve time, volume; };
    static double eval(const Curve& c, double x, double eps, int& seg, float def);
    static float leftLimit(const Curve& c, int seg, float def);
    static float startValue(const Curve& c, int seg, float def);
    static Curve sanitize(const std::vector<Point>& pts, float lo, float hi);
    static Json curveJson(const Curve& c);
    static std::vector<Point> curveFromJson(const Json& j);

    static constexpr int kHeads = 4;
    struct Head { double delay = 0.0, slope = 0.0; float gain = 0.0f; bool on = false; };

    RtShared<Curves> curves_;
    Curves empty_;
    const Curves* seen_ = nullptr;
    crt::Tape tape_;
    Head heads_[kHeads];
    int cur_ = 0;
    int segT_ = -1, segV_ = -1, lastLen_ = -1;
    double sr_ = 48000.0, spb_ = 24000.0, nextTick_ = 0.0, lastX_ = 0.0;
    Ramp mix_, timeAmt_, volAmt_;
    float vol_ = 1.0f;
    bool primed_ = false, wasPlaying_ = false;
    int stopLeft_ = 0, stopLen_ = 1;
    std::atomic<float> mPhase_{0.0f}, mBehind_{0.0f}, mVol_{1.0f};
};

// ---- Hiccup: tricks fired on a step grid by a seeded draw ----
class Hiccup : public Plugin {
public:
    enum Trick { None = -1, Repeat, Reverse, Stop, Crush, Gate, NumTricks };
    Hiccup();
    const char* typeId() const override { return "glitch"; }
    const char* displayName() const override { return "Hiccup"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    double tailSeconds() const override { return 2.0 * stepBeats(pGrid->getInt()) + 0.1; }      // a trick reaches back two steps (at 60 BPM)
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    float meter(int i) const override { return i == 0 ? (float)(mTrick_.load(std::memory_order_relaxed) + 1) : 0.0f; }   // 0 = nothing, 1.. = trick

    // The trick a step fires with the settings as they are (step 0 starts at tick 0). For a display and for tests.
    int trickAt(int64 step) const;
    static double stepBeats(int gridChoice);
    static const char* trickName(int trick);
    // What the bit-crush burst does at an intensity: samples held, and quantising levels each side of zero.
    static int crushHold(float intensity, double sr);
    static float crushLevels(float intensity);

    Param *pGrid, *pCycle, *pProb[NumTricks], *pRepLen, *pPitch, *pIntensity, *pSeed, *pMix;

private:
    struct Step {
        int kind = None;
        int64 t0 = 0;           // number of the step's first sample on the tape
        double len = 1.0;       // samples
        double repLen = 1.0;    // samples in one repeat
        float pitch = 0.0f;     // semitones down per repeat
        int hold = 1;
        float levels = 32768.0f;
    };
    void sample(const Step& s, int64 now, float amt, float& l, float& r) const;
    crt::Tape tape_;
    Step cur_, prev_;
    int64 stepIndex_ = 0;
    int lastGrid_ = -1;
    int fadeLeft_ = 0, fadeLen_ = 96;
    double sr_ = 48000.0, nextTick_ = 0.0;
    Ramp mix_, amt_;
    bool primed_ = false, wasPlaying_ = false;
    std::atomic<int> mTrick_{-1};
};

// ---- Thumb Pad: eleven effects on an X / Y pad ----
class ThumbPad : public Plugin {
public:
    enum Mode { LowPass, HighPass, Flanger, Phaser, Echo, Wash, Drive, Crush, Ring, Vowel, TapeSpeed, NumModes };
    ThumbPad();
    const char* typeId() const override { return "xyfx"; }
    const char* displayName() const override { return "Thumb Pad"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    double tailSeconds() const override { return 8.5; }
    std::vector<Preset> factoryPresets() const override;
    float meter(int i) const override { return i == 0 ? mGain_.load(std::memory_order_relaxed) : 0.0f; }   // how far in the effect is, 0..1

    // What X and Y do in a mode, for labels on a pad.
    static const char* axisName(int mode, int axis);
    // The pad's mappings (x, y in 0..1), as the code uses them. For labels and for tests.
    static double lowPassHz(double x) { return 80.0 * std::pow(18000.0 / 80.0, x); }
    static double highPassHz(double x) { return 20.0 * std::pow(8000.0 / 20.0, x); }
    static double filterQ(double y) { return 0.7071 + y * y * 12.0; }
    static double flangerMs(double x) { return 0.25 * std::pow(10.0 / 0.25, x); }
    static double flangerFeedback(double y) { return 0.9 * y; }
    static double phaserHz(double x) { return 200.0 * std::pow(4000.0 / 200.0, x); }
    static double phaserFeedback(double y) { return 0.85 * y; }
    static double echoMs(double x) { return 30.0 * std::pow(900.0 / 30.0, x); }
    static double echoFeedback(double y) { return 0.92 * y; }
    static double washSeconds(double x) { return 0.4 * std::pow(8.0 / 0.4, x); }
    static double washDampHz(double y) { return 1500.0 * std::pow(18000.0 / 1500.0, y); }
    static double driveDb(double x) { return 36.0 * x; }
    static double driveToneHz(double y) { return 800.0 * std::pow(18000.0 / 800.0, y); }
    static double crushHz(double x, double sr) { return 500.0 * std::pow(sr / 500.0, x); }
    static double crushBits(double y) { return 2.0 + 14.0 * y; }
    static double ringHz(double x) { return 20.0 * std::pow(4000.0 / 20.0, x); }
    static double tapeSpeed(double x) { return 2.0 * x; }
    static double tapeInertiaSeconds(double y) { return 0.005 * std::pow(1.0 / 0.005, y); }
    static void vowelFormants(double x, double hz[3], double gain[3]);
    static double vowelQ(double y) { return 4.0 + 14.0 * y; }

    Param *pMode, *pX, *pY, *pActive, *pMix;

private:
    void clearEffect();
    void effect(int mode, float x, float y, float inL, float inR, float& l, float& r);

    static constexpr int kStages = 6, kWash = 4;
    double sr_ = 48000.0;
    int mode_ = 0;
    bool primed_ = false, dirty_ = false;
    float xs_ = 0.5f, ys_ = 0.5f, xr_ = 0.5f, yr_ = 0.5f;      // pad position: ramped across the quantum, then smoothed
    float smooth_ = 0.0f;
    float gain_ = 0.0f, gainStep_ = 0.0f;
    Ramp mix_;
    dsp::Svf svf_[2], formant_[3][2];
    crt::Tape tape_;                    // flanger, echo and tape speed
    double delaySm_ = -1.0;             // echo time as it glides (samples); negative = not set yet
    double echoCoef_ = 0.0;
    float apZ_[kStages][2] = {};        // phaser
    float phFb_[2] = {};
    dsp::DelayLine wash_[kWash], washAp_[2][2];
    float washLp_[kWash] = {};
    int washLen_[kWash] = {}, washApLen_[2] = {};
    dsp::OnePole tone_[2], dcBlock_[2];
    double adaaX_[2] = {}, adaaF_[2] = {};
    bool adaaOn_ = false;
    double crushPhase_ = 1.0;
    float crushHeld_[2] = {};
    double ringPhase_ = 0.0;
    double tapeBehind_ = 0.0, tapeSpeed_ = 1.0;
    std::atomic<float> mGain_{0.0f};
};

// ---- Platter: a record that can be grabbed and scratched ----
class Platter : public Plugin {
public:
    Platter();
    const char* typeId() const override { return "scratch"; }
    const char* displayName() const override { return "Platter"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    double tailSeconds() const override { return pHold->getBool() ? kSeconds + 1.0 : 0.0; }     // held, it can play anything it has kept
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // 0: how far behind the input the needle is (seconds), 1: the record's speed under the needle (1 = normal,
    // negative = backwards), 2: 1 while the record is held.
    float meter(int i) const override;

    Param *pHold, *pPosition, *pRange, *pInertia, *pSpeed, *pStart, *pBrake, *pCut;
    static constexpr double kSeconds = 10.0;        // how much is kept

private:
    crt::Tape tape_;
    double sr_ = 48000.0;
    bool engaged_ = false, primed_ = false;
    float fade_ = 0.0f;             // 0 = the input, 1 = the record
    double base_ = 0.0;             // how far behind the input the record itself is (samples)
    double motor_ = 1.0;            // the record's own speed
    double off1_ = 0.0, off2_ = 0.0;        // the hand: where the needle is pushed to (samples), smoothed twice
    double lastDelay_ = 0.0;
    float cut_ = 1.0f;
    float moveGain_ = 1.0f;         // the level that goes with the speed under the needle (none when it stands still)
    Ramp pos_;
    std::atomic<float> mBehind_{0.0f}, mSpeed_{1.0f}, mHeld_{0.0f};
};

// ---- Motes: granular sampler ----
class Motes : public Instrument {
public:
    enum Window { Hann, Triangle, Bell, FlatTop, Pluck, Swell, NumWindows };
    static constexpr int kMaxPoly = 16;
    Motes();
    const char* typeId() const override { return "grains"; }
    const char* displayName() const override { return "Motes"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    double tailSeconds() const override { return pRelease->get() * 0.001 + pSize->get() * 0.001; }
    Json saveExtra() const override;
    void loadExtra(const Json& j, const std::string& projectDir) override;
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    float meter(int i) const override { return i == 0 ? (float)grainsActive_.load(std::memory_order_relaxed) : 0.0f; }   // grains sounding

    // Control thread. Passing nullptr clears the sample.
    void setSample(SamplePtr s);
    SamplePtr sample() const;
    std::string samplePath() const;
    // The shape of a grain: its gain at a place in the grain (0..1). For a display and for tests.
    static float windowAt(int shape, double phase);

    Param *pPosition, *pSpread, *pScan, *pLoop, *pSize, *pDensity, *pJitter, *pShape, *pReverse, *pPanSpread,
          *pRoot, *pKeytrack, *pCoarse, *pFine, *pPitchSpread, *pAttack, *pDecay, *pSustain, *pRelease,
          *pCutoff, *pReso, *pLevel, *pPoly, *pSeed;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    struct Source { SamplePtr sample; };
    struct Grain {
        bool on = false;
        int shape = 0;
        double pos = 0.0;       // frames into the sample
        double ratio = 1.0;     // this grain's own detune, negative when it plays backwards
        float phase = 0.0f, phaseInc = 0.0f;
        float gl = 0.0f, gr = 0.0f;
    };
    static constexpr int kSlots = kMaxPoly + 8, kGrains = 48;
    struct Voice {
        bool active = false;
        uint32 id = 0, age = 0;
        int key = 60, group = 0;
        float gain = 1.0f, panL = 1.0f, panR = 1.0f;
        float keySemis = 0.0f, fineCents = 0.0f;
        float bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;
        int stage = 0;          // 0 attack, 1 decay / sustain, 3 release
        float env = 0.0f, relScale = 1.0f;
        float kill = 1.0f, killStep = 0.0f;
        float modX = 0.5f, modY = 0.5f;
        double scan = 0.0;      // frames the position has moved since the note began
        double next = 0.0;      // samples until the next grain
        bool ended = false;     // the position ran off the end of an unlooped sample
        dsp::Noise rng;
        dsp::Svf filtL, filtR;
        float cutoff = -1.0f;
        int grains = 0;
        Grain g[kGrains];
    };
    void startVoice(const NoteEvent& e);
    void spawn(Voice& v, int offset, double ratio);
    void renderVoice(Voice& v, float* L, float* R, int n);

    RtShared<Source> source_;
    const Source* cur_ = nullptr;
    const Source* last_ = nullptr;
    Voice voices_[kSlots];
    uint32 ageCounter_ = 0;
    double sr_ = 48000.0;
    float attackRate_ = 1.0f, decayCoef_ = 0.0f, sustain_ = 1.0f, releaseCoef_ = 0.0f;
    float bend_ = 0.0f;
    Ramp level_;
    bool primed_ = false;
    std::atomic<int> voicesActive_{0}, grainsActive_{0};
};

} // namespace pulp
