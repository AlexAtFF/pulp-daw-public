#pragma once
// LOW FOUR: an electric bass with four knobs. Each note is a plucked string (the same loop as Twine),
// heard through a pickup, a touch of amplifier drive and a tone control. Body is the weight of the
// low end, Bite the hardness of the attack and the growl, Sustain runs from a short muted thump to a
// long ringing note, and Tone is the tone knob on the instrument. A playing style picks finger, pick,
// slap, muted or fretless.
#include "pulp/BasslineParts.h"

namespace pulp {

class LowFour : public Instrument {
public:
    enum Style { Finger, Pick, Slap, Muted, Fretless, NumStyles };
    static constexpr int kSounding = 8;
    static constexpr int kVoices = kSounding + 4;

    LowFour();
    const char* typeId() const override { return "bass"; }
    const char* displayName() const override { return "Low Four"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    double tailSeconds() const override { return 0.1; }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }

    Param *pStyle, *pBody, *pBite, *pSustain, *pTone, *pOctave, *pVolume;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    struct Voice {
        bool active = false, released = false, retune = true;
        uint32 id = 0, age = 0;
        int key = 60, group = 0, style = 0;
        float fine = 0.0f, relScale = 1.0f, modY = 0.5f;
        float bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;
        float scoop = 0.0f;                 // fretless: the note starts a little flat and comes up
        double semis = -1000.0;
        bl::StringLoop loop;
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
    void updateTone();

    Voice voices_[kVoices];
    bl::StringMemory memory_;
    uint32 ageCounter_ = 0;
    double sr_ = 48000.0;
    float bend_ = 0.0f, dcA_ = 0.0f, scoopCoef_ = 0.0f;
    float seen_[3] = {-1.0f, -1.0f, -100.0f};       // sustain, bite, octave as the strings were last tuned
    float eqSeen_[2] = {-1.0f, -1.0f};
    dsp::Biquad shelf_[2], presence_[2];
    dsp::Svf tone_[2];
    bl::Up2 up_[2];
    bl::Down2 down_[2];
    int tail_ = 0;
    bool primed_ = false;
    Ramp vol_, drive_, makeUp_;
    ScopeTap scope_;
    std::atomic<int> voicesActive_{0};
};

} // namespace pulp
