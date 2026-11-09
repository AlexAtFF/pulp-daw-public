#pragma once
// Helpers for the Sextet and Ripple tests: rendering a synth through the real engine, driving a
// plugin directly, and measuring pitch, partial levels and aliasing.
#include "Util.h"
#include <chrono>
#include <functional>

namespace ft {
using namespace pulp;
using namespace tu;

inline Note mk(int tick, int len, int key, float vel = kDefaultVelocity) {
    Note n; n.tick = tick; n.length = len; n.key = key; n.velocity = vel; return n;
}

struct RenderSpec {
    double sr = 48000.0, bpm = 120.0;       // 120 bpm at 48 kHz: one tick is exactly 25 samples
    int block = 512, threads = 1;
    int64 length = 48000;
};

// Renders notes on one channel holding the synth, through the engine.
inline std::shared_ptr<SampleData> play(const std::shared_ptr<Plugin>& synth, std::vector<Note> notes, const RenderSpec& rs = {},
                                        const std::function<void(Project&, ChannelLive&)>& setup = {}) {
    Project p;
    p.global->tempo.set((float)rs.bpm);
    auto& c = p.addChannel(synth, "synth");
    const int id = c.id;
    auto live = c.live;
    if (setup) setup(p, *live);
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.tick < b.tick; });
    p.pattern().notesFor(id) = notes;
    RenderOptions o;
    o.sampleRate = rs.sr;
    o.blockSize = rs.block;
    o.threads = rs.threads;
    o.lengthSamples = rs.length;
    return renderOffline(p, o);
}

// Calls a plugin's process() directly, one quantum at a time, with events at absolute sample times.
struct Drive {
    Plugin& p;
    double sr;
    int64 pos = 0;
    float bend = 0.0f;
    std::vector<float> L, R;
    std::vector<std::pair<int64, NoteEvent>> evs;
    explicit Drive(Plugin& pl, double rate = 48000.0) : p(pl), sr(rate) { p.prepare(sr); }
    void ev(int64 at, const NoteEvent& e) { evs.push_back({at, e}); }
    void on(int64 at, int key, uint32 id, float vel = kDefaultVelocity) {
        NoteEvent e; e.type = NoteEvent::On; e.key = key; e.id = id; e.velocity = vel; ev(at, e);
    }
    void off(int64 at, uint32 id) { NoteEvent e; e.type = NoteEvent::Off; e.id = id; ev(at, e); }
    void run(int64 samples) {
        std::stable_sort(evs.begin(), evs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (; samples > 0; samples -= kQuantum, pos += kQuantum) {
            float l[kQuantum] = {}, r[kQuantum] = {};
            NoteEvent e[64];
            int n = 0;
            for (auto& x : evs)
                if (x.first >= pos && x.first < pos + kQuantum && n < 64) { e[n] = x.second; e[n].offset = (int)(x.first - pos); ++n; }
            ProcessContext c;
            c.sampleRate = sr;
            c.io[0] = l; c.io[1] = r;
            c.events = e; c.numEvents = n;
            c.pitchBendSemis = bend;
            { Rt::Scope rt; p.process(c); }
            L.insert(L.end(), l, l + kQuantum);
            R.insert(R.end(), r, r + kQuantum);
        }
    }
};

inline bool allFinite(const std::vector<float>& d) { for (float v : d) if (!std::isfinite(v)) return false; return true; }

// The frequency of a steady partial near guess, from how its phase moves between two windows.
// Good to a small fraction of a cent; guess must be within sr / (2 * hop) Hz.
inline double preciseHz(const std::vector<float>& d, size_t start, double sr, double guess, int win = 16384, int hop = 4096) {
    auto phase = [&](size_t s) {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < win; ++i) {
            const double w = 0.5 - 0.5 * std::cos(kTwoPi * i / win), a = kTwoPi * guess * (double)(s + (size_t)i) / sr;
            re += w * d[s + (size_t)i] * std::cos(a);
            im -= w * d[s + (size_t)i] * std::sin(a);
        }
        return std::atan2(im, re);
    };
    double dp = phase(start + (size_t)hop) - phase(start);
    while (dp > kPi) dp -= kTwoPi;
    while (dp < -kPi) dp += kTwoPi;
    return guess + dp * sr / (kTwoPi * hop);
}
inline double centsOff(double hz, double expected) { return 1200.0 * std::log2(hz / expected); }

// The amplitude of the partial at exactly hz (a sine of amplitude A reads A).
inline double ampAt(const std::vector<float>& d, size_t start, int n, double hz, double sr) {
    double re = 0.0, im = 0.0, ws = 0.0;
    for (int i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * i / n), a = kTwoPi * hz * i / sr;
        re += w * d[start + (size_t)i] * std::cos(a);
        im -= w * d[start + (size_t)i] * std::sin(a);
        ws += w;
    }
    return 2.0 * std::hypot(re, im) / ws;
}

// Magnitude spectrum with a four-term Blackman-Harris window (side lobes at -92 dB). A sine of
// amplitude A that sits on a bin reads A. n must be a power of two.
inline std::vector<double> spectrum(const std::vector<float>& d, size_t start, int n) {
    std::vector<float> re((size_t)n), im((size_t)n, 0.0f);
    double ws = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = kTwoPi * i / n;
        const double w = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
        re[(size_t)i] = (float)(w * d[start + (size_t)i]);
        ws += w;
    }
    dsp::fft(re.data(), im.data(), n);
    std::vector<double> m((size_t)n / 2);
    for (int i = 0; i < n / 2; ++i) m[(size_t)i] = 2.0 * std::hypot((double)re[(size_t)i], (double)im[(size_t)i]) / ws;
    return m;
}

// How far below the strongest partial the strongest component that is NOT a multiple of f0 lies,
// looking between minHz and maxHz. Everything a harmonic sound is supposed to contain sits on
// multiples of f0, so what is left is aliasing (and noise). Returns dB (negative is good).
inline double aliasDb(const std::vector<float>& d, size_t start, int n, double sr, double f0, double minHz, double maxHz) {
    const auto m = spectrum(d, start, n);
    const double binHz = sr / n;
    double strongest = 0.0, alias = 0.0;
    for (int b = 8; b < n / 2; ++b) {
        const double hz = b * binHz;
        strongest = std::max(strongest, m[(size_t)b]);
        if (hz < minHz || hz > maxHz) continue;
        const double k = std::round(hz / f0);
        if (k >= 1.0 && std::fabs(hz - k * f0) < 8.0 * binHz) continue;
        alias = std::max(alias, m[(size_t)b]);
    }
    if (strongest < 1.0e-7) return -200.0;         // silence has no aliases
    return 20.0 * std::log10(std::max(alias, 1.0e-12) / strongest);
}

// Largest step between neighbouring samples in [from, to).
inline float maxStep(const std::vector<float>& d, size_t from, size_t to) {
    float m = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < std::min(to, d.size()); ++i) m = std::max(m, std::fabs(d[i] - d[i - 1]));
    return m;
}

inline double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Every parameter appears in exactly one group.
inline bool groupsCoverParams(const Plugin& p) {
    std::vector<std::string> seen;
    for (auto& g : p.paramGroups())
        for (auto& id : g.ids) {
            if (!p.param(id)) return false;
            if (std::find(seen.begin(), seen.end(), id) != seen.end()) return false;
            seen.push_back(id);
        }
    return seen.size() == p.params().size();
}

} // namespace ft
