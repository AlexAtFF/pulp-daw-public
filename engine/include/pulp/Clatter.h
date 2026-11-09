#pragma once
// CLATTER: a drum synthesizer with eleven models (kick, snare, clap, closed and open hat, tom, cymbal,
// rimshot, cowbell, clave, shaker) built from the same parts: two sines with a pitch envelope, up to six
// band-limited square oscillators, noise, two filters and a handful of envelopes. Six shared controls
// (pitch, tone, noise, decay, snap, colour) mean the sensible thing in each model. The key transposes;
// in Kit mode the key picks the drum instead. Hits overlap freely, except that a closed hat chokes
// an open one.
#include "pulp/BasslineParts.h"

namespace pulp {

class Clatter : public Instrument {
public:
    enum Model { Kick, Snare, Clap, ClosedHat, OpenHat, Tom, Cymbal, Rimshot, Cowbell, Clave, Shaker, Kit, NumModels };
    static constexpr int kSounding = 16;
    static constexpr int kVoices = kSounding + 4;

    Clatter();
    const char* typeId() const override { return "drumsynth"; }
    const char* displayName() const override { return "Clatter"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }
    // Kit mode: the drum a key plays (key % 12) and the semitones it is shifted by within its octave.
    static int kitModel(int key);
    static int kitOffset(int key);

    Param *pModel, *pPitch, *pTone, *pNoise, *pDecay, *pSnap, *pColour, *pVelSens, *pVolume;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    enum FilterMode { Low, Band, High };
    struct Voice {
        bool active = false;
        uint32 id = 0, age = 0;
        int model = 0;
        // two sines with a shared pitch envelope; the first can carry a second and third harmonic
        int nSine = 0;
        double phS[2] = {0.0, 0.0}, dtS[2] = {0.0, 0.0};
        float lvlS[2] = {0.0f, 0.0f}, coefS[2] = {0.0f, 0.0f}, h2 = 0.0f, h3 = 0.0f;
        bl::Fall envS[2];
        float pitchEnv = 0.0f, pitchCoef = 0.0f;
        // square oscillators and noise, into the filters
        int nSq = 0;
        double phQ[6] = {}, dtQ[6] = {};
        float lvlMetal = 0.0f, lvlNoise = 0.0f;
        dsp::Noise noise;
        float grain = 0.0f, grainEnv = 0.0f, grainCoef = 0.0f, grainRate = 0.0f;
        dsp::Svf f1, f2;
        int mode1 = Low, mode2 = High;
        bool use2 = false;
        bl::Fall envN, envN2;
        float coefN = 0.0f, coefN2 = 0.0f, lvl1 = 1.0f, lvl2 = 0.0f;
        float snapEnv = 0.0f, snapCoef = 0.0f;
        float att = 1.0f, attStep = 0.0f;
        // clap: short bursts before the tail
        int burstsLeft = 0, burstGap = 0, burstCount = 0;
        float burstEnv = 0.0f, burstCoef = 0.0f, burstLvl = 0.0f;
        float gain = 1.0f, panL = 1.0f, panR = 1.0f;
        float kill = 1.0f, killStep = 0.0f;
    };
    void startVoice(const NoteEvent& e);
    void setup(Voice& v, int model, double semis, float tone, float noise, float decay, float snap, float colour);

    Voice voices_[kVoices];
    uint32 ageCounter_ = 0;
    double sr_ = 48000.0;
    float bend_ = 0.0f;
    bool primed_ = false;
    Ramp vol_;
    ScopeTap scope_;
    std::atomic<int> voicesActive_{0};
};

} // namespace pulp
