#pragma once
// Tone effects: Ten Band (graphic equaliser), Motion Filter (filter with its own envelope follower and LFO),
// Quarter Inch (tape), Glass (tube stages), Stack (amplifier and cabinet), Splat (fuzz), Coarse (bit depth and
// sample rate reduction), Bend (drawable waveshaper) and Middle and Sides (mid/side levels and shelves).
// Contour EQ, Sweep, Grit and Stereo Kit in Effects.h are the plain equaliser, filter, distortion and stereo
// tool; nothing here repeats them.
#include "pulp/Effects.h"

namespace pulp {

namespace tonefx {

constexpr int kOsLatency = 32;      // what every oversampled effect here is late by, in samples at the project rate

inline float dbGain(float db) { return std::exp(db * 0.11512925464970229f); }      // 10^(db/20); exactly 1 at 0 dB
inline void flushD(double& v) { if (std::fabs(v) < 1.0e-20) v = 0.0; }

// ---- oversampling ----
// A Kaiser low-pass at the project's Nyquist frequency on the way up (32 * factor + 1 taps) and again on the way
// down. The way down is shortened by one tap for every half fast sample that the stages in between are late by
// (a first-order anti-aliased stage is half a sample late), so the whole trip is always exactly kOsLatency
// samples whatever the factor and whatever sits in between.
struct OsTables {
    int factor = 4, halfDelays = 0, nDown = 129;
    float up[8][33];        // one row of taps per output phase
    float down[260];
};
const OsTables* osTables(int factor, int halfDelays);      // factor 2, 4 or 8; halfDelays 0..4. Never allocates.

struct Oversampler {
    static constexpr int kUp = 33, kDown = 260, kMaxFactor = 8;
    void setTables(const OsTables* t) { t_ = t; }
    void reset() { std::memset(up_, 0, sizeof up_); std::memset(down_, 0, sizeof down_); upPos_ = downPos_ = 0; }
    int factor() const { return t_->factor; }
    // one input sample in, `factor` samples at the fast rate out
    inline void up(float x, float* out) {
        upPos_ = upPos_ == 0 ? kUp - 1 : upPos_ - 1;
        up_[upPos_] = up_[upPos_ + kUp] = x;
        const float* h = up_ + upPos_;
        const int R = t_->factor;
        for (int p = 0; p < R; ++p) {
            const float* c = t_->up[p];
            float s = 0.0f;
            for (int k = 0; k < kUp; ++k) s += c[k] * h[k];
            out[p] = s;
        }
    }
    // `factor` fast samples in, one filtered sample out
    inline float down(const float* in) {
        downPos_ = downPos_ == 0 ? kDown - 1 : downPos_ - 1;
        down_[downPos_] = down_[downPos_ + kDown] = in[0];
        const float* h = down_ + downPos_;
        const float* c = t_->down;
        const int n = t_->nDown;
        float s = 0.0f;
        for (int j = 0; j < n; ++j) s += c[j] * h[j];
        const int R = t_->factor;
        for (int p = 1; p < R; ++p) {
            downPos_ = downPos_ == 0 ? kDown - 1 : downPos_ - 1;
            down_[downPos_] = down_[downPos_ + kDown] = in[p];
        }
        return std::fabs(s) < 1.0e-30f ? 0.0f : s;
    }
private:
    const OsTables* t_ = nullptr;
    float up_[2 * kUp] = {}, down_[2 * kDown] = {};
    int upPos_ = 0, downPos_ = 0;
};

// The same delay for a signal that does not go through the filters.
struct LatencyDelay {
    float buf[2][kOsLatency] = {};
    int pos = 0;
    void reset() { std::memset(buf, 0, sizeof buf); pos = 0; }
    inline void push(float l, float r, float& dl, float& dr) {
        dl = buf[0][pos]; dr = buf[1][pos];
        buf[0][pos] = l; buf[1][pos] = r;
        pos = (pos + 1) % kOsLatency;
    }
};

// Turns the oversampled path on and off without a click: it runs unheard for one quantum to fill its filters,
// is faded in over the next, and is faded out again before it stops.
struct OsGate {
    enum { Off, Prime, On };
    int state = Off;
    Ramp mix;
    void reset() { state = Off; mix.snap(0.0f); }
    void startOn() { state = On; mix.snap(1.0f); }
    // Call once a quantum. Returns true when the oversampled path has to run in this quantum; `justStarted` is set
    // in the quantum it starts in (the caller clears the filters then).
    bool begin(bool want, int n, bool& justStarted) {
        justStarted = false;
        if (want) {
            if (state == Off) { state = Prime; mix.snap(0.0f); justStarted = true; }
            else { state = On; mix.target(1.0f, n); }
        } else if (state != Off) {
            if (mix.cur <= 0.0f && mix.left == 0) state = Off;
            else mix.target(0.0f, n);
        }
        return state != Off;
    }
};

// A choice that changes the structure of an effect (a mode, a voicing) is not switched under the sound: the wet
// signal dips to nothing over one quantum, the switch happens and the new arrangement runs unheard for a quantum
// (long enough to fill the oversampling filters), and it comes back over the next. Four milliseconds in all.
struct ChoiceDip {
    int active = -1;
    Ramp gain;
    void reset() { active = -1; gain.snap(1.0f); }
    // Call once a quantum with the wanted value. Returns true in the quantum in which `active` changed.
    bool begin(int want, int n) {
        if (active < 0) { active = want; gain.snap(1.0f); return true; }
        if (want != active) {
            if (gain.cur <= 0.0f && gain.left == 0) { active = want; return true; }
            gain.target(0.0f, n);
            return false;
        }
        gain.target(1.0f, n);
        return false;
    }
};

// ---- filters whose coefficients glide ----
// Biquad coefficients that move in a straight line to a new set over a number of samples. Every set on the way
// is stable, because the stable region of (a1, a2) is convex.
struct GlideCoefs {
    double c[5] = {1, 0, 0, 0, 0};      // b0 b1 b2 a1 a2
    double t[5] = {1, 0, 0, 0, 0}, d[5] = {};
    int left = 0;
    void snap(const dsp::Biquad& q) { t[0] = q.b0; t[1] = q.b1; t[2] = q.b2; t[3] = q.a1; t[4] = q.a2; for (int i = 0; i < 5; ++i) c[i] = t[i]; left = 0; }
    void glide(const dsp::Biquad& q, int n) {
        t[0] = q.b0; t[1] = q.b1; t[2] = q.b2; t[3] = q.a1; t[4] = q.a2;
        bool same = true;
        for (int i = 0; i < 5; ++i) same = same && t[i] == c[i];
        if (same || n <= 0) { for (int i = 0; i < 5; ++i) c[i] = t[i]; left = 0; return; }
        for (int i = 0; i < 5; ++i) d[i] = (t[i] - c[i]) / (double)n;
        left = n;
    }
    inline void step() {
        if (left > 0) {
            if (--left == 0) { for (int i = 0; i < 5; ++i) c[i] = t[i]; }
            else { for (int i = 0; i < 5; ++i) c[i] += d[i]; }
        }
    }
    bool moving() const { return left > 0; }
    // magnitude of the set it is heading for
    double magnitude(double hz, double sr) const {
        const double w = kTwoPi * hz / sr;
        const double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
        const double nr = t[0] + t[1] * cr + t[2] * c2r, ni = t[1] * ci + t[2] * c2i;
        const double dr = 1 + t[3] * cr + t[4] * c2r, di = t[3] * ci + t[4] * c2i;
        return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }
};
struct BqState {
    double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    inline double run(const GlideCoefs& k, double x) {
        const double y = k.c[0] * x + z1;
        z1 = k.c[1] * x - k.c[3] * y + z2;
        z2 = k.c[2] * x - k.c[4] * y;
        return y;
    }
    void flush() { flushD(z1); flushD(z2); }
    bool quiet() const { return std::fabs(z1) < 1.0e-15 && std::fabs(z2) < 1.0e-15; }
};

// One-pole filter in double precision (coupling capacitors, DC blockers, tone controls).
struct Pole {
    double z = 0, a = 0;
    void setHz(double hz, double sr) { a = 1.0 - std::exp(-kTwoPi * hz / sr); }
    inline double lp(double x) { z += a * (x - z); return z; }
    inline double hp(double x) { z += a * (x - z); return x - z; }
    void reset() { z = 0; }
    void flush() { flushD(z); }
};

// ---- the saturating curve the tube, tape and amplifier stages share ----
// u / sqrt(1 + (k u)^2): k is 1 above zero and `kn` below, so the two polarities can saturate differently.
// Slope 1 and no curvature at zero on both sides.
struct Sig {
    static inline double f(double u, double kn) { const double k = u >= 0.0 ? 1.0 : kn; return u / std::sqrt(1.0 + k * k * u * u); }
    static inline double slope(double u, double kn) { const double k = u >= 0.0 ? 1.0 : kn; const double q = 1.0 + k * k * u * u; return 1.0 / (q * std::sqrt(q)); }
};
// The curve with first-order antiderivative anti-aliasing: the output is the average of the curve between the
// previous input and this one, which takes the edge off what would fold back. It is half a sample late.
// On one side of zero the average is simply (u0 + u1) / (r0 + r1), with r = sqrt(1 + (k u)^2).
struct AdaaSig {
    double u0 = 0, r0 = 1;
    void reset(double rest, double kn) { u0 = rest; const double k = rest >= 0.0 ? 1.0 : kn; r0 = std::sqrt(1.0 + k * k * rest * rest); }
    inline double run(double u1, double kn) {
        const double k1 = u1 >= 0.0 ? 1.0 : kn;
        const double r1 = std::sqrt(1.0 + k1 * k1 * u1 * u1);
        double y;
        if ((u1 >= 0.0) == (u0 >= 0.0)) y = (u0 + u1) / (r0 + r1);
        else y = (u1 * u1 / (1.0 + r1) - u0 * u0 / (1.0 + r0)) / (u1 - u0);
        u0 = u1; r0 = r1;
        return y;
    }
};

} // namespace tonefx

// ---- Ten Band: graphic equaliser ----
// Ten octave bands from 31 Hz to 16 kHz. The bands overlap, so the gain each filter is given is worked out from
// all ten sliders together: the curve then passes through the slider settings at the band centres.
class TenBand : public Plugin {
public:
    static constexpr int kBands = 10;
    static constexpr double kQ = 1.0;           // the same for every band and every gain
    static constexpr double kMaxFilterDb = 40.0;
    static double centreHz(int band) { return 31.25 * (double)(1 << band); }
    TenBand();
    const char* typeId() const override { return "geq"; }
    const char* displayName() const override { return "Ten Band"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }
    double magnitudeDb(double hz) const;        // the curve the sliders give once they have settled, for drawing and tests
    void filterGains(double* db) const;         // the ten filter gains behind that curve
    Param *pGain[kBands], *pOut;
private:
    void design(dsp::Biquad& b, int band, double db) const;
    void solve(const double* sliders, double* filterDb) const;
    double respDb(int band, double db, int at) const;
    double sr_ = 48000.0;
    double cosw_[kBands] = {}, alpha_[kBands] = {}, cos2w_[kBands] = {};
    double inv_[kBands][kBands] = {};
    double cur_[kBands] = {}, solved_[kBands] = {}, fdb_[kBands] = {};
    tonefx::GlideCoefs co_[kBands];
    tonefx::BqState st_[kBands][2];
    bool active_[kBands] = {};
    double slew_ = 1.0;
    Ramp out_;
    bool primed_ = false;
    ScopeTap scope_;
};

// ---- Motion Filter: multimode filter moved by its own envelope follower and LFO ----
class MotionFilter : public Plugin {
public:
    enum Mode { Low, Band, High, Notch, Peak, NumModes };
    static constexpr int kDivisions = 14;
    MotionFilter();
    const char* typeId() const override { return "modfilter"; }
    const char* displayName() const override { return "Motion Filter"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return tonefx::kOsLatency; }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // 0: the envelope follower (0..1), 1: the left cutoff in Hz, 2: the right cutoff in Hz, as of the last sample processed
    float meter(int i) const override { return i >= 0 && i < 3 ? pub_[i].load(std::memory_order_relaxed) : 0.0f; }
    static float lfoWave(int shape, double phase);      // -1..1; phase in cycles
    static double divisionTicks(int index);             // length of one LFO cycle for a tempo-synced setting
    static float resonanceQ(float reso) { return 0.7071f + reso * reso * 24.0f; }
    static constexpr double kSmoothMs = 2.0;            // the cutoff follows its modulation through this much smoothing
    Param *pMode, *pSlope, *pCutoff, *pReso, *pDrive, *pEnvAmt, *pEnvSens, *pAttack, *pRelease;
    Param *pLfoShape, *pLfoSync, *pLfoRate, *pLfoDiv, *pLfoDepth, *pLfoPhase, *pMix, *pOut;
private:
    dsp::Svf svf_[2][2];
    tonefx::Oversampler os_[2];
    tonefx::AdaaSig sat_[2];
    tonefx::OsGate gate_;
    tonefx::LatencyDelay dly_;
    tonefx::ChoiceDip dip_;
    double sr_ = 48000.0, free_ = 0.0;
    float env_ = 0.0f;
    double sm_[2] = {0, 0};         // smoothed log2 of the cutoff
    float lastHz_[2] = {-1, -1}, g_[2] = {0, 0};
    Ramp q_, drive_, mix_, out_;
    bool primed_ = false;
    std::atomic<float> pub_[3];
};

// ---- Quarter Inch: tape ----
class QuarterInch : public Plugin {
public:
    static constexpr int kSpeeds = 4;
    static constexpr double kDelaySeconds = 0.002;      // room for wow and flutter to move in; part of the latency
    static constexpr double kMaxWow = 0.006, kMaxFlutter = 0.003;     // peak speed deviation at full depth
    static constexpr double kEmphasisDb = 4.0, kEmphasisHz = 3000.0;
    QuarterInch();
    const char* typeId() const override { return "tape"; }
    const char* displayName() const override { return "Quarter Inch"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return tonefx::kOsLatency + base_; }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // Corner frequencies for a speed setting: high roll-off, head bump, low roll-off.
    static void speedCorners(int speed, double& highHz, double& bumpHz, double& lowHz);
    // The magnetic part. `p` holds three numbers of state. Returns the value that goes into the saturating curve.
    static inline double hysteresis(double u, double amount, double* p) {
        static constexpr double width[3] = {0.02, 0.07, 0.18}, weight[3] = {0.3, 0.4, 0.3};
        double dev = 0.0;
        for (int i = 0; i < 3; ++i) {
            const double lo = u - width[i], hi = u + width[i];
            p[i] = p[i] < lo ? lo : (p[i] > hi ? hi : p[i]);
            dev += weight[i] * (p[i] - u);
        }
        return u + amount * kHystMax * dev;
    }
    static constexpr double kHystMax = 0.6;
    Param *pDrive, *pHyst, *pSpeed, *pBump, *pWow, *pFlutter, *pHiss, *pOut, *pMix;
private:
    struct Chan {
        tonefx::Oversampler os;
        tonefx::AdaaSig sat;
        double p[3] = {0, 0, 0}, uPrev = 0;
        tonefx::BqState pre, de, bump, lp;
        tonefx::Pole hp, hissTone;
    };
    Chan ch_[2];
    tonefx::GlideCoefs preC_, deC_, bumpC_, lpC_;
    std::vector<float> wet_[2], dry_[2];
    int mask_ = 0, w_ = 0, base_ = 96, dryPos_ = 0;
    double sr_ = 48000.0;
    double corner_[3] = {0, 0, 0};          // gliding high, bump and low corner
    float bumpSeen_ = -100.0f;
    double phase_[5] = {};
    dsp::Noise hissL_{1u}, hissR_{2u};
    Ramp drive_, hyst_, wow_, flutter_, hiss_, out_, mix_;
    bool primed_ = false;
};

// ---- Glass: one to three tube stages ----
class Glass : public Plugin {
public:
    static constexpr int kMaxStages = 3;
    static constexpr double kKneeNeg = 0.55;        // the negative side of a stage saturates later and softer
    static constexpr double kSagAttackMs = 40.0, kSagReleaseMs = 300.0, kSagDepth = 0.65;
    Glass();
    const char* typeId() const override { return "tube"; }
    const char* displayName() const override { return "Glass"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return tonefx::kOsLatency; }
    std::vector<Preset> factoryPresets() const override;
    float meter(int i) const override { return i == 0 ? sagPub_.load(std::memory_order_relaxed) : 0.0f; }     // dB the sag is holding the level down by
    Param *pStages, *pDrive, *pBias, *pTone, *pSag, *pOut, *pMix;
private:
    struct Chan {
        tonefx::Oversampler os;
        tonefx::AdaaSig st[kMaxStages];
        tonefx::Pole couple[kMaxStages];
        tonefx::BqState tone;
        tonefx::Pole dc;
    };
    Chan ch_[2];
    const tonefx::OsTables* tab_[kMaxStages + 1] = {};
    tonefx::GlideCoefs toneC_;
    tonefx::LatencyDelay dly_;
    tonefx::ChoiceDip dip_;
    double sr_ = 48000.0;
    float sagEnv_ = 0.0f, sagAtt_ = 1.0f, sagRel_ = 1.0f, toneSeen_ = -100.0f;
    bool toneOn_ = false;
    Ramp drive_, bias_, sag_, out_, mix_;
    bool primed_ = false;
    std::atomic<float> sagPub_{0.0f};
};

// ---- Stack: amplifier and cabinet ----
class Stack : public Plugin {
public:
    enum Voicing { Clean, Crunch, Lead, Bass, NumVoicings };
    enum Cab { CabOff, Cab112, Cab212, Cab412, CabBass15, NumCabs };
    static constexpr int kMaxStages = 3, kCabFilters = 6, kToneFilters = 4;
    static constexpr double kKneeNeg = 0.6, kPowerKneeNeg = 0.85, kPowerBias = 0.1;
    struct Voice {
        int stages;
        float gainLo, gainHi;       // dB of gain in front of the stages at Gain 0 and at Gain 10
        float hpHz;                 // low cut in front of the first stage
        float preHz, preDb, preQ;   // voicing peak in front of the first stage
        float interLp;              // high roll-off between stages
        float bias;
        float bassHz, midHz, midQ, trebleHz, midNoonDb;
        float powerDb;              // how hard the power stage is driven
        float levelDb;              // trim so the voicings come out at similar levels
    };
    static const Voice& voice(int v);
    Stack();
    const char* typeId() const override { return "amp"; }
    const char* displayName() const override { return "Stack"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return tonefx::kOsLatency; }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // Curves as currently set, for drawing and tests (they do their own filter design, so they are for the interface thread).
    double toneStackDb(double hz) const;        // bass, middle, treble and presence
    double cabinetDb(double hz) const;          // the cabinet and microphone
    double responseDb(double hz) const;         // the whole amplifier for a signal too quiet to distort
    Param *pVoicing, *pGain, *pBass, *pMid, *pTreble, *pPresence, *pMaster, *pCab, *pMic, *pMix;
private:
    struct Settings { float bass, mid, treble, presence, mic; };
    Settings settings() const { return {pBass->get(), pMid->get(), pTreble->get(), pPresence->get(), pMic->get()}; }
    static void designInput(int voicing, double sr, dsp::Biquad& hp, dsp::Biquad& pre);
    static void designTone(int voicing, const Settings& s, double fastSr, dsp::Biquad* four);
    static void designCab(int cab, float mic, double sr, dsp::Biquad* six);
    struct Chan {
        tonefx::Oversampler os;
        tonefx::BqState inHp, inPre;
        tonefx::AdaaSig st[kMaxStages], power;
        tonefx::Pole couple[kMaxStages], inter[kMaxStages];
        tonefx::BqState tone[kToneFilters];
        tonefx::BqState cab[kCabFilters];
        tonefx::Pole dc;
    };
    Chan ch_[2];
    const tonefx::OsTables* tab_[kMaxStages + 2] = {};
    tonefx::GlideCoefs inHpC_, inPreC_, toneC_[kToneFilters], cabC_[kCabFilters];
    tonefx::LatencyDelay dly_;
    tonefx::ChoiceDip dip_;
    double sr_ = 48000.0;
    Settings seen_ = {-1, -1, -1, -1, -1};
    Ramp gain_, master_, mix_;
    bool primed_ = false;
};

// ---- Splat: fuzz ----
class Splat : public Plugin {
public:
    // The whole fuzz as one curve of the input after its gain: the bias the asymmetry pushes it off centre by,
    // octave blend, the dead zone that makes it sputter, and the two clipping levels. Every piece is a straight
    // line, so its antiderivative is exact.
    struct Shape {
        double off = 0.0;           // bias: the input is moved this far up before anything else
        double s = 1.0;             // slope the negative half is folded with: 1 - 2 * octave
        double tp = 0.0, tn = 0.0;  // dead zone above and below zero
        double hi = 1.0, lo = 1.0;  // clipping levels
        double f(double x) const;
        double F(double x) const;   // integral of f from 0 to x
    };
    // `level` is the size of the signal after the gain (the peak follower's value): the asymmetry moves the bias
    // by a share of it, so the two halves of the wave are clipped for different lengths of time however hard the
    // fuzz is driven. That, not the two clipping levels, is what brings in the even harmonics.
    static Shape shapeFor(float sputter, float octave, float asym, double level = 0.0);
    static constexpr double kBiasShare = 0.6, kBiasReleaseMs = 60.0;
    Splat();
    const char* typeId() const override { return "fuzz"; }
    const char* displayName() const override { return "Splat"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return tonefx::kOsLatency; }
    std::vector<Preset> factoryPresets() const override;
    static constexpr double kToneLowHz = 720.0, kToneHighHz = 1400.0;
    Param *pFuzz, *pAsym, *pSputter, *pOctave, *pTone, *pLevel, *pMix;
private:
    struct Chan {
        tonefx::Oversampler os;
        double x0 = 0;
        tonefx::Pole dc, lo, hi;
    };
    Chan ch_[2];
    tonefx::LatencyDelay dly_;
    double sr_ = 48000.0;
    double env_ = 0.0, envRel_ = 0.0;       // peak follower on the signal after its gain, for the bias
    Ramp fuzz_, asym_, sputter_, octave_, tone_, level_, mix_;
    bool primed_ = false;
};

// ---- Coarse: bit depth and sample rate reduction ----
class Coarse : public Plugin {
public:
    enum Mode { Hold, HoldFiltered, Interpolated, Filtered, NumModes };
    static constexpr float kBitsOff = 24.0f, kRateOff = 48000.0f;
    Coarse();
    const char* typeId() const override { return "crush"; }
    const char* displayName() const override { return "Coarse"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    std::vector<Preset> factoryPresets() const override;
    static double stepFor(float bits) { return std::exp2(1.0 - (double)bits); }     // the size of one quantisation step
    Param *pBits, *pDither, *pRate, *pMode, *pMix;
private:
    struct Chan {
        tonefx::BqState aa[2], rc[2];
        double held = 0, prev = 0;
    };
    Chan ch_[2];
    tonefx::GlideCoefs lpC_[2];
    tonefx::ChoiceDip dip_;
    dsp::Noise dither_{1u};
    double sr_ = 48000.0, phase_ = 1.0;
    float rateSeen_ = -1.0f;
    Ramp step_, ratio_, mix_;
    bool primed_ = false;
};

// ---- Bend: waveshaper with a drawable curve ----
// The curve is up to 16 points (x and y in -1..1) joined by a smooth line that never overshoots between points.
// It is not a parameter: it travels through saveExtra / loadExtra as {"curve": [[x, y], ...]}.
class Bend : public Plugin {
public:
    static constexpr int kMaxPoints = 16;
    using Points = std::vector<std::pair<float, float>>;
    struct Curve {
        int n = 2;
        double x[kMaxPoints] = {-1, 1}, y[kMaxPoints] = {-1, 1}, m[kMaxPoints] = {1, 1};
        double c2[kMaxPoints] = {}, c3[kMaxPoints] = {};
        unsigned char cell[64] = {};
        double at(double v) const;      // the curve at v; flat beyond its first and last point
    };
    enum Symmetry { Mirror, Full };
    Bend();
    const char* typeId() const override { return "shaper"; }
    const char* displayName() const override { return "Bend"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return tonefx::kOsLatency; }
    std::vector<Preset> factoryPresets() const override;
    Json saveExtra() const override;
    void loadExtra(const Json& j, const std::string& projectDir) override;
    // The curve. Points are sorted, clamped to -1..1 and cut to 16; fewer than two gives the straight line.
    // Call from the interface thread; the audio thread picks the new curve up at its next quantum.
    void setCurve(Points points);
    Points curve() const;
    static Points straightLine() { return {{-1.0f, -1.0f}, {1.0f, 1.0f}}; }
    // What comes out for a sample that went in, between the two gains, as currently set (curve and symmetry).
    double transfer(double x) const;
    Param *pPre, *pPost, *pSymmetry, *pOver, *pMix;
private:
    static std::shared_ptr<const Curve> build(const Points& pts);
    static inline double shape(const Curve& cv, int symmetry, double zero, double v) {
        if (symmetry == Mirror) return v > 0.0 ? cv.at(v) : (v < 0.0 ? -cv.at(-v) : 0.0);
        return cv.at(v) - zero;
    }
    RtShared<Curve> curve_;
    mutable std::mutex m_;
    Points points_;
    tonefx::Oversampler os_[2];
    const tonefx::OsTables* tab_[3] = {};
    tonefx::OsGate gate_;
    tonefx::LatencyDelay dly_;
    tonefx::ChoiceDip dip_;
    int factor_ = 0;
    double sr_ = 48000.0;
    Ramp pre_, post_, mix_;
    bool primed_ = false;
};

// ---- Middle and Sides: level and shelves for the middle and for the sides ----
// Stereo Kit already has width, balance, polarity switches and mono-below; this adds what it lacks.
class MiddleSides : public Plugin {
public:
    static constexpr float kLevelOff = -60.0f;      // the lowest level setting mutes that part
    MiddleSides();
    const char* typeId() const override { return "midside"; }
    const char* displayName() const override { return "Middle and Sides"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    double midDb(double hz) const;      // the two curves as currently set
    double sideDb(double hz) const;
    Param *pMidLevel, *pMidLow, *pMidLowHz, *pMidHigh, *pMidHighHz;
    Param *pSideLevel, *pSideLow, *pSideLowHz, *pSideHigh, *pSideHighHz;
    Param *pWidth, *pBalance, *pOut;
private:
    struct Shelf {
        tonefx::GlideCoefs co;
        tonefx::BqState st;
        float seenDb = 0.0f, seenHz = -1.0f;
        bool active = false;
    };
    void updateShelf(Shelf& s, bool high, float db, float hz, int n);
    Shelf shelf_[4];        // mid low, mid high, side low, side high
    double sr_ = 48000.0;
    Ramp mid_, side_, gl_, gr_;
    bool primed_ = false;
};

} // namespace pulp
