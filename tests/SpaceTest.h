#pragma once
// Helpers shared by the test_space_*.cpp files.
#include "Util.h"
#include "pulp/Space.h"
#include <chrono>

namespace st {
using namespace pulp;

struct Stereo { std::vector<float> l, r; };

struct Transport {
    double sr = 48000.0, tempo = 120.0;
    bool playing = false;
    double startTick = 0.0;
};

// Runs a plugin over a signal in 64-sample quanta, as the engine does.
inline Stereo run(Plugin& p, const Stereo& in, const Transport& t = {}) {
    Stereo out = in;
    const size_t n = in.l.size() / kQuantum * kQuantum;
    out.l.resize(n); out.r.resize(n);
    const double tps = t.tempo * kPPQ / (60.0 * t.sr);
    for (size_t i = 0; i < n; i += kQuantum) {
        ProcessContext c;
        c.sampleRate = t.sr;
        c.tempo = t.tempo;
        c.playing = t.playing;
        c.tickPos = t.playing ? t.startTick + (double)i * tps : 0.0;
        c.io[0] = out.l.data() + i;
        c.io[1] = out.r.data() + i;
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
inline Stereo tone(double hz, double seconds, float amp, double sr = 48000.0) {
    Stereo s = silence(seconds, sr);
    for (size_t i = 0; i < s.l.size(); ++i) s.l[i] = s.r[i] = amp * (float)std::sin(kTwoPi * hz * (double)i / sr);
    return s;
}
inline Stereo impulse(double seconds, double sr = 48000.0, size_t at = 0, float amp = 1.0f) {
    Stereo s = silence(seconds, sr);
    s.l[at] = s.r[at] = amp;
    return s;
}
inline Stereo noise(double seconds, float amp, double sr = 48000.0, uint32 seed = 11u) {
    Stereo s = silence(seconds, sr);
    dsp::Noise a(seed), b(seed * 3u + 18u);
    for (size_t i = 0; i < s.l.size(); ++i) { s.l[i] = amp * a.next(); s.r[i] = amp * b.next(); }
    return s;
}
inline Stereo dc(double seconds, float v, double sr = 48000.0) {
    Stereo s = silence(seconds, sr);
    std::fill(s.l.begin(), s.l.end(), v);
    s.r = s.l;
    return s;
}
inline void append(Stereo& a, const Stereo& b) {
    a.l.insert(a.l.end(), b.l.begin(), b.l.end());
    a.r.insert(a.r.end(), b.r.begin(), b.r.end());
}
inline bool allFinite(const Stereo& s) {
    for (float v : s.l) if (!std::isfinite(v)) return false;
    for (float v : s.r) if (!std::isfinite(v)) return false;
    return true;
}
inline bool hasDenormal(const Stereo& s) {
    for (float v : s.l) if (std::fpclassify(v) == FP_SUBNORMAL) return true;
    for (float v : s.r) if (std::fpclassify(v) == FP_SUBNORMAL) return true;
    return false;
}
inline float peak(const Stereo& s, size_t from = 0, size_t to = (size_t)-1) {
    return std::max(tu::peakOf(s.l, from, to), tu::peakOf(s.r, from, to));
}
inline double db(double ratio) { return 20.0 * std::log10(ratio + 1e-300); }

// Magnitude of the frequency response of a (short) impulse response at one frequency.
inline double magAt(const std::vector<float>& h, double hz, double sr, size_t count = (size_t)-1) {
    const double w = kTwoPi * hz / sr;
    const double cw = std::cos(w), sw = std::sin(w);
    double re = 0.0, im = 0.0, cr = 1.0, ci = 0.0;
    const size_t n = std::min(count, h.size());
    for (size_t i = 0; i < n; ++i) {
        re += h[i] * cr; im -= h[i] * ci;
        const double t = cr * cw - ci * sw;
        ci = cr * sw + ci * cw;
        cr = t;
    }
    return std::sqrt(re * re + im * im);
}

// Frequencies of the notches of a response: local minima at least `depthDb` below 0 dB between lo and hi.
inline std::vector<double> notches(const std::vector<float>& h, double sr, double lo, double hi, double step, double depthDb, size_t count = (size_t)-1) {
    std::vector<double> f, m;
    for (double hz = lo; hz <= hi; hz += step) { f.push_back(hz); m.push_back(magAt(h, hz, sr, count)); }
    std::vector<double> out;
    for (size_t i = 1; i + 1 < m.size(); ++i)
        if (m[i] < m[i - 1] && m[i] <= m[i + 1] && db(m[i]) < -depthDb) {
            // parabola through the three points for a finer estimate
            const double a = m[i - 1], b = m[i], c = m[i + 1], den = a - 2.0 * b + c;
            out.push_back(f[i] + (den != 0.0 ? 0.5 * (a - c) / den * step : 0.0));
        }
    return out;
}

// Amplitude and frequency of a tone near f0 over time (quadrature demodulation with a triangular window).
struct Track { std::vector<double> amp, hz; };
inline Track demod(const std::vector<float>& x, double sr, double f0, int avg) {
    const size_t n = x.size();
    std::vector<double> i1(n), q1(n);
    for (size_t k = 0; k < n; ++k) {
        const double ph = kTwoPi * f0 * (double)k / sr;
        i1[k] = x[k] * std::cos(ph);
        q1[k] = -x[k] * std::sin(ph);
    }
    auto smooth = [&](std::vector<double>& v) {
        std::vector<double> o(n, 0.0);
        double sum = 0.0;
        for (size_t k = 0; k < n; ++k) {
            sum += v[k];
            if (k >= (size_t)avg) sum -= v[k - (size_t)avg];
            if (k + 1 >= (size_t)avg) o[k + 1 - (size_t)avg / 2 - 1] = sum / avg;      // centred
        }
        v.swap(o);
    };
    for (int pass = 0; pass < 4; ++pass) { smooth(i1); smooth(q1); }        // four passes: the image at twice f0 is gone
    Track t;
    t.amp.assign(n, 0.0);
    t.hz.assign(n, f0);
    double prev = 0.0;
    for (size_t k = 0; k < n; ++k) {
        t.amp[k] = 2.0 * std::sqrt(i1[k] * i1[k] + q1[k] * q1[k]);
        const double ph = std::atan2(q1[k], i1[k]);
        if (k > 0) {
            double d = ph - prev;
            while (d > kPi) d -= kTwoPi;
            while (d < -kPi) d += kTwoPi;
            t.hz[k] = f0 + d * sr / kTwoPi;
        }
        prev = ph;
    }
    return t;
}
// Level over time by a sliding RMS (window in samples), for when the pitch moves too far for the level from demod to be trusted.
inline std::vector<double> slidingRms(const std::vector<float>& x, size_t window) {
    std::vector<double> o(x.size(), 0.0);
    double sum = 0.0;
    for (size_t k = 0; k < x.size(); ++k) {
        sum += (double)x[k] * x[k];
        if (k >= window) sum -= (double)x[k - window] * x[k - window];
        if (k + 1 >= window) o[k - window / 2] = std::sqrt(std::max(0.0, sum) / (double)window);
    }
    return o;
}
inline double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

// Rate of a slow oscillation in v between samples a and b, from its upward crossings of its own mean.
inline double oscRate(const std::vector<double>& v, size_t a, size_t b, double sr) {
    double mean = 0.0;
    for (size_t i = a; i < b; ++i) mean += v[i];
    mean /= (double)(b - a);
    double first = -1.0, last = -1.0;
    int count = 0;
    for (size_t i = a + 1; i < b; ++i)
        if (v[i - 1] < mean && v[i] >= mean) {
            const double x = (double)(i - 1) + (mean - v[i - 1]) / (v[i] - v[i - 1]);
            if (first < 0) first = x;
            last = x;
            ++count;
        }
    return count < 2 ? 0.0 : (count - 1) * sr / (last - first);
}
inline void minMax(const std::vector<double>& v, size_t a, size_t b, double& mn, double& mx) {
    mn = 1e300; mx = -1e300;
    for (size_t i = a; i < b; ++i) { mn = std::min(mn, v[i]); mx = std::max(mx, v[i]); }
}

// Rate of any periodic signal between loHz and hiHz, from the highest peak of its autocorrelation.
inline double acRate(const std::vector<double>& v, size_t a, size_t b, double sr, double loHz, double hiHz) {
    const size_t dec = std::max<size_t>(1, (size_t)(sr / (hiHz * 40.0)));
    std::vector<double> d;
    for (size_t i = a; i + dec <= b; i += dec) { double sum = 0.0; for (size_t k = 0; k < dec; ++k) sum += v[i + k]; d.push_back(sum / (double)dec); }
    double mean = 0.0;
    for (double x : d) mean += x;
    mean /= (double)d.size();
    for (double& x : d) x -= mean;
    const double dsr = sr / (double)dec;
    const size_t lo = (size_t)(dsr / hiHz), hi = std::min((size_t)(dsr / loHz), d.size() / 2);
    auto ac = [&](size_t lag) { double s = 0.0; const size_t n = d.size() - hi - 1; for (size_t i = 0; i < n; ++i) s += d[i] * d[i + lag]; return s; };
    size_t best = lo;
    double top = -1e300;
    std::vector<double> r(hi + 2, 0.0);
    for (size_t lag = lo > 0 ? lo - 1 : 0; lag <= hi + 1 && lag < r.size(); ++lag) r[lag] = ac(lag);
    for (size_t lag = std::max<size_t>(lo, 1); lag <= hi; ++lag)
        if (r[lag] > top && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) top = r[lag];
    for (size_t lag = std::max<size_t>(lo, 1); lag <= hi; ++lag)       // the first peak about as high as the highest: one period, not two
        if (r[lag] >= 0.85 * top && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) { best = lag; break; }
    const double y0 = r[best - 1], y1 = r[best], y2 = r[best + 1], den = y0 - 2.0 * y1 + y2;
    const double fine = (double)best + (den != 0.0 ? 0.5 * (y0 - y2) / den : 0.0);
    return dsr / fine;
}

// Level in dB of one frequency in a signal (windowed DFT at that one frequency), relative to a full-scale sine.
inline double levelDb(const std::vector<float>& x, size_t from, size_t n, double hz, double sr) {
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * (double)i / (double)(n - 1));
        re += w * x[from + i] * std::cos(kTwoPi * hz * (double)i / sr);
        im += w * x[from + i] * std::sin(kTwoPi * hz * (double)i / sr);
        wsum += w;
    }
    return 20.0 * std::log10(2.0 * std::sqrt(re * re + im * im) / wsum + 1e-12);
}

inline double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace st
