#pragma once
// SQUELCH: a monophonic acid bass line synth. One band-limited saw or square oscillator into a
// four-pole diode-ladder low-pass with its own decay envelope, then a drive stage. Notes that overlap
// slide instead of retriggering, and loud notes are accented. Everything non-linear runs at twice the
// sample rate.
#include "pulp/BasslineParts.h"

namespace pulp {

class Squelch : public Instrument {
public:
    static constexpr float kAccentVelocity = 110.5f / 127.0f;      // a note above this is accented

    Squelch();
    const char* typeId() const override { return "acid"; }
    const char* displayName() const override { return "Squelch"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voices_.load(std::memory_order_relaxed); }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }
    // 0: where the filter is right now, in Hz. 1: the accent charge (0..1). 2: the filter envelope (0..1).
    float meter(int index) const override;

    Param *pWave, *pTune, *pFine, *pCutoff, *pReso, *pEnvMod, *pDecay, *pAccent, *pSlide, *pDrive, *pAmpDecay, *pVolume;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    enum AmpStage { Idle, Attack, Decay, Release };
    void trigger(const NoteEvent& e, bool legato);
    void gateOff();
    void goIdle();
    void hold(uint32 id);
    bool letGo(uint32 id);          // true when nothing is held any more

    double sr_ = 48000.0, fs2_ = 96000.0;
    bool active_ = false, gate_ = false, primed_ = false;
    int64 clock_ = 0, releasedAt_ = -1;
    uint32 held_[8] = {};
    int numHeld_ = 0;
    uint32 currentId_ = 0;
    int group_ = 0;

    // pitch
    double key_ = 60.0, keyTarget_ = 60.0, phase_ = 0.0, dt_ = 0.0;
    float bend_ = 0.0f;
    // envelopes
    int ampStage_ = Idle;
    float amp_ = 0.0f, relCoef_ = 0.0f;
    bool fenvRising_ = false, accented_ = false;
    float fenv_ = 0.0f, accCap_ = 0.0f;
    float velGain_ = 1.0f, velTarget_ = 1.0f, accGain_ = 1.0f;
    float kill_ = 1.0f;
    int killDir_ = 0;               // -1 fading out, +1 coming back
    float modX_ = 0.5f, modY_ = 0.5f;
    float panL_ = 1.0f, panR_ = 1.0f, panLT_ = 1.0f, panRT_ = 1.0f;
    // filter
    double g_ = 0.0, k_ = 0.0, drive_ = 1.0, driveNorm_ = 1.0, hpG_ = 0.0;
    double s1_ = 0.0, s2_ = 0.0, s3_ = 0.0, s4_ = 0.0, sh_ = 0.0;
    double driveX_ = 0.0, driveF_ = 0.0, dc_ = 0.0, dcA_ = 0.0;
    bl::HalfBand pre_;
    bl::Down2 down_;
    Ramp vol_;
    ScopeTap scope_;
    std::atomic<int> voices_{0};
    std::atomic<float> meterCutoff_{0.0f}, meterAccent_{0.0f}, meterEnv_{0.0f};
};

} // namespace pulp
