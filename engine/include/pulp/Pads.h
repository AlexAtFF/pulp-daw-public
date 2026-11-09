#pragma once
// Two sample instruments for making beats:
//   Sixteen ("pads")   a drum sampler with sixteen velocity-sensitive pads, up to four layers each
//   Chop ("slicer")    cuts a loop at its hits and puts each slice on its own key
#include "pulp/Audio.h"
#include "pulp/Plugin.h"

namespace pulp {

// ---- Sixteen: pads are keys 60 (C5, pad 1) to 75. Each pad plays one of its layers, chosen by velocity. ----
class PadKit : public Instrument {
public:
    static constexpr int kPads = 16, kLayers = 4, kBaseKey = kRootKey, kGroups = 8;
    PadKit();
    const char* typeId() const override { return "pads"; }
    const char* displayName() const override { return "Sixteen"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    Json saveExtra() const override;
    void loadExtra(const Json& j, const std::string& projectDir) override;
    std::vector<Preset> factoryPresets() const override;      // the factory drum kits
    std::vector<ParamGroup> paramGroups() const override;      // one group per pad
    float meter(int pad) const override;                      // how hard the pad was last hit, fading: for lighting it up

    // Control thread.
    bool setLayer(int pad, int layer, SamplePtr s);           // nullptr removes the layer (later layers move down)
    int addLayer(int pad, SamplePtr s);                       // returns the layer index, or -1 when the pad is full
    void clearPad(int pad);
    void clearAll();
    int layerCount(int pad) const;
    SamplePtr layer(int pad, int layer) const;
    std::string padName(int pad) const;
    void setPadName(int pad, const std::string& name);
    // Fills the pads from a list of audio files, putting kicks, snares, hats and so on where a drummer expects
    // them by the words in their names. Returns how many pads were filled.
    int loadKit(const std::vector<std::string>& files, const std::string& projectDir = {});
    static int padForName(const std::string& fileName);       // -1 when the name says nothing

    Param* pLevel[kPads]; Param* pPan[kPads]; Param* pTune[kPads]; Param* pLength[kPads]; Param* pGroup[kPads];

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    struct Pad { std::vector<SamplePtr> layers; std::string name; };
    struct Kit { Pad pads[kPads]; };
    struct Voice {
        bool active = false;
        uint32 id = 0;
        int pad = 0;
        const SampleData* s = nullptr;
        double pos = 0.0, inc = 1.0;
        int64 last = 0;                 // stop here (the pad's length), fading out before it
        float gainL = 1.0f, gainR = 1.0f;
        float kill = 1.0f, killStep = 0.0f;
        uint32 age = 0;
    };
    static constexpr int kVoices = 32;
    std::shared_ptr<Kit> copyKit() const;
    RtShared<Kit> kit_;
    const Kit* cur_ = nullptr;
    const Kit* last_ = nullptr;
    Voice voices_[kVoices];
    uint32 ageCounter_ = 0;
    double sr_ = 48000.0;
    float bend_ = 0.0f;
    std::atomic<int> voicesActive_{0};
    std::atomic<float> hit_[kPads];
};

// ---- Chop: slices on keys 60 (C5) upward ----
class Chop : public Instrument {
public:
    static constexpr int kBaseKey = kRootKey, kMaxSlices = 64;
    Chop();
    const char* typeId() const override { return "slicer"; }
    const char* displayName() const override { return "Chop"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    Json saveExtra() const override;
    void loadExtra(const Json& j, const std::string& projectDir) override;
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;

    // Control thread.
    void setSample(SamplePtr s);                              // also finds the slices and the length in beats
    SamplePtr sample() const;
    std::vector<int64> slices() const;                        // where each slice starts, in frames, ascending; the first is 0
    void setSlices(std::vector<int64> starts);
    void detect();                                            // find the slices again with the current sensitivity
    void sliceEvenly(int count);                              // equal slices (for loops with no clear hits)
    double beats() const;                                     // the loop's length in beats (whole bars where it can tell)
    void setBeats(double beats);
    // The hits in a recording: frame positions, the first always 0. sensitivity 0..1 (more = more slices).
    static std::vector<int64> findHits(const SampleData& s, float sensitivity, int maxSlices = kMaxSlices);

    Param *pSens, *pMode, *pAttack, *pFade, *pPitch, *pFine, *pReverse, *pGate;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    struct Source { SamplePtr sample; std::vector<int64> starts; double beats = 4.0; };
    struct Voice {
        bool active = false, releasing = false, reverse = false;
        uint32 id = 0;
        int group = 0;
        double pos = 0.0, inc = 1.0;
        int64 first = 0, last = 0;
        float gainL = 1.0f, gainR = 1.0f, env = 0.0f;
        float kill = 1.0f, killStep = 0.0f;
        uint32 age = 0;
    };
    static constexpr int kVoices = 16;
    RtShared<Source> source_;
    const Source* cur_ = nullptr;
    const Source* last_ = nullptr;
    Voice voices_[kVoices];
    uint32 ageCounter_ = 0;
    double sr_ = 48000.0;
    float bend_ = 0.0f;
    std::atomic<int> voicesActive_{0};
};

} // namespace pulp
