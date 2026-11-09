#pragma once
// Small DSP building blocks shared by instruments and effects.
#include "pulp/Core.h"

namespace pulp::dsp {

// RBJ cookbook biquad, transposed direct form II.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    inline float process(float x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float)y;
    }
    void set(double B0, double B1, double B2, double A0, double A1, double A2) {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    void lowpass(double hz, double q, double sr) {
        const double w = kTwoPi * clampT(hz, 1.0, sr * 0.49) / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void highpass(double hz, double q, double sr) {
        const double w = kTwoPi * clampT(hz, 1.0, sr * 0.49) / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    void bandpass(double hz, double q, double sr) {      // constant 0 dB peak gain
        const double w = kTwoPi * clampT(hz, 1.0, sr * 0.49) / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        set(al, 0, -al, 1 + al, -2 * c, 1 - al);
    }
    void notch(double hz, double q, double sr) {
        const double w = kTwoPi * clampT(hz, 1.0, sr * 0.49) / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        set(1, -2 * c, 1, 1 + al, -2 * c, 1 - al);
    }
    void allpass(double hz, double q, double sr) {
        const double w = kTwoPi * clampT(hz, 1.0, sr * 0.49) / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        set(1 - al, -2 * c, 1 + al, 1 + al, -2 * c, 1 - al);
    }
    void peak(double hz, double q, double gainDb, double sr) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w = kTwoPi * clampT(hz, 1.0, sr * 0.49) / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    void lowShelf(double hz, double q, double gainDb, double sr) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w = kTwoPi * clampT(hz, 1.0, sr * 0.49) / sr, c = std::cos(w), al = std::sin(w) / (2 * q), s = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
            (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
    }
    void highShelf(double hz, double q, double gainDb, double sr) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w = kTwoPi * clampT(hz, 1.0, sr * 0.49) / sr, c = std::cos(w), al = std::sin(w) / (2 * q), s = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
            (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
    }
    // Magnitude response at a frequency, for drawing curves and for tests.
    double magnitude(double hz, double sr) const {
        const double w = kTwoPi * hz / sr;
        const double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
        const double nr = b0 + b1 * cr + b2 * c2r, ni = b1 * ci + b2 * c2i;
        const double dr = 1 + a1 * cr + a2 * c2r, di = a1 * ci + a2 * c2i;
        return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }
};

// Zero-delay-feedback state variable filter (Simper / TPT). Stable under fast modulation.
struct Svf {
    float ic1 = 0, ic2 = 0;
    float g = 0, k = 1, a1 = 1, a2 = 0, a3 = 0;
    void reset() { ic1 = ic2 = 0; }
    void set(float hz, float q, float sr) {
        g = (float)std::tan(kPi * clampT(hz, 5.0f, sr * 0.49f) / sr);
        k = 1.0f / std::max(q, 0.05f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    // Returns low, band and high outputs.
    inline void process(float x, float& lp, float& bp, float& hp) {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2; bp = v1; hp = x - k * v1 - v2;
    }
    inline float lowpass(float x) { float l, b, h; process(x, l, b, h); return l; }
};

struct OnePole {
    float z = 0, a = 0;
    void setHz(double hz, double sr) { a = (float)(1.0 - std::exp(-kTwoPi * hz / sr)); }
    void setTime(double seconds, double sr) { a = seconds <= 0 ? 1.0f : (float)(1.0 - std::exp(-1.0 / (seconds * sr))); }
    inline float lp(float x) { z += a * (x - z); return z; }
    inline float hp(float x) { z += a * (x - z); return x - z; }
    void reset(float v = 0) { z = v; }
};

// Deterministic noise (xorshift), so generated sounds and tests are reproducible.
struct Noise {
    uint32 s;
    explicit Noise(uint32 seed = 0x9E3779B9u) : s(seed ? seed : 1u) {}
    inline float next() {          // -1..1
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)((double)s / 2147483648.0 - 1.0);
    }
    inline float uni() { return 0.5f * (next() + 1.0f); }   // 0..1
};

// Fractional delay line with linear interpolation. Size is fixed at prepare time.
struct DelayLine {
    std::vector<float> buf;
    int w = 0, mask = 0;
    void prepare(int maxSamples) {
        int n = 1;
        while (n < maxSamples + 4) n <<= 1;
        buf.assign((size_t)n, 0.0f);
        mask = n - 1;
        w = 0;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); }
    inline void push(float x) { buf[(size_t)w] = x; w = (w + 1) & mask; }
    inline float tap(int delay) const { return buf[(size_t)((w - 1 - delay) & mask)]; }   // delay 0 = the last pushed sample
    inline float tapFrac(float delay) const {
        const int d = (int)delay;
        const float f = delay - (float)d;
        const float a = buf[(size_t)((w - 1 - d) & mask)], b = buf[(size_t)((w - 2 - d) & mask)];
        return a + (b - a) * f;
    }
};

inline float softClip(float x) { return std::tanh(x); }

// In-place radix-2 FFT. n must be a power of two. Used by analysers, tests and offline tools.
void fft(float* re, float* im, int n, bool inverse = false);

} // namespace pulp::dsp
