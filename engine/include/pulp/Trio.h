#pragma once
// TRIO: the simple default synth. Three band-limited oscillators into a low-pass filter
// with its own envelope, then an amplitude envelope.
#include "pulp/Dsp.h"
#include "pulp/Plugin.h"

namespace pulp {

// Shared ADSR used by the native synths. Linear attack, exponential decay and release.
struct Adsr {
    enum Stage { Idle, Attack, Decay, Sustain, Release };
    Stage stage = Idle;
    float value = 0.0f;
    float attackRate = 1.0f, decayCoef = 0.0f, sustain = 1.0f, releaseCoef = 0.0f;
    void set(float attackMs, float decayMs, float sustainLevel, float releaseMs, double sr) {
        attackRate = attackMs <= 0.05f ? 1.0f : (float)(1000.0 / (attackMs * sr));
        decayCoef = (float)std::exp(-1.0 / (0.001 * std::max(1.0f, decayMs) * sr / 4.6));
        releaseCoef = (float)std::exp(-1.0 / (0.001 * std::max(1.0f, releaseMs) * sr / 4.6));
        sustain = sustainLevel;
    }
    void start() { stage = Attack; if (attackRate >= 1.0f) { value = 1.0f; stage = Decay; } }
    void release() { if (stage != Idle) stage = Release; }
    inline float next() {
        switch (stage) {
            case Attack: value += attackRate; if (value >= 1.0f) { value = 1.0f; stage = Decay; } break;
            case Decay: value = sustain + (value - sustain) * decayCoef; break;
            case Release: value *= releaseCoef; if (value < 1.0e-4f) { value = 0.0f; stage = Idle; } break;
            default: break;
        }
        return value;
    }
    bool active() const { return stage != Idle; }
};

// One anti-aliased oscillator (polyBLEP).
struct BlepOsc {
    enum Shape { Sine, Triangle, Square, Saw, SoftSaw, Noise };
    double phase = 0.0;
    uint32 noise = 0x1234567u;
    static inline double blep(double t, double dt) {
        if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
        return 0.0;
    }
    inline float next(int shape, double dt) {
        double v = 0.0;
        const double t = phase;
        switch (shape) {
            case Sine: v = std::sin(kTwoPi * t); break;
            case Triangle: v = t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t; break;
            case Square: {
                v = t < 0.5 ? 1.0 : -1.0;
                v += blep(t, dt);
                double t2 = t + 0.5; if (t2 >= 1.0) t2 -= 1.0;
                v -= blep(t2, dt);
                break;
            }
            case Saw: v = 2.0 * t - 1.0 - blep(t, dt); break;
            // a saw with its fundamental boosted (the sine is in phase with the saw's own fundamental)
            case SoftSaw: { const double s = 2.0 * t - 1.0 - blep(t, dt); v = 0.6 * s - 0.5 * std::sin(kTwoPi * t); break; }
            default:
                noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
                v = (double)noise / 2147483648.0 - 1.0;
                break;
        }
        phase += dt;
        if (phase >= 1.0) phase -= 1.0;
        return (float)v;
    }
};

class Trio : public Instrument {
public:
    Trio();
    const char* typeId() const override { return "trio"; }
    const char* displayName() const override { return "Trio"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    struct Osc { Param *shape, *coarse, *fine, *level, *pan, *phase, *spread; };
    struct Voice {
        bool active = false;
        uint32 id = 0, age = 0;
        int key = 60, group = 0;
        float velGain = 1.0f, panL = 1.0f, panR = 1.0f;
        float fine = 0.0f;
        float bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;
        float modX = 0.5f, modY = 0.5f, relScale = 1.0f;
        BlepOsc osc[3][2];
        Adsr amp, filt;
        dsp::Svf svf[2];
        float kill = 1.0f, killStep = 0.0f;
    };
    static constexpr int kVoices = 24;
    void startVoice(const NoteEvent& e);

    Osc osc_[3];
    Param *pAttack, *pDecay, *pSustain, *pRelease;
    Param *pCutoff, *pReso, *pEnvAmt, *pKeyTrack, *pFAttack, *pFDecay, *pFSustain, *pFRelease;
    Param *pAm, *pVolume, *pVelSens;
    Voice voices_[kVoices];
    uint32 ageCounter_ = 0;
    double sr_ = 48000.0;
    float bend_ = 0.0f;
    std::atomic<int> voicesActive_{0};
};

} // namespace pulp
