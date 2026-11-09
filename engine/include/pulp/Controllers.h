#pragma once
// Internal controllers: effects that sit in a mixer slot, let the sound through and put out a
// control value (0..1) that any parameter can be linked to.
//   Wobble    an LFO                          Follower  an envelope follower with a sidechain input
//   Pad XY    two values set by hand          Formula   a typed expression of time, tempo and three knobs
#include "pulp/Dsp.h"
#include "pulp/Effects.h"
#include "pulp/Formula.h"

namespace pulp {

class Wobble : public Plugin {
public:
    Wobble();
    const char* typeId() const override { return "ctl_lfo"; }
    const char* displayName() const override { return "Wobble"; }
    void prepare(double sr) override { sr_ = sr; reset(); }
    void process(ProcessContext& c) override;
    void reset() override { free_ = 0.0; out_.store(pBase->get()); }
    int numControlOutputs() const override { return 1; }
    float controlOutput(int) const override { return out_.load(std::memory_order_relaxed); }
    const char* controlOutputName(int) const override { return "LFO"; }
    float meter(int) const override { return out_.load(std::memory_order_relaxed); }
    std::vector<Preset> factoryPresets() const override;
    Param *pShape, *pSync, *pRate, *pSteps, *pBase, *pDepth, *pPhase;
    static float wave(int shape, double phase);        // -1..1, phase in cycles
private:
    double sr_ = 48000.0, free_ = 0.0;      // free_ counts cycles while the song is stopped
    std::atomic<float> out_{0.5f};
};

class Follower : public Plugin {
public:
    Follower();
    const char* typeId() const override { return "ctl_peak"; }
    const char* displayName() const override { return "Follower"; }
    bool wantsSidechain() const override { return true; }
    void prepare(double sr) override { sr_ = sr; reset(); }
    void process(ProcessContext& c) override;
    void reset() override { env_ = 0.0f; out_.store(clampT(pBase->get(), 0.0f, 1.0f)); }
    int numControlOutputs() const override { return 1; }
    float controlOutput(int) const override { return out_.load(std::memory_order_relaxed); }
    const char* controlOutputName(int) const override { return "Peak"; }
    float meter(int) const override { return out_.load(std::memory_order_relaxed); }
    std::vector<Preset> factoryPresets() const override;
    Param *pBase, *pAmount, *pAttack, *pRelease, *pSource, *pMute;
private:
    double sr_ = 48000.0;
    float env_ = 0.0f;
    std::atomic<float> out_{0.0f};
};

class PadXy : public Plugin {
public:
    PadXy();
    const char* typeId() const override { return "ctl_xy"; }
    const char* displayName() const override { return "Pad XY"; }
    void prepare(double sr) override { sr_ = sr; reset(); }
    void process(ProcessContext& c) override;
    void reset() override { primed_ = false; }
    int numControlOutputs() const override { return 2; }
    float controlOutput(int i) const override { return out_[i ? 1 : 0].load(std::memory_order_relaxed); }
    const char* controlOutputName(int i) const override { return i ? "Y" : "X"; }
    float meter(int i) const override { return controlOutput(i); }
    std::vector<Preset> factoryPresets() const override;
    Param *pX, *pY, *pSmooth;
private:
    double sr_ = 48000.0;
    float cur_[2] = {0.5f, 0.5f};
    bool primed_ = false;
    std::atomic<float> out_[2];
};

class FormulaCtl : public Plugin {
public:
    FormulaCtl();
    const char* typeId() const override { return "ctl_formula"; }
    const char* displayName() const override { return "Formula"; }
    void prepare(double sr) override { sr_ = sr; reset(); }
    void process(ProcessContext& c) override;
    void reset() override { free_ = 0.0; }
    int numControlOutputs() const override { return 1; }
    float controlOutput(int) const override { return out_.load(std::memory_order_relaxed); }
    const char* controlOutputName(int) const override { return "Value"; }
    float meter(int) const override { return out_.load(std::memory_order_relaxed); }
    Json saveExtra() const override;
    void loadExtra(const Json& j, const std::string& projectDir) override;
    std::vector<Preset> factoryPresets() const override;
    Param *pA, *pB, *pC;
    // The expression. Variables: a, b, c (the knobs), t (seconds), beat (beats since the start), tempo.
    bool setFormula(const std::string& text, std::string* error = nullptr);    // control thread; a bad formula is refused and the old one stays
    const std::string& formula() const { return text_; }
    static std::vector<std::string> variables() { return {"a", "b", "c", "t", "beat", "tempo"}; }
private:
    double sr_ = 48000.0, free_ = 0.0;      // seconds counted while the song is stopped
    std::string text_;
    RtShared<Formula> compiled_;
    std::atomic<float> out_{0.0f};
};

} // namespace pulp
