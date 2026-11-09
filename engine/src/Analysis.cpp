#include "pulp/Analysis.h"
#include "pulp/Effects.h"
#include <algorithm>
#include <cmath>

namespace pulp {

namespace {
inline float toDb(double power) { return power <= 1.0e-12 ? -120.0f : (float)(10.0 * std::log10(power)); }
}

Gauge::Gauge() {
    pView = addChoice("view", "Display", {"Spectrum", "Spectrogram", "Oscilloscope", "Vectorscope", "Loudness", "Tuner"}, 0);
    pRange = addFloat("range", "Range shown", 30, 120, 84, "dB");
    pHold = addBool("hold", "Hold peaks", false);
    for (auto& m : meters_) m.store(-120.0f);
    meters_[Correlation].store(0.0f);
}

std::vector<Preset> Gauge::factoryPresets() const {
    return buildPresets(*this, {
        {"Spectrum", {{"view", 0}}},
        {"Spectrogram", {{"view", 1}}},
        {"Oscilloscope", {{"view", 2}}},
        {"Vectorscope and correlation", {{"view", 3}}},
        {"Loudness (LUFS)", {{"view", 4}}},
        {"Tuner", {{"view", 5}}},
        {"Spectrum, peaks held", {{"view", 0}, {"hold", 1}}},
    });
}

void Gauge::prepare(double sampleRate) {
    sr_ = sampleRate;
    if (!ringL_) { ringL_.reset(new float[kRing]()); ringR_.reset(new float[kRing]()); }
    if (!segments_) segments_.reset(new float[kMaxSegments]());
    // the two filters of the loudness standard's "K" weighting, designed for this sample rate
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / sr_), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        for (auto& b : shelf_) { b = Biquad(); b.b0 = (Vh + Vb * K / Q + K * K) / a0; b.b1 = 2.0 * (K * K - Vh) / a0; b.b2 = (Vh - Vb * K / Q + K * K) / a0; b.a1 = 2.0 * (K * K - 1.0) / a0; b.a2 = (1.0 - K / Q + K * K) / a0; }
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / sr_), a0 = 1.0 + K / Q + K * K;
        for (auto& b : highpass_) { b = Biquad(); b.b0 = 1.0; b.b1 = -2.0; b.b2 = 1.0; b.a1 = 2.0 * (K * K - 1.0) / a0; b.a2 = (1.0 - K / Q + K * K) / a0; }
    }
    segLen_ = std::max(1, (int)std::lround(sr_ * 0.1));
    reset();
    numSegments_.store(0);
}

void Gauge::reset() {
    for (auto& b : shelf_) b.z1 = b.z2 = 0.0;
    for (auto& b : highpass_) b.z1 = b.z2 = 0.0;
    segSum_ = 0.0;
    segFill_ = 0;
    peakL_ = peakR_ = ms_ = lr_ = ll_ = rr_ = 0.0;
    holdL_ = holdR_ = 0;
}

void Gauge::resetIntegrated() { resetAsked_.store(true); }

void Gauge::process(ProcessContext& c) {
    if (!ringL_) return;
    const float* l = c.io[0];
    const float* r = c.io[1];
    // integrated loudness starts again each time playback starts
    if ((c.playing && !wasPlaying_) || resetAsked_.exchange(false)) { numSegments_.store(0, std::memory_order_relaxed); segSum_ = 0.0; segFill_ = 0; }
    wasPlaying_ = c.playing;
    int w = write_.load(std::memory_order_relaxed);
    const double slow = 1.0 - std::exp(-1.0 / (0.3 * sr_));             // 300 ms averaging for RMS and correlation
    const double fall = std::pow(10.0, -20.0 / 20.0 / sr_);             // peaks fall 20 dB a second after their hold
    const int holdLen = pHold->getBool() ? (1 << 30) : (int)(sr_ * 1.0);
    for (int i = 0; i < kQuantum; ++i) {
        const double a = l[i], b = r[i];
        ringL_[w] = l[i];
        ringR_[w] = r[i];
        w = (w + 1) & (kRing - 1);
        const double aa = std::abs(a), ab = std::abs(b);
        if (aa >= peakL_) { peakL_ = aa; holdL_ = holdLen; } else if (holdL_ > 0) --holdL_; else peakL_ *= fall;
        if (ab >= peakR_) { peakR_ = ab; holdR_ = holdLen; } else if (holdR_ > 0) --holdR_; else peakR_ *= fall;
        ms_ += slow * (0.5 * (a * a + b * b) - ms_);
        lr_ += slow * (a * b - lr_);
        ll_ += slow * (a * a - ll_);
        rr_ += slow * (b * b - rr_);
        const double ka = highpass_[0].run(shelf_[0].run(a)), kb = highpass_[1].run(shelf_[1].run(b));
        segSum_ += ka * ka + kb * kb;
        if (++segFill_ >= segLen_) {
            const int n = numSegments_.load(std::memory_order_relaxed);
            if (n < kMaxSegments) { segments_[n] = (float)(segSum_ / (double)segLen_); numSegments_.store(n + 1, std::memory_order_release); }
            segSum_ = 0.0;
            segFill_ = 0;
        }
    }
    write_.store(w, std::memory_order_relaxed);
    if (std::abs(ms_) < 1.0e-30) ms_ = 0.0;         // keep the averages out of denormal territory in silence
    if (std::abs(lr_) < 1.0e-30) lr_ = 0.0;
    if (ll_ < 1.0e-30) ll_ = 0.0;
    if (rr_ < 1.0e-30) rr_ = 0.0;
    meters_[PeakL].store(toDb(peakL_ * peakL_), std::memory_order_relaxed);
    meters_[PeakR].store(toDb(peakR_ * peakR_), std::memory_order_relaxed);
    meters_[Rms].store(toDb(ms_), std::memory_order_relaxed);
    meters_[Correlation].store(ll_ > 1.0e-12 && rr_ > 1.0e-12 ? (float)clampT(lr_ / std::sqrt(ll_ * rr_), -1.0, 1.0) : 0.0f, std::memory_order_relaxed);
    // momentary: the last 400 ms; short-term: the last three seconds
    const int n = numSegments_.load(std::memory_order_relaxed);
    auto over = [&](int count) {
        if (n < 1) return -120.0f;
        double sum = 0.0;
        const int from = std::max(0, n - count);
        for (int k = from; k < n; ++k) sum += segments_[k];
        const double mean = sum / (double)(n - from);
        return mean <= 1.0e-12 ? -120.0f : (float)(-0.691 + 10.0 * std::log10(mean));
    };
    meters_[Momentary].store(over(4), std::memory_order_relaxed);
    meters_[ShortTerm].store(over(30), std::memory_order_relaxed);
}

float Gauge::meter(int index) const { return index >= 0 && index < NumMeters ? meters_[index].load(std::memory_order_relaxed) : 0.0f; }

int Gauge::readStereo(float* left, float* right, int n) const {
    if (!ringL_) return 0;
    n = std::min(n, (int)kRing);
    const int w = write_.load(std::memory_order_relaxed);
    for (int k = 0; k < n; ++k) {
        const int i = (w - n + k + kRing * 2) & (kRing - 1);
        left[k] = ringL_[i];
        right[k] = ringR_[i];
    }
    return n;
}

int Gauge::readScope(float* dst, int n) const {
    if (!ringL_) return 0;
    n = std::min(n, (int)kRing);
    const int w = write_.load(std::memory_order_relaxed);
    for (int k = 0; k < n; ++k) {
        const int i = (w - n + k + kRing * 2) & (kRing - 1);
        dst[k] = 0.5f * (ringL_[i] + ringR_[i]);
    }
    return n;
}

// Blocks of 400 ms overlapping by three quarters; blocks quieter than -70 LUFS are left out, then blocks
// more than 10 LU below the average of the rest.
double Gauge::integratedLufs() const {
    if (!segments_) return -120.0;
    const int n = numSegments_.load(std::memory_order_acquire);
    if (n < 4) return -120.0;
    auto loud = [](double meanSquare) { return -0.691 + 10.0 * std::log10(std::max(meanSquare, 1.0e-15)); };
    double sum = 0.0;
    int count = 0;
    for (int k = 0; k + 4 <= n; ++k) {
        const double ms = 0.25 * ((double)segments_[k] + segments_[k + 1] + segments_[k + 2] + segments_[k + 3]);
        if (loud(ms) > -70.0) { sum += ms; ++count; }
    }
    if (count == 0) return -120.0;
    const double gate = loud(sum / count) - 10.0;
    sum = 0.0;
    count = 0;
    for (int k = 0; k + 4 <= n; ++k) {
        const double ms = 0.25 * ((double)segments_[k] + segments_[k + 1] + segments_[k + 2] + segments_[k + 3]);
        if (loud(ms) > -70.0 && loud(ms) > gate) { sum += ms; ++count; }
    }
    return count == 0 ? -120.0 : loud(sum / count);
}

} // namespace pulp
