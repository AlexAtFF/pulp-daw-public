#pragma once
// Helpers shared by the test_creative_*.cpp files.
#include "Util.h"
#include "pulp/Creative.h"
#include <chrono>
#include <functional>

namespace ct {
using namespace pulp;

struct Stereo { std::vector<float> l, r; };

struct Transport {
    double sr = 48000.0, tempo = 120.0;     // at 120 BPM and 48 kHz a beat is 24000 samples and a tick is 25
    bool playing = true;
    double startTick = 0.0;
};

// Runs a plugin over a signal in 64-sample quanta, as the engine does. `each` is called before every quantum
// with its number, to move controls.
inline Stereo run(Plugin& p, const Stereo& in, const Transport& t = {}, const std::function<void(size_t)>& each = {}) {
    Stereo out = in;
    const size_t n = in.l.size() / kQuantum * kQuantum;
    out.l.resize(n); out.r.resize(n);
    const double tps = t.tempo * kPPQ / (60.0 * t.sr);
    for (size_t i = 0; i < n; i += kQuantum) {
        if (each) each(i / kQuantum);
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
    s.l.assign((size_t)std::llround(seconds * sr) / kQuantum * kQuantum, 0.0f);
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
// A sine whose frequency rises in a straight line from f0 by `slope` Hz every second.
inline double chirpPhase(double t, double f0, double slope) { return kTwoPi * (f0 * t + 0.5 * slope * t * t); }
inline Stereo chirp(double f0, double slope, double seconds, float amp, double sr = 48000.0) {
    Stereo s = silence(seconds, sr);
    for (size_t i = 0; i < s.l.size(); ++i) s.l[i] = s.r[i] = amp * (float)std::sin(chirpPhase((double)i / sr, f0, slope));
    return s;
}
// a busy test signal: a loud sine burst, then noise with a tone in it, different on the two sides
inline Stereo busy(double seconds, double sr = 48000.0) {
    Stereo s = noise(seconds, 0.3f, sr);
    for (size_t i = 0; i < 2000 && i < s.l.size(); ++i) s.l[i] = s.r[i] = 0.9f * (float)std::sin((double)i * 0.05);
    for (size_t i = 2000; i < s.l.size(); ++i) { const float v = 0.25f * (float)std::sin(kTwoPi * 330.0 * (double)i / sr); s.l[i] += v; s.r[i] -= 0.5f * v; }
    return s;
}
inline void append(Stereo& a, const Stereo& b) {
    a.l.insert(a.l.end(), b.l.begin(), b.l.end());
    a.r.insert(a.r.end(), b.r.begin(), b.r.end());
}
inline Stereo slice(const Stereo& s, size_t from, size_t to) {
    Stereo o;
    o.l.assign(s.l.begin() + (std::ptrdiff_t)from, s.l.begin() + (std::ptrdiff_t)to);
    o.r.assign(s.r.begin() + (std::ptrdiff_t)from, s.r.begin() + (std::ptrdiff_t)to);
    return o;
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
// The largest change from one sample to the next between two places.
inline float maxStep(const std::vector<float>& x, size_t from = 1, size_t to = (size_t)-1) {
    float m = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}
inline double db(double ratio) { return 20.0 * std::log10(ratio + 1e-300); }
inline bool same(const Stereo& a, const Stereo& b) { return a.l == b.l && a.r == b.r; }

// Level in dB of one frequency in a signal (windowed DFT at that one frequency), relative to a full-scale sine.
inline double levelDb(const std::vector<float>& x, size_t from, size_t n, double hz, double sr = 48000.0) {
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * (double)i / (double)(n - 1));
        re += w * x[from + i] * std::cos(kTwoPi * hz * (double)i / sr);
        im += w * x[from + i] * std::sin(kTwoPi * hz * (double)i / sr);
        wsum += w;
    }
    return 20.0 * std::log10(2.0 * std::sqrt(re * re + im * im) / wsum + 1e-12);
}
// Magnitude of the frequency response of an impulse response at one frequency.
inline double magAt(const std::vector<float>& h, double hz, double sr, size_t count = (size_t)-1) {
    const double w = kTwoPi * hz / sr;
    double re = 0.0, im = 0.0;
    const size_t n = std::min(count, h.size());
    for (size_t i = 0; i < n; ++i) { re += h[i] * std::cos(w * (double)i); im -= h[i] * std::sin(w * (double)i); }
    return std::sqrt(re * re + im * im);
}
// Gain in dB a plugin applies to a steady sine, measured over the second half of `seconds`.
inline double gainDbAt(Plugin& p, double hz, float amp = 0.1f, double seconds = 0.5, const Transport& t = {}) {
    auto out = run(p, tone(hz, seconds, amp, t.sr), t);
    const size_t n = out.l.size() / 2;
    return levelDb(out.l, out.l.size() - n, n, hz, t.sr) - 20.0 * std::log10((double)amp);
}
inline double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- samples made in code, for the granular sampler

// a sine sweep, full scale
inline std::shared_ptr<SampleData> sweepSample(double seconds = 2.0, double sr = 44100.0, double f0 = 110.0, double f1 = 1760.0) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    const int n = (int)(seconds * sr);
    s->ch[0].resize((size_t)n);
    double ph = 0.0;
    for (int i = 0; i < n; ++i) {
        const double f = f0 * std::pow(f1 / f0, (double)i / n);
        ph += kTwoPi * f / sr;
        s->ch[0][(size_t)i] = (float)std::sin(ph);
    }
    return s;
}
// something like a sung "ah": a pulse train at 140 Hz through three formants, normalised to full scale, stereo
inline std::shared_ptr<SampleData> vowelSample(double seconds = 2.0, double sr = 44100.0) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = 2;
    const int n = (int)(seconds * sr);
    std::vector<float> x((size_t)n, 0.0f);
    double ph = 0.0;
    for (int i = 0; i < n; ++i) {
        const double f = 140.0 * (1.0 + 0.01 * std::sin(kTwoPi * 5.0 * i / sr));
        ph += f / sr;
        if (ph >= 1.0) { ph -= 1.0; x[(size_t)i] = 1.0f; }
    }
    dsp::Biquad f1, f2, f3;
    f1.bandpass(700.0, 8.0, sr); f2.bandpass(1150.0, 10.0, sr); f3.bandpass(2600.0, 12.0, sr);
    s->ch[0].resize((size_t)n); s->ch[1].resize((size_t)n);
    float pk = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float v = f1.process(x[(size_t)i]) + 0.5f * f2.process(x[(size_t)i]) + 0.3f * f3.process(x[(size_t)i]);
        s->ch[0][(size_t)i] = v;
        pk = std::max(pk, std::fabs(v));
    }
    for (int i = 0; i < n; ++i) { s->ch[0][(size_t)i] /= pk; s->ch[1][(size_t)i] = s->ch[0][(size_t)std::max(0, i - 9)]; }
    return s;
}
// a drum loop made of bursts: noise and low tones, each with its own decay, full scale at the loudest hit
inline std::shared_ptr<SampleData> burstLoop(double seconds = 2.0, double sr = 44100.0) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    const int n = (int)(seconds * sr);
    s->ch[0].assign((size_t)n, 0.0f);
    dsp::Noise rnd(77u);
    const int hits = 8;
    for (int h = 0; h < hits; ++h) {
        const int at = (int)((double)h / hits * n);
        const bool low = h % 2 == 0;
        const double decay = low ? 0.06 : 0.025;
        const float amp = h % 4 == 0 ? 1.0f : 0.6f;
        for (int i = at; i < n && i < at + (int)(0.2 * sr); ++i) {
            const double tt = (double)(i - at) / sr;
            const float body = low ? (float)std::sin(kTwoPi * 180.0 * tt) : rnd.next();
            s->ch[0][(size_t)i] += amp * body * (float)std::exp(-tt / decay);
        }
    }
    float pk = 0.0f;
    for (float v : s->ch[0]) pk = std::max(pk, std::fabs(v));
    for (float& v : s->ch[0]) v /= pk;
    return s;
}

} // namespace ct
