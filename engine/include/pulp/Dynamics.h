#pragma once
// Dynamics effects: Triband (three-band compressor and maximizer), Snap (transient shaper),
// Curb (soft clipper), Glow (one-knob enhancer), Sibilant (de-esser) and Doorman (gate / expander).
// Squeeze, Lid and Grit in Effects.h are the plain compressor, limiter and distortion; nothing here repeats them.
#include "pulp/Effects.h"

namespace pulp {

namespace dyn {

inline float timeCoef(double ms, double sr) { return ms <= 0.0 ? 1.0f : (float)(1.0 - std::exp(-1.0 / (0.001 * ms * sr))); }
inline float levelDb(float lin) { return lin > 1.0e-7f ? 20.0f * std::log10(lin) : -140.0f; }
inline float dbGain(float db) { return std::exp(db * 0.11512925464970229f); }      // 10^(db/20); exactly 1 at 0 dB
inline void flush(dsp::Biquad& b) {
    if (std::fabs(b.z1) < 1.0e-20) b.z1 = 0.0;
    if (std::fabs(b.z2) < 1.0e-20) b.z2 = 0.0;
}

// The largest of the last `win` values, in constant time per sample (monotonic queue).
struct SlidingMax {
    std::vector<float> v;
    std::vector<long long> t;
    int head = 0, count = 0, cap = 1, win = 1;
    long long n = 0;
    void prepare(int window) {
        win = std::max(1, window);
        cap = win + 2;
        v.assign((size_t)cap, 0.0f);
        t.assign((size_t)cap, 0);
        reset();
    }
    void reset() { head = count = 0; n = 0; }
    inline float push(float x) {
        while (count > 0 && v[(size_t)((head + count - 1) % cap)] <= x) --count;
        const int tail = (head + count) % cap;
        v[(size_t)tail] = x;
        t[(size_t)tail] = n;
        ++count;
        while (t[(size_t)head] <= n - win) { head = (head + 1) % cap; --count; }
        ++n;
        return v[(size_t)head];
    }
};

// Look-ahead peak limiter stage (stereo linked): the gain needed by the loudest sample in the window is held,
// released exponentially and then averaged over the window, so it has fully arrived when the sample comes out.
struct Limiter {
    SlidingMax peak;
    std::vector<float> dl[2], avg;
    int look = 1, w = 0;
    float env = 1.0f;
    double avgSum = 1.0;
    void prepare(int latency);
    void reset();
    int latency() const { return look - 1; }
    void beginBlock() { avgSum = 0.0; for (float a : avg) avgSum += a; }      // rebuilt so rounding cannot build up
    // l and r go in and come back delayed by latency(); the return value is the gain to apply to them.
    inline float process(float& l, float& r, float ceiling, float rel) {
        const float pk = peak.push(std::max(std::fabs(l), std::fabs(r)));
        const float hold = pk > ceiling ? ceiling / pk : 1.0f;
        env = std::min(hold, env + (1.0f - env) * rel);
        avgSum += (double)env - (double)avg[(size_t)w];
        avg[(size_t)w] = env;
        const float g = (float)std::min(1.0, avgSum / (double)look);
        dl[0][(size_t)w] = l;
        dl[1][(size_t)w] = r;
        w = (w + 1) % look;
        l = dl[0][(size_t)w];
        r = dl[1][(size_t)w];
        return g;
    }
};

// Three-way Linkwitz-Riley split, 24 dB per octave. The low band goes through the all-pass that the
// upper split amounts to, so low + mid + high is an all-pass of the input (flat magnitude).
struct Split3 {
    dsp::Biquad lp1a, lp1b, hp1a, hp1b, lp2a, lp2b, hp2a, hp2b, ap;
    void set(double fLow, double fHigh, double sr);
    void reset();
    void flushAll();
    inline void process(float x, float& lo, float& mid, float& hi) {
        const float l = lp1b.process(lp1a.process(x));
        const float h = hp1b.process(hp1a.process(x));
        lo = ap.process(l);
        mid = lp2b.process(lp2a.process(h));
        hi = hp2b.process(hp2a.process(h));
    }
};

// Four times oversampling for a memoryless non-linearity: a 129-tap Kaiser low-pass at the original Nyquist
// frequency on the way up and again on the way down. 64 + 64 fast samples = 32 samples late.
struct Over4 {
    static constexpr int kTaps = 129, kLatency = 32, kUp = 33, kDown = 132;
    struct Tables { float fir[kTaps]; float phase[4][kUp]; };
    static const Tables& tables();
    Over4() : t_(&tables()) {}
    void reset();
    // one input sample in, the four samples at the fast rate out
    inline void up(float x, float out[4]) {
        upPos_ = upPos_ == 0 ? kUp - 1 : upPos_ - 1;
        up_[upPos_] = up_[upPos_ + kUp] = x;
        const float* h = up_ + upPos_;
        for (int p = 0; p < 4; ++p) {
            const float* c = t_->phase[p];
            float s = 0.0f;
            for (int k = 0; k < kUp; ++k) s += c[k] * h[k];
            out[p] = s;
        }
    }
    // four fast samples in, one filtered sample out
    inline float down(const float in[4]) {
        downPos_ = downPos_ == 0 ? kDown - 1 : downPos_ - 1;
        down_[downPos_] = down_[downPos_ + kDown] = in[0];
        const float* h = down_ + downPos_;
        const float* c = t_->fir;
        float s = 0.0f;
        for (int j = 0; j < kTaps; ++j) s += c[j] * h[j];
        for (int p = 1; p < 4; ++p) {
            downPos_ = downPos_ == 0 ? kDown - 1 : downPos_ - 1;
            down_[downPos_] = down_[downPos_ + kDown] = in[p];
        }
        return s;
    }
    // the same delay for a signal that does not go through the filters
    inline float delay(float x) {
        const float y = dry_[dryPos_];
        dry_[dryPos_] = x;
        dryPos_ = (dryPos_ + 1) % kLatency;
        return y;
    }
private:
    const Tables* t_;
    float up_[2 * kUp] = {}, down_[2 * kDown] = {}, dry_[kLatency] = {};
    int upPos_ = 0, downPos_ = 0, dryPos_ = 0;
};

} // namespace dyn

// ---- Triband: three-band compressor / expander with a maximizer ----
class Triband : public Plugin {
public:
    static constexpr int kBands = 3;
    static constexpr float kMaxBoostDb = 12.0f;     // the most an upward-expanding band may add
    Triband();
    const char* typeId() const override { return "multiband"; }
    const char* displayName() const override { return "Triband"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return lim_.latency(); }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // 0..2: gain reduction of the low, mid and high band in dB (negative while a band expands); 3: the limiter
    float meter(int i) const override { return i >= 0 && i < 4 ? gr_[i].load(std::memory_order_relaxed) : 0.0f; }
    double crossoverLow() const { return pXLow->get(); }
    double crossoverHigh() const { return std::max(pXHigh->get(), pXLow->get() * 1.5f); }   // the bands never cross over each other
    Param *pXLow, *pXHigh;
    Param *pThresh[kBands], *pRatio[kBands], *pAttack[kBands], *pRelease[kBands], *pKnee[kBands], *pMakeup[kBands], *pMute[kBands], *pSolo[kBands];
    Param *pMaximize, *pCeiling, *pLimRelease, *pLimit;
private:
    dyn::Split3 split_[2];
    dyn::Limiter lim_;
    double sr_ = 48000.0;
    float seenLo_ = -1.0f, seenHi_ = -1.0f;
    float env_[kBands] = {};
    Ramp band_[kBands], max_, ceil_, limMix_;
    bool primed_ = false;
    std::atomic<float> gr_[4];
};

// ---- Snap: transient shaper ----
class Snap : public Plugin {
public:
    Snap();
    const char* typeId() const override { return "transient"; }
    const char* displayName() const override { return "Snap"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    std::vector<Preset> factoryPresets() const override;
    // 0: gain taken away right now in dB, 1: gain added right now in dB
    float meter(int i) const override { return i == 0 ? cutPub_.load(std::memory_order_relaxed) : (i == 1 ? boostPub_.load(std::memory_order_relaxed) : 0.0f); }
    Param *pAttack, *pSustain, *pOut, *pMix;
private:
    dyn::SlidingMax peak_;
    double sr_ = 48000.0;
    float fa_ = 0, sa_ = 0, fs_ = 0, ss_ = 0;       // fast and slow envelopes for the attack and the sustain side
    float cFa_ = 1, cSa_ = 1, cFs_ = 1, cSs_ = 1;
    Ramp att_, sus_, out_, mix_;
    bool primed_ = false;
    std::atomic<float> cutPub_{0.0f}, boostPub_{0.0f};
};

// ---- Curb: soft clipper with 4x oversampling ----
// The clipper runs at four times the rate. Filtering back down lets the signal ring a little past the threshold,
// so a very short look-ahead limiter (a third of a millisecond) takes that overshoot off again without folding
// anything back into the audible band; a plain clamp behind it only ever catches rounding.
class Curb : public Plugin {
public:
    Curb();
    const char* typeId() const override { return "clip"; }
    const char* displayName() const override { return "Curb"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return dyn::Over4::kLatency + lim_.latency(); }
    std::vector<Preset> factoryPresets() const override;
    float meter(int i) const override { return i == 0 ? grPub_.load(std::memory_order_relaxed) : 0.0f; }   // dB the loudest sample was over the threshold
    // The static curve: output for an input sample, threshold and knee (0 hard .. 1 fully soft) as linear values.
    static float shape(float x, float threshold, float knee);
    Param *pDrive, *pThresh, *pKnee, *pOver, *pOut, *pMatch;
private:
    dyn::Over4 os_[2];
    dyn::Limiter lim_;
    float limRel_ = 0.0f;
    float naive_[2][dyn::Over4::kLatency] = {};
    int naivePos_ = 0;
    double sr_ = 48000.0;
    Ramp drive_, thr_, knee_, out_, osMix_;
    bool primed_ = false;
    std::atomic<float> grPub_{0.0f};
};

// ---- Glow: one-knob enhancer ----
class Glow : public Plugin {
public:
    Glow();
    const char* typeId() const override { return "enhance"; }
    const char* displayName() const override { return "Glow"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return look_ + dyn::Over4::kLatency + lim_.latency(); }
    std::vector<Preset> factoryPresets() const override;
    // 0: limiter gain reduction in dB, 1..3: compression of the low, mid and high band in dB
    float meter(int i) const override { return i >= 0 && i < 4 ? grPub_[i].load(std::memory_order_relaxed) : 0.0f; }
    Param *pAmount, *pCharacter, *pOut;
private:
    dsp::Biquad lp_[2], hp_[2];         // the low and the high band; the middle is what is left
    dsp::Biquad lpDet_[2], hpDet_[2];   // the same split on the signal before the look-ahead delay, for the detectors
    dsp::DelayLine ahead_[2];
    int look_ = 72;                     // 1.5 ms: the compressors see a peak coming and are down by the time it arrives
    dyn::Over4 os_[2];
    dyn::Limiter lim_;
    double sr_ = 48000.0;
    float env_[3] = {};
    float att_[3] = {}, rel_[3] = {};
    float limRel_ = 0.0f;
    Ramp shelf_[3], makeup_, wet_, out_;
    bool primed_ = false;
    std::atomic<float> grPub_[4];
};

// ---- Sibilant: de-esser ----
class Sibilant : public Plugin {
public:
    Sibilant();
    const char* typeId() const override { return "deess"; }
    const char* displayName() const override { return "Sibilant"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    std::vector<Preset> factoryPresets() const override;
    float meter(int i) const override { return i == 0 ? grPub_.load(std::memory_order_relaxed) : 0.0f; }
    Param *pFreq, *pWidth, *pThresh, *pRange, *pRelease, *pMode, *pListen;
private:
    void update();
    dsp::Biquad detHp_[2][2], detLp_[2][2], bell_[2];      // detector: 24 dB per octave either side of the band
    double sr_ = 48000.0;
    float seenF_ = -1.0f, seenW_ = -1.0f;
    float pk_ = 0.0f, envDb_ = 0.0f, pkDecay_ = 0.0f, att_ = 1.0f;
    Ramp wide_, listen_;
    bool primed_ = false;
    std::atomic<float> grPub_{0.0f};
};

// ---- Doorman: noise gate / expander with look-ahead and a sidechain ----
class Doorman : public Plugin {
public:
    Doorman();
    const char* typeId() const override { return "gate"; }
    const char* displayName() const override { return "Doorman"; }
    bool wantsSidechain() const override { return true; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return lat_; }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // 0: attenuation right now in dB, 1: 1 while the gate is open
    float meter(int i) const override { return i == 0 ? grPub_.load(std::memory_order_relaxed) : (i == 1 ? openPub_.load(std::memory_order_relaxed) : 0.0f); }
    static constexpr double kMaxLookMs = 5.0;       // the latency is always this, whatever the look-ahead control says
    static constexpr double kDetectorDecayMs = 3.0; // the level detector falls this fast between peaks
    Param *pThresh, *pHyst, *pRange, *pAttack, *pHold, *pRelease, *pLook, *pSide, *pScHp, *pScLp, *pListen;
private:
    dsp::Biquad hp_[2], lp_[2];
    dsp::DelayLine audio_[2], gain_;
    double sr_ = 48000.0;
    int lat_ = 240;
    float seenHp_ = -1.0f, seenLp_ = -1.0f;
    float env_ = 0.0f, envDecay_ = 0.0f;
    bool open_ = false;
    int holdLeft_ = 0;
    double gDb_ = 0.0, gLin_ = 1.0;
    Ramp gainDelay_, listen_;
    bool primed_ = false;
    std::atomic<float> grPub_{0.0f}, openPub_{0.0f};
};

} // namespace pulp
