#pragma once
// Helpers shared by the test_vocal_*.cpp files.
#include "Util.h"
#include "pulp/Vocal.h"
#include <cstdlib>
#include <filesystem>
#include <functional>

namespace vt {
using namespace pulp;
using namespace tu;

struct Stereo { std::vector<float> l, r; };

// Set PULP_VOCAL_FIGURES to make the tests print what they measured.
inline bool figures() { static const bool on = std::getenv("PULP_VOCAL_FIGURES") != nullptr; return on; }
#define VT_FIG(...) do { if (vt::figures()) { printf("      "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Runs a plugin over a signal in 64-sample quanta, as the engine does. `each` is called before every quantum.
inline Stereo run(Plugin& p, const Stereo& in, double sr = 48000.0, const Stereo* side = nullptr,
                  const std::function<void(size_t)>& each = {}) {
    Stereo out = in;
    const size_t n = in.l.size() / kQuantum * kQuantum;
    out.l.resize(n); out.r.resize(n);
    for (size_t i = 0; i < n; i += kQuantum) {
        if (each) each(i);
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
    s.l.assign((size_t)(seconds * sr) / kQuantum * kQuantum, 0.0f);
    s.r = s.l;
    return s;
}
inline Stereo mono(const std::vector<float>& v) {
    Stereo s;
    s.l = v;
    s.l.resize(v.size() / kQuantum * kQuantum);
    s.r = s.l;
    return s;
}
inline Stereo tone(double hz, double seconds, float amp, double sr = 48000.0) {
    Stereo s = silence(seconds, sr);
    for (size_t i = 0; i < s.l.size(); ++i) s.l[i] = s.r[i] = amp * (float)std::sin(kTwoPi * hz * (double)i / sr);
    return s;
}
inline Stereo noise(double seconds, float amp, double sr = 48000.0, uint32 seed = 11u, bool sameBothSides = false) {
    Stereo s = silence(seconds, sr);
    dsp::Noise a(seed), b(seed * 7u + 29u);
    for (size_t i = 0; i < s.l.size(); ++i) { s.l[i] = amp * a.next(); s.r[i] = sameBothSides ? s.l[i] : amp * b.next(); }
    return s;
}
inline void append(Stereo& a, const Stereo& b) {
    a.l.insert(a.l.end(), b.l.begin(), b.l.end());
    a.r.insert(a.r.end(), b.r.begin(), b.r.end());
}
inline Stereo add(Stereo a, const Stereo& b) {
    for (size_t i = 0; i < std::min(a.l.size(), b.l.size()); ++i) { a.l[i] += b.l[i]; a.r[i] += b.r[i]; }
    return a;
}
inline bool allFinite(const Stereo& s) {
    for (float v : s.l) if (!std::isfinite(v)) return false;
    for (float v : s.r) if (!std::isfinite(v)) return false;
    return true;
}
inline float peak(const Stereo& s, size_t from = 0, size_t to = (size_t)-1) { return std::max(peakOf(s.l, from, to), peakOf(s.r, from, to)); }
// Largest difference between out and in delayed by `delay` samples, from sample `from` of out.
inline double nullAgainstDelayed(const std::vector<float>& out, const std::vector<float>& in, size_t delay, size_t from = 0) {
    double worst = 0.0;
    for (size_t i = std::max(from, delay); i < out.size(); ++i) worst = std::max(worst, (double)std::fabs(out[i] - in[i - delay]));
    return worst;
}
inline double centsBetween(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }
inline double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }

// ---- a synthetic sung vowel: harmonics of f0(t) falling at 6 dB per octave, shaped by resonances ----
struct Formant { double hz, q, gain; };
inline const std::vector<Formant>& vowelAh() { static const std::vector<Formant> f = {{700, 6, 1.0}, {1200, 7, 0.6}, {2600, 8, 0.35}}; return f; }
inline std::vector<float> voice(double seconds, double sr, const std::function<double(double)>& f0, const std::vector<Formant>& formants,
                                float amp = 0.4f, double fadeMs = 8.0, double tilt = 1.0) {
    const size_t n = (size_t)(seconds * sr) / kQuantum * kQuantum;
    std::vector<float> out(n, 0.0f);
    std::vector<double> a;
    double phase = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double f = f0((double)i / sr);
        if ((i & 63) == 0) {
            const int H = std::max(1, (int)(std::min(0.42 * sr, 9000.0) / f));
            a.assign((size_t)H, 0.0);
            for (int h = 1; h <= H; ++h) {
                const double hz = h * f;
                double env = formants.empty() ? 1.0 : 0.12;
                for (auto& F : formants) {
                    const double x = hz / F.hz;
                    env += F.gain / std::sqrt((1.0 - x * x) * (1.0 - x * x) + (x / F.q) * (x / F.q));
                }
                a[(size_t)(h - 1)] = env / std::pow((double)h, tilt);
            }
        }
        double v = 0.0;
        for (size_t h = 0; h < a.size(); ++h) v += a[h] * std::cos((double)(h + 1) * phase);
        out[i] = (float)v;
        phase += kTwoPi * f / sr;
        if (phase > kTwoPi * 64.0) phase -= kTwoPi * 64.0;
    }
    const float pk = peakOf(out);
    const size_t fade = (size_t)(0.001 * fadeMs * sr);
    for (size_t i = 0; i < n; ++i) {
        float g = pk > 0.0f ? amp / pk : 0.0f;
        if (i < fade) g *= (float)(0.5 - 0.5 * std::cos(kPi * (double)i / (double)fade));
        if (n - 1 - i < fade) g *= (float)(0.5 - 0.5 * std::cos(kPi * (double)(n - 1 - i) / (double)fade));
        out[i] *= g;
    }
    return out;
}
inline std::vector<float> steadyVoice(double hz, double seconds, double sr = 48000.0, float amp = 0.4f, bool withFormants = true) {
    return voice(seconds, sr, [hz](double) { return hz; }, withFormants ? vowelAh() : std::vector<Formant>{}, amp);
}

// One of the sung notes of the factory library ("C4", "G#3", ...), with a tenth of a second of silence in
// front. False when the library has not been built on this machine: the test then says so and passes.
inline bool factoryVoice(const char* note, Stereo& out, double& sr, double seconds = 2.5) {
    const std::string path = std::string(PULP_SOURCE_DIR) + "/factory/Instruments/Voices/Solo Voice Aah/Solo Voice Aah " + note + ".flac";
    if (!std::filesystem::exists(path)) { printf("      (no factory voice recording %s here: skipped)\n", note); return false; }
    auto smp = readAudioFile(path);
    if (!smp || smp->frames() < (int64)(1.5 * smp->sampleRate)) return false;
    sr = smp->sampleRate;
    out = silence(0.1, sr);
    Stereo body;
    body.l = smp->ch[0];
    body.r = smp->channels > 1 ? smp->ch[1] : smp->ch[0];
    const size_t keep = std::min(body.l.size(), (size_t)(seconds * sr)) / kQuantum * kQuantum;
    body.l.resize(keep); body.r.resize(keep);
    append(out, body);
    return true;
}

// ---- measuring ----
// The signal low-passed (fourth order) so that only the fundamental near `approxHz` is left.
inline std::vector<float> fundamentalOnly(const std::vector<float>& x, double approxHz, double sr) {
    dsp::Biquad a, b;
    a.lowpass(1.45 * approxHz, 0.54119610, sr);
    b.lowpass(1.45 * approxHz, 1.30656296, sr);
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = b.process(a.process(x[i]));
    return y;
}
// Fundamental frequency between samples a and b (zero crossings of the low-passed signal).
inline double fundamentalHz(const std::vector<float>& x, size_t a, size_t b, double approxHz, double sr) {
    return measureHz(fundamentalOnly(x, approxHz, sr), a, b, sr);
}
// Fundamental frequency in windows: one value per hop.
inline std::vector<double> pitchTrack(const std::vector<float>& x, size_t from, size_t to, double approxHz, double sr, double winMs = 30.0, double hopMs = 10.0) {
    const auto y = fundamentalOnly(x, approxHz, sr);
    const size_t win = (size_t)(0.001 * winMs * sr), hop = (size_t)(0.001 * hopMs * sr);
    std::vector<double> out;
    for (size_t a = from; a + win <= std::min(to, y.size()); a += hop) out.push_back(measureHz(y, a, a + win, sr));
    return out;
}
// Period-by-correlation pitch at one place: the lag near the expected period at which three periods of the
// signal best match the three that follow one lag later. Works when the fundamental itself is weak.
inline double correlationHz(const std::vector<float>& x, size_t centre, double approxHz, double sr) {
    const double T0 = sr / approxHz;
    const int M = (int)(3.0 * T0), lo = (int)(T0 * 0.9), hi = (int)(T0 * 1.1) + 1;
    const long start = (long)centre - (long)((M + T0) / 2.0);
    if (start < 0 || (size_t)(start + M + hi + 2) >= x.size()) return 0.0;
    auto at = [&](int tau) {
        double r = 0.0, e = 0.0;
        for (int i = 0; i < M; ++i) {
            const double a = x[(size_t)(start + i)], b = x[(size_t)(start + i + tau)];
            r += a * b; e += a * a + b * b;
        }
        return e > 0.0 ? 2.0 * r / e : 0.0;
    };
    int best = lo;
    double bestV = -2.0;
    for (int tau = lo; tau <= hi; ++tau) { const double v = at(tau); if (v > bestV) { bestV = v; best = tau; } }
    const double a = at(best - 1), b = bestV, c = at(best + 1), den = a - 2.0 * b + c;
    const double d = den < 0.0 ? clampT(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
    return sr / (best + d);
}
inline std::vector<double> correlationTrack(const std::vector<float>& x, size_t from, size_t to, double approxHz, double sr, double hopMs = 10.0) {
    std::vector<double> out;
    for (size_t c = from; c < to; c += (size_t)(0.001 * hopMs * sr)) { const double hz = correlationHz(x, c, approxHz, sr); if (hz > 0.0) out.push_back(hz); }
    return out;
}
// Averaged power spectrum (Hann window, half-overlapped frames of n samples) from sample `from`. n/2+1 values.
inline std::vector<double> powerSpectrum(const std::vector<float>& x, size_t from, size_t to, int n = 4096) {
    std::vector<double> p((size_t)n / 2 + 1, 0.0);
    std::vector<float> re((size_t)n), im((size_t)n);
    int frames = 0;
    for (size_t a = from; a + (size_t)n <= std::min(to, x.size()); a += (size_t)n / 2) {
        for (int i = 0; i < n; ++i) { re[(size_t)i] = x[a + (size_t)i] * (float)(0.5 - 0.5 * std::cos(kTwoPi * i / n)); im[(size_t)i] = 0.0f; }
        dsp::fft(re.data(), im.data(), n);
        for (int k = 0; k <= n / 2; ++k) p[(size_t)k] += (double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k];
        ++frames;
    }
    if (frames) for (auto& v : p) v /= frames;
    return p;
}
inline double bandPower(const std::vector<double>& spec, double f1, double f2, double sr) {
    const int n = ((int)spec.size() - 1) * 2;
    double s = 0.0;
    for (int k = std::max(0, (int)std::ceil(f1 * n / sr)); k <= std::min(n / 2, (int)std::floor(f2 * n / sr)); ++k) s += spec[(size_t)k];
    return s;
}
// Where the spectral envelope peaks between f1 and f2: the spectrum is first averaged over `smoothHz`
// (two harmonic spacings wash the harmonics out), then the maximum is refined with a parabola.
inline double envelopePeakHz(const std::vector<double>& spec, double sr, double smoothHz, double f1, double f2) {
    const int n = ((int)spec.size() - 1) * 2;
    const int half = std::max(1, (int)std::lround(0.5 * smoothHz * n / sr));
    std::vector<double> pre(spec.size() + 1, 0.0), sm(spec.size(), 0.0);
    for (size_t k = 0; k < spec.size(); ++k) pre[k + 1] = pre[k] + spec[k];
    for (int k = 0; k < (int)spec.size(); ++k) {
        const int a = std::max(0, k - half), b = std::min((int)spec.size() - 1, k + half);
        sm[(size_t)k] = (pre[(size_t)b + 1] - pre[(size_t)a]) / (b - a + 1);
    }
    int best = (int)(f1 * n / sr);
    for (int k = best; k <= (int)(f2 * n / sr) && k < (int)spec.size() - 1; ++k) if (sm[(size_t)k] > sm[(size_t)best]) best = k;
    best = clampT(best, 1, (int)spec.size() - 2);
    const double a = std::log(sm[(size_t)best - 1] + 1e-30), b = std::log(sm[(size_t)best] + 1e-30), c = std::log(sm[(size_t)best + 1] + 1e-30);
    const double den = a - 2.0 * b + c, d = den < 0.0 ? clampT(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
    return (best + d) * sr / n;
}
// Level in dB of one frequency (windowed single-bin DFT), relative to a full-scale sine.
inline double levelDbAt(const std::vector<float>& x, size_t from, size_t n, double hz, double sr) {
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * (double)i / (double)(n - 1));
        re += w * x[from + i] * std::cos(kTwoPi * hz * (double)i / sr);
        im += w * x[from + i] * std::sin(kTwoPi * hz * (double)i / sr);
        wsum += w;
    }
    return 20.0 * std::log10(2.0 * std::sqrt(re * re + im * im) / wsum + 1e-12);
}

} // namespace vt
