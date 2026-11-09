#pragma once
// The first set of built-in effects. Each sits in a mixer slot, reports its latency for
// delay compensation, has presets, and exposes every control as an automatable parameter.
#include "pulp/Dsp.h"
#include "pulp/Plugin.h"

namespace pulp {

// Lock-free ring of recent output for spectrum displays.
struct ScopeTap {
    static constexpr int kSize = 8192;
    float buf[kSize] = {};
    std::atomic<int> w{0};
    void push(const float* l, const float* r, int n) {
        int i = w.load(std::memory_order_relaxed);
        for (int k = 0; k < n; ++k) { buf[i] = 0.5f * (l[k] + r[k]); i = (i + 1) & (kSize - 1); }
        w.store(i, std::memory_order_relaxed);
    }
    int read(float* dst, int n) const {
        n = std::min(n, (int)kSize);
        const int i = w.load(std::memory_order_relaxed);
        for (int k = 0; k < n; ++k) dst[k] = buf[(i - n + k + kSize * 2) & (kSize - 1)];
        return n;
    }
};

// Helper for preset tables: {name, {{id, value}, ...}} applied over the defaults.
struct PresetValue { const char* id; float v; };
struct PresetDef { const char* name; std::vector<PresetValue> values; };
std::vector<Preset> buildPresets(const Plugin& p, const std::vector<PresetDef>& defs);

// ---- Contour EQ: seven-band parametric equaliser ----
class ContourEq : public Plugin {
public:
    enum Type { Peak, LowShelf, HighShelf, LowCut, HighCut, Notch, NumTypes };
    static constexpr int kBands = 7;
    ContourEq();
    const char* typeId() const override { return "eq7"; }
    const char* displayName() const override { return "Contour EQ"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }
    double magnitudeDb(double hz) const;        // the curve as currently set, for drawing and tests
    Param *pType[kBands], *pFreq[kBands], *pGain[kBands], *pQ[kBands], *pOut;
private:
    void update();
    static void design(dsp::Biquad& b, int type, double hz, double gainDb, double q, double sr);
    dsp::Biquad bq_[kBands][2];
    float seen_[kBands][4];
    bool active_[kBands] = {};
    double sr_ = 48000.0;
    Ramp out_;
    bool primed_ = false;
    ScopeTap scope_;
};

// ---- Squeeze: compressor with a sidechain input ----
class Squeeze : public Plugin {
public:
    Squeeze();
    const char* typeId() const override { return "comp"; }
    const char* displayName() const override { return "Squeeze"; }
    bool wantsSidechain() const override { return true; }
    void prepare(double sr) override { sr_ = sr; reset(); }
    void process(ProcessContext& c) override;
    void reset() override { envDb_ = 0.0f; }
    std::vector<Preset> factoryPresets() const override;
    float meter(int) const override { return grPub_.load(std::memory_order_relaxed); }
    Param *pThresh, *pRatio, *pAttack, *pRelease, *pKnee, *pMakeup, *pSide;
    static float curveDb(float inDb, float thresh, float ratio, float knee);   // static transfer curve: output level for an input level
private:
    double sr_ = 48000.0;
    float envDb_ = 0.0f;        // current gain reduction, positive dB
    std::atomic<float> grPub_{0.0f};
};

// ---- Lid: look-ahead brickwall limiter with a noise gate ----
class Lid : public Plugin {
public:
    Lid();
    const char* typeId() const override { return "limiter"; }
    const char* displayName() const override { return "Lid"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return look_ - 1; }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    // 0: gain reduction in dB (compressor and limiter together), 1: the gate's gain (0..1), 2: the compressor's share in dB
    float meter(int i) const override { return i == 0 ? grPub_.load(std::memory_order_relaxed) : (i == 2 ? compPub_.load(std::memory_order_relaxed) : gatePub_.load(std::memory_order_relaxed)); }
    Param *pGain, *pCeiling, *pRelease, *pGate, *pGateRelease, *pComp, *pCompThresh, *pCompRatio, *pCompKnee, *pCompAttack, *pCompRelease;
private:
    double sr_ = 48000.0;
    int look_ = 240;
    std::vector<float> dl_[2];          // audio delay
    std::vector<long long> dqN_;        // monotonic deque for the sliding minimum: sample numbers
    std::vector<float> dqV_;            // and the gains required at those samples
    int dqHead_ = 0, dqCount_ = 0;
    std::vector<float> avg_;            // moving-average ring
    double avgSum_ = 0.0;
    int w_ = 0;
    long long n_ = 0;
    float env_ = 1.0f, gateEnv_ = 1.0f, gateLevel_ = 0.0f, compDb_ = 0.0f, compDet_ = 0.0f;
    std::atomic<float> grPub_{0.0f}, gatePub_{0.0f}, compPub_{0.0f};
};

// ---- Chamber: algorithmic reverb ----
class Chamber : public Plugin {
public:
    Chamber();
    const char* typeId() const override { return "reverb"; }
    const char* displayName() const override { return "Chamber"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    double tailSeconds() const override { return pDecay->get() + 0.3; }
    std::vector<Preset> factoryPresets() const override;
    Param *pSize, *pDecay, *pDamp, *pPre, *pEarly, *pWidth, *pLowCut, *pMix;
private:
    static constexpr int kCombs = 8, kAllpass = 4, kTaps = 6;
    struct Comb { dsp::DelayLine d; int len = 1; float fb = 0.5f, lp = 0.0f; };
    struct Ap { dsp::DelayLine d; int len = 1; };
    Comb comb_[2][kCombs];
    Ap ap_[2][kAllpass];
    dsp::DelayLine pre_[2];
    dsp::OnePole hp_[2];
    double sr_ = 48000.0;
    float lastSize_ = -1.0f;
    Ramp mix_;
    bool primed_ = false;
};

// ---- Repeater: stereo delay ----
class Repeater : public Plugin {
public:
    Repeater();
    const char* typeId() const override { return "delay"; }
    const char* displayName() const override { return "Repeater"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    double tailSeconds() const override { return 6.0; }
    std::vector<Preset> factoryPresets() const override;
    Param *pSync, *pTime, *pFeedback, *pPing, *pLowCut, *pHighCut, *pModDepth, *pModRate, *pDrive, *pOffset, *pMix;
    double delaySamples(double tempo) const;        // left tap time as currently set
private:
    dsp::DelayLine d_[2];
    dsp::OnePole lp_[2], hp_[2];
    double sr_ = 48000.0, phase_ = 0.0;
    float cur_[2] = {0, 0};
    bool primed_ = false;
    Ramp mix_;
};

// ---- Shimmer: chorus ----
class Shimmer : public Plugin {
public:
    Shimmer();
    const char* typeId() const override { return "chorus"; }
    const char* displayName() const override { return "Shimmer"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    std::vector<Preset> factoryPresets() const override;
    Param *pRate, *pDepth, *pDelay, *pFeedback, *pVoices, *pSpread, *pMix;
private:
    dsp::DelayLine d_[2];
    double sr_ = 48000.0, phase_ = 0.0;
    Ramp mix_;
    bool primed_ = false;
};

// ---- Grit: distortion with 4x oversampling ----
class Grit : public Plugin {
public:
    Grit();
    const char* typeId() const override { return "drive"; }
    const char* displayName() const override { return "Grit"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;
    void reset() override;
    int latencySamples() const override { return kLatency; }
    std::vector<Preset> factoryPresets() const override;
    Param *pDrive, *pType, *pTone, *pBias, *pOut, *pMix;
private:
    // Two 65-tap linear-phase filters at four times the sample rate: 32 + 32 fast samples = 16 samples late.
    static constexpr int kTaps = 65, kOver = 4, kLatency = 16, kUpHist = 17;
    float fir_[kTaps];
    float up_[2][kUpHist] = {}, down_[2][kTaps] = {};
    int upPos_ = 0, downPos_ = 0;
    float dryDl_[2][kLatency] = {};
    int dryPos_ = 0;
    dsp::OnePole tone_[2], dc_[2];
    double sr_ = 48000.0;
    Ramp drive_, out_, mix_;
    bool primed_ = false;
};

// ---- Sweep: multimode filter ----
class Sweep : public Plugin {
public:
    Sweep();
    const char* typeId() const override { return "filter"; }
    const char* displayName() const override { return "Sweep"; }
    void prepare(double sr) override { sr_ = sr; reset(); }
    void process(ProcessContext& c) override;
    void reset() override;
    std::vector<Preset> factoryPresets() const override;
    Param *pType, *pCutoff, *pReso, *pSlope, *pMix;
private:
    dsp::Svf svf_[2][2];
    double sr_ = 48000.0;
    float cut_ = -1.0f;
    Ramp mix_;
    bool primed_ = false;
};

// ---- Stereo Kit: width, balance, gain, phase, mono bass ----
class StereoKit : public Plugin {
public:
    StereoKit();
    const char* typeId() const override { return "stereo"; }
    const char* displayName() const override { return "Stereo Kit"; }
    void prepare(double sr) override { sr_ = sr; reset(); }
    void process(ProcessContext& c) override;
    void reset() override;
    std::vector<Preset> factoryPresets() const override;
    Param *pWidth, *pBalance, *pGain, *pInvL, *pInvR, *pSwap, *pMonoBelow;
private:
    dsp::Biquad hpSide_;        // takes the lows out of the side signal
    float hpSeen_ = -1.0f;
    double sr_ = 48000.0;
    Ramp width_, gL_, gR_;
    bool primed_ = false;
};

} // namespace pulp
