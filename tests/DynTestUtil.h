#pragma once
// Helpers shared by the dynamics test files.
#include "Util.h"
#include "pulp/Dynamics.h"
#include <cstdlib>

namespace dt {
using namespace pulp;
using namespace tu;

struct Stereo { std::vector<float> l, r; };

// Runs a plugin over a signal in 64-sample quanta, as the engine does.
inline Stereo run(Plugin& p, const Stereo& in, double sr = 48000.0, const Stereo* side = nullptr) {
    Stereo out = in;
    const size_t n = in.l.size() / kQuantum * kQuantum;
    out.l.resize(n); out.r.resize(n);
    for (size_t i = 0; i < n; i += kQuantum) {
        ProcessContext c;
        c.sampleRate = sr;
        c.io[0] = out.l.data() + i;
        c.io[1] = out.r.data() + i;
        if (side) { c.sidechain[0] = side->l.data() + i; c.sidechain[1] = side->r.data() + i; }
        p.process(c);
    }
    return out;
}
inline Stereo silence(double seconds, double sr = 48000.0) {
    Stereo s;
    s.l.assign((size_t)(seconds * sr), 0.0f);
    s.r = s.l;
    return s;
}
inline Stereo level(double seconds, float v, double sr = 48000.0) {
    Stereo s = silence(seconds, sr);
    for (auto& x : s.l) x = v;
    s.r = s.l;
    return s;
}
inline Stereo tone(double hz, double seconds, float amp, double sr = 48000.0) {
    Stereo s = silence(seconds, sr);
    for (size_t i = 0; i < s.l.size(); ++i) s.l[i] = s.r[i] = amp * (float)std::sin(kTwoPi * hz * (double)i / sr);
    return s;
}
inline Stereo noise(double seconds, float amp, double sr = 48000.0, uint32 seed = 11u) {
    Stereo s = silence(seconds, sr);
    dsp::Noise a(seed), b(seed * 7u + 29u);
    for (size_t i = 0; i < s.l.size(); ++i) { s.l[i] = amp * a.next(); s.r[i] = amp * b.next(); }
    return s;
}
// Raised-cosine fade at the start, so a tone begins without an edge.
inline Stereo fadeIn(Stereo s, double ms, double sr = 48000.0) {
    const size_t n = std::min(s.l.size(), (size_t)(0.001 * ms * sr));
    for (size_t i = 0; i < n; ++i) { const float g = (float)(0.5 - 0.5 * std::cos(kPi * (double)i / (double)n)); s.l[i] *= g; s.r[i] *= g; }
    return s;
}
inline void append(Stereo& a, const Stereo& b) {
    a.l.insert(a.l.end(), b.l.begin(), b.l.end());
    a.r.insert(a.r.end(), b.r.begin(), b.r.end());
}
inline void addTo(Stereo& a, const Stereo& b) {
    for (size_t i = 0; i < std::min(a.l.size(), b.l.size()); ++i) { a.l[i] += b.l[i]; a.r[i] += b.r[i]; }
}
// Something like music: kick, snare noise, a bass note, a chord and hats, normalised to a peak level.
inline Stereo groove(double seconds, float peak, double sr = 48000.0) {
    Stereo s = silence(seconds, sr);
    dsp::Noise nz(4242u);
    const size_t n = s.l.size();
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / sr;
        const double beat = std::fmod(t, 0.5), off = std::fmod(t + 0.25, 0.5), eighth = std::fmod(t, 0.125);
        double v = 0.9 * std::sin(kTwoPi * 55.0 * beat) * std::exp(-beat / 0.08);
        v += 0.5 * nz.next() * std::exp(-off / 0.04);
        v += 0.15 * std::sin(kTwoPi * 110.0 * t) * (0.6 + 0.4 * std::sin(kTwoPi * 0.7 * t));
        v += 0.05 * (std::sin(kTwoPi * 440.0 * t) + std::sin(kTwoPi * 554.37 * t) + std::sin(kTwoPi * 659.26 * t));
        v += 0.12 * nz.next() * std::exp(-eighth / 0.015);
        s.l[i] = (float)v;
    }
    const float pk = peakOf(s.l);
    for (auto& v : s.l) v *= peak / pk;
    s.r = s.l;
    return s;
}
inline bool allFinite(const Stereo& s) {
    for (float v : s.l) if (!std::isfinite(v)) return false;
    for (float v : s.r) if (!std::isfinite(v)) return false;
    return true;
}
inline double db(double lin) { return 20.0 * std::log10(std::max(lin, 1.0e-30)); }
// Level in dB of one frequency in a signal (windowed DFT at that single frequency), relative to a full scale sine.
inline double levelDbAt(const std::vector<float>& x, size_t from, size_t n, double hz, double sr = 48000.0) {
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * (double)i / (double)(n - 1));
        re += w * x[from + i] * std::cos(kTwoPi * hz * (double)i / sr);
        im += w * x[from + i] * std::sin(kTwoPi * hz * (double)i / sr);
        wsum += w;
    }
    return 20.0 * std::log10(2.0 * std::sqrt(re * re + im * im) / wsum + 1e-12);
}
// Power between two frequencies in a stretch of signal (Hann window, n a power of two).
inline double bandPower(const std::vector<float>& x, size_t from, int n, double f0, double f1, double sr = 48000.0) {
    std::vector<float> re((size_t)n), im((size_t)n, 0.0f);
    for (int i = 0; i < n; ++i) re[(size_t)i] = x[from + (size_t)i] * (float)(0.5 - 0.5 * std::cos(kTwoPi * i / (double)(n - 1)));
    dsp::fft(re.data(), im.data(), n);
    double p = 0.0;
    for (int k = (int)std::ceil(f0 * n / sr); k <= (int)std::floor(f1 * n / sr) && k < n / 2; ++k) p += (double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k];
    return p;
}
// Largest difference between a signal and a delayed copy of another.
inline double maxDiffDelayed(const std::vector<float>& out, const std::vector<float>& in, int delay, size_t from, size_t to = (size_t)-1) {
    double worst = 0.0;
    to = std::min(to, out.size());
    for (size_t i = std::max(from, (size_t)delay); i < to; ++i) worst = std::max(worst, (double)std::fabs(out[i] - in[i - (size_t)delay]));
    return worst;
}
inline float maxStep(const std::vector<float>& x, size_t from = 1, size_t to = (size_t)-1) {
    float m = 0.0f;
    to = std::min(to, x.size());
    for (size_t i = std::max<size_t>(from, 1); i < to; ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}

// Measured figures are printed when PULP_DYN_FIGURES is set, so a normal run stays quiet.
inline bool figures() { static const bool on = std::getenv("PULP_DYN_FIGURES") != nullptr; return on; }
#define FIG(...) do { if (dt::figures()) { printf("    figure: " __VA_ARGS__); printf("\n"); } } while (0)

} // namespace dt
