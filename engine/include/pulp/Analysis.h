#pragma once
// Gauge: an effect that changes nothing and measures what passes through it. The program draws
// its displays (spectrum, spectrogram, oscilloscope, vectorscope, loudness, tuner) from what it exposes.
#include "pulp/Plugin.h"

namespace pulp {

class Gauge : public Plugin {
public:
    enum View { Spectrum, Spectrogram, Oscilloscope, Vectorscope, Loudness, Tuner, NumViews };
    enum Meter { PeakL, PeakR, Rms, Momentary, ShortTerm, Correlation, NumMeters };
    static constexpr int kRing = 16384;
    Gauge();
    const char* typeId() const override { return "gauge"; }
    const char* displayName() const override { return "Gauge"; }
    void prepare(double sampleRate) override;
    void process(ProcessContext& ctx) override;
    void reset() override;
    std::vector<Preset> factoryPresets() const override;
    // Levels in dB (peaks and RMS in dBFS, loudness in LUFS; -120 means silence); correlation is -1..1.
    float meter(int index) const override;
    int readScope(float* dst, int n) const override;               // the latest mono samples
    int readStereo(float* left, float* right, int n) const;        // the latest n samples of each side
    // Loudness of everything since playback last started (EBU R 128 / ITU-R BS.1770 gating). -120 when there
    // has not been enough sound to say. Safe to call from the interface thread.
    double integratedLufs() const;
    void resetIntegrated();
    Param *pView, *pRange, *pHold;

private:
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
    static constexpr int kMaxSegments = 36000;          // 100 ms each: one hour
    double sr_ = 48000.0;
    Biquad shelf_[2], highpass_[2];
    std::unique_ptr<float[]> ringL_, ringR_;
    std::atomic<int> write_{0};
    // loudness: mean squares of 100 ms stretches of the weighted signal
    std::unique_ptr<float[]> segments_;
    std::atomic<int> numSegments_{0};
    double segSum_ = 0.0;
    int segFill_ = 0, segLen_ = 4800;
    bool wasPlaying_ = false;
    std::atomic<bool> resetAsked_{false};
    double peakL_ = 0.0, peakR_ = 0.0, ms_ = 0.0, lr_ = 0.0, ll_ = 0.0, rr_ = 0.0;
    int holdL_ = 0, holdR_ = 0;
    std::atomic<float> meters_[NumMeters];
};

} // namespace pulp
