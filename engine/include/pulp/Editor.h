#pragma once
// The audio editor: destructive edits on a piece of audio, and Splice, the effect that sits in a
// mixer slot, records whatever passes through it and holds the audio being edited.
#include "pulp/Audio.h"
#include "pulp/Plugin.h"

namespace pulp::edit {

// Every edit works on frames [a, b) and returns new audio; the old audio is untouched (that is the undo).
std::shared_ptr<SampleData> trim(const SampleData& s, int64 a, int64 b);            // keep only the range
std::shared_ptr<SampleData> cut(const SampleData& s, int64 a, int64 b);             // remove the range
std::shared_ptr<SampleData> silence(const SampleData& s, int64 a, int64 b);
std::shared_ptr<SampleData> fade(const SampleData& s, int64 a, int64 b, bool fadeIn);
std::shared_ptr<SampleData> gain(const SampleData& s, int64 a, int64 b, float g);
std::shared_ptr<SampleData> normalize(const SampleData& s, int64 a, int64 b, float peak = 0.98f);
std::shared_ptr<SampleData> reverse(const SampleData& s, int64 a, int64 b);
std::shared_ptr<SampleData> slice(const SampleData& s, int64 a, int64 b);           // a copy of just the range (for dragging out)

// Noise removal: learn what the noise looks like from a stretch that holds only noise,
// then take that much out of every moment of the range.
struct NoiseProfile {
    int fftSize = 0;
    double sampleRate = 0.0;
    std::vector<float> mag;             // average magnitude per bin
    bool valid() const { return fftSize > 0 && !mag.empty(); }
};
NoiseProfile learnNoise(const SampleData& s, int64 a, int64 b);
std::shared_ptr<SampleData> removeNoise(const SampleData& s, int64 a, int64 b, const NoiseProfile& profile, float amount);    // amount 0..1

} // namespace pulp::edit

namespace pulp {

class Splice : public Plugin {
public:
    Splice();
    const char* typeId() const override { return "editor"; }
    const char* displayName() const override { return "Splice"; }
    void prepare(double sr) override;
    void process(ProcessContext& c) override;      // the sound passes through untouched
    void reset() override {}
    std::vector<Preset> factoryPresets() const override;
    Json saveExtra() const override;
    void loadExtra(const Json& j, const std::string& projectDir) override;
    Param *pRec, *pMode;
    enum Mode { Now, OnPlay, OnInput };

    // ---- control thread only ----
    void pump();                                    // moves what was captured since the last call into the audio; call a few times a second
    SamplePtr audio() const { return audio_; }
    void setAudio(SamplePtr s, bool isEdit);        // isEdit: the old audio goes on the undo stack and the content counts as unsaved
    bool undo();
    bool canUndo() const { return !undo_.empty(); }
    void clear();
    bool unsaved() const { return unsaved_; }       // holds audio that is in no file yet
    std::string file;                               // where the audio was last loaded from or saved to (project-style path)
    void markSaved(const std::string& path) { file = path; unsaved_ = false; }
    int64 selA = 0, selB = 0;                       // selection in frames; equal = nothing selected (edits then apply to everything)
    edit::NoiseProfile noise;
    int64 dropped() const { return dropped_.load(); }
    bool capturing() const { return capturing_.load(std::memory_order_relaxed); }

private:
    static constexpr int kRing = 1 << 17;           // stereo frames (about 2.7 s at 48 kHz)
    std::unique_ptr<float[]> ring_;
    std::atomic<int64> w_{0}, r_{0}, dropped_{0};
    std::atomic<bool> capturing_{false};
    bool latched_ = false;                          // audio thread: "on input" has heard something
    double sr_ = 48000.0;
    SamplePtr audio_;
    std::shared_ptr<SampleData> live_;              // the copy being recorded into, while recording
    std::vector<SamplePtr> undo_;
    bool unsaved_ = false;
};

} // namespace pulp
