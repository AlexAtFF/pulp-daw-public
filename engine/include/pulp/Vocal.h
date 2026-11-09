#pragma once
// Vocal effects: True Note (automatic pitch correction), Talker (channel vocoder), Second Voice
// (harmonizer and doubler) and Lint (noise reduction that learns a noise profile).
// Shared pieces live in pulp::vocal: a pitch detector, a pitch-synchronous grain shifter, scales, an FFT.
#include "pulp/Effects.h"

namespace pulp {
namespace vocal {

// ---- keys and scales ----
enum Scale { Chromatic, Major, Minor, PentaMajor, PentaMinor, Blues, Dorian, Mixolydian, HarmonicMinor, NumScales };
std::vector<std::string> keyNames();                // C .. B
std::vector<std::string> scaleNames();              // the NumScales scales above
unsigned scaleMask(int scale, int key);             // pitch-class set: bit 0 = C ... bit 11 = B
double nearestNote(double key, unsigned mask);      // nearest allowed MIDI key number; the input itself when the set is empty
int stepNote(int note, int steps, unsigned mask);   // from a note, move by a number of notes of the set

// A power-of-two ring addressed by absolute sample number.
struct Ring {
    std::vector<float> buf;
    float* d = nullptr;
    int64 mask = 0;
    void prepare(int minSize) {
        int n = 64;
        while (n < minSize) n <<= 1;
        buf.assign((size_t)n, 0.0f);
        d = buf.data();
        mask = n - 1;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); }
    inline float& at(int64 i) { return d[i & mask]; }
    inline float at(int64 i) const { return d[i & mask]; }
};

// Radix-2 FFT with its tables made in prepare(), for use on the audio thread.
class Fft {
public:
    void prepare(int n);
    void run(float* re, float* im, bool inverse) const;     // the inverse divides by n
    int size() const { return n_; }
private:
    int n_ = 0;
    std::vector<int> rev_;
    std::vector<float> cos_, sin_;
};

// Monophonic pitch detector. A coarse search on a decimated copy (normalised square difference, first strong
// peak) finds the period; a second pass at the full rate over about two periods, centred a fixed time
// behind the newest sample, refines it. Estimates are time-stamped so a user can ask for the period at a time.
class PitchDetector {
public:
    struct Estimate { double time = 0.0, period = 0.0; float clarity = 0.0f; bool voiced = false; };
    static constexpr int kHopDec = 64, kMaxEst = 128;
    static constexpr double kCentreSeconds = 0.0095;        // how far behind "now" the refined window is centred
    void prepare(double sr, int monoRingSize);
    void reset();
    void setRange(int r) { range_ = r <= 0 ? 0 : 1; }       // 0 = low voice (60..600 Hz), 1 = high voice (110..1200 Hz)
    void push(float x);                                     // one mono sample; analyses every hop
    int64 now() const { return n_; }                        // samples pushed so far
    bool periodAt(double time, double& period) const;       // true when voiced there; period is always usable
    const Estimate* latest() const { return count_ > 0 ? &est_[(size_t)((count_ - 1) % kMaxEst)] : nullptr; }
    int64 estimates() const { return count_; }
    Ring& mono() { return mono_; }
    double defaultPeriod() const { return defaultT_; }
    int hop() const { return kHopDec * dec_; }
private:
    void analyse();
    double sr_ = 48000.0, defaultT_ = 240.0;
    int dec_ = 4, centre_ = 456, range_ = 0;
    int tmin_[2] = {20, 10}, tmax_[2] = {201, 110}, win_[2] = {442, 242};      // decimated lags and window
    int fineMin_[2] = {72, 36}, fineMax_[2] = {860, 470};
    dsp::Biquad lp_[2];
    Ring mono_;
    std::vector<float> decBuf_, w_, lin_, nsdf_;
    std::vector<double> sq_, psq_, vals_;
    int decW_ = 0, decPhase_ = 0, hopCount_ = 0;
    int64 n_ = 0, count_ = 0;
    Estimate est_[kMaxEst];
    bool lastVoiced_ = false;
    double lastPeriod_ = 0.0;
};

// What a new grain is given: the period of the input there, the pitch ratio and the formant ratio.
// centre: shifting is about to start (the ratios are still 1), so the first grain of a voiced stretch
// should already be cut around the voice's pulse.
struct GrainCtl { double period = 240.0, pitch = 1.0, formant = 1.0; bool voiced = false, centre = false; };

// Pitch-synchronous overlap-add. Output marks sit one output period apart; each grain is a two-period,
// raised-cosine windowed piece of the input read around a point that moves on by exactly one input period
// per grain, so consecutive grains line up in phase. The formant ratio is the speed the grain is read at:
// 1 keeps the spectral envelope where it was, the pitch ratio moves it with the pitch (plain resampling).
// With pitch = formant = 1 and no offset the output is the input, bit for bit.
class GrainShifter {
public:
    static constexpr int kMaxGrains = 24, kTaps = 8, kPhases = 256, kWin = 1024;
    // hist: nch rings of input addressed by absolute sample number (kept by the owner, same mask);
    // lookahead: samples of input that exist after the output position
    void prepare(double sr, int lookahead, float* const* hist, int64 mask, int nch);
    void reset(double startTime);
    inline bool due(int64 t) const { return (double)t >= spawnAt_; }
    double nextMark() const { return nextMark_; }
    // The input's period just before the next grain, measured by lining its last period up with the
    // previous grain's (so the estimate has no lag and follows cycle-to-cycle changes). `estimate` is the
    // detector's value; it is returned unchanged when the measurement cannot be made or is not clear.
    double localPeriod(double estimate, int64 newest);
    void spawn(const GrainCtl& c, int64 newest);            // newest: number of the last input sample that exists
    void render(int64 t, float* out);
    int active() const { return n_; }
    double offset() const { return delta_; }
private:
    struct Grain {
        double centre = 0, start = 0, end = 0, invL = 0, invR = 0, src = 0, rate = 1;
        int64 ioff = 0;
        float gain = 1.0f;
        float coef[kTaps] = {};
        bool ident = false, fixed = true, intTap = true, first = false;
    };
    struct Tables;
    static const Tables& tables();
    const Tables* tab_ = nullptr;
    Grain g_[kMaxGrains];
    int n_ = 0, nonIdent_ = 0;
    bool hasPrev_ = false, prevIdent_ = true, prevUnity_ = true, prevVoiced_ = false;
    double nextMark_ = 0.0, lastMark_ = 0.0, spawnAt_ = 0.0, prevSpacing_ = 240.0, prevT_ = 240.0, delta_ = 0.0;
    double minT_ = 32.0, maxT_ = 880.0, lookahead_ = 0.0;
    std::vector<float> scratch_;
    std::vector<double> scratchSq_;
    float* hist_[2] = {nullptr, nullptr};
    int64 mask_ = 0;
    int nch_ = 1;
    int64 findPulse(int64 end, int period, int half, double at, double& peakEnergy, double& energyAt);
    void centreOnPulse(double s, double T, int64 newest, bool jump);
};

int latencyFor(double sr);      // the delay True Note and Second Voice add: 18 ms

} // namespace vocal

// ---- True Note: automatic pitch correction for one voice ----
class TrueNote : public Plugin {
public:
    enum { kCustomScale = vocal::NumScales };
    TrueNote();
    const char* typeId() const override { return "tune"; }
    const char* displayName() const override { return "True Note"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return latency_; }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // 0 = detected pitch as a MIDI key number (0 when unvoiced), 1 = correction applied in cents, 2 = 1 when voiced
    float meter(int i) const override { return i == 0 ? mPitch_.load(std::memory_order_relaxed) : i == 1 ? mCents_.load(std::memory_order_relaxed) : i == 2 ? mVoiced_.load(std::memory_order_relaxed) : 0.0f; }
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }
    Param *pKey, *pScale, *pNote[12], *pSpeed, *pAmount, *pTranspose, *pRange, *pFormant, *pFShift, *pMix;
private:
    void spawnGrain();
    vocal::PitchDetector det_;
    vocal::GrainShifter sh_;
    vocal::Ring hist_[2];
    double sr_ = 48000.0;
    int latency_ = 864;
    int64 n_ = 0;
    // read once per quantum
    unsigned mask_ = 0xFFF;
    double amount_ = 1.0, rateK_ = 0.0, transpose_ = 0.0, fshift_ = 1.0;
    bool snap_ = false, preserve_ = true;
    // retune state
    bool wasVoiced_ = false;
    double note_ = 60.0, corr_ = 0.0, lastMark_ = 0.0;
    Ramp mix_;
    bool primed_ = false;
    std::atomic<float> mPitch_{0.0f}, mCents_{0.0f}, mVoiced_{0.0f};
    ScopeTap scope_;
};

// ---- Talker: channel vocoder ----
class Talker : public Plugin {
public:
    static constexpr int kMaxBands = 40, kMaxOsc = 8;
    enum Carrier { Saw, Pulse, NoiseCarrier };
    Talker();
    const char* typeId() const override { return "vocoder"; }
    const char* displayName() const override { return "Talker"; }
    bool wantsSidechain() const override { return true; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    double tailSeconds() const override { return 0.001 * pRelease->get() * 7.0 + 0.05; }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // 0 = 1 when the sidechain is the carrier, 1 = how unvoiced the input is judged (0..1), 2 = bands in use
    float meter(int i) const override { return i == 0 ? mSide_.load(std::memory_order_relaxed) : i == 1 ? mUnvoiced_.load(std::memory_order_relaxed) : i == 2 ? mBands_.load(std::memory_order_relaxed) : 0.0f; }
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }
    // Band edges of the analysis bank as currently set (Hz), for tests and a display.
    void bandEdges(int band, double& lo, double& hi) const;
    Param *pBands, *pLow, *pHigh, *pWidth, *pFormant, *pAttack, *pRelease, *pSibil, *pAir, *pAirFreq;
    Param *pSide, *pCarrier, *pChord, *pPitch, *pOut, *pMix;
private:
    struct Sec { float g = 0.1f, k = 1.0f, a1 = 0, a2 = 0, a3 = 0; };       // one two-pole section's coefficients
    struct Band {
        Sec an[2], sy[2];
        float anNorm = 1.0f, anQuad = 1.0f, syNorm = 1.0f;
        bool synthOn = true, idle = true;
        float anS[2][2] = {};               // analysis states: [section][ic1, ic2]
        float syS[2][2][2] = {};            // synthesis states: [channel][section][ic1, ic2]
        float env = 0.0f;
    };
    void layout(int bands, float lo, float hi, float width, float formant);
    void renderCarrier(float* l, float* r, int n);
    Band band_[kMaxBands];
    int bands_ = 0, pendingBands_ = 0;
    float seen_[4] = {-1, -1, -1, -1};
    double sr_ = 48000.0;
    // internal carrier
    double phase_[kMaxOsc] = {};
    dsp::Noise carNoise_[2], sibNoise_;
    // unvoiced detector and air
    dsp::Svf hfDet_, lfDet_, air_[2];
    float hfP_ = 0.0f, lfP_ = 0.0f, unv_ = 0.0f;
    float airSeen_ = -1.0f;
    Ramp out_, mix_, airGain_, fade_;
    bool primed_ = false;
    std::atomic<float> mSide_{0.0f}, mUnvoiced_{0.0f}, mBands_{0.0f};
    ScopeTap scope_;
};

// ---- Second Voice: harmonizer and doubler ----
class SecondVoice : public Plugin {
public:
    static constexpr int kVoices = 4;
    SecondVoice();
    const char* typeId() const override { return "harmony"; }
    const char* displayName() const override { return "Second Voice"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return latency_; }
    double tailSeconds() const override { return 0.1; }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // 0 = detected pitch as a MIDI key number (0 when unvoiced), 1 = 1 when voiced, 2 = voices sounding
    float meter(int i) const override { return i == 0 ? mPitch_.load(std::memory_order_relaxed) : i == 1 ? mVoiced_.load(std::memory_order_relaxed) : i == 2 ? mActive_.load(std::memory_order_relaxed) : 0.0f; }
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }
    Param *pMode, *pKey, *pScale, *pFormant, *pRange, *pDry, *pWet;
    Param *pOn[kVoices], *pInterval[kVoices], *pLevel[kVoices], *pPan[kVoices], *pDelay[kVoices], *pDetune[kVoices];
private:
    struct Voice {
        vocal::GrainShifter sh;
        vocal::Ring dl;
        Ramp gl, gr;
        bool running = false, fading = false;
        int dA = 0, dB = 0;
        float xf = 0.0f;
        // read once per quantum
        int interval = 0;
        double detune = 0.0;
    };
    void spawnGrain(Voice& v);
    vocal::PitchDetector det_;
    vocal::Ring dry_[2];
    Voice v_[kVoices];
    double sr_ = 48000.0;
    int latency_ = 864;
    int64 n_ = 0;
    unsigned mask_ = 0xFFF;
    bool doubler_ = false, preserve_ = true;
    bool noteValid_ = false;
    double note_ = 60.0;
    Ramp dryG_;
    bool primed_ = false;
    std::atomic<float> mPitch_{0.0f}, mVoiced_{0.0f}, mActive_{0.0f};
    ScopeTap scope_;
};

// ---- Lint: noise reduction with a learned noise profile ----
class Lint : public Plugin {
public:
    static constexpr int kMaxFft = 8192, kMaxBins = kMaxFft / 2 + 1;
    Lint();
    const char* typeId() const override { return "denoise"; }
    const char* displayName() const override { return "Lint"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return latency_; }
    double tailSeconds() const override { return 0.1; }
    Json saveExtra() const override;
    void loadExtra(const Json& j, const std::string& projectDir) override;
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // 0 = average reduction over the spectrum in dB, 1 = 1 while learning, 2 = 1 when a profile is held, 3 = frames learned so far
    float meter(int i) const override { return i == 0 ? mReduction_.load(std::memory_order_relaxed) : i == 1 ? mLearning_.load(std::memory_order_relaxed) : i == 2 ? mHasProfile_.load(std::memory_order_relaxed) : i == 3 ? mFrames_.load(std::memory_order_relaxed) : 0.0f; }
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }
    bool hasProfile() const { return mHasProfile_.load(std::memory_order_relaxed) >= 0.5f; }
    int fftSize() const { return fft_.size(); }
    Param *pLearn, *pAmount, *pFloor, *pTime, *pFreq, *pAttack, *pRelease;
private:
    // The profile as it is stored and exchanged: noise power density in tenths of a decibel per bin.
    struct Profile {
        bool valid = false, same = true;        // same: one profile for both channels
        double rate = 48000.0;
        int bins = 0;
        std::vector<short> q[2];
    };
    void frame();
    void applyProfile();            // builds the working arrays from mine_ for the current rate and frame size
    void finishLearning();
    bool tryPublish();
    double sr_ = 48000.0;
    int n_ = 2048, hop_ = 512, bins_ = 1025, latency_ = 1984;
    vocal::Fft fft_;
    std::vector<float> win_, in_[2], acc_[2], re_, im_, fr_[2];
    std::vector<float> noise_[2], pt_[2], gain_[2], pw_;
    std::vector<double> learn_[2], prefix_;
    int64 count_ = 0, framesSince_ = 0;
    int learnFrames_ = 0;
    bool learning_ = false, primed_ = false, gainsPrimed_ = false, active_ = false;
    float wet_ = 0.0f;
    double winPower_ = 1024.0;
    // profile hand-over between the audio thread and the others
    Profile mine_;                          // audio thread's copy
    mutable Profile shared_;                // guarded by busy_
    mutable std::atomic<bool> busy_{false};
    std::atomic<uint32> loadSerial_{0};
    uint32 seenSerial_ = 0;
    bool publishPending_ = false;
    std::atomic<float> mReduction_{0.0f}, mLearning_{0.0f}, mHasProfile_{0.0f}, mFrames_{0.0f};
    ScopeTap scope_;
};

} // namespace pulp
