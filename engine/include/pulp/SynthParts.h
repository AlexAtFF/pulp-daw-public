#pragma once
// Parts shared by the Sextet (FM) and Ripple (wavetable) synths: an envelope that lands on zero,
// a stereo state-variable filter whose cutoff can move every sample, LFO shapes, a pan law and a
// 2:1 half-band decimator with no latency.
#include "pulp/Dsp.h"

namespace pulp::parts {

// ADSR. Linear attack, exponential decay and release. Decay to a sustain of zero and release both
// aim slightly below zero, so the envelope reaches exactly zero in finite time and stops there
// (no step at the end of a note, no denormal tail).
struct Env {
    enum Stage : unsigned char { Idle, Attack, Decay, Release };
    Stage stage = Idle;
    float value = 0.0f;
    float attackRate = 2.0f, decayCoef = 0.0f, decayTarget = 1.0f, releaseCoef = 0.0f;
    static constexpr float kUnder = 0.001f;     // how far below zero the falling stages aim

    // Times in milliseconds; rate is how many times per second next() is called.
    void set(float attackMs, float decayMs, float sustain, float releaseMs, double rate) {
        attackRate = attackMs <= 0.02f ? 2.0f : (float)(1000.0 / ((double)attackMs * rate));
        decayCoef = (float)std::exp(-4.6 / (0.001 * std::max(0.5f, decayMs) * rate));
        releaseCoef = (float)std::exp(-4.6 / (0.001 * std::max(0.5f, releaseMs) * rate));
        decayTarget = sustain < 0.0005f ? -kUnder : sustain;
    }
    void start() { stage = Attack; value = 0.0f; }
    void release() { if (stage != Idle) stage = Release; }
    bool active() const { return stage != Idle; }
    inline float next() {
        switch (stage) {
            case Attack:
                value += attackRate;
                if (value >= 1.0f) { value = 1.0f; stage = Decay; }
                break;
            case Decay:
                value = decayTarget + (value - decayTarget) * decayCoef;
                if (value <= 0.0f) { value = 0.0f; stage = Idle; }
                break;
            case Release:
                value = (value + kUnder) * releaseCoef - kUnder;
                if (value <= 0.0f) { value = 0.0f; stage = Idle; }
                break;
            default: break;
        }
        return value;
    }
};

// Stereo zero-delay-feedback state-variable filter. g = tan(pi * fc / sr) and k = 1 / Q are passed
// per sample, so a cutoff ramp costs one division per sample and never zippers.
struct StereoSvf {
    float ic1[2] = {0.0f, 0.0f}, ic2[2] = {0.0f, 0.0f};
    void reset() { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.0f; }
    void flush() { for (int c = 0; c < 2; ++c) { flushDenormal(ic1[c]); flushDenormal(ic2[c]); } }
    // The output is cl * low + cb * band + ch * high.
    inline void process(float g, float k, float cl, float cb, float ch, float& l, float& r) {
        const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        {
            const float v3 = l - ic2[0];
            const float v1 = a1 * ic1[0] + a2 * v3;
            const float v2 = ic2[0] + a2 * ic1[0] + a3 * v3;
            ic1[0] = 2.0f * v1 - ic1[0];
            ic2[0] = 2.0f * v2 - ic2[0];
            l = cl * v2 + cb * v1 + ch * (l - k * v1 - v2);
        }
        {
            const float v3 = r - ic2[1];
            const float v1 = a1 * ic1[1] + a2 * v3;
            const float v2 = ic2[1] + a2 * ic1[1] + a3 * v3;
            ic1[1] = 2.0f * v1 - ic1[1];
            ic2[1] = 2.0f * v2 - ic2[1];
            r = cl * v2 + cb * v1 + ch * (r - k * v1 - v2);
        }
    }
};

inline float svfG(double hz, double sr) { return (float)std::tan(kPi * clampT(hz, 10.0, sr * 0.45) / sr); }
// Resonance 0..1 to 1/Q: Q runs from 0.707 to about 25.
inline float svfK(float reso) { return 1.0f / (0.7071f * std::pow(35.0f, clampT(reso, 0.0f, 1.0f))); }

// LFO shapes, all -1..1. The phase is 0..1; rnd is the held value of the random-steps shape.
inline const std::vector<std::string>& lfoNames() {
    static const std::vector<std::string> n = {"Sine", "Triangle", "Saw up", "Saw down", "Square", "Random steps"};
    return n;
}
inline float lfoShape(int wave, float ph, float rnd) {
    switch (wave) {
        case 0: return (float)std::sin(kTwoPi * ph);
        case 1: return ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);
        case 2: return 2.0f * ph - 1.0f;
        case 3: return 1.0f - 2.0f * ph;
        case 4: return ph < 0.5f ? 1.0f : -1.0f;
        default: return rnd;
    }
}

// The pan law the other native instruments use: the far side is turned down, the near side stays.
inline void panGains(float pan, float& l, float& r) {
    pan = clampT(pan, -1.0f, 1.0f);
    l = pan > 0.0f ? 1.0f - pan : 1.0f;
    r = pan < 0.0f ? 1.0f + pan : 1.0f;
}

// Coefficients of an elliptic half-band low-pass built from two chains of first-order all-pass
// sections (the usual polyphase form). transition is the width of the transition band as a
// fraction of the sample rate; the pass band ends at 0.25 - transition / 2.
inline void designHalfband(int numCoefs, double transition, double* coef) {
    double k = std::tan((1.0 - transition * 2.0) * kPi / 4.0);
    k *= k;
    const double kk = std::pow(1.0 - k * k, 0.25);
    const double e = 0.5 * (1.0 - kk) / (1.0 + kk);
    const double e4 = e * e * e * e;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    const int order = numCoefs * 2 + 1;
    for (int index = 0; index < numCoefs; ++index) {
        const int c = index + 1;
        double num = 0.0, den = 0.0, qp = 1.0;
        for (int i = 0, sign = 1; qp > 1.0e-100 && i < 64; ++i, sign = -sign) {
            qp = std::pow(q, (double)(i * (i + 1)));
            num += qp * std::sin((i * 2 + 1) * c * kPi / order) * sign;
        }
        num *= std::pow(q, 0.25);
        qp = 1.0;
        for (int i = 1, sign = -1; qp > 1.0e-100 && i < 64; ++i, sign = -sign) {
            qp = std::pow(q, (double)(i * i));
            den += qp * std::cos(i * 2 * c * kPi / order) * sign;
        }
        den += 0.5;
        double ww = num / den;
        ww *= ww;
        const double x = std::sqrt((1.0 - ww * k) * (1.0 - ww / k)) / (1.0 + ww);
        coef[index] = (1.0 - x) / (1.0 + x);
    }
}

// Halves the sample rate. Infinite impulse response, so there is no look-ahead and no latency:
// a sound that starts in the first of the two input samples is in the output sample they make.
struct Decimator2 {
    static constexpr int kCoefs = 8;
    static const double* coefs() {
        static const struct C { double c[kCoefs]; C() { designHalfband(kCoefs, 0.045, c); } } k;
        return k.c;
    }
    const double* c = nullptr;
    double x[kCoefs] = {}, y[kCoefs] = {};
    void prepare() { c = coefs(); reset(); }
    void reset() { for (int i = 0; i < kCoefs; ++i) x[i] = y[i] = 0.0; }
    void flush() {
        for (int i = 0; i < kCoefs; ++i) {
            if (std::fabs(x[i]) < 1.0e-30) x[i] = 0.0;
            if (std::fabs(y[i]) < 1.0e-30) y[i] = 0.0;
        }
    }
    // first is the earlier of the two input samples.
    inline float process(float first, float second) {
        double s0 = second, s1 = first;
        for (int i = 0; i < kCoefs; i += 2) {
            const double t0 = (s0 - y[i]) * c[i] + x[i];
            const double t1 = (s1 - y[i + 1]) * c[i + 1] + x[i + 1];
            x[i] = s0; x[i + 1] = s1;
            y[i] = t0; y[i + 1] = t1;
            s0 = t0; s1 = t1;
        }
        return (float)(0.5 * (s0 + s1));
    }
};

} // namespace pulp::parts
