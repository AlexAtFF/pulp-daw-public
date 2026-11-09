#pragma once
// THUD: a kick drum synthesizer. A sine whose pitch sweeps from a start to an end frequency along a
// chosen curve, an amplitude decay, a click, a drive stage (run at twice the rate), a clean copy of
// the sine underneath as the sub, and a tone filter. The key transposes it: key 60 plays the stated
// frequencies. A new hit chokes the tail of the last one over a few milliseconds, so retriggering
// never clicks.
#include "pulp/BasslineParts.h"

namespace pulp {

class Thud : public Instrument {
public:
    static constexpr int kSounding = 6;
    static constexpr int kVoices = kSounding + 4;

    Thud();
    const char* typeId() const override { return "kick"; }
    const char* displayName() const override { return "Thud"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }
    // The frequency the sweep is at, t seconds into a hit at key 60, with the present settings.
    double sweepHz(double seconds) const;

    Param *pStart, *pEnd, *pSweep, *pCurve, *pDecay, *pClick, *pClickTone, *pDrive, *pSub, *pTone;
    Param *pTune, *pFine, *pKeyTrack, *pChoke, *pGate, *pVelSens, *pVolume;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    struct Voice {
        bool active = false, released = false;
        uint32 id = 0, age = 0;
        int key = 60, group = 0;
        double t = 0.0, phase = 0.0;
        bl::Fall half;                      // the square root of the amplitude: half before the drive, half after
        float fine = 0.0f, velGain = 1.0f, panL = 1.0f, panR = 1.0f, modY = 0.5f, relScale = 1.0f;
        float bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;
        float g = 1.0f, norm = 1.0f;        // drive gain, smoothed, and the make-up that goes with it
        dsp::Noise noise;
        dsp::Svf clickFilter;
        float clickEnv = 0.0f;
        int clickLeft = 0;                  // samples the click filter still runs once the burst has ended
        float kill = 1.0f, killStep = 0.0f;
    };
    void startVoice(const NoteEvent& e);

    Voice voices_[kVoices];
    uint32 ageCounter_ = 0;
    double sr_ = 48000.0;
    float bend_ = 0.0f, modX_ = 0.5f;
    bl::Down2 down_[2];
    dsp::Svf tone_[2];
    int tail_ = 0;
    bool primed_ = false;
    Ramp vol_;
    ScopeTap scope_;
    std::atomic<int> voicesActive_{0};
};

} // namespace pulp
