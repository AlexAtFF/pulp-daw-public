#pragma once
// The channel sampler: plays one audio file, repitched by key.
#include "pulp/Audio.h"
#include "pulp/Dsp.h"
#include "pulp/Plugin.h"

namespace pulp {

class Sampler : public Instrument {
public:
    Sampler();
    const char* typeId() const override { return "sampler"; }
    const char* displayName() const override { return "Sampler"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    Json saveExtra() const override;
    void loadExtra(const Json& j, const std::string& projectDir) override;
    std::vector<ParamGroup> paramGroups() const override {
        return {{"Sample", {"root", "fine", "keytrack", "reverse", "normalize", "start", "end", "cutself"}},
                {"Envelope", {"env", "attack", "decay", "sustain", "release"}},
                {"Filter", {"cutoff", "reso"}}};
    }

    // Control thread. Passing nullptr clears the sample.
    void setSample(SamplePtr s);
    SamplePtr sample() const;
    std::string samplePath() const;

    Param *pRoot, *pFine, *pKeytrack, *pReverse, *pNormalize, *pStart, *pEnd,
          *pEnvOn, *pAttack, *pDecay, *pSustain, *pRelease, *pCutSelf, *pCutoff, *pReso;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    struct Source { SamplePtr sample; float normGain = 1.0f; };
    struct Voice {
        bool active = false;
        uint32 id = 0;
        int key = 60, group = 0;
        double pos = 0.0;
        int64 first = 0, last = 0;      // playable range, [first, last)
        bool reverse = false;
        float gainL = 1.0f, gainR = 1.0f;
        float keySemis = 0.0f;          // key - root when key tracking is on
        float fineCents = 0.0f;
        float bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;   // portamento and slide notes, in semitones
        int stage = 0;                  // 0 attack, 1 decay/sustain, 3 release, 4 one-shot (no envelope)
        float env = 1.0f;
        float relScale = 1.0f;
        float kill = 1.0f, killStep = 0.0f;     // fast fade when a voice is cut or stolen
        bool filterOn = false;
        dsp::Svf filtL, filtR;
        uint32 age = 0;
    };
    static constexpr int kVoices = 48;
    void startVoice(const NoteEvent& e);
    void renderVoice(Voice& v, float* L, float* R, int n);

    RtShared<Source> source_;
    const Source* cur_ = nullptr;        // audio thread, valid for the current quantum
    const Source* last_ = nullptr;
    Voice voices_[kVoices];
    uint32 ageCounter_ = 0;
    double sr_ = 48000.0;
    float attackRate_ = 1, decayCoef_ = 0, sustain_ = 1, releaseCoef_ = 0;
    float bend_ = 0.0f;
    std::atomic<int> voicesActive_{0};
};

// A silent channel whose notes are passed on to its child channels.
class LayerInstrument : public Instrument {
public:
    const char* typeId() const override { return "layer"; }
    const char* displayName() const override { return "Layer"; }
    void prepare(double) override {}
protected:
    void handleEvent(const NoteEvent&, ProcessContext&) override {}
    void render(float*, float*, int, ProcessContext&) override {}
};

} // namespace pulp
