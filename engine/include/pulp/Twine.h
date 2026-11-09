#pragma once
// TWINE: a plucked-string instrument. Each voice is a delay loop one period long (the Karplus-Strong
// family) with a fractional-delay read, a loss filter, all-pass stiffness and a pluck that is a pulse
// as wide as the pick position, plus a little noise. The loop is tuned analytically, so every key is in
// tune at every sample rate, and its gain is set so the fundamental dies away in the same time on
// every key. A small bank of body resonances and a stereo spread finish it.
#include "pulp/BasslineParts.h"

namespace pulp {

class Twine : public Instrument {
public:
    static constexpr int kSounding = 32;            // notes that can ring at once
    static constexpr int kVoices = kSounding + 4;   // plus spares, so a stolen voice can fade while its successor starts

    Twine();
    const char* typeId() const override { return "pluck"; }
    const char* displayName() const override { return "Twine"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    double tailSeconds() const override { return 0.5; }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }

    Param *pDecay, *pDecayTrack, *pDamping, *pBright, *pPosition, *pStiff, *pNoise, *pMute, *pRelease, *pRing;
    Param *pBody, *pBodySize, *pSpread, *pVelTone, *pTune, *pFine, *pVolume;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    struct Voice {
        bool active = false, released = false, retune = true;
        uint32 id = 0, age = 0;
        int key = 60, group = 0;
        float fine = 0.0f, relScale = 1.0f, modY = 0.5f;
        float bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;
        double semis = -1000.0;
        bl::StringLoop loop;
        // the pluck
        int excPos = 0, excTotal = 0;
        double excLen = 0.0, edge = 0.0;
        float hi = 0.0f, lo = 0.0f, e1 = 0.0f, e2 = 0.0f, ea = 1.0f, excGain = 0.0f, noiseAmt = 0.0f;
        dsp::Noise noise;
        float dc = 0.0f;
        int quiet = 0;
        float panL = 1.0f, panR = 1.0f;
        float kill = 1.0f, killStep = 0.0f;
    };
    void startVoice(const NoteEvent& e);
    void tuneVoice(Voice& v);
    void updateBody();

    Voice voices_[kVoices];
    bl::StringMemory memory_;
    uint32 ageCounter_ = 0;
    double sr_ = 48000.0;
    float bend_ = 0.0f, dcA_ = 0.0f;
    // parameter values the strings were last tuned with
    float seen_[8] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    dsp::Svf body_[3][2];
    float bodySize_ = -1.0f;       // the size the resonators are tuned for; it follows the control over a few quanta
    int tail_ = 0;
    bool primed_ = false;
    Ramp vol_, bodyMix_;
    ScopeTap scope_;
    std::atomic<int> voicesActive_{0};
};

} // namespace pulp
