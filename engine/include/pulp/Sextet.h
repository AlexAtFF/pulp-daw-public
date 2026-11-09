#pragma once
// SEXTET: a six-operator phase-modulation (FM) synth with a free routing matrix.
//
// Every operator is a sine (or one of a few other shapes) with its own envelope. How much operator j
// modulates operator i is one cell of a 6 x 6 matrix; the diagonal is self-feedback. The Algorithm
// choice replaces the matrix with a ready-made routing, so nobody has to think in matrices.
// The voices run at twice the sample rate and are brought down by a half-band filter with no latency.
#include "pulp/Effects.h"
#include "pulp/Plugin.h"
#include "pulp/SynthParts.h"

namespace pulp {

class Sextet : public Instrument {
public:
    static constexpr int kOps = 6;
    static constexpr int kMaxPoly = 32;
    static constexpr int kSlots = kMaxPoly + 8;     // the extra ones hold voices that are fading out after being stolen

    Sextet();
    const char* typeId() const override { return "fm6"; }
    const char* displayName() const override { return "Sextet"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }

    // The ready-made routings. Algorithm 0 is "Custom matrix"; 1..numAlgorithms() are the fixed ones.
    static int numAlgorithms();
    static const char* algorithmName(int algorithm);
    // Control thread: copies an algorithm's routing into the matrix and output parameters and switches
    // to "Custom matrix", so it can be edited from there. Feedback amounts are left alone.
    void applyAlgorithm(int algorithm);

    struct OpParams {
        Param *wave, *mode, *coarse, *fine, *hz, *detune, *level, *pan, *vel, *kscale,
              *attack, *decay, *sustain, *release, *rks, *fb, *lfo, *out;
    };
    OpParams op[kOps];
    Param* matrix[kOps][kOps];      // matrix[i][j]: how much operator j modulates operator i; matrix[i][i] is op[i].fb
    Param *pAlgo, *pModDepth, *pGuard, *pPEnvAmt, *pPEnvTime, *pGlide;
    Param *pLfoWave[2], *pLfoRate[2], *pLfoDelay[2], *pLfoPitch[2], *pLfoAmp[2];
    Param *pFilter, *pCutoff, *pReso, *pVoices, *pVolume;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    // The parameter values as of the start of the quantum, in the form the voices use.
    struct OpSnap {
        int wave = 0;
        bool fixed = false;
        float ratio = 1.0f;         // frequency ratio with the detune folded in (the detune alone when fixed)
        float hz = 440.0f;
        float level = 0.0f, pan = 0.0f, vel = 0.0f, kscale = 0.0f, ams = 0.0f, out = 0.0f;
        float attack = 1.0f, decay = 600.0f, sustain = 0.7f, release = 200.0f, rks = 0.0f;
        bool modulates = false;     // feeds another operator
    };
    struct LfoSnap { int wave = 0; float rate = 5.0f, delay = 0.0f, pitch = 0.0f, amp = 0.0f; };
    struct Snap {
        OpSnap op[kOps];
        float mat[kOps][kOps] = {};
        int order[kOps] = {0, 1, 2, 3, 4, 5};   // modulators before what they modulate, wherever the routing allows
        float norm = 1.0f, modDepth = 1.0f;
        bool guard = true;
        float penvAmt = 0.0f, penvRate = 0.0f, glideMs = 0.0f;
        LfoSnap lfo[2];
        int voices = 16;
    };
    struct Voice {
        bool active = false, released = false;
        uint32 id = 0, age = 0;
        int key = 60, group = 0;
        float vel2 = 1.0f, pan = 0.0f, fine = 0.0f, modX = 0.5f, modY = 0.5f, relScale = 1.0f;
        float bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;      // semitones; the step is per output sample
        float kill = 1.0f, killStep = 0.0f;
        double time = 0.0;          // output samples since the note began, at the last control point
        int ctlLeft = 0;            // oversampled samples until the next control point
        float lfoPhase[2] = {0.0f, 0.0f}, lfoRnd[2] = {0.0f, 0.0f};
        dsp::Noise rng;
        uint32 phase[kOps] = {}, inc[kOps] = {};
        std::int32_t incStep[kOps] = {};
        parts::Env env[kOps];
        float amp[kOps] = {}, ampStep[kOps] = {};
        float gl[kOps] = {}, glStep[kOps] = {}, gr[kOps] = {}, grStep[kOps] = {};
        float con[kOps][kOps] = {}, conStep[kOps][kOps] = {};       // phase swing in cycles per unit of the source's output
        float y[kOps] = {}, yPrev[kOps] = {};
    };

    void startVoice(const NoteEvent& e);
    void control(Voice& v, bool snap);
    void renderVoice(Voice& v, float* osL, float* osR, int n);

    Snap snap_;
    Voice voices_[kSlots];
    uint32 ageCounter_ = 0;
    double sr_ = 48000.0;
    float hostBend_ = 0.0f, lastKey_ = -1.0f;
    bool primed_ = false;
    parts::Decimator2 dec_[2];
    parts::StereoSvf post_;
    int postMode_ = 0;
    float postG_ = 1.0f, postGStep_ = 0.0f, postK_ = 1.0f, postKStep_ = 0.0f;
    float dcL_ = 0.0f, dcR_ = 0.0f, dcCoef_ = 0.0f;
    Ramp volume_;
    ScopeTap scope_;
    std::atomic<int> voicesActive_{0};
};

} // namespace pulp
