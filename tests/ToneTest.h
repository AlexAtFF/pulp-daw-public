#pragma once
// Helpers shared by the test_tone_*.cpp files.
#include "Util.h"
#include "pulp/Tone.h"
#include <chrono>
#include <cstdlib>
#include <functional>

namespace tt {
using namespace pulp;
using namespace tu;

struct Stereo { std::vector<float> l, r; };

struct Transport {
    double sr = 48000.0, tempo = 120.0;
    bool playing = false;
    double startTick = 0.0;
};

// Runs a plugin over a signal in 64-sample quanta, as the engine does. `each` is called before every quantum.
inline Stereo run(Plugin& p, const Stereo& in, const Transport& t = {}, const std::function<void(size_t)>& each = {}) {
    Stereo out = in;
    const size_t n = in.l.size() / kQuantum * kQuantum;
    out.l.resize(n); out.r.resize(n);
    const double ticksPerSample = t.tempo * kPPQ / (60.0 * t.sr);
    for (size_t i = 0; i < n; i += kQuantum) {
        if (each) each(i);
        ProcessContext c;
        c.sampleRate = t.sr;
        c.tempo = t.tempo;
        c.playing = t.playing;
        c.tickPos = t.playing ? t.startTick + (double)i * ticksPerSample : 0.0;
        c.io[0] = out.l.data() + i;
        c.io[1] = out.r.data() + i;
        p.process(c);
    }
    return out;
}
inline Stereo runAt(Plugin& p, const Stereo& in, double sr) { Transport t; t.sr = sr; return run(p, in, t); }

inline Stereo silence(double seconds, double sr = 48000.0) {
    Stereo s;
    s.l.assign((size_t)(seconds * sr) / kQuantum * kQuantum, 0.0f);
    s.r = s.l;
    return s;
}
inline Stereo sineWave(double hz, double seconds, float amp, double sr = 48000.0) {
    Stereo s = silence(seconds, sr);
    for (size_t i = 0; i < s.l.size(); ++i) s.l[i] = s.r[i] = amp * (float)std::sin(kTwoPi * hz * (double)i / sr);
    return s;
}
inline Stereo impulse(double seconds, double sr = 48000.0, size_t at = 0, float amp = 1.0f) {
    Stereo s = silence(seconds, sr);
    s.l[at] = s.r[at] = amp;
    return s;
}
inline Stereo level(double seconds, float v, double sr = 48000.0) {
    Stereo s = silence(seconds, sr);
    std::fill(s.l.begin(), s.l.end(), v);
    s.r = s.l;
    return s;
}
inline Stereo noise(double seconds, float amp, double sr = 48000.0, uint32 seed = 11u) {
    Stereo s = silence(seconds, sr);
    dsp::Noise a(seed), b(seed * 7u + 29u);
    for (size_t i = 0; i < s.l.size(); ++i) { s.l[i] = amp * a.next(); s.r[i] = amp * b.next(); }
    return s;
}
inline void append(Stereo& a, const Stereo& b) {
    a.l.insert(a.l.end(), b.l.begin(), b.l.end());
    a.r.insert(a.r.end(), b.r.begin(), b.r.end());
}
inline void addTo(Stereo& a, const Stereo& b) {
    for (size_t i = 0; i < std::min(a.l.size(), b.l.size()); ++i) { a.l[i] += b.l[i]; a.r[i] += b.r[i]; }
}
// Something like music: kick, snare noise, a bass note, a chord and hats, with a little stereo, at a peak level.
inline Stereo groove(double seconds, float peak, double sr = 48000.0) {
    Stereo s = silence(seconds, sr);
    dsp::Noise nz(4242u);
    for (size_t i = 0; i < s.l.size(); ++i) {
        const double t = (double)i / sr;
        const double beat = std::fmod(t, 0.5), off = std::fmod(t + 0.25, 0.5), eighth = std::fmod(t, 0.125);
        double v = 0.9 * std::sin(kTwoPi * 55.0 * beat) * std::exp(-beat / 0.08);
        v += 0.5 * nz.next() * std::exp(-off / 0.04);
        v += 0.15 * std::sin(kTwoPi * 110.0 * t) * (0.6 + 0.4 * std::sin(kTwoPi * 0.7 * t));
        v += 0.05 * (std::sin(kTwoPi * 440.0 * t) + std::sin(kTwoPi * 554.37 * t) + std::sin(kTwoPi * 659.26 * t));
        const double hat = 0.12 * nz.next() * std::exp(-eighth / 0.015);
        s.l[i] = (float)(v + hat);
        s.r[i] = (float)(v - 0.5 * hat + 0.03 * std::sin(kTwoPi * 880.0 * t));
    }
    const float pk = std::max(peakOf(s.l), peakOf(s.r));
    for (auto& v : s.l) v *= peak / pk;
    for (auto& v : s.r) v *= peak / pk;
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
    return 20.0 * std::log10(2.0 * std::sqrt(re * re + im * im) / wsum + 1e-15);
}
// Gain in dB a plugin gives a steady sine, measured at that frequency after it settles. The plugin must be prepared.
inline double gainDbAt(Plugin& p, double hz, float amp = 0.1f, double sr = 48000.0, bool right = false) {
    p.reset();
    const auto out = runAt(p, sineWave(hz, 0.7, amp, sr), sr);
    const size_t n = 16384, from = out.l.size() - n;
    return levelDbAt(right ? out.r : out.l, from, n, hz, sr) - db(amp);
}
// Power between two frequencies in a stretch of signal (Hann window, n a power of two), as a level in dB.
inline double bandDb(const std::vector<float>& x, size_t from, int n, double f0, double f1, double sr = 48000.0) {
    std::vector<float> re((size_t)n), im((size_t)n, 0.0f);
    double wsum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * i / (double)(n - 1));
        re[(size_t)i] = x[from + (size_t)i] * (float)w;
        wsum += w;
    }
    dsp::fft(re.data(), im.data(), n);
    double p = 0.0;
    for (int k = (int)std::ceil(f0 * n / sr); k <= (int)std::floor(f1 * n / sr) && k < n / 2; ++k) p += (double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k];
    // a full scale sine comes out as 0 dB (its energy is spread over the window's main lobe: 1.5 bins of noise bandwidth)
    return 10.0 * std::log10(4.0 * p / (wsum * wsum) / 1.5 + 1e-30);
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
// Level of the harmonics of a tone relative to its fundamental, in dB (index 2 = second harmonic).
inline double harmonicDb(const std::vector<float>& x, double hz, int harmonic, double sr = 48000.0, size_t n = 16384) {
    const size_t from = x.size() - n;
    return levelDbAt(x, from, n, hz * harmonic, sr) - levelDbAt(x, from, n, hz, sr);
}
// What a 9 kHz tone folds back to at 48 kHz when it is distorted without oversampling: the third to the seventh
// harmonic land on 21, 12, 3, 6 and 15 kHz. The sum of those, relative to the 9 kHz tone, in dB.
inline double aliasDb(const std::vector<float>& x, size_t n = 16384) {
    const size_t from = x.size() - n;
    double sum = 0.0;
    for (double hz : {21000.0, 12000.0, 3000.0, 6000.0, 15000.0}) sum += std::pow(10.0, levelDbAt(x, from, n, hz) / 10.0);
    return 10.0 * std::log10(sum + 1e-30) - levelDbAt(x, from, n, 9000.0);
}

// The nine effects, for the tests that every one of them has to pass.
struct Kind { const char* id; const char* name; int minPresets; int latency48; std::function<std::shared_ptr<Plugin>()> make; };
inline const std::vector<Kind>& kinds() {
    static const std::vector<Kind> k = {
        {"geq", "Ten Band", 6, 0, [] { return std::make_shared<TenBand>(); }},
        {"modfilter", "Motion Filter", 6, 32, [] { return std::make_shared<MotionFilter>(); }},
        {"tape", "Quarter Inch", 6, 128, [] { return std::make_shared<QuarterInch>(); }},
        {"tube", "Glass", 6, 32, [] { return std::make_shared<Glass>(); }},
        {"amp", "Stack", 6, 32, [] { return std::make_shared<Stack>(); }},
        {"fuzz", "Splat", 6, 32, [] { return std::make_shared<Splat>(); }},
        {"crush", "Coarse", 6, 0, [] { return std::make_shared<Coarse>(); }},
        {"shaper", "Bend", 8, 32, [] { return std::make_shared<Bend>(); }},
        {"midside", "Middle and Sides", 6, 0, [] { return std::make_shared<MiddleSides>(); }},
    };
    return k;
}
inline std::shared_ptr<Plugin> make(const char* id) {
    for (auto& k : kinds()) if (std::string(k.id) == id) return k.make();
    return nullptr;
}
inline void set(Plugin& p, const char* id, float v) {
    Param* prm = p.param(id);
    if (!prm) { t::fail(__FILE__, __LINE__, std::string("no parameter ") + id); return; }
    prm->set(v);
}

// Measured figures are printed when PULP_TONE_FIGURES is set, so a normal run stays quiet.
inline bool figures() { static const bool on = std::getenv("PULP_TONE_FIGURES") != nullptr; return on; }
#define FIG(...) do { if (tt::figures()) { printf("    figure: " __VA_ARGS__); printf("\n"); } } while (0)

} // namespace tt
