#pragma once
// ZEST: the flagship subtractive synthesizer.
//
// Two band-limited oscillators (each a unison stack of up to seven voices), a sub oscillator and noise go
// through a mixer into two multimode filters (serial or parallel), then an amplifier. Three envelopes, three
// LFOs and an eight-slot modulation matrix move it; drive, chorus, delay and reverb finish it.
//
// How it is put together:
//   * every quantum is cut into control chunks of at most kCtl (16) samples on a fixed grid; envelopes 2 and 3,
//     the LFOs and the matrix are evaluated once per chunk and everything they drive (oscillator increments,
//     pulse width, gains, filter coefficients, pan) is interpolated per sample inside the chunk;
//   * the two main oscillators are polyBLEP / polyBLAMP oscillators run at twice the sample rate and brought down
//     through a half-band filter, per voice; the filters, the amplifier and the effects run at the sample rate;
//   * parameters are read once per quantum and the continuous ones are ramped across it;
//   * the effects run on the whole quantum once its last sample has been rendered. Nothing adds latency.
#include "pulp/Dsp.h"
#include "pulp/Effects.h"
#include "pulp/Plugin.h"

namespace pulp {

class Zest : public Instrument {
public:
    enum Shape { Sine, Triangle, Saw, Pulse, TwinSaw, NumShapes };
    enum FilterType { LowPass12, LowPass24, BandPass, HighPass, Notch };
    enum LfoShape { LfoSine, LfoTriangle, LfoSaw, LfoSquare, LfoSampleHold };
    enum Source { SrcNone, SrcEnv2, SrcEnv3, SrcLfo1, SrcLfo2, SrcLfo3, SrcVelocity, SrcKey, SrcModX, SrcModY, SrcRandom, NumSources };
    enum Dest {
        DstNone, DstPitch, DstOsc1Pitch, DstOsc2Pitch, DstPulseWidth, DstOscMix, DstNoise, DstCutoff1, DstCutoff2,
        DstResonance, DstBalance, DstAmp, DstPan, DstDetune, DstLfo1Rate, DstLfo2Rate, DstLfo3Rate, NumDests
    };
    enum PlayMode { Poly, Mono, Legato };
    // Note lengths for the tempo-synced LFOs and the delay.
    enum Division { Div8Bars, Div4Bars, Div2Bars, Div1Bar, DivHalfDotted, DivHalf, DivQuarterDotted, DivQuarter, DivEighthDotted,
                    DivQuarterTriplet, DivEighth, DivSixteenthDotted, DivEighthTriplet, DivSixteenth, DivSixteenthTriplet, DivThirtySecond, NumDivisions };
    static double divisionBeats(int division);

    static constexpr int kUnison = 7;       // voices per oscillator
    static constexpr int kSlots = 8;        // modulation matrix slots
    static constexpr int kMaxPoly = 32;     // notes
    static constexpr int kVoices = 48;      // note slots: the polyphony limit plus room for notes that are fading out
    static constexpr int kCtl = 16;         // control chunk, samples
    static constexpr int kHeld = 16;        // held-note stack for the mono modes
    static constexpr int kPink = 7;         // sections of the pink noise filter

    // Building blocks of a voice (public so the file-local DSP routines can work on them).
    struct Env {
        enum Stage { Idle, Attack, Decay, Release };
        int stage = Idle;
        float value = 0.0f;
    };
    struct Filt {
        float ic1[2][2] = {}, ic2[2][2] = {};       // [stage][channel]
        float g = 0, k = 1, k2 = 1, drive = 0, gain = 1;      // values at the end of the last chunk
        double dx[2] = {}, dF[2] = {};              // drive: last input and its antiderivative
        bool driveOn = false;
    };

    Zest();
    const char* typeId() const override { return "zest"; }
    const char* displayName() const override { return "Zest"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    double tailSeconds() const override;
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    // ---- parameters ----
    struct OscP { Param *shape, *coarse, *fine, *level, *pan, *pw, *phase, *voices, *detune, *spread; };
    struct FiltP { Param *type, *cutoff, *reso, *env, *key, *drive; };
    struct EnvP { Param *a, *d, *s, *r; };
    struct LfoP { Param *shape, *sync, *rate, *div, *fade, *mode; };
    struct SlotP { Param *src, *dst, *amt; };
    OscP osc_[2];
    Param* pSync;
    Param *pSubShape, *pSubOct, *pSubLevel, *pNoiseType, *pNoiseLevel;
    FiltP filt_[2];
    Param *pRoute, *pBalance;
    EnvP env_[3];
    LfoP lfo_[3];
    SlotP slot_[kSlots];
    Param *pDrvOn, *pDrvAmt, *pDrvTone;
    Param *pChoOn, *pChoRate, *pChoDepth, *pChoMix;
    Param *pDlyOn, *pDlySync, *pDlyTime, *pDlyDiv, *pDlyFb, *pDlyMix, *pDlyPing;
    Param *pRevOn, *pRevDecay, *pRevDamp, *pRevMix;
    Param *pPoly, *pMode, *pGlide, *pVelSens, *pVelFilt, *pVolume;

    // Continuous values that are ramped across the quantum (one value per control chunk).
    enum Sm {
        SmLevel1, SmLevel2, SmPan1, SmPan2, SmPw1, SmPw2, SmFine1, SmFine2, SmDetune1, SmDetune2, SmSpread1, SmSpread2,
        SmSub, SmNoise, SmCut1, SmCut2, SmReso1, SmReso2, SmEnv1, SmEnv2, SmKey1, SmKey2, SmDrive1, SmDrive2,
        SmBalance, SmBend, SmAmt0, SmAmtLast = SmAmt0 + kSlots - 1, NumSm
    };
    // Everything else, read once per quantum.
    struct Snap {
        int shape[2] = {Saw, Saw}, coarse[2] = {0, 0}, uni[2] = {1, 1};
        bool resetPhase[2] = {false, false}, sync = false;
        int subShape = 0, subOct = 1, noiseType = 0;
        int ftype[2] = {LowPass24, LowPass12};
        bool parallel = false;
        float envA[3] = {}, envD[3] = {}, envS[3] = {}, envRelMs[3] = {};
        int lfoShape[3] = {};
        bool lfoGlobal[3] = {};
        float lfoHz[3] = {}, lfoFade[3] = {};      // rate in Hz (tempo sync resolved), fade-in in samples
        int numSlots = 0, slotIdx[kSlots] = {}, slotSrc[kSlots] = {}, slotDst[kSlots] = {};
        bool modXUsed = false, modYUsed = false;
        int mode = Poly, poly = 16;
        float glideSamples = 0, velSens = 0.7f, velFilt = 0;
    };

    struct Voice {
        bool active = false, first = true;
        uint32 id = 0, age = 0;
        int key = 60, group = 0;
        float velocity = 0.8f, velGain = 1.0f, notePan = 0.0f, fine = 0.0f, modX = 0.5f, modY = 0.5f, relScale = 1.0f, rnd = 0.0f;
        float bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;      // glide and slide notes, in semitones
        // oscillators
        double ph[2][kUnison] = {}, dt[2][kUnison] = {};          // dt is per oversampled sample
        double mph[kUnison] = {}, mdt[kUnison] = {};                // hard sync: the hidden master of each oscillator 2 voice
        float carry[kUnison] = {};
        double syncDc = 0.0;                                        // the mean of the synced waveform, taken out
        float gl[2][kUnison] = {}, gr[2][kUnison] = {}, pw[2] = {0.5f, 0.5f};
        int prevN[2] = {1, 1};
        bool oscOn[2] = {false, false};
        float hbX[2][5] = {}, hbY[2][5] = {};                      // half-band decimator, per channel
        double subPh = 0.0, subDt = 0.0;
        float subGain = 0.0f;
        uint32 noise = 1u;
        float noiseGain = 0.0f, pink[2][kPink] = {}, dark[2] = {};
        // filters and amplifier
        Filt f[2];
        bool f2On = false;
        float w1 = 1.0f, w2 = 0.0f;
        Env env[3];
        float rel[3] = {};
        uint32 envEpoch = 0;
        float outL = 0.0f, outR = 0.0f;
        float kill = 1.0f, killStep = 0.0f;
        // modulation
        double lfoCyc[3] = {};
        float lfoRateMul[3] = {1.0f, 1.0f, 1.0f};
        uint32 lfoSeed = 0;
        float ageSamples = 0.0f;
    };

    // ---- effects ----
    struct FxDrive { double x1[2] = {}, F1[2] = {}; float lp[2] = {}; Ramp wet, gain; bool running = false; };
    struct FxChorus { dsp::DelayLine d[2]; double phase = 0.0; Ramp wet, depth; bool running = false; };
    struct FxDelay {
        std::vector<float> buf[2];
        int mask = 0, w = 0;
        int64 valid = 0;                // samples written since the delay was last cleared
        float time = 0.0f;              // smoothed delay, samples
        float lp[2] = {}, hp[2] = {};
        Ramp wet, fb;
        bool running = false;
    };
    struct FxReverb {
        dsp::DelayLine line[8], ap[2][4];
        int len[8] = {}, apLen[2][4] = {};
        float lp[8] = {};
        Ramp wet;
        bool running = false;
    };

    void initVoice(Voice& v, const NoteEvent& e);
    void retarget(Voice& v, const NoteEvent& e, bool retrigger, bool glide);
    Voice* allocVoice();
    void noteOn(const NoteEvent& e);
    void noteOff(uint32 id);
    void releaseVoice(Voice& v);
    void setNoteValues(Voice& v, const NoteEvent& e);
    void renderChunk(int pos, int m);
    void renderVoice(Voice& v, float* outL, float* outR, int m);
    void finishQuantum(ProcessContext& ctx);
    void clearEffects();
    double fxTailSeconds() const;

    Snap q_;
    float smStart_[NumSm] = {}, smTarget_[NumSm] = {}, cv_[NumSm] = {};
    bool primed_ = false;
    float uniDet_[kUnison + 1][kUnison] = {}, uniPan_[kUnison + 1][kUnison] = {}, uniPhase_[kUnison + 1][kUnison] = {};
    const float* sinTab_ = nullptr;

    Voice voices_[kVoices];
    NoteEvent held_[kHeld];
    int heldCount_ = 0, monoVoice_ = -1, lastMode_ = Poly;
    float lastKey_ = -1.0f;
    uint32 ageCounter_ = 0, rng_ = 0x2F6E2B1u, envEpoch_ = 1;
    float envSeen_[9] = {};

    double sr_ = 48000.0;
    std::atomic<double> tempo_{140.0};          // also read by tailSeconds() from another thread
    bool wasPlaying_ = false, tickJumped_ = false;
    double expectedTick_ = 0.0;
    double glfoCyc_[3] = {}, glfoInc_[3] = {};
    float glfo_[3] = {};
    float darkCoef_ = 0.1f, darkGain_ = 4.0f, pinkCoef_[kPink] = {}, pinkGain_[kPink] = {}, pinkWhite_ = 0.0f;

    int quantumPos_ = 0;
    float mixL_[kQuantum] = {}, mixR_[kQuantum] = {};
    FxDrive drive_;
    FxChorus chorus_;
    FxDelay delay_;
    FxReverb reverb_;
    Ramp volume_;
    int64 idleSamples_ = 0;
    bool fxAsleep_ = true, fxWake_ = false, outPrimed_ = false;
    ScopeTap scope_;
    std::atomic<int> voicesActive_{0};
};

} // namespace pulp
