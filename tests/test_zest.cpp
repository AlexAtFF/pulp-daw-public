// Zest, the subtractive synthesizer.
#include "Util.h"
#include "pulp/Zest.h"
#include <chrono>
using namespace pulp;
using namespace tu;

namespace {

struct ZestChan { int id; std::shared_ptr<ChannelLive> live; Zest* synth; };
ZestChan addZest(Project& p, const char* name = "Zest") {
    auto t = std::make_shared<Zest>();
    Zest* raw = t.get();
    auto& c = p.addChannel(t, name);
    return {c.id, c.live, raw};
}
Note mk(int tick, int len, int key, float vel = kDefaultVelocity) { Note n; n.tick = tick; n.length = len; n.key = key; n.velocity = vel; return n; }
void set(Zest& c, const char* id, float v) {
    Param* p = c.param(id);
    CHECK_MSG(p != nullptr, "no parameter called %s", id);
    if (p) p->set(v);
}
// One oscillator, filter wide open, flat envelope, phase reset: a clean, repeatable tone for measuring.
void plain(Zest& c, int shape) {
    set(c, "o1shape", (float)shape);
    set(c, "o1level", 1.0f);
    set(c, "o1phase", 1);
    set(c, "o2phase", 1);
    set(c, "f1type", Zest::LowPass12);
    set(c, "f1reso", 0.0f);
    set(c, "e1atk", 0.0f);
    set(c, "e1sus", 1.0f);
    set(c, "velsens", 0.0f);
}
const Preset* presetNamed(const std::vector<Preset>& all, const char* name) {
    for (auto& p : all) if (p.name == name) return &p;
    CHECK_MSG(false, "no preset called %s", name);
    return &all[0];
}

// Drives the synth directly, one quantum at a time, so a test can move parameters, the transport and the pitch
// bend while it plays. Everything inside process() runs with the real-time marker set.
struct Rig {
    Zest syn;
    double sr = 48000.0, tempo = 120.0, tick = 0.0;
    bool playing = false;
    float bend = 0.0f;
    struct Ev { int64 at; NoteEvent e; };
    std::vector<Ev> evs;
    std::vector<float> L, R;
    int64 pos = 0;
    uint32 nextId = 1;
    int maxVoices = 0;
    explicit Rig(double rate = 48000.0) : sr(rate) { syn.prepare(sr); }
    uint32 on(int64 at, int key, float vel = kDefaultVelocity) {
        NoteEvent e; e.type = NoteEvent::On; e.key = key; e.velocity = vel; e.id = nextId++;
        evs.push_back({at, e});
        return e.id;
    }
    uint32 onEv(int64 at, NoteEvent e) { e.type = NoteEvent::On; e.id = nextId++; evs.push_back({at, e}); return e.id; }
    void off(int64 at, uint32 id) { NoteEvent e; e.type = NoteEvent::Off; e.id = id; evs.push_back({at, e}); }
    void ev(int64 at, NoteEvent::Type type, uint32 id = 0) { NoteEvent e; e.type = type; e.id = id; evs.push_back({at, e}); }
    uint32 note(int64 at, int64 len, int key, float vel = kDefaultVelocity) { const uint32 id = on(at, key, vel); off(at + len, id); return id; }
    void run(int64 samples, const std::function<void(int64)>& each = {}) {
        const int64 end = pos + samples;
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
        size_t next = 0;
        while (pos < end) {
            if (each) each(pos);
            NoteEvent q[kMaxEventsPerQuantum];
            int n = 0;
            while (next < evs.size() && evs[next].at < pos + kQuantum && n < kMaxEventsPerQuantum) {
                q[n] = evs[next].e;
                q[n].offset = (int)std::max<int64>(0, evs[next].at - pos);
                ++n; ++next;
            }
            float l[kQuantum] = {}, r[kQuantum] = {};
            ProcessContext c;
            c.sampleRate = sr; c.tempo = tempo; c.tickPos = tick; c.playing = playing;
            c.io[0] = l; c.io[1] = r; c.events = q; c.numEvents = n; c.pitchBendSemis = bend;
            {
                Rt::Scope rt;
                syn.process(c);
            }
            L.insert(L.end(), l, l + kQuantum);
            R.insert(R.end(), r, r + kQuantum);
            pos += kQuantum;
            maxVoices = std::max(maxVoices, syn.activeVoices());
            if (playing) tick += kQuantum * tempo * kPPQ / (60.0 * sr);
        }
        evs.erase(evs.begin(), evs.begin() + (long long)next);
    }
};

double centsOff(double hz, double key) { return hz > 0.0 ? 1200.0 * std::log2(hz / keyToHz(key)) : -99999.0; }

// The fundamental of a harmonic sound: low-pass hard just above where it is expected, then count zero crossings.
double fundHz(const std::vector<float>& d, size_t a, size_t b, double guessHz, double sr) {
    std::vector<float> x(d.begin() + (long long)a, d.begin() + (long long)std::min(b, d.size()));
    for (int pass = 0; pass < 6; ++pass) {
        dsp::Biquad bq;
        bq.lowpass(guessHz * 1.35, 0.707, sr);
        for (float& v : x) v = bq.process(v);
    }
    const size_t skip = std::min(x.size() / 4, (size_t)(sr * 0.05));
    double mean = 0.0;
    for (size_t i = skip; i < x.size(); ++i) mean += x[i];
    mean /= (double)std::max<size_t>(1, x.size() - skip);
    for (float& v : x) v -= (float)mean;
    return measureHz(x, skip, x.size(), sr);
}

// Windowed (Blackman-Harris) spectrum of n samples starting at from; magnitudes for bins 0..n/2.
std::vector<double> spectrum(const std::vector<float>& d, size_t from, int n) {
    std::vector<float> re((size_t)n, 0.0f), im((size_t)n, 0.0f);
    for (int i = 0; i < n && from + (size_t)i < d.size(); ++i) {
        const double w = 0.35875 - 0.48829 * std::cos(kTwoPi * i / n) + 0.14128 * std::cos(2 * kTwoPi * i / n) - 0.01168 * std::cos(3 * kTwoPi * i / n);
        re[(size_t)i] = d[from + (size_t)i] * (float)w;
    }
    dsp::fft(re.data(), im.data(), n);
    std::vector<double> m((size_t)n / 2 + 1);
    for (int i = 0; i <= n / 2; ++i) m[(size_t)i] = std::hypot((double)re[(size_t)i], (double)im[(size_t)i]);
    return m;
}
double binPeak(const std::vector<double>& m, double hz, double sr, int n, int reach = 3) {
    const int b = (int)std::lround(hz * n / sr);
    double v = 0.0;
    for (int k = std::max(0, b - reach); k <= std::min((int)m.size() - 1, b + reach); ++k) v = std::max(v, m[(size_t)k]);
    return v;
}
// How far everything that is not a harmonic of f0 (and lies below maxHz) sits below the strongest harmonic, in dB.
double aliasDb(const std::vector<float>& d, size_t from, int n, double f0, double sr, double maxHz = 1.0e9) {
    const auto m = spectrum(d, from, n);
    double strongest = 0.0, worst = 0.0;
    for (int b = 8; b < n / 2; ++b) {
        const double hz = b * sr / n;
        const double k = std::round(hz / f0);
        const double dist = std::fabs(hz - k * f0) * n / sr;      // bins from the nearest harmonic
        if (k >= 1 && dist < 1.0) strongest = std::max(strongest, m[(size_t)b]);
        if ((k < 1 || dist > 8.0) && hz < maxHz) worst = std::max(worst, m[(size_t)b]);
    }
    return 20.0 * std::log10((worst + 1e-30) / (strongest + 1e-30));
}
// Amplitude of the sinusoid at hz in d[a, b).
double toneAmp(const std::vector<float>& d, size_t a, size_t b, double hz, double sr) {
    double re = 0.0, im = 0.0, sw = 0.0;
    b = std::min(b, d.size());
    for (size_t i = a; i < b; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * (double)(i - a) / (double)(b - a));
        re += w * d[i] * std::cos(kTwoPi * hz * (double)i / sr);
        im += w * d[i] * std::sin(kTwoPi * hz * (double)i / sr);
        sw += w;
    }
    return 2.0 * std::hypot(re, im) / sw;
}
double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-30)); }
bool allFinite(const std::vector<float>& d) { for (float v : d) if (!std::isfinite(v)) return false; return true; }
float maxStep(const std::vector<float>& d, size_t a, size_t b) {
    float m = 0.0f;
    for (size_t i = std::max<size_t>(a, 1); i < std::min(b, d.size()); ++i) m = std::max(m, std::fabs(d[i] - d[i - 1]));
    return m;
}
// Level in consecutive windows: the envelope of a sound, sampled at sr / win.
std::vector<float> rmsSeries(const std::vector<float>& d, size_t a, size_t b, int win) {
    std::vector<float> out;
    for (size_t i = a; i + (size_t)win <= std::min(b, d.size()); i += (size_t)win) out.push_back((float)rmsOf(d, i, i + (size_t)win));
    return out;
}
// The rate at which a level wobbles (tremolo, beating), in Hz.
double wobbleHz(const std::vector<float>& d, size_t a, size_t b, double sr, int win = 96) {
    auto e = rmsSeries(d, a, b, win);
    double mean = 0.0;
    for (float v : e) mean += v;
    mean /= (double)std::max<size_t>(1, e.size());
    for (float& v : e) v -= (float)mean;
    for (int pass = 0; pass < 2; ++pass) {          // smooth a little so ripple from the carrier does not add crossings
        float z = e.empty() ? 0.0f : e[0];
        for (float& v : e) { z += 0.35f * (v - z); v = z; }
    }
    return measureHz(e, 0, e.size(), sr / win);
}
int64 firstSound(const std::vector<float>& d) { for (size_t i = 0; i < d.size(); ++i) if (d[i] != 0.0f) return (int64)i; return -1; }
int64 lastSound(const std::vector<float>& d) { for (size_t i = d.size(); i > 0; --i) if (d[i - 1] != 0.0f) return (int64)i - 1; return -1; }
std::vector<float> cut(const std::vector<float>& d, size_t a, size_t b) { return std::vector<float>(d.begin() + (long long)a, d.begin() + (long long)std::min(b, d.size())); }

std::shared_ptr<SampleData> render(Project& p, int64 len, double sr = 48000.0, int block = 512, int threads = 1) {
    RenderOptions o;
    o.sampleRate = sr;
    o.lengthSamples = len;
    o.blockSize = block;
    o.threads = threads;
    return renderOffline(p, o);
}

} // namespace

// ------------------------------------------------------------------ tuning

TEST(zest_oscillators_are_in_tune) {
    double worst = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int shape = 0; shape < Zest::NumShapes; ++shape)
            for (int key : {36, 57, 60, 72, 84}) {
                Project p;
                auto ch = addZest(p);
                plain(*ch.synth, shape);
                set(*ch.synth, "o1pw", 0.3f);
                p.pattern().notesFor(ch.id).push_back(mk(0, kPPQ * 8, key));
                auto out = render(p, (int64)sr, sr);
                CHECK(out != nullptr);
                if (!out) return;
                const double cents = centsOff(fundHz(out->ch[0], 0, (size_t)sr, keyToHz(key), sr), key);
                worst = std::max(worst, std::fabs(cents));
                CHECK_MSG(std::fabs(cents) < 1.0, "shape %d key %d at %.0f Hz: %.2f cents off", shape, key, sr, cents);
                CHECK(peakOf(out->ch[0]) > 0.1f && peakOf(out->ch[0]) < 1.0f);
            }
    printf("    worst tuning error over 5 shapes, 5 keys, 3 sample rates: %.4f cents\n", worst);
}

TEST(zest_coarse_fine_sub_sync_and_unison_are_in_tune) {
    const double sr = 48000.0;
    auto tune = [&](const std::function<void(Zest&)>& setup, int key, double expectKey, const char* what) {
        Rig r(sr);
        plain(r.syn, Zest::Saw);
        setup(r.syn);
        r.on(0, key);
        r.run(48000);
        const double cents = centsOff(fundHz(r.L, 0, 48000, keyToHz(expectKey), sr), expectKey);
        CHECK_MSG(std::fabs(cents) < 1.5, "%s: %.2f cents off", what, cents);
    };
    tune([](Zest& c) { set(c, "o1coarse", 7); set(c, "o1fine", 50); }, 60, 67.5, "coarse +7, fine +50");
    tune([](Zest& c) { set(c, "o1coarse", -24); set(c, "o1fine", -100); }, 72, 47.0, "coarse -24, fine -100");
    tune([](Zest& c) { set(c, "o1level", 0); set(c, "o2level", 1); set(c, "o2coarse", 12); set(c, "o2shape", Zest::Pulse); }, 57, 69.0, "oscillator 2 an octave up");
    for (int shape = 0; shape < 2; ++shape)
        for (int oct = 0; oct < 2; ++oct)
            tune([&](Zest& c) { set(c, "o1level", 0); set(c, "sublevel", 1); set(c, "subshape", (float)shape); set(c, "suboct", (float)oct); }, 72, 72.0 - 12.0 * (oct + 1), "sub oscillator");
    // hard sync: whatever oscillator 2 is tuned to, the pitch heard is oscillator 1's
    for (int shape = 0; shape < Zest::NumShapes; ++shape)
        tune([&](Zest& c) { set(c, "o1level", 0); set(c, "o2level", 1); set(c, "o2sync", 1); set(c, "o2shape", (float)shape); set(c, "o2coarse", 7); set(c, "o2fine", 31); set(c, "o2pw", 0.3f); }, 60, 60.0, "hard sync");
    tune([](Zest& c) { set(c, "o1level", 0); set(c, "o2level", 1); set(c, "o2sync", 1); set(c, "o1coarse", -12); set(c, "o2coarse", 9); }, 72, 60.0, "hard sync follows oscillator 1 coarse");
    // a unison stack sits on the note: the centre voice is exact and the others are placed symmetrically
    tune([](Zest& c) { set(c, "o1uni", 7); set(c, "o1detune", 0); }, 60, 60.0, "unison of 7 with no detune");
    // detuned stacks: a spectral peak for each voice, exactly where the layout puts it (the outer voices at the full
    // detune, the inner ones bunched toward the middle)
    auto stack = [&](int voices, float detune, const std::vector<double>& cents) {
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        set(r.syn, "o1uni", (float)voices); set(r.syn, "o1detune", detune); set(r.syn, "o1spread", 0);
        r.on(0, 81);
        r.run(1 << 17);
        const int n = 1 << 16;
        const auto m = spectrum(r.L, 30000, n);
        double strongest = 0.0;
        for (double v : m) strongest = std::max(strongest, v);
        int peaks = 0;
        for (int b = (int)(keyToHz(79) * n / sr); b < (int)(keyToHz(83) * n / sr); ++b)
            if (m[(size_t)b] > 0.2 * strongest && m[(size_t)b] >= m[(size_t)b - 1] && m[(size_t)b] > m[(size_t)b + 1]) ++peaks;
        CHECK_MSG(peaks == voices, "unison of %d: %d peaks", voices, peaks);
        for (double c : cents) {
            const double hz = keyToHz(81.0 + c / 100.0);
            int best = (int)std::lround(hz * n / sr) - 5;
            for (int b = best; b <= (int)std::lround(hz * n / sr) + 5; ++b) if (m[(size_t)b] > m[(size_t)best]) best = b;
            CHECK_MSG(std::fabs(centsOff(best * sr / n, 81.0 + c / 100.0)) < 2.0 && m[(size_t)best] > 0.2 * strongest, "unison of %d: the voice at %+.1f cents is %.2f cents off", voices, c, centsOff(best * sr / n, 81.0 + c / 100.0));
        }
    };
    stack(2, 50.0f, {-50.0, 50.0});
    stack(3, 30.0f, {-30.0, 0.0, 30.0});
    stack(7, 60.0f, {-60.0, -60.0 * std::pow(2.0 / 3.0, 1.5), -60.0 * std::pow(1.0 / 3.0, 1.5), 0.0, 60.0 * std::pow(1.0 / 3.0, 1.5), 60.0 * std::pow(2.0 / 3.0, 1.5), 60.0});
    // a synced oscillator has no DC, whatever the ratio (it is worked out and taken away)
    for (int shape = 0; shape < Zest::NumShapes; ++shape)
        for (int coarse : {3, 7, 16}) {
            Rig r(sr);
            plain(r.syn, Zest::Saw);
            set(r.syn, "o1level", 0); set(r.syn, "o2level", 1); set(r.syn, "o2sync", 1); set(r.syn, "o2shape", (float)shape); set(r.syn, "o2coarse", (float)coarse); set(r.syn, "o2fine", 23); set(r.syn, "o2pw", 0.3f);
            r.on(0, 60);
            r.run(48000);
            double mean = 0.0;
            for (size_t i = 4800; i < 48000; ++i) mean += r.L[i];
            mean /= 43200.0;
            CHECK_MSG(std::fabs(mean) < 0.003 * peakOf(r.L), "synced shape %d, +%d: mean %.5f against a peak of %.3f", shape, coarse, mean, peakOf(r.L));
            CHECK(peakOf(r.L) > 0.1f);
        }
    {   // the same patch after the sample rate changes
        Rig r(44100.0);
        plain(r.syn, Zest::Saw);
        r.note(0, 20000, 60);
        r.run(44160);
        CHECK_NEAR(centsOff(fundHz(r.L, 0, 20000, keyToHz(60), 44100.0), 60), 0.0, 1.5);
        r.syn.prepare(96000.0);
        r.sr = 96000.0;
        r.L.clear(); r.R.clear(); r.pos = 0;
        r.on(0, 60);
        r.run(96000);
        CHECK_NEAR(centsOff(fundHz(r.L, 0, 96000, keyToHz(60), 96000.0), 60), 0.0, 1.5);
    }
}

TEST(zest_pitch_bend_and_per_note_fine_pitch) {
    const double sr = 48000.0;
    for (float bend : {2.0f, -12.0f, 0.37f}) {
        Rig r(sr);
        plain(r.syn, Zest::Triangle);
        r.bend = bend;
        r.on(0, 64);
        r.run(48000);
        CHECK_NEAR(centsOff(fundHz(r.L, 0, 48000, keyToHz(64.0 + bend), sr), 64), bend * 100.0, 1.5);
    }
    {   // a bend that moves while the note sounds is followed
        Rig r(sr);
        plain(r.syn, Zest::Triangle);
        r.on(0, 64);
        r.run(96000, [&](int64 at) { r.bend = at < 24000 ? 0.0f : std::min(2.0f, (float)(at - 24000) / 12000.0f); });
        CHECK_NEAR(centsOff(fundHz(r.L, 2000, 22000, keyToHz(64), sr), 64), 0.0, 1.5);
        const double mid = centsOff(fundHz(r.L, 28000, 32000, keyToHz(65), sr), 64);
        CHECK_MSG(mid > 40.0 && mid < 130.0, "mid-bend %.1f cents", mid);
        CHECK_NEAR(centsOff(fundHz(r.L, 60000, 90000, keyToHz(66), sr), 64), 200.0, 1.5);
    }
    {   // the piano roll's fine pitch
        Rig r(sr);
        plain(r.syn, Zest::Saw);
        NoteEvent e; e.key = 60; e.fineCents = 50.0f;
        r.onEv(0, e);
        r.run(48000);
        CHECK_NEAR(centsOff(fundHz(r.L, 0, 48000, keyToHz(60.5), sr), 60), 50.0, 1.5);
    }
}

// ------------------------------------------------------------------ aliasing

TEST(zest_oscillators_are_band_limited) {
    const int n = 1 << 15;
    const char* names[] = {"sine", "triangle", "saw", "pulse", "twin saw"};
    for (double sr : {44100.0, 48000.0})
        for (int shape = 0; shape < Zest::NumShapes; ++shape)
            for (int key : {84, 96, 108}) {
                Rig r(sr);
                plain(r.syn, shape);
                set(r.syn, "o1pw", 0.3f);
                r.on(0, key);
                r.run(1 << 16);
                const double all = aliasDb(r.L, 16384, n, keyToHz(key), sr), audible = aliasDb(r.L, 16384, n, keyToHz(key), sr, 16000.0),
                             below = aliasDb(r.L, 16384, n, keyToHz(key), sr, keyToHz(key) - 150.0);
                if (sr == 48000.0) printf("    %-9s key %3d: aliases %6.1f dB (whole band), %6.1f dB (below 16 kHz), %6.1f dB (below the fundamental)\n", names[shape], key, all, audible, below);
                CHECK_MSG(all < (key == 108 ? -45.0 : -50.0), "%s key %d at %.0f: %.1f dB", names[shape], key, sr, all);
                CHECK_MSG(audible < (key == 108 ? -52.0 : -57.0), "%s key %d at %.0f: %.1f dB below 16 kHz", names[shape], key, sr, audible);
                CHECK_MSG(below < -78.0, "%s key %d at %.0f: %.1f dB below the fundamental", names[shape], key, sr, below);
            }
    // hard sync, the harshest case: the result is periodic at oscillator 1's pitch, so anything between its harmonics is aliasing
    for (int shape = 0; shape < Zest::NumShapes; ++shape) {
        Rig r;
        plain(r.syn, Zest::Saw);
        set(r.syn, "o1level", 0); set(r.syn, "o2level", 1); set(r.syn, "o2shape", (float)shape); set(r.syn, "o2sync", 1); set(r.syn, "o2coarse", 7); set(r.syn, "o2fine", 31); set(r.syn, "o2pw", 0.3f);
        r.on(0, 84);
        r.run(1 << 16);
        const double all = aliasDb(r.L, 16384, n, keyToHz(84), r.sr), audible = aliasDb(r.L, 16384, n, keyToHz(84), r.sr, 16000.0);
        printf("    synced %-9s key  84: aliases %6.1f dB (whole band), %6.1f dB (below 16 kHz)\n", names[shape], all, audible);
        CHECK_MSG(all < -48.0 && audible < -55.0, "synced %s: %.1f dB, %.1f dB below 16 kHz", names[shape], all, audible);
    }
    {   // the brightest patch: two seven-voice saw stacks, wide detune, high up. Nothing may fold down below the note.
        Rig r;
        plain(r.syn, Zest::Saw);
        set(r.syn, "o1uni", 7); set(r.syn, "o1detune", 30); set(r.syn, "o2uni", 7); set(r.syn, "o2level", 1); set(r.syn, "o2detune", 45);
        r.on(0, 96);
        r.run(1 << 16);
        const auto m = spectrum(r.L, 16384, n);
        double top = 0.0, under = 0.0;
        for (int b = 8; b < n / 2; ++b) {
            top = std::max(top, m[(size_t)b]);
            if (b * r.sr / n < keyToHz(96) * 0.9) under = std::max(under, m[(size_t)b]);
        }
        printf("    7+7 unison saws key 96: %.1f dB below the note\n", db(under / top));
        CHECK_MSG(db(under / top) < -65.0, "%.1f dB", db(under / top));
    }
    {   // a pulse whose width is being modulated quickly stays clean below the note too
        Rig r;
        plain(r.syn, Zest::Pulse);
        set(r.syn, "l1rate", 7); set(r.syn, "m1src", Zest::SrcLfo1); set(r.syn, "m1dst", Zest::DstPulseWidth); set(r.syn, "m1amt", 0.8f);
        r.on(0, 96);
        r.run(1 << 16);
        const auto m = spectrum(r.L, 16384, n);
        double top = 0.0, under = 0.0;
        for (int b = 8; b < n / 2; ++b) {
            top = std::max(top, m[(size_t)b]);
            if (b * r.sr / n < keyToHz(96) * 0.8 && b * r.sr / n > 200.0) under = std::max(under, m[(size_t)b]);
        }
        CHECK_MSG(db(under / top) < -60.0, "pulse width modulation: %.1f dB below the note", db(under / top));
    }
}

// ------------------------------------------------------------------ note timing

TEST(zest_note_on_lands_on_the_exact_sample) {
    const double sr = 48000.0, bpm = 120.0;       // 25 samples per tick
    auto renderAt = [&](int tick) {
        Project p;
        p.global->tempo.set((float)bpm);
        auto ch = addZest(p);
        p.pattern().notesFor(ch.id).push_back(mk(tick, kPPQ, 60));
        return render(p, 24000);
    };
    auto ref = renderAt(0), late = renderAt(137);
    CHECK(ref && late);
    if (!ref || !late) return;
    const int64 at = (int64)std::llround(137 * samplesPerTick(bpm, sr));
    CHECK(at == 3425);
    CHECK(firstSound(ref->ch[0]) == 0);
    CHECK_MSG(firstSound(late->ch[0]) == at, "first sound at %lld, expected %lld", (long long)firstSound(late->ch[0]), (long long)at);
    // and it is the same note, just later
    double diff = 0.0;
    for (size_t i = 0; i + (size_t)at < late->ch[0].size(); ++i) diff = std::max(diff, (double)std::fabs(late->ch[0][i + (size_t)at] - ref->ch[0][i]));
    CHECK_MSG(diff < 1.0e-4, "shifted note differs by %g", diff);
    // every offset inside a quantum
    for (int offset : {0, 1, 15, 16, 17, 33, 63}) {
        Rig r;
        r.on(640 + offset, 72);
        r.run(1280);
        CHECK_MSG(firstSound(r.L) == 640 + offset, "offset %d: first sound at %lld", offset, (long long)firstSound(r.L));
    }
}

TEST(zest_portamento_and_slide_notes_glide) {
    const double sr = 48000.0, bpm = 120.0;
    const double spt = samplesPerTick(bpm, sr);
    auto at = [&](double step) { return (size_t)std::llround(step * kTicksPerStep * spt); };
    {   // the channel's own portamento
        Project p;
        p.global->tempo.set((float)bpm);
        auto ch = addZest(p);
        plain(*ch.synth, Zest::Triangle);
        ch.live->porta.set(1);
        ch.live->mono.set(1);
        ch.live->portaTime.set(200.0f);
        p.pattern().notesFor(ch.id) = {mk(0, 4 * kTicksPerStep, 60), mk(4 * kTicksPerStep, 8 * kTicksPerStep, 72)};
        auto out = render(p, (int64)at(12));
        CHECK(out != nullptr);
        if (!out) return;
        const double early = fundHz(out->ch[0], at(4) + 400, at(4) + 2400, keyToHz(62), sr);       // just after the second note starts
        const double late = fundHz(out->ch[0], at(4) + 12000, at(4) + 22000, keyToHz(72), sr);     // after the 200 ms glide
        CHECK_MSG(early > keyToHz(60) * 1.01 && early < keyToHz(66), "early pitch %.1f Hz", early);
        CHECK_MSG(std::fabs(centsOff(late, 72)) < 2.0, "late pitch %.1f Hz", late);
    }
    {   // slide notes bend the sounding notes of their colour and nothing else
        Project p;
        p.global->tempo.set((float)bpm);
        auto ch = addZest(p);
        plain(*ch.synth, Zest::Sine);
        Note held = mk(0, 16 * kTicksPerStep, 60);
        Note other = mk(0, 16 * kTicksPerStep, 72);
        other.color = 3;
        other.pan = 1.0f;
        held.pan = -1.0f;
        Note slide = mk(4 * kTicksPerStep, 4 * kTicksPerStep, 67);
        slide.slide = true;
        p.pattern().notesFor(ch.id) = {held, other, slide};
        sortNotes(p.pattern().notesFor(ch.id));
        auto out = render(p, (int64)at(16));
        CHECK(out != nullptr);
        if (!out) return;
        CHECK_NEAR(measureHz(out->ch[0], at(1), at(3.5), sr), keyToHz(60), 0.3);
        const double mid = measureHz(out->ch[0], at(5.5), at(6.5), sr);
        CHECK_MSG(mid > keyToHz(62) && mid < keyToHz(65.5), "mid-slide pitch %.2f Hz", mid);
        CHECK_NEAR(measureHz(out->ch[0], at(9), at(15), sr), keyToHz(67), 0.4);
        CHECK_NEAR(measureHz(out->ch[1], at(9), at(15), sr), keyToHz(72), 0.4);
        CHECK(peakOf(out->ch[1], at(1), at(15)) > 0.1f);
    }
    {   // the synth's own glide time, in poly mode: each new note comes from the last one played
        Rig r(sr);
        plain(r.syn, Zest::Triangle);
        set(r.syn, "glide", 250);
        r.note(0, 6000, 60);
        r.note(12000, 30000, 72);
        r.run(48000);
        const double early = fundHz(r.L, 12400, 14400, keyToHz(62), sr);
        CHECK_MSG(early > keyToHz(60.3) && early < keyToHz(66), "early pitch %.1f Hz", early);
        CHECK_NEAR(centsOff(fundHz(r.L, 26000, 40000, keyToHz(72), sr), 72), 0.0, 1.5);
    }
}

TEST(zest_mono_and_legato_modes) {
    const double sr = 48000.0;
    auto rig = [&](int mode) {
        auto r = std::make_unique<Rig>(sr);
        plain(r->syn, Zest::Saw);
        set(r->syn, "mode", (float)mode); set(r->syn, "glide", 100);
        set(r->syn, "f1type", Zest::LowPass24); set(r->syn, "f1cutoff", 300); set(r->syn, "f1env", 5); set(r->syn, "e2atk", 0); set(r->syn, "e2dec", 60); set(r->syn, "e2sus", 0);
        set(r->syn, "e1rel", 600);
        return r;
    };
    // how bright the sound is just after a moment: steepest step against level
    auto brightness = [&](const Rig& r, size_t a) { return db(maxStep(r.L, a, a + 480) / (rmsOf(r.L, a, a + 480) + 1e-9)); };
    {   // legato: overlapping notes share one voice, the pitch glides, the envelopes carry on
        auto r = rig(Zest::Legato);
        const uint32 a = r->on(0, 60), b = r->on(24000, 72);
        r->off(36000, b);       // let go of the second while the first is still down: back to the first
        r->off(60000, a);
        r->run(96000);
        CHECK(r->maxVoices == 1);
        CHECK_NEAR(centsOff(fundHz(r->L, 8000, 23000, keyToHz(60), sr), 60), 0.0, 2.0);
        const double early = fundHz(r->L, 24300, 26300, keyToHz(63), sr);
        CHECK_MSG(early > keyToHz(60.5) && early < keyToHz(68), "gliding up: %.1f Hz", early);
        CHECK_NEAR(centsOff(fundHz(r->L, 30000, 35800, keyToHz(72), sr), 72), 0.0, 2.0);
        CHECK_NEAR(centsOff(fundHz(r->L, 44000, 59000, keyToHz(60), sr), 60), 0.0, 2.0);
        // the filter envelope did not restart at the second note: it stays as dull as it was
        CHECK_MSG(brightness(*r, 24100) < brightness(*r, 100) - 6.0, "legato note is as bright (%.1f dB) as the first (%.1f dB)", brightness(*r, 24100), brightness(*r, 100));
        CHECK(lastSound(r->L) > 60000 && lastSound(r->L) < 96000);
    }
    {   // legato: a note that does not overlap starts afresh, on pitch at once
        auto r = rig(Zest::Legato);
        set(r->syn, "e1rel", 20);
        r->note(0, 6000, 60);
        r->note(24000, 20000, 72);
        r->run(48000);
        CHECK_NEAR(centsOff(fundHz(r->L, 28000, 42000, keyToHz(72), sr), 72), 0.0, 2.0);
        CHECK_MSG(toneAmp(r->L, 24100, 25100, keyToHz(72), sr) > 0.1 && toneAmp(r->L, 24100, 25100, keyToHz(60), sr) < 0.3 * toneAmp(r->L, 24100, 25100, keyToHz(72), sr), "no glide expected: %.3f at the new note, %.3f at the old", toneAmp(r->L, 24100, 25100, keyToHz(72), sr), toneAmp(r->L, 24100, 25100, keyToHz(60), sr));
        CHECK(std::fabs(brightness(*r, 24100) - brightness(*r, 100)) < 3.0);
    }
    {   // mono: the envelopes restart on every note, the pitch always glides, still one voice
        auto r = rig(Zest::Mono);
        const uint32 a = r->on(0, 60), b = r->on(24000, 72);
        r->off(40000, a);
        r->off(60000, b);
        r->run(96000);
        CHECK(r->maxVoices == 1);
        const double early = fundHz(r->L, 24300, 26300, keyToHz(63), sr);
        CHECK_MSG(early > keyToHz(60.5) && early < keyToHz(68), "gliding up: %.1f Hz", early);
        CHECK_NEAR(centsOff(fundHz(r->L, 32000, 59000, keyToHz(72), sr), 72), 0.0, 2.0);
        CHECK_MSG(std::fabs(brightness(*r, 24100) - brightness(*r, 100)) < 4.0, "retriggered note %.1f dB, first %.1f dB", brightness(*r, 24100), brightness(*r, 100));
        // the retrigger starts from the level the note is at: no click
        CHECK(maxStep(r->L, 23990, 24010) < 1.2f * maxStep(r->L, 0, 2400));
    }
    {   // mono: a note played while the last one is still fading reuses its voice and glides from it
        auto r = rig(Zest::Mono);
        r->note(0, 6000, 60);
        r->note(12000, 30000, 67);
        r->run(48000);
        CHECK(r->maxVoices == 1);
        const double early = fundHz(r->L, 12300, 14300, keyToHz(62), sr);
        CHECK_MSG(early > keyToHz(60.3) && early < keyToHz(66), "gliding: %.1f Hz", early);
    }
    {   // the channel's own mono switch (it kills the previous note) works with the synth left in poly mode
        Project p;
        p.global->tempo.set(120.0f);
        auto ch = addZest(p);
        plain(*ch.synth, Zest::Saw);
        ch.live->mono.set(1);
        p.pattern().notesFor(ch.id) = {mk(0, 8 * kTicksPerStep, 60), mk(4 * kTicksPerStep, 8 * kTicksPerStep, 67)};
        auto out = render(p, 72000);
        CHECK(out != nullptr);
        if (!out) return;
        CHECK_NEAR(centsOff(fundHz(out->ch[0], 26000, 46000, keyToHz(67), sr), 67), 0.0, 2.0);      // the first note is gone
    }
}

// ------------------------------------------------------------------ per-note values

TEST(zest_velocity_pan_and_mod_x_y_are_heard) {
    const double sr = 48000.0;
    auto play = [&](const std::function<void(Zest&)>& setup, const std::function<void(NoteEvent&)>& noteSetup) {
        auto r = std::make_unique<Rig>(sr);
        plain(r->syn, Zest::Saw);
        setup(r->syn);
        NoteEvent e; e.key = 57; e.velocity = kDefaultVelocity;
        noteSetup(e);
        r->onEv(0, e);
        r->run(24000);
        return r;
    };
    auto level = [&](const Rig& r) { return rmsOf(r.L, 4000, 20000); };
    {   // velocity to level, and how much of it
        auto ref = play([](Zest&) {}, [](NoteEvent&) {});
        auto soft = play([](Zest& c) { set(c, "velsens", 1); }, [](NoteEvent& e) { e.velocity = 0.4f; });
        auto hard = play([](Zest& c) { set(c, "velsens", 1); }, [](NoteEvent& e) { e.velocity = 1.0f; });
        auto flat = play([](Zest& c) { set(c, "velsens", 0); }, [](NoteEvent& e) { e.velocity = 0.4f; });
        const double v = 0.4 / kDefaultVelocity;
        CHECK_NEAR(level(*soft) / level(*ref), v * v, 0.01);
        CHECK_NEAR(level(*hard) / level(*ref), 1.27 * 1.27, 0.02);
        CHECK_NEAR(level(*flat) / level(*ref), 1.0, 0.001);
        // velocity 100 is the reference level: a single note of the untouched patch peaks between -12 and -6 dBFS
        Rig init(sr);
        init.note(0, 20000, 60);
        init.run(24000);
        CHECK_MSG(peakOf(init.L) > 0.25f && peakOf(init.L) < 0.5f, "peak %.3f", peakOf(init.L));
    }
    {   // velocity to cutoff
        auto setup = [](Zest& c) { set(c, "f1cutoff", 600); set(c, "velfilt", 1); };
        auto soft = play(setup, [](NoteEvent& e) { e.velocity = 0.4f; });
        auto hard = play(setup, [](NoteEvent& e) { e.velocity = 1.0f; });
        const double f5 = keyToHz(57) * 5;
        CHECK_MSG(toneAmp(hard->L, 4000, 20000, f5, sr) > 6.0 * toneAmp(soft->L, 4000, 20000, f5, sr), "fifth harmonic: hard %.4f, soft %.4f", toneAmp(hard->L, 4000, 20000, f5, sr), toneAmp(soft->L, 4000, 20000, f5, sr));
    }
    {   // per-note pan
        auto left = play([](Zest&) {}, [](NoteEvent& e) { e.pan = -1.0f; });
        auto half = play([](Zest&) {}, [](NoteEvent& e) { e.pan = 0.5f; });
        CHECK(peakOf(left->R) == 0.0f && peakOf(left->L) > 0.2f);
        CHECK_NEAR(rmsOf(half->L, 4000, 20000) / rmsOf(half->R, 4000, 20000), 0.5, 0.01);
    }
    {   // mod X opens and closes the filter, mod Y raises and lowers the resonance (their default meaning)
        auto setup = [](Zest& c) { set(c, "f1cutoff", 1100); set(c, "f1reso", 0.3f); };        // the cutoff on the fifth harmonic
        const double f3 = keyToHz(57) * 3, f15 = keyToHz(57) * 15;
        auto mid = play(setup, [](NoteEvent&) {});
        auto shut = play(setup, [](NoteEvent& e) { e.modX = 0.2f; });       // three octaves down: 137 Hz
        auto open = play(setup, [](NoteEvent& e) { e.modX = 0.9f; });       // four octaves up
        CHECK_NEAR(db(toneAmp(shut->L, 4000, 20000, f3, sr) / toneAmp(mid->L, 4000, 20000, f3, sr)), -30.0, 3.0);       // about (137 / 660) squared
        CHECK_NEAR(db(toneAmp(open->L, 4000, 20000, f15, sr) / toneAmp(mid->L, 4000, 20000, f15, sr)), 19.0, 2.0);      // was (1100 / 3300) squared, now open
        // resonance: that fifth harmonic rings up or dies down. Q is 2 in the middle, 22.6 at the top, 0.71 at the bottom.
        const double f5 = keyToHz(57) * 5;
        auto dull = play(setup, [](NoteEvent& e) { e.modY = 0.2f; });
        auto ring = play(setup, [](NoteEvent& e) { e.modY = 0.85f; });
        const double a = toneAmp(mid->L, 4000, 20000, f5, sr);
        CHECK_NEAR(toneAmp(ring->L, 4000, 20000, f5, sr) / a, (22.63 / 1.7) / (2.0 / 1.063), 0.5);
        CHECK_NEAR(toneAmp(dull->L, 4000, 20000, f5, sr) / a, 0.7071 / (2.0 / 1.063), 0.03);
        // both filters hear them
        auto two = [](Zest& c) { set(c, "f2cutoff", 1100); set(c, "fbal", 1); };
        auto mid2 = play(two, [](NoteEvent&) {});
        auto shut2 = play(two, [](NoteEvent& e) { e.modX = 0.2f; });
        CHECK(toneAmp(shut2->L, 4000, 20000, f3, sr) < 0.06 * toneAmp(mid2->L, 4000, 20000, f3, sr));
    }
    {   // once the matrix uses mod X and mod Y for something else, the default meaning steps aside
        auto setup = [](Zest& c) {
            set(c, "f1cutoff", 1100); set(c, "f1reso", 0.3f);
            set(c, "m1src", Zest::SrcModX); set(c, "m1dst", Zest::DstPan); set(c, "m1amt", 1);
            set(c, "m2src", Zest::SrcModY); set(c, "m2dst", Zest::DstAmp); set(c, "m2amt", 0.5f);
        };
        const double f3 = keyToHz(57) * 3, f5 = keyToHz(57) * 5;
        auto mid = play(setup, [](NoteEvent&) {});
        auto x = play(setup, [](NoteEvent& e) { e.modX = 0.2f; });      // -0.6: panned left, filter untouched
        auto y = play(setup, [](NoteEvent& e) { e.modY = 0.9f; });      // +0.8: 40 % louder, resonance untouched
        CHECK_NEAR(toneAmp(x->L, 4000, 20000, f3, sr) / toneAmp(mid->L, 4000, 20000, f3, sr), 1.0, 0.01);
        CHECK_NEAR(rmsOf(x->R, 4000, 20000) / rmsOf(x->L, 4000, 20000), 0.4, 0.01);
        CHECK_NEAR(toneAmp(y->L, 4000, 20000, f5, sr) / toneAmp(mid->L, 4000, 20000, f5, sr), 1.4, 0.01);
        CHECK_NEAR(toneAmp(y->L, 4000, 20000, f3, sr) / toneAmp(mid->L, 4000, 20000, f3, sr), 1.4, 0.01);
    }
}

// ------------------------------------------------------------------ voices

TEST(zest_voice_limit_holds) {
    const double sr = 48000.0;
    {   // a limit of 4: ten notes arrive one after another and never more than 4 sound (plus those fading out over 3 ms)
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        set(r.syn, "poly", 4);
        for (int i = 0; i < 10; ++i) r.on(i * 2400, 48 + i * 2);
        int worstSettled = 0;
        r.run(48000, [&](int64 at) { if (at % 2400 >= 256) worstSettled = std::max(worstSettled, r.syn.activeVoices()); });
        CHECK_MSG(worstSettled == 4, "%d voices sounding with a limit of 4", worstSettled);
        CHECK(r.maxVoices <= 5);
        // the notes that survive are the last four
        const int n = 1 << 14;
        const auto m = spectrum(r.L, 30000, n);
        CHECK(binPeak(m, keyToHz(48 + 18), sr, n) > 20.0 * binPeak(m, keyToHz(48 + 10), sr, n));
        CHECK(binPeak(m, keyToHz(48 + 12), sr, n) > 20.0 * binPeak(m, keyToHz(48), sr, n));
        // stealing fades the old note out: no step bigger than the sound itself makes
        for (int i = 4; i < 10; ++i) CHECK_MSG(maxStep(r.L, (size_t)i * 2400 - 4, (size_t)i * 2400 + 200) < 0.12f, "step %.3f at steal %d", maxStep(r.L, (size_t)i * 2400 - 4, (size_t)i * 2400 + 200), i);
    }
    {   // released notes are taken before held ones
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        set(r.syn, "poly", 3); set(r.syn, "e1rel", 3000);
        r.on(0, 48);
        r.note(100, 1000, 55);
        r.on(200, 60);
        r.on(6000, 64);         // one too many: the released 55 goes, the held 48 stays
        r.run(24000);
        const int n = 1 << 13;
        const auto m = spectrum(r.L, 12000, n);
        CHECK(binPeak(m, keyToHz(48), sr, n) > 20.0 * binPeak(m, keyToHz(55), sr, n));
        CHECK(binPeak(m, keyToHz(64), sr, n) > 20.0 * binPeak(m, keyToHz(55), sr, n));
    }
    {   // unison does not use up polyphony: 16 notes of 7 + 7 voices are 16 notes
        Rig r(sr);
        set(r.syn, "o1uni", 7); set(r.syn, "o2uni", 7); set(r.syn, "o2level", 0.8f);
        for (int i = 0; i < 20; ++i) r.on(i * 640, 40 + i * 2);
        r.run(20 * 640 + 6400);
        CHECK(r.syn.activeVoices() == 16);
        CHECK(allFinite(r.L));
    }
    {   // the limit goes up to 32
        Rig r(sr);
        set(r.syn, "poly", 32);
        for (int i = 0; i < 40; ++i) r.on(i * 640, 30 + i * 2);
        r.run(40 * 640 + 6400);
        CHECK(r.syn.activeVoices() == 32);
    }
    {   // Kill fades everything out in 3 ms; KillNote takes one note; AllOff releases
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        const uint32 a = r.on(0, 60);
        r.on(0, 67);
        r.ev(12000, NoteEvent::KillNote, a);
        r.ev(24000, NoteEvent::Kill);
        r.run(36000);
        CHECK(r.syn.activeVoices() == 0);
        const int64 last = lastSound(r.L);
        CHECK_MSG(last >= 24000 + 100 && last <= 24000 + 150, "sound ends %lld samples after the kill", (long long)(last - 24000));
        CHECK(maxStep(r.L, 23990, 24200) < 1.3f * maxStep(r.L, 20000, 23990));
        CHECK(maxStep(r.L, 11990, 12200) < 1.3f * maxStep(r.L, 8000, 11990));
        const int n = 1 << 13;
        const auto m = spectrum(r.L, 14000, n);
        CHECK(binPeak(m, keyToHz(67), sr, n) > 50.0 * binPeak(m, keyToHz(60), sr, n));
        Rig q(sr);
        q.on(0, 60); q.on(0, 64);
        q.ev(4800, NoteEvent::AllOff);
        q.run(48000);
        CHECK(q.syn.activeVoices() == 0 && lastSound(q.L) > 6000 && lastSound(q.L) < 40000);
    }
}

TEST(zest_note_ends_fall_smoothly_to_silence) {
    const double sr = 48000.0;
    Zest proto;
    const auto presets = proto.factoryPresets();
    for (const char* name : {"Init Saw", "Acid Squelch", "Stadium Saw Stack", "Funk Clav", "Drawbar Organ", "Sub Drop Impact", "Wind Over Wires"}) {
        Rig r(sr);
        r.syn.loadState(presetNamed(presets, name)->state);
        for (const char* fx : {"drvon", "choon", "dlyon", "revon"}) set(r.syn, fx, 0);
        set(r.syn, "e1rel", 80);
        r.note(0, 12000, 60);
        r.run(48000);
        const int64 last = lastSound(r.L);
        CHECK_MSG(last > 12000 && last < 12000 + 12000, "%s: sound ends at %lld", name, (long long)last);
        if (last < 600) continue;
        const float peak = peakOf(r.L, 0, 12000);
        // the last 2 ms are at least 54 dB down and nothing jumps on the way out
        CHECK_MSG(peakOf(r.L, (size_t)last - 96, (size_t)last + 1) < peak * 0.002f, "%s: tail ends at %.5f of the peak", name, peakOf(r.L, (size_t)last - 96, (size_t)last + 1) / peak);
        CHECK_MSG(peakOf(r.L, (size_t)last - 480, (size_t)last + 1) < peak * 0.02f, "%s: 10 ms before the end %.4f of the peak", name, peakOf(r.L, (size_t)last - 480, (size_t)last + 1) / peak);
        CHECK(r.syn.activeVoices() == 0);
        for (size_t i = (size_t)last + 1; i < r.L.size(); ++i) if (r.L[i] != 0.0f || r.R[i] != 0.0f) { CHECK_MSG(false, "%s: not silent after the note", name); break; }
    }
    {   // the shortest release still ends without a step
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        set(r.syn, "e1rel", 1);
        r.note(0, 10007, 81);
        r.run(24000);
        CHECK_MSG(maxStep(r.L, 10000, 11000) < 1.2f * maxStep(r.L, 5000, 10000), "step %.4f against %.4f", maxStep(r.L, 10000, 11000), maxStep(r.L, 5000, 10000));
        CHECK(lastSound(r.L) < 10200);
    }
    {   // and the start of a note does not click either, even with the attack at zero: the first samples rise from nothing
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        set(r.syn, "o1phase", 0);       // free: the oscillator may be anywhere in its cycle
        r.on(0, 45);
        r.run(4800);
        CHECK_MSG(std::fabs(r.L[0]) < 0.03f && std::fabs(r.L[1]) < 0.06f, "first samples %.4f %.4f", r.L[0], r.L[1]);
        CHECK_MSG(maxStep(r.L, 0, 48) < 0.08f, "largest step in the first millisecond %.4f", maxStep(r.L, 0, 48));
    }
}

// ------------------------------------------------------------------ filters

TEST(zest_filters_have_the_right_response) {
    const double sr = 48000.0, hz = 440.0;
    auto gain = [&](const std::function<void(Zest&)>& setup, int key = 69) {
        auto amp = [&](bool with) {
            Rig r(sr);
            plain(r.syn, Zest::Sine);
            if (with) setup(r.syn);
            r.on(0, key);
            r.run(24000);
            return toneAmp(r.L, 8000, 24000, keyToHz(key), sr);
        };
        return db(amp(true) / amp(false));
    };
    const double two = -10.0 * std::log10(1.0 + 256.0), four = -10.0 * std::log10(1.0 + 65536.0);      // two and four poles, two octaves out
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1cutoff", (float)hz / 4); }), two, 0.3);
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1type", Zest::LowPass24); set(c, "f1cutoff", (float)hz / 4); }), four, 0.4);
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1type", Zest::LowPass24); set(c, "f1cutoff", (float)hz); }), -3.01, 0.2);
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1type", Zest::HighPass); set(c, "f1cutoff", (float)hz * 4); }), two, 0.3);
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1type", Zest::BandPass); set(c, "f1cutoff", (float)hz); }), 0.0, 0.2);
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1type", Zest::BandPass); set(c, "f1cutoff", (float)hz / 4); }), -10.0 * std::log10(1.0 + 0.5 * 3.75 * 3.75), 0.3);
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1type", Zest::BandPass); set(c, "f1cutoff", (float)hz * 4); }), -10.0 * std::log10(1.0 + 0.5 * 3.75 * 3.75), 0.3);
    CHECK(gain([&](Zest& c) { set(c, "f1type", Zest::Notch); set(c, "f1cutoff", (float)hz); }) < -45.0);
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1type", Zest::Notch); set(c, "f1cutoff", (float)hz / 4); }), db(15.0 / std::sqrt(225.0 + 32.0)), 0.2);
    // resonance: Q 4 at half way, 22.6 at full, with part of the gain taken back
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1cutoff", (float)hz); set(c, "f1reso", 0.5f); }), db(4.0 / 1.175), 0.3);
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1cutoff", (float)hz); set(c, "f1reso", 1.0f); }), db(22.63 / 1.7), 0.4);
    CHECK(gain([&](Zest& c) { set(c, "f1type", Zest::LowPass24); set(c, "f1cutoff", (float)hz); set(c, "f1reso", 1.0f); }) > 15.0);
    // key tracking: at 100 % the cutoff stays the same interval from the note
    for (int key : {48, 60, 84})
        CHECK_NEAR(gain([&](Zest& c) { set(c, "f1cutoff", (float)keyToHz(60) / 4); set(c, "f1key", 1); }, key), two, 0.4);
    CHECK(gain([&](Zest& c) { set(c, "f1cutoff", (float)keyToHz(60) / 4); set(c, "f1key", 0); }, 84) < two - 20.0);
    // envelope amount, in octaves (the filter envelope held at full)
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1cutoff", (float)hz / 16); set(c, "f1env", 2); set(c, "e2atk", 0); set(c, "e2sus", 1); }), two, 0.3);
    CHECK_NEAR(gain([&](Zest& c) { set(c, "f1cutoff", (float)hz); set(c, "f1env", -2); set(c, "e2atk", 0); set(c, "e2sus", 1); }), two, 0.3);
    // routing: serial runs filter 1 into filter 2, the balance says how much of filter 2 is in the path
    auto serial = [&](float bal) { return gain([&, bal](Zest& c) { set(c, "f1cutoff", (float)hz / 4); set(c, "f2cutoff", (float)hz / 4); set(c, "fbal", bal); }); };
    CHECK_NEAR(serial(0.0f), two, 0.3);
    CHECK_NEAR(serial(1.0f), 2.0 * two, 0.6);
    // parallel: the balance crossfades from filter 1 alone, through both, to filter 2 alone
    auto parallel = [&](float bal) { return gain([&, bal](Zest& c) { set(c, "froute", 1); set(c, "f1cutoff", (float)hz / 4); set(c, "f2type", Zest::BandPass); set(c, "f2cutoff", (float)hz); set(c, "fbal", bal); }); };
    CHECK_NEAR(parallel(0.0f), two, 0.3);
    CHECK_NEAR(parallel(1.0f), 0.0, 0.2);
    CHECK_NEAR(parallel(0.75f), 0.0, 0.5);       // filter 2 at full, filter 1 at half and 24 dB down
    // drive: the same sound, squashed and with odd harmonics added; at zero it is exactly out of the path
    {
        Rig clean(sr), driven(sr);
        for (Rig* r : {&clean, &driven}) { plain(r->syn, Zest::Sine); r->on(0, 57); }
        set(driven.syn, "f1drive", 1.0f);
        clean.run(24000); driven.run(24000);
        const double f = keyToHz(57);
        CHECK(toneAmp(clean.L, 8000, 24000, 3 * f, sr) < 1.0e-4 * toneAmp(clean.L, 8000, 24000, f, sr));
        CHECK_MSG(toneAmp(driven.L, 8000, 24000, 3 * f, sr) > 0.05 * toneAmp(driven.L, 8000, 24000, f, sr), "third harmonic %.4f of the fundamental", toneAmp(driven.L, 8000, 24000, 3 * f, sr) / toneAmp(driven.L, 8000, 24000, f, sr));
        CHECK(toneAmp(driven.L, 8000, 24000, 2 * f, sr) < 1.0e-3 * toneAmp(driven.L, 8000, 24000, f, sr));
        CHECK(peakOf(driven.L) < 2.0f * peakOf(clean.L) && peakOf(driven.L) > 0.3f * peakOf(clean.L));
    }
}

TEST(zest_filter_sweeps_do_not_zip_and_stay_stable) {
    const double sr = 48000.0;
    const int n = 1 << 15;
    // A sine through a filter (or a gain) that is being swept is still just that sine: any stepping in the sweep would
    // show up as a hash of sidebands right across the spectrum.
    struct Sweep { const char* id; float from, to; bool expo; std::function<void(Zest&)> setup; };
    const std::vector<Sweep> sweeps = {
        {"f1cutoff", 150.0f, 9000.0f, true, [](Zest&) {}},
        {"f1cutoff", 9000.0f, 60.0f, true, [](Zest& c) { set(c, "f1type", Zest::LowPass24); set(c, "f1reso", 0.6f); }},
        {"f1reso", 0.0f, 1.0f, false, [](Zest& c) { set(c, "f1cutoff", 700); }},
        {"f2cutoff", 200.0f, 5000.0f, true, [](Zest& c) { set(c, "fbal", 1); set(c, "f2type", Zest::BandPass); }},
        {"fbal", 0.0f, 1.0f, false, [](Zest& c) { set(c, "f2cutoff", 300); }},
        {"o1level", 1.0f, 0.1f, false, [](Zest&) {}},
        {"o1pan", -1.0f, 1.0f, false, [](Zest&) {}},
        {"volume", -30.0f, 0.0f, false, [](Zest&) {}},
        {"sublevel", 0.0f, 1.0f, false, [](Zest&) {}},
        {"m1amt", 0.0f, 1.0f, false, [](Zest& c) { set(c, "m1src", Zest::SrcVelocity); set(c, "m1dst", Zest::DstCutoff1); set(c, "f1cutoff", 100); }},
    };
    for (auto& sw : sweeps) {
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        sw.setup(r.syn);
        Param* p = r.syn.param(sw.id);
        r.on(0, 57);
        r.run(65536, [&](int64 at) {
            const float t = clampT((float)(at - 8000) / 40000.0f, 0.0f, 1.0f);
            p->set(sw.expo ? sw.from * std::pow(sw.to / sw.from, t) : sw.from + (sw.to - sw.from) * t);
        });
        const auto m = spectrum(r.L, 12000, n);
        double carrier = 0.0, hash = 0.0;
        for (int b = 1; b < n / 2; ++b) {
            carrier = std::max(carrier, m[(size_t)b]);
            if (b * sr / n > 2500.0) hash = std::max(hash, m[(size_t)b]);
        }
        CHECK_MSG(db(hash / carrier) < -80.0, "sweeping %s: %.1f dB of hash above 2.5 kHz", sw.id, db(hash / carrier));
        CHECK(allFinite(r.L));
    }
    {   // a step in a parameter is spread over the quantum, not jumped
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        r.on(0, 57);
        r.run(24000, [&](int64 at) { set(r.syn, "o1level", at < 12800 ? 1.0f : 0.2f); set(r.syn, "volume", at < 19200 ? 0.0f : -20.0f); });
        CHECK(maxStep(r.L, 12700, 13000) < 1.1f * maxStep(r.L, 8000, 12700));
        CHECK(maxStep(r.L, 19100, 19400) < 1.1f * maxStep(r.L, 14000, 19100));
        CHECK_NEAR(rmsOf(r.L, 14000, 19000) / rmsOf(r.L, 8000, 12000), 0.2, 0.002);
    }
    {   // a ramp starts at its target: a render made at -12 dB is the 0 dB render scaled, from the first sample
        Rig a(sr), b(sr);
        set(b.syn, "volume", -12); set(b.syn, "o1level", 0.4f); set(a.syn, "o1level", 0.4f);
        a.on(0, 60); b.on(0, 60);
        a.run(2048); b.run(2048);
        double worst = 0.0;
        for (size_t i = 0; i < a.L.size(); ++i) worst = std::max(worst, (double)std::fabs(b.L[i] - a.L[i] * dbToGain(-12.0f)));
        CHECK_MSG(worst < 1.0e-6, "difference %g", worst);
        CHECK(std::fabs(a.L[5]) > 1.0e-4f);
    }
    // Fast and violent modulation: nothing blows up. Cutoff, resonance, type and drive jump every quantum.
    for (double rate : {44100.0, 96000.0}) {
        Rig r(rate);
        set(r.syn, "o2level", 1); set(r.syn, "o2shape", Zest::Pulse); set(r.syn, "noiselevel", 0.5f); set(r.syn, "fbal", 0.5f); set(r.syn, "froute", 1);
        set(r.syn, "l1shape", Zest::LfoSquare); set(r.syn, "l1rate", 40); set(r.syn, "m1src", Zest::SrcLfo1); set(r.syn, "m1dst", Zest::DstCutoff1); set(r.syn, "m1amt", 1);
        set(r.syn, "l2shape", Zest::LfoSampleHold); set(r.syn, "l2rate", 40); set(r.syn, "m2src", Zest::SrcLfo2); set(r.syn, "m2dst", Zest::DstResonance); set(r.syn, "m2amt", 1);
        for (int k : {36, 60, 67, 96}) r.on(0, k);
        dsp::Noise rnd(77);
        r.run(96000, [&](int64) {
            for (const char* f : {"f1", "f2"}) {
                const std::string s = f;
                r.syn.param(s + "cutoff")->setNorm(rnd.uni());
                r.syn.param(s + "reso")->set(rnd.uni());
                r.syn.param(s + "type")->set(std::floor(rnd.uni() * 4.99f));
                r.syn.param(s + "drive")->set(rnd.uni());
            }
        });
        CHECK(allFinite(r.L) && allFinite(r.R));
        CHECK_MSG(peakOf(r.L) < 40.0f, "peak %.1f at %.0f Hz", peakOf(r.L), rate);
        CHECK(peakOf(r.L) > 0.05f);
    }
}

// ------------------------------------------------------------------ envelopes and LFOs

TEST(zest_envelopes_follow_their_times) {
    const double sr = 48000.0;
    auto env = [&](const std::function<void(Zest&)>& setup, float release = 0.5f) {
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        setup(r.syn);
        NoteEvent e; e.key = 96; e.release = release;       // 2093 Hz: 23 samples a cycle
        const uint32 id = r.onEv(0, e);
        r.off(48000, id);
        r.run(96000);
        std::vector<float> lv;
        for (size_t i = 0; i + 48 <= r.L.size(); i += 48) lv.push_back(peakOf(r.L, i, i + 48));       // one value per millisecond
        return lv;
    };
    {
        const auto e = env([](Zest& c) { set(c, "e1atk", 100); set(c, "e1dec", 200); set(c, "e1sus", 0.5f); set(c, "e1rel", 300); });
        const float top = *std::max_element(e.begin(), e.end());
        CHECK_MSG(e[50] > 0.6f * top && e[50] < 0.75f * top, "half way up the attack: %.3f", e[50] / top);     // a rounded rise: past half way at half time
        const size_t peakAt = (size_t)(std::max_element(e.begin(), e.end()) - e.begin());
        CHECK_MSG(e[97] > 0.96f * top && peakAt >= 98 && peakAt <= 102, "end of the attack: %.3f at 97 ms, the top is at %d ms", e[97] / top, (int)peakAt);
        CHECK(e[20] < 0.4f * top);
        CHECK_NEAR(e[300] / top, 0.5 + 0.5 * std::exp(-4.6), 0.01);            // the decay time is the time to within 1 % of the sustain
        CHECK_NEAR(e[200] / top, 0.5 + 0.5 * std::exp(-2.3), 0.015);
        CHECK_NEAR(e[900] / top, 0.5, 0.003);
        CHECK_NEAR(e[1150] / top, 0.5 * std::exp(-2.3), 0.01);                  // half way through the release
        CHECK_NEAR(e[1300] / top, 0.5 * std::exp(-4.6), 0.004);                 // the release time: 40 dB down
        CHECK(e[1500] == 0.0f);                                                  // and really gone soon after
    }
    {   // the per-note release value scales it: 0 is a quarter of the time, 1 is one and three quarters
        const auto fast = env([](Zest& c) { set(c, "e1rel", 400); }, 0.0f), slow = env([](Zest& c) { set(c, "e1rel", 400); }, 1.0f);
        const float top = *std::max_element(fast.begin(), fast.end());
        CHECK_NEAR(fast[1100] / top, std::exp(-4.6), 0.004);
        CHECK_NEAR(slow[1700] / top, std::exp(-4.6), 0.004);
    }
    {   // attack zero is still half a millisecond: no click, but nothing you would call slow
        const auto e = env([](Zest&) {});
        CHECK(e[1] > 0.95f * e[10]);
    }
    {   // a decay to a sustain of zero ends the note by itself
        Rig r(sr);
        set(r.syn, "e1dec", 100); set(r.syn, "e1sus", 0);
        r.on(0, 60);
        r.run(48000);
        CHECK(r.syn.activeVoices() == 0);
        CHECK(lastSound(r.L) > 4800 && lastSound(r.L) < 24000);
    }
    {   // the envelope times can be changed while a note sounds
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        set(r.syn, "e1rel", 5000);
        r.note(0, 4800, 72);
        r.run(48000, [&](int64 at) { if (at >= 24000) set(r.syn, "e1rel", 20); });
        CHECK(peakOf(r.L, 20000, 24000) > 0.2f);
        CHECK(lastSound(r.L) < 24000 + 3000);
    }
}

TEST(zest_lfos_run_at_their_rate_shape_and_tempo) {
    const double sr = 48000.0;
    auto tremolo = [&](const std::function<void(Rig&)>& setup, int64 from = 4800, int64 len = 96000) {
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        set(r.syn, "m1src", Zest::SrcLfo1); set(r.syn, "m1dst", Zest::DstAmp); set(r.syn, "m1amt", 0.5f);
        setup(r);
        r.on(0, 84);
        r.run(from + len + 4800);
        return wobbleHz(r.L, (size_t)from, (size_t)(from + len), sr);
    };
    CHECK_NEAR(tremolo([](Rig& r) { set(r.syn, "l1rate", 5); }), 5.0, 0.05);
    CHECK_NEAR(tremolo([](Rig& r) { set(r.syn, "l1rate", 0.5f); }, 4800, 48000 * 9), 0.5, 0.02);
    CHECK_NEAR(tremolo([](Rig& r) { set(r.syn, "l1rate", 23); }), 23.0, 0.2);
    for (int shape : {Zest::LfoTriangle, Zest::LfoSaw, Zest::LfoSquare})
        CHECK_NEAR(tremolo([&](Rig& r) { set(r.syn, "l1rate", 4); set(r.syn, "l1shape", (float)shape); }), 4.0, 0.05);
    // tempo sync: a quarter note at 120 is 2 Hz, at 150 it is 2.5 Hz; an eighth triplet at 120 is 6 Hz
    CHECK_NEAR(tremolo([](Rig& r) { set(r.syn, "l1sync", 1); set(r.syn, "l1div", Zest::DivQuarter); r.tempo = 120; }), 2.0, 0.03);
    CHECK_NEAR(tremolo([](Rig& r) { set(r.syn, "l1sync", 1); set(r.syn, "l1div", Zest::DivQuarter); r.tempo = 150; }), 2.5, 0.03);
    CHECK_NEAR(tremolo([](Rig& r) { set(r.syn, "l1sync", 1); set(r.syn, "l1div", Zest::DivEighthTriplet); r.tempo = 120; }), 6.0, 0.06);
    CHECK_NEAR(tremolo([](Rig& r) { set(r.syn, "l1sync", 1); set(r.syn, "l1div", Zest::DivQuarter); set(r.syn, "l1mode", 1); r.tempo = 120; r.playing = true; }), 2.0, 0.03);
    CHECK_NEAR(Zest::divisionBeats(Zest::Div1Bar), 4.0, 0.0);
    CHECK_NEAR(Zest::divisionBeats(Zest::DivSixteenthDotted), 0.375, 0.0);
    // LFOs 2 and 3 are the same machine
    CHECK_NEAR(tremolo([](Rig& r) { set(r.syn, "m1src", Zest::SrcLfo2); set(r.syn, "l2rate", 3); }), 3.0, 0.05);
    CHECK_NEAR(tremolo([](Rig& r) { set(r.syn, "m1src", Zest::SrcLfo3); set(r.syn, "l3rate", 7); }), 7.0, 0.07);

    // shapes, read off the pitch: an octave of modulation either way at 2 Hz
    auto pitchAt = [&](int shape, const std::vector<std::pair<int, int>>& windows) {
        Rig r(sr);
        plain(r.syn, Zest::Triangle);
        set(r.syn, "l1shape", (float)shape); set(r.syn, "l1rate", 2);
        set(r.syn, "m1src", Zest::SrcLfo1); set(r.syn, "m1dst", Zest::DstPitch); set(r.syn, "m1amt", 0.70711f);
        r.on(0, 72);
        r.run(96000);
        std::vector<double> out;
        for (auto& w : windows) out.push_back(centsOff(measureHz(r.L, (size_t)w.first, (size_t)w.second, sr), 72));
        return out;
    };
    {
        const auto sq = pitchAt(Zest::LfoSquare, {{2000, 10000}, {14000, 22000}, {26000, 34000}});
        CHECK_NEAR(sq[0], 1200.0, 3.0);
        CHECK_NEAR(sq[1], -1200.0, 3.0);
        CHECK_NEAR(sq[2], 1200.0, 3.0);
        const auto saw = pitchAt(Zest::LfoSaw, {{200, 700}, {11700, 12300}, {23000, 23600}});     // falls from the top
        CHECK_MSG(saw[0] > 1050.0 && std::fabs(saw[1]) < 80.0 && saw[2] < -1050.0, "saw: %.0f %.0f %.0f cents", saw[0], saw[1], saw[2]);
        const auto tri = pitchAt(Zest::LfoTriangle, {{5700, 6300}, {11700, 12300}, {17700, 18300}});  // up from the middle first
        CHECK_MSG(tri[0] > 1100.0 && std::fabs(tri[1]) < 80.0 && tri[2] < -1100.0, "triangle: %.0f %.0f %.0f cents", tri[0], tri[1], tri[2]);
        const auto sine = pitchAt(Zest::LfoSine, {{5700, 6300}, {11700, 12300}, {17700, 18300}});
        CHECK_MSG(sine[0] > 1150.0 && std::fabs(sine[1]) < 110.0 && sine[2] < -1150.0, "sine: %.0f %.0f %.0f cents", sine[0], sine[1], sine[2]);
        // sample and hold: steady inside each cycle (half a second), a new value in the next
        const auto sh = pitchAt(Zest::LfoSampleHold, {{2000, 11000}, {13000, 22000}, {26000, 35000}, {37000, 46000}, {50000, 59000}, {61000, 70000}});
        CHECK_NEAR(sh[0], sh[1], 2.0);
        CHECK_NEAR(sh[2], sh[3], 2.0);
        CHECK_NEAR(sh[4], sh[5], 2.0);
        CHECK_MSG(std::fabs(sh[0] - sh[2]) > 20.0 && std::fabs(sh[2] - sh[4]) > 20.0, "sample and hold steps: %.0f %.0f %.0f cents", sh[0], sh[2], sh[4]);
        for (double c : sh) CHECK(std::fabs(c) <= 1203.0);
    }
    {   // fade-in: no tremolo to begin with, all of it once the time is up
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        set(r.syn, "l1rate", 8); set(r.syn, "l1fade", 1000); set(r.syn, "m1src", Zest::SrcLfo1); set(r.syn, "m1dst", Zest::DstAmp); set(r.syn, "m1amt", 0.5f);
        r.on(0, 84);
        r.run(96000);
        auto depth = [&](size_t a, size_t b) { const auto e = rmsSeries(r.L, a, b, 96); return (*std::max_element(e.begin(), e.end()) - *std::min_element(e.begin(), e.end())) / *std::max_element(e.begin(), e.end()); };
        CHECK_MSG(depth(600, 6600) < 0.14, "depth in the first eighth of a second %.3f", depth(600, 6600));
        CHECK_MSG(depth(21000, 27000) > 0.3 && depth(21000, 27000) < 0.5, "depth half way in %.3f", depth(21000, 27000));
        CHECK_NEAR(depth(60000, 72000), 2.0 / 3.0, 0.02);                    // all of it: from 0.5 to 1.5 of the level
    }
    {   // per note: every note gets the LFO from its start. Global: the LFO runs on regardless of the notes.
        auto second = [&](int mode) {
            Rig r(sr);
            plain(r.syn, Zest::Sine);
            set(r.syn, "l1rate", 3); set(r.syn, "l1mode", (float)mode); set(r.syn, "m1src", Zest::SrcLfo1); set(r.syn, "m1dst", Zest::DstAmp); set(r.syn, "m1amt", 0.8f);
            set(r.syn, "e1rel", 5);
            r.note(0, 9600, 84);
            r.note(20032, 9600, 84);
            r.run(32000);
            return maxAbsDiff(cut(r.L, 0, 9600), cut(r.L, 20032, 29632));
        };
        CHECK_MSG(second(0) < 1.0e-4, "per-note LFO: the second note differs by %g", second(0));
        CHECK_MSG(second(1) > 0.05, "global LFO: the second note differs by only %g", second(1));
    }
    {   // a global LFO takes its place from the song position, so a render that starts later in the song (an export
        // of a section, or playback from a marker) gives the same sound as one that played from the top
        for (int synced = 0; synced < 2; ++synced) {
            auto section = [&](double startSample) {
                Rig r(sr);
                plain(r.syn, Zest::Sine);
                set(r.syn, "l1mode", 1); set(r.syn, "l1sync", (float)synced); set(r.syn, "l1div", Zest::DivHalfDotted); set(r.syn, "l1rate", 1.3f);
                set(r.syn, "l2mode", 1); set(r.syn, "l2shape", Zest::LfoSampleHold); set(r.syn, "l2sync", (float)synced); set(r.syn, "l2div", Zest::DivEighth); set(r.syn, "l2rate", 6.1f);
                set(r.syn, "m1src", Zest::SrcLfo1); set(r.syn, "m1dst", Zest::DstAmp); set(r.syn, "m1amt", 0.8f);
                set(r.syn, "m2src", Zest::SrcLfo2); set(r.syn, "m2dst", Zest::DstPan); set(r.syn, "m2amt", 0.8f);
                r.playing = true;
                r.tempo = 133.0;
                r.tick = startSample * r.tempo * kPPQ / (60.0 * sr);
                r.note((int64)(64000 - startSample), 30000, 84);
                r.run((int64)(100032 - startSample));
                return cut(r.L, r.L.size() - 36032, r.L.size());
            };
            const auto whole = section(0.0), part = section(38400.0);
            CHECK(peakOf(whole) > 0.1f);
            CHECK_MSG(maxAbsDiff(whole, part) < 2.0e-4, "%s global LFO: a later start differs by %g", synced ? "synced" : "free", maxAbsDiff(whole, part));
        }
    }
}

// ------------------------------------------------------------------ modulation matrix

TEST(zest_matrix_routes_every_source_and_destination) {
    const double sr = 48000.0;
    const double vel = kDefaultVelocity;
    auto play = [&](const std::function<void(Zest&)>& setup, int key = 69, int64 len = 48000, const std::function<void(NoteEvent&)>& noteSetup = {}) {
        auto r = std::make_unique<Rig>(sr);
        plain(r->syn, Zest::Sine);
        setup(r->syn);
        NoteEvent e; e.key = key; e.velocity = kDefaultVelocity;
        if (noteSetup) noteSetup(e);
        r->onEv(0, e);
        r->run(len);
        return r;
    };
    auto slot = [](Zest& c, int n, int src, int dst, float amt) {
        const std::string s = "m" + std::to_string(n);
        set(c, (s + "src").c_str(), (float)src); set(c, (s + "dst").c_str(), (float)dst); set(c, (s + "amt").c_str(), amt);
    };
    const double hz = keyToHz(69);
    auto amp = [&](const Rig& r, double f = 440.0) { return toneAmp(r.L, 12000, 44000, f, sr); };
    auto base = play([](Zest&) {});

    // ---- destinations, each driven by the velocity (a steady 100 / 127) ----
    {   // pitch: amount * |amount| * 24 semitones
        auto r = play([&](Zest& c) { slot(c, 1, Zest::SrcVelocity, Zest::DstPitch, 0.5f); });
        CHECK_NEAR(centsOff(measureHz(r->L, 4000, 44000, sr), 69), 0.25 * 2400.0 * vel, 1.5);
        auto d = play([&](Zest& c) { slot(c, 3, Zest::SrcVelocity, Zest::DstPitch, -1.0f); });
        CHECK_NEAR(centsOff(measureHz(d->L, 4000, 44000, sr), 69), -2400.0 * vel, 1.5);
    }
    {   // oscillator 1 pitch and oscillator 2 pitch move one oscillator each
        auto both = [&](int dst) {
            return play([&, dst](Zest& c) { set(c, "o2level", 1); set(c, "o2shape", Zest::Sine); set(c, "o2coarse", 19); set(c, "o1pan", -1); set(c, "o2pan", 1); slot(c, 2, Zest::SrcVelocity, dst, 0.5f); });
        };
        auto a = both(Zest::DstOsc1Pitch), b = both(Zest::DstOsc2Pitch);
        CHECK_NEAR(centsOff(measureHz(a->L, 4000, 44000, sr), 69), 600.0 * vel, 1.5);
        CHECK_NEAR(centsOff(measureHz(a->R, 4000, 44000, sr), 88), 0.0, 1.5);
        CHECK_NEAR(centsOff(measureHz(b->L, 4000, 44000, sr), 69), 0.0, 1.5);
        CHECK_NEAR(centsOff(measureHz(b->R, 4000, 44000, sr), 88), 600.0 * vel, 1.5);
    }
    {   // pulse width: a square has no second harmonic until its width moves (0.45 at full amount)
        auto sq = play([](Zest& c) { set(c, "o1shape", Zest::Pulse); });
        auto pw = play([&](Zest& c) { set(c, "o1shape", Zest::Pulse); slot(c, 1, Zest::SrcVelocity, Zest::DstPulseWidth, 0.5f); });
        CHECK(amp(*sq, 2 * hz) < 0.01 * amp(*sq, hz));
        const double w = 0.5 + 0.5 * 0.45 * vel;
        CHECK_NEAR(amp(*pw, 2 * hz) / amp(*pw, hz), std::fabs(std::sin(2 * kPi * w)) / (2.0 * std::sin(kPi * w)), 0.03);
    }
    {   // oscillator mix: up fades oscillator 1 out, down fades oscillator 2 out
        auto mixed = [&](float amt) {
            return play([&, amt](Zest& c) { set(c, "o2level", 1); set(c, "o2shape", Zest::Sine); set(c, "o2coarse", 12); slot(c, 4, Zest::SrcVelocity, Zest::DstOscMix, amt); });
        };
        auto none = mixed(0.0f), up = mixed(1.0f), down = mixed(-1.0f);
        CHECK_NEAR(amp(*up, hz) / amp(*none, hz), 1.0 - vel, 0.01);
        CHECK_NEAR(amp(*up, 2 * hz) / amp(*none, 2 * hz), 1.0, 0.01);
        CHECK_NEAR(amp(*down, 2 * hz) / amp(*none, 2 * hz), 1.0 - vel, 0.01);
        CHECK_NEAR(amp(*down, hz) / amp(*none, hz), 1.0, 0.01);
    }
    {   // noise level
        auto r = play([&](Zest& c) { set(c, "o1level", 0); slot(c, 1, Zest::SrcVelocity, Zest::DstNoise, 0.5f); });
        auto full = play([&](Zest& c) { set(c, "o1level", 0); set(c, "noiselevel", 1); });
        CHECK_NEAR(rmsOf(r->L, 12000, 44000) / rmsOf(full->L, 12000, 44000), 0.5 * vel, 0.03);
    }
    {   // cutoffs: amount * 8 octaves
        const double oct = 0.25 * 8.0 * vel, expect = -10.0 * std::log10(1.0 + std::pow(4.0 / std::pow(2.0, oct), 4.0));
        auto one = play([&](Zest& c) { set(c, "f1cutoff", 110); slot(c, 5, Zest::SrcVelocity, Zest::DstCutoff1, 0.25f); });
        CHECK_NEAR(db(amp(*one) / amp(*base)), expect, 0.3);
        auto twoF = play([&](Zest& c) { set(c, "f2cutoff", 110); set(c, "fbal", 1); slot(c, 5, Zest::SrcVelocity, Zest::DstCutoff2, 0.25f); });
        CHECK_NEAR(db(amp(*twoF) / amp(*base)), expect, 0.3);
        auto wrong = play([&](Zest& c) { set(c, "f1cutoff", 110); slot(c, 5, Zest::SrcVelocity, Zest::DstCutoff2, 0.25f); });
        CHECK_NEAR(db(amp(*wrong) / amp(*base)), -10.0 * std::log10(257.0), 0.3);
    }
    {   // resonance
        auto r = play([&](Zest& c) { set(c, "f1cutoff", 440); slot(c, 6, Zest::SrcVelocity, Zest::DstResonance, (float)(0.5 / vel)); });
        CHECK_NEAR(db(amp(*r) / amp(*base)), db(4.0 / 1.175), 0.3);
    }
    {   // filter balance
        auto r = play([&](Zest& c) { set(c, "f2cutoff", 110); slot(c, 7, Zest::SrcVelocity, Zest::DstBalance, 1.0f); });
        // serial: (1 - b) of filter 1 plus b of filter 2 after it, which is two octaves above its cutoff and nearly out of phase
        const double b = vel, re = 1.0 - 16.0, im = 4.0 / 0.7071, mag2 = re * re + im * im;
        CHECK_NEAR(db(amp(*r) / amp(*base)), db(std::hypot((1.0 - b) + b * re / mag2, -b * im / mag2)), 0.4);
    }
    {   // amplitude and pan
        auto a = play([&](Zest& c) { slot(c, 8, Zest::SrcVelocity, Zest::DstAmp, 0.5f); });
        CHECK_NEAR(amp(*a) / amp(*base), 1.0 + 0.5 * vel, 0.005);
        auto p = play([&](Zest& c) { slot(c, 8, Zest::SrcVelocity, Zest::DstPan, 1.0f); });
        CHECK_NEAR(rmsOf(p->L, 12000, 44000) / rmsOf(p->R, 12000, 44000), 1.0 - vel, 0.005);
        auto q = play([&](Zest& c) { slot(c, 8, Zest::SrcVelocity, Zest::DstPan, -0.5f); });
        CHECK_NEAR(rmsOf(q->R, 12000, 44000) / rmsOf(q->L, 12000, 44000), 1.0 - 0.5 * vel, 0.005);
    }
    {   // unison detune: 100 cents at full amount. Two voices either side of the note beat at the difference.
        auto r = play([&](Zest& c) { set(c, "o1uni", 2); set(c, "o1detune", 0); set(c, "o1spread", 0); slot(c, 1, Zest::SrcVelocity, Zest::DstDetune, 0.5f); }, 69, 96000);
        const double ct = 0.5 * 100.0 * vel;
        CHECK_NEAR(wobbleHz(r->L, 4800, 91200, sr, 48), hz * (std::pow(2.0, ct / 1200.0) - std::pow(2.0, -ct / 1200.0)), 0.3);
    }
    {   // LFO rates: four octaves either way at full amount
        for (int l = 0; l < 3; ++l) {
            auto r = play([&](Zest& c) {
                const std::string n = "l" + std::to_string(l + 1);
                set(c, (n + "rate").c_str(), 4);
                slot(c, 1, Zest::SrcLfo1 + l, Zest::DstAmp, 0.5f);
                slot(c, 2, Zest::SrcVelocity, Zest::DstLfo1Rate + l, 0.25f);
            }, 84, 110000);
            CHECK_NEAR(wobbleHz(r->L, 4800, 105600, sr), 4.0 * std::pow(2.0, 4.0 * 0.25 * vel), 0.1);
        }
    }
    // ---- sources ----
    {   // the two envelopes: the pitch starts an octave up and falls back as the envelope decays
        for (int e = 0; e < 2; ++e) {
            auto r = play([&](Zest& c) {
                const std::string n = e == 0 ? "e2" : "e3";
                set(c, (n + "atk").c_str(), 0); set(c, (n + "dec").c_str(), 300); set(c, (n + "sus").c_str(), 0);
                slot(c, 1, e == 0 ? Zest::SrcEnv2 : Zest::SrcEnv3, Zest::DstPitch, 0.70711f);
            });
            const double early = centsOff(measureHz(r->L, 100, 600, sr), 69), mid = centsOff(measureHz(r->L, 7000, 7400, sr), 69), late = centsOff(measureHz(r->L, 30000, 44000, sr), 69);
            CHECK_MSG(early > 1000.0 && early < 1200.0, "envelope %d: starts %.0f cents up", e + 2, early);
            CHECK_NEAR(mid, 1200.0 * std::exp(-4.6 * 0.5), 10.0);       // half way through the 300 ms decay
            CHECK_NEAR(late, 0.0, 1.5);
        }
        // a sustaining envelope holds its modulation
        auto r = play([&](Zest& c) { set(c, "e3atk", 0); set(c, "e3sus", 1); slot(c, 1, Zest::SrcEnv3, Zest::DstPitch, 0.70711f); });
        CHECK_NEAR(centsOff(measureHz(r->L, 4000, 44000, sr), 69), 1200.0, 1.5);
    }
    {   // key: plus and minus one over four octaves around middle C (key 60)
        auto hi = play([&](Zest& c) { slot(c, 1, Zest::SrcKey, Zest::DstAmp, 1.0f); }, 84), lo = play([&](Zest& c) { slot(c, 1, Zest::SrcKey, Zest::DstAmp, 1.0f); }, 36);
        auto hi0 = play([](Zest&) {}, 84), lo0 = play([](Zest&) {}, 36);
        CHECK_NEAR(rmsOf(hi->L, 12000, 44000) / rmsOf(hi0->L, 12000, 44000), 1.5, 0.005);
        CHECK_NEAR(rmsOf(lo->L, 12000, 44000) / rmsOf(lo0->L, 12000, 44000), 0.5, 0.005);
    }
    {   // mod X and mod Y, centred on 0.5
        auto x = play([&](Zest& c) { slot(c, 1, Zest::SrcModX, Zest::DstPitch, 0.70711f); }, 69, 48000, [](NoteEvent& e) { e.modX = 1.0f; });
        CHECK_NEAR(centsOff(measureHz(x->L, 4000, 44000, sr), 69), 1200.0, 1.5);
        auto y = play([&](Zest& c) { slot(c, 1, Zest::SrcModY, Zest::DstPitch, 0.70711f); }, 69, 48000, [](NoteEvent& e) { e.modY = 0.25f; });
        CHECK_NEAR(centsOff(measureHz(y->L, 4000, 44000, sr), 69), -600.0, 1.5);
        auto mid = play([&](Zest& c) { slot(c, 1, Zest::SrcModX, Zest::DstPitch, 1.0f); slot(c, 2, Zest::SrcModY, Zest::DstPitch, 1.0f); });
        CHECK_NEAR(centsOff(measureHz(mid->L, 4000, 44000, sr), 69), 0.0, 1.5);
    }
    {   // random: a new value for every note, the same values every time the song is rendered
        auto pans = [&]() {
            Rig r(sr);
            plain(r.syn, Zest::Sine);
            set(r.syn, "e1rel", 5);
            slot(r.syn, 1, Zest::SrcRandom, Zest::DstPan, 1.0f);
            std::vector<double> out;
            for (int i = 0; i < 12; ++i) r.note(i * 4800, 3000, 72);
            r.run(12 * 4800);
            for (int i = 0; i < 12; ++i) {
                const double l = rmsOf(r.L, (size_t)i * 4800 + 500, (size_t)i * 4800 + 2900), rr = rmsOf(r.R, (size_t)i * 4800 + 500, (size_t)i * 4800 + 2900);
                out.push_back((rr - l) / std::max(l, rr));
            }
            return out;
        };
        const auto a = pans(), b = pans();
        double lo = 1.0, hi = -1.0;
        int distinct = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            CHECK_NEAR(a[i], b[i], 1.0e-9);
            lo = std::min(lo, a[i]); hi = std::max(hi, a[i]);
            bool seen = false;
            for (size_t j = 0; j < i; ++j) seen = seen || std::fabs(a[i] - a[j]) < 0.01;
            distinct += seen ? 0 : 1;
        }
        CHECK_MSG(distinct >= 10 && lo < -0.4 && hi > 0.4, "%d distinct pans from %.2f to %.2f", distinct, lo, hi);
    }
    {   // several slots add up, and an empty slot (no source, or no destination) does nothing
        auto r = play([&](Zest& c) {
            slot(c, 1, Zest::SrcVelocity, Zest::DstAmp, 0.5f); slot(c, 2, Zest::SrcVelocity, Zest::DstAmp, 0.25f);
            slot(c, 3, Zest::SrcNone, Zest::DstAmp, 1.0f); slot(c, 4, Zest::SrcVelocity, Zest::DstNone, 1.0f);
        });
        CHECK_NEAR(amp(*r) / amp(*base), 1.0 + 0.75 * vel, 0.005);
    }
}

// ------------------------------------------------------------------ sources

TEST(zest_unison_spreads_in_stereo_and_keeps_its_level) {
    const double sr = 48000.0;
    auto play = [&](int voices, float spread, float detune) {
        auto r = std::make_unique<Rig>(sr);
        plain(r->syn, Zest::Saw);
        set(r->syn, "o1phase", 0);
        set(r->syn, "o1uni", (float)voices); set(r->syn, "o1spread", spread); set(r->syn, "o1detune", detune);
        r->on(0, 57);
        r->run(96000);
        return r;
    };
    auto width = [](const Rig& r) {
        double ll = 0, rr = 0, lr = 0;
        for (size_t i = 4800; i < r.L.size(); ++i) { ll += (double)r.L[i] * r.L[i]; rr += (double)r.R[i] * r.R[i]; lr += (double)r.L[i] * r.R[i]; }
        return 1.0 - lr / std::sqrt(ll * rr + 1e-30);
    };
    auto one = play(1, 1.0f, 30.0f);
    CHECK(width(*one) < 1.0e-9);
    for (int n = 2; n <= Zest::kUnison; ++n) {
        auto wide = play(n, 1.0f, 30.0f), narrow = play(n, 0.0f, 30.0f);
        CHECK_MSG(width(*wide) > 0.25, "%d voices: width %.3f", n, width(*wide));
        CHECK_MSG(width(*narrow) < 1.0e-9, "%d voices with no spread: width %.3g", n, width(*narrow));
        // the stack is about as loud as one voice, and balanced left to right
        const double level = db(rmsOf(narrow->L, 4800, 96000) / rmsOf(one->L, 4800, 96000));
        CHECK_MSG(std::fabs(level) < 2.0, "%d voices: %.2f dB against one", n, level);
        CHECK_MSG(std::fabs(db(rmsOf(wide->L, 4800, 96000) / rmsOf(wide->R, 4800, 96000))) < 1.5, "%d voices: left/right %.2f dB", n, db(rmsOf(wide->L, 4800, 96000) / rmsOf(wide->R, 4800, 96000)));
    }
    {   // phase reset: every note of a stack starts the same way; free: it does not
        auto twice = [&](int phase) {
            Rig r(sr);
            plain(r.syn, Zest::Saw);
            set(r.syn, "o1phase", (float)phase); set(r.syn, "o1uni", 5); set(r.syn, "e1rel", 5);
            r.note(0, 4800, 60);
            r.note(9600, 4800, 60);
            r.run(19200);
            return maxAbsDiff(cut(r.L, 0, 4800), cut(r.L, 9600, 14400));
        };
        CHECK(twice(1) < 1.0e-5);
        CHECK(twice(0) > 0.05);
    }
    {   // oscillator pan
        Rig r(sr);
        plain(r.syn, Zest::Saw);
        set(r.syn, "o1pan", -1); set(r.syn, "o2level", 1); set(r.syn, "o2pan", 1); set(r.syn, "o2coarse", 7);
        r.on(0, 60);
        r.run(48000);
        CHECK_NEAR(centsOff(fundHz(r.L, 0, 48000, keyToHz(60), sr), 60), 0.0, 1.5);
        CHECK_NEAR(centsOff(fundHz(r.R, 0, 48000, keyToHz(67), sr), 67), 0.0, 1.5);
    }
}

TEST(zest_sub_and_noise_sources) {
    const double sr = 48000.0;
    const int n = 1 << 14;
    auto noise = [&](int type) {
        auto r = std::make_unique<Rig>(sr);
        plain(r->syn, Zest::Saw);
        set(r->syn, "o1level", 0); set(r->syn, "noiselevel", 1); set(r->syn, "noisetype", (float)type);
        r->on(0, 60);
        r->run(48000 * 6);
        return r;
    };
    // power per hertz in a band, averaged over many windows
    auto band = [&](const Rig& r, double lo, double hi) {
        double sum = 0.0;
        int count = 0;
        for (size_t at = 4800; at + (size_t)n < r.L.size(); at += (size_t)n / 2) {
            const auto m = spectrum(r.L, at, n);
            for (int b = (int)(lo * n / sr); b <= (int)(hi * n / sr); ++b) { sum += m[(size_t)b] * m[(size_t)b]; ++count; }
        }
        return 10.0 * std::log10(sum / count);
    };
    auto white = noise(0), pink = noise(1), dark = noise(2);
    CHECK_NEAR(band(*white, 3500, 4500) - band(*white, 400, 600), 0.0, 1.0);
    CHECK_NEAR(band(*white, 14000, 18000) - band(*white, 400, 600), 0.0, 1.0);
    CHECK_NEAR(band(*pink, 3500, 4500) - band(*pink, 440, 560), -9.0, 1.5);           // 3 dB an octave
    CHECK_NEAR(band(*pink, 7000, 9000) - band(*pink, 110, 140), -18.0, 2.5);
    CHECK_NEAR(band(*dark, 3900, 4100) - band(*dark, 150, 250), -15.0, 1.5);          // one pole from 700 Hz
    for (auto* r : {white.get(), pink.get(), dark.get()}) {
        const double level = db(rmsOf(r->L, 4800, r->L.size()) / rmsOf(white->L, 4800, white->L.size()));
        CHECK_MSG(std::fabs(level) < 3.0, "noise level %.1f dB against white", level);
        double ll = 0, rr = 0, lr = 0;
        for (size_t i = 4800; i < r->L.size(); ++i) { ll += (double)r->L[i] * r->L[i]; rr += (double)r->R[i] * r->R[i]; lr += (double)r->L[i] * r->R[i]; }
        CHECK(std::fabs(lr / std::sqrt(ll * rr)) < 0.1);        // a different stream each side
        CHECK(peakOf(r->L) < 2.0f);
    }
    // the coloured noises have the same colour and the same level at any sample rate
    for (double rate : {44100.0, 96000.0})
        for (int type = 1; type <= 2; ++type) {
            Rig r(rate);
            plain(r.syn, Zest::Saw);
            set(r.syn, "o1level", 0); set(r.syn, "noiselevel", 1); set(r.syn, "noisetype", (float)type);
            r.on(0, 60);
            r.run((int64)rate * 6 / 64 * 64);
            const int len = rate > 50000.0 ? 1 << 15 : 1 << 14;
            auto bandAt = [&](double lo, double hi) {
                double sum = 0.0;
                int count = 0;
                for (size_t at = 4800; at + (size_t)len < r.L.size(); at += (size_t)len / 2) {
                    const auto m = spectrum(r.L, at, len);
                    for (int b = (int)(lo * len / rate); b <= (int)(hi * len / rate); ++b) { sum += m[(size_t)b] * m[(size_t)b]; ++count; }
                }
                return 10.0 * std::log10(sum / count);
            };
            const Rig& ref = type == 1 ? *pink : *dark;
            CHECK_NEAR(db(rmsOf(r.L, 9600, r.L.size()) / rmsOf(ref.L, 4800, ref.L.size())), 0.0, 1.0);
            if (type == 1) CHECK_NEAR(bandAt(3500, 4500) - bandAt(440, 560), -9.0, 1.5);
            else CHECK_NEAR(bandAt(3900, 4100) - bandAt(150, 250), -15.0, 1.5);
        }
    {   // the sub: a pure sine or a band-limited square, mixed in at its level
        Rig r(sr);
        plain(r.syn, Zest::Sine);
        set(r.syn, "sublevel", 0.5f);
        r.on(0, 69);
        r.run(48000);
        CHECK_NEAR(toneAmp(r.L, 12000, 44000, 220.0, sr) / toneAmp(r.L, 12000, 44000, 440.0, sr), 0.5, 0.005);
        CHECK(toneAmp(r.L, 12000, 44000, 660.0, sr) < 1.0e-4 * toneAmp(r.L, 12000, 44000, 440.0, sr));
        Rig s(sr);
        plain(s.syn, Zest::Sine);
        set(s.syn, "o1level", 0); set(s.syn, "sublevel", 1); set(s.syn, "subshape", 1); set(s.syn, "suboct", 1);
        s.on(0, 69);
        s.run(48000);
        CHECK_NEAR(toneAmp(s.L, 12000, 44000, 330.0, sr) / toneAmp(s.L, 12000, 44000, 110.0, sr), 1.0 / 3.0, 0.01);     // a square: odd harmonics at 1/n
        CHECK(toneAmp(s.L, 12000, 44000, 220.0, sr) < 0.002 * toneAmp(s.L, 12000, 44000, 110.0, sr));
    }
}

// ------------------------------------------------------------------ effects

TEST(zest_effects_do_what_they_say) {
    const double sr = 48000.0;
    // a short blip, so echoes and tails can be told from the note
    auto blip = [&](const std::function<void(Rig&)>& setup, int64 len = 96000) {
        auto r = std::make_unique<Rig>(sr);
        plain(r->syn, Zest::Sine);
        set(r->syn, "e1rel", 5);
        setup(*r);
        r->note(0, 960, 84);
        r->run(len);
        return r;
    };
    auto dry = blip([](Rig&) {});
    CHECK(lastSound(dry->L) < 2400);
    auto lagOfEcho = [&](const std::vector<float>& wet, int64 from, int64 to) {      // where the blip's echo sits in [from, to)
        double best = 0.0;
        int64 at = -1;
        for (int64 lag = from; lag < to; ++lag) {
            double s = 0.0;
            for (int64 i = 0; i < 1200; i += 2) s += (double)dry->L[(size_t)i] * wet[(size_t)(i + lag)];
            if (s > best) { best = s; at = lag; }
        }
        return at;
    };
    {   // delay, in milliseconds: the echo arrives when it says (the feedback path's tone filter adds about a sample)
        auto r = blip([](Rig& g) { set(g.syn, "dlyon", 1); set(g.syn, "dlysync", 0); set(g.syn, "dlytime", 100); set(g.syn, "dlyping", 0); set(g.syn, "dlymix", 1); set(g.syn, "dlyfb", 0.5f); });
        const int64 lag = lagOfEcho(r->L, 4000, 5600);
        CHECK_MSG(std::llabs(lag - 4800) <= 2, "first echo after %lld samples", (long long)lag);
        CHECK_MSG(std::llabs(lagOfEcho(r->L, 8800, 10400) - 9600) <= 4, "second echo after %lld samples", (long long)lagOfEcho(r->L, 8800, 10400));
        CHECK(maxAbsDiff(cut(dry->L, 0, 4000), cut(r->L, 0, 4000)) == 0.0);     // the dry sound is untouched
        // each repeat is the feedback amount quieter (a little more, through the tone filter)
        const double e1 = rmsOf(r->L, 4800, 6000), e2 = rmsOf(r->L, 9600, 10800), e3 = rmsOf(r->L, 14400, 15600);
        CHECK_MSG(e2 / e1 > 0.4 && e2 / e1 < 0.5 && e3 / e2 > 0.4 && e3 / e2 < 0.5, "repeat ratios %.3f %.3f", e2 / e1, e3 / e2);
        CHECK(e1 / rmsOf(dry->L, 0, 1200) > 0.75);
    }
    {   // delay, tempo synced: a dotted eighth at 120 is 0.375 s, at 150 it is 0.3 s
        for (double tempo : {120.0, 150.0}) {
            auto r = blip([&](Rig& g) { set(g.syn, "dlyon", 1); set(g.syn, "dlyping", 0); set(g.syn, "dlymix", 1); g.tempo = tempo; });
            const int64 expect = (int64)std::llround(0.75 * 60.0 / tempo * sr);
            const int64 lag = lagOfEcho(r->L, expect - 800, expect + 800);
            CHECK_MSG(std::llabs(lag - expect) <= 2, "tempo %.0f: echo after %lld samples, expected %lld", tempo, (long long)lag, (long long)expect);
        }
        auto q = blip([&](Rig& g) { set(g.syn, "dlyon", 1); set(g.syn, "dlyping", 0); set(g.syn, "dlymix", 1); set(g.syn, "dlydiv", Zest::DivSixteenth); });
        CHECK(std::llabs(lagOfEcho(q->L, 5000, 7000) - 6000) <= 2);
    }
    {   // ping-pong: left, then right, then left
        auto r = blip([](Rig& g) { set(g.syn, "dlyon", 1); set(g.syn, "dlysync", 0); set(g.syn, "dlytime", 100); set(g.syn, "dlymix", 1); set(g.syn, "dlyfb", 0.6f); });
        CHECK(rmsOf(r->L, 4800, 6000) > 20.0 * rmsOf(r->R, 4800, 6000));
        CHECK(rmsOf(r->R, 9600, 10800) > 20.0 * rmsOf(r->L, 9600, 10800));
        CHECK(rmsOf(r->L, 14400, 15600) > 20.0 * rmsOf(r->R, 14400, 15600));
    }
    {   // the delay cannot run away: feedback tops out below one
        auto r = blip([](Rig& g) { set(g.syn, "dlyon", 1); set(g.syn, "dlysync", 0); set(g.syn, "dlytime", 20); set(g.syn, "dlyfb", 0.95f); set(g.syn, "dlymix", 1); g.note(2000, 90000, 60); }, 480000);
        CHECK(allFinite(r->L) && peakOf(r->L) < 6.0f);
        CHECK(rmsOf(r->L, 440000, 480000) < 0.02 * rmsOf(r->L, 40000, 80000));
    }
    {   // reverb: a tail that decays at the set rate, wider than the mono note that feeds it, and then true silence
        for (float decay : {1.0f, 3.0f}) {
            auto r = blip([&](Rig& g) { set(g.syn, "revon", 1); set(g.syn, "revdecay", decay); set(g.syn, "revdamp", 0); set(g.syn, "revmix", 1); }, 48000 * 9);
            const double early = rmsOf(r->L, 12000, 24000), late = rmsOf(r->L, 12000 + (size_t)(decay * 24000), 24000 + (size_t)(decay * 24000));
            CHECK_MSG(db(late / early) < -24.0 && db(late / early) > -36.0, "decay %.0f s: %.1f dB over half of it (30 expected)", decay, db(late / early));
            CHECK(maxAbsDiff(cut(r->L, 12000, 24000), cut(r->R, 12000, 24000)) > 0.5 * peakOf(r->L, 12000, 24000));
            CHECK(peakOf(r->L, 2400, 48000) < peakOf(dry->L) && peakOf(r->L, 0, 2400) > 0.9f * peakOf(dry->L));      // at full mix the tail is no louder than the note
            const int64 last = std::max(lastSound(r->L), lastSound(r->R));
            CHECK_MSG(last > (int64)(decay * 0.8 * sr) && last < (int64)((2.0 * decay + 0.4) * sr), "decay %.0f s: goes silent after %.2f s", decay, (double)last / sr);
        }
        // damping takes the top off the tail
        auto bright = blip([](Rig& g) { set(g.syn, "revon", 1); set(g.syn, "revdamp", 0); set(g.syn, "revmix", 1); set(g.syn, "o1shape", Zest::Saw); });
        auto dull = blip([](Rig& g) { set(g.syn, "revon", 1); set(g.syn, "revdamp", 1); set(g.syn, "revmix", 1); set(g.syn, "o1shape", Zest::Saw); });
        auto top = [&](const Rig& r) {
            const auto m = spectrum(r.L, 24000, 1 << 14);
            double hi = 0, lo = 0;
            for (size_t b = 10; b < m.size(); ++b) (b * sr / (1 << 14) > 5000.0 ? hi : lo) += m[b] * m[b];
            return 10.0 * std::log10(hi / lo);
        };
        CHECK_MSG(top(*dull) < top(*bright) - 10.0, "damped tail %.1f dB of highs, undamped %.1f dB", top(*dull), top(*bright));
    }
    {   // chorus: a mono sound comes out moving and wide; at zero mix it is out of the path
        auto play = [&](float mix, float rate) {
            auto r = std::make_unique<Rig>(sr);
            plain(r->syn, Zest::Saw);
            set(r->syn, "choon", 1); set(r->syn, "chomix", mix); set(r->syn, "chorate", rate); set(r->syn, "chodepth", 1);
            r->on(0, 69);
            r->run(96000);
            return r;
        };
        Rig plainRig(sr);
        plain(plainRig.syn, Zest::Saw);
        plainRig.on(0, 69);
        plainRig.run(96000);
        auto off = play(0.0f, 1.0f), on = play(1.0f, 1.0f);
        CHECK(maxAbsDiff(off->L, plainRig.L) == 0.0 && maxAbsDiff(off->R, plainRig.R) == 0.0);
        CHECK(maxAbsDiff(on->L, on->R) > 0.1);
        CHECK(std::fabs(db(rmsOf(on->L, 4800, 96000) / rmsOf(plainRig.L, 4800, 96000))) < 2.5);
        // the comb it makes sweeps at the chorus rate: the level of one harmonic rises and falls with it
        std::vector<float> h;
        for (size_t at = 4800; at + 480 <= on->L.size(); at += 480) h.push_back((float)toneAmp(on->L, at, at + 480, 440.0 * 7, sr));
        const float hMax = *std::max_element(h.begin(), h.end()), hMin = *std::min_element(h.begin(), h.end());
        CHECK_MSG(hMin < 0.6f * hMax, "seventh harmonic moves between %.4f and %.4f", hMin, hMax);
    }
    {   // drive: odd harmonics, level held in check, tone control, and little aliasing
        auto play = [&](float amount, float tone, int key) {
            auto r = std::make_unique<Rig>(sr);
            plain(r->syn, Zest::Sine);
            set(r->syn, "drvon", 1); set(r->syn, "drvamt", amount); set(r->syn, "drvtone", tone);
            r->on(0, key);
            r->run(1 << 16);
            return r;
        };
        const double f = keyToHz(57);
        auto none = play(0.0f, 20000.0f, 57), hot = play(30.0f, 20000.0f, 57), muffled = play(30.0f, 500.0f, 57);
        CHECK(toneAmp(none->L, 12000, 60000, 3 * f, sr) < 0.02 * toneAmp(none->L, 12000, 60000, f, sr));      // 0 dB of drive is tanh of a 0.4 signal: all but clean
        CHECK(toneAmp(hot->L, 12000, 60000, 3 * f, sr) > 0.2 * toneAmp(hot->L, 12000, 60000, f, sr));
        CHECK(toneAmp(hot->L, 12000, 60000, 5 * f, sr) > 0.08 * toneAmp(hot->L, 12000, 60000, f, sr));
        CHECK(toneAmp(hot->L, 12000, 60000, 2 * f, sr) < 0.002 * toneAmp(hot->L, 12000, 60000, f, sr));
        CHECK_MSG(peakOf(hot->L) < 1.2f * peakOf(none->L) && rmsOf(hot->L, 12000, 60000) > 0.3 * rmsOf(none->L, 12000, 60000), "driven peak %.3f, clean %.3f", peakOf(hot->L), peakOf(none->L));
        CHECK_NEAR(toneAmp(muffled->L, 12000, 60000, 9 * f, sr) / toneAmp(hot->L, 12000, 60000, 9 * f, sr), 1.0 / std::sqrt(1.0 + (9 * f / 500.0) * (9 * f / 500.0)), 0.03);    // one pole at 500 Hz
        for (int key : {84, 96}) {
            auto r = play(24.0f, 20000.0f, key);
            const double a = aliasDb(r->L, 16384, 1 << 15, keyToHz(key), sr, 16000.0);
            printf("    drive at 24 dB on a sine at key %d: aliases %.1f dB below 16 kHz\n", key, a);
            CHECK_MSG(a < -45.0, "drive aliasing at key %d: %.1f dB", key, a);
        }
    }
    {   // nothing adds latency: with everything on, the note still starts on its sample and no later
        Rig r(sr);
        for (const char* fx : {"drvon", "choon", "dlyon", "revon"}) set(r.syn, fx, 1);
        r.on(1000, 72);
        r.run(4800);
        CHECK(firstSound(r.L) == 1000);
        CHECK(r.syn.latencySamples() == 0);
        // and the level of the first cycles is the dry note's (the effects are in from the first sample: no fade-in from dry)
        Rig d(sr);
        d.on(1000, 72);
        d.run(4800);
        CHECK(peakOf(r.L, 1000, 1400) > 0.4f * peakOf(d.L, 1000, 1400));
    }
    {   // switching an effect on or off in the middle of a note crossfades it: no click
        for (const char* fx : {"drvon", "choon", "dlyon", "revon"}) {
            Rig r(sr);
            plain(r.syn, Zest::Sine);
            set(r.syn, "drvamt", 6);
            r.on(0, 57);
            r.run(48000, [&](int64 at) { set(r.syn, fx, at >= 12800 && at < 32000 ? 1.0f : 0.0f); });
            const float normal = maxStep(r.L, 4000, 12000);
            CHECK_MSG(maxStep(r.L, 12700, 13100) < 1.5f * normal, "%s on: step %.4f against %.4f", fx, maxStep(r.L, 12700, 13100), normal);
            CHECK_MSG(maxStep(r.L, 31900, 32300) < 2.0f * normal, "%s off: step %.4f against %.4f", fx, maxStep(r.L, 31900, 32300), normal);
        }
    }
    {   // the scope tap holds the latest output (left and right averaged), for a spectrum display
        Rig r(sr);
        r.on(0, 60);
        r.run(6400);
        float scope[512];
        CHECK(r.syn.readScope(scope, 512) == 512);
        double worst = 0.0;
        for (int i = 0; i < 512; ++i) worst = std::max(worst, (double)std::fabs(scope[i] - 0.5f * (r.L[6400 - 512 + (size_t)i] + r.R[6400 - 512 + (size_t)i])));
        CHECK(worst < 1.0e-7 && peakOf(std::vector<float>(scope, scope + 512)) > 0.1f);
    }
    {   // the tail estimate covers what the effects really do
        Rig r(sr);
        set(r.syn, "revon", 1); set(r.syn, "revdecay", 2); set(r.syn, "dlyon", 1);
        CHECK(r.syn.tailSeconds() > 4.0 && r.syn.tailSeconds() < 60.0);
        Rig d(sr);
        CHECK(d.syn.tailSeconds() < 1.0);
    }
}

// ------------------------------------------------------------------ presets and state

TEST(zest_presets_all_sound_and_reload) {
    const double sr = 48000.0;
    Zest proto;
    const auto presets = proto.factoryPresets();
    CHECK_MSG(presets.size() >= 24, "%d presets", (int)presets.size());
    std::vector<std::string> names;
    std::vector<std::vector<double>> features;
    float loudestChord = 0.0f;
    std::string loudestName;
    for (auto& pr : presets) {
        CHECK_MSG(std::find(names.begin(), names.end(), pr.name) == names.end(), "two presets called %s", pr.name.c_str());
        names.push_back(pr.name);
        // every value in the table belongs to a real parameter and is inside its range
        for (auto& kv : pr.state["params"].items()) {
            const Param* p = proto.param(kv.first);
            CHECK_MSG(p != nullptr, "%s sets %s, which does not exist", pr.name.c_str(), kv.first.c_str());
            if (p) CHECK_MSG(kv.second.f() >= p->info.min && kv.second.f() <= p->info.max, "%s sets %s to %g, outside %g..%g", pr.name.c_str(), kv.first.c_str(), kv.second.f(), p->info.min, p->info.max);
        }
        // one note at velocity 100: around -9 dBFS at its loudest
        Rig r(sr);
        r.syn.loadState(pr.state);
        r.playing = true;
        r.note(0, 48000 * 9, 60);
        r.run(48000 * 11);
        CHECK_MSG(allFinite(r.L) && allFinite(r.R), "%s produced a non-finite sample", pr.name.c_str());
        const float peak = std::max(peakOf(r.L), peakOf(r.R));
        CHECK_MSG(gainToDb(peak) > -12.0f && gainToDb(peak) < -6.0f, "%s: one note peaks at %.1f dBFS", pr.name.c_str(), gainToDb(peak));
        CHECK_MSG(rmsOf(r.L, 0, 48000 * 9) > 0.004 && rmsOf(r.R, 0, 48000 * 9) > 0.004, "%s is all but silent (rms %.5f)", pr.name.c_str(), rmsOf(r.L, 0, 48000 * 9));
        // what it sounds like, roughly: eight octave bands, the level over time, and the stereo width
        std::vector<double> f;
        {
            const int n = 1 << 14;
            std::vector<double> bands(8, 1e-12);
            for (size_t at = 0; at + (size_t)n <= 48000 * 4; at += (size_t)n) {
                const auto m = spectrum(r.L, at, n);
                for (int b = 2; b < n / 2; ++b) {
                    const int band = clampT((int)std::floor(std::log2(b * sr / n / 80.0)), 0, 7);
                    bands[(size_t)band] += m[(size_t)b] * m[(size_t)b];
                }
            }
            double total = 0.0;
            for (double v : bands) total += v;
            for (double v : bands) f.push_back(10.0 * std::log10(v / total + 1e-9));
            for (size_t at : {0u, 1200u, 4800u, 12000u, 24000u, 48000u, 96000u, 192000u, 432000u, 440000u, 456000u})
                f.push_back(20.0 * std::log10(rmsOf(r.L, at, at + 2400) / peak + 1e-5));
            double ll = 0, rr = 0, lr = 0;
            for (size_t i = 0; i < 96000; ++i) { ll += (double)r.L[i] * r.L[i]; rr += (double)r.R[i] * r.R[i]; lr += (double)r.L[i] * r.R[i]; }
            f.push_back(30.0 * (1.0 - lr / std::sqrt(ll * rr + 1e-30)));
        }
        features.push_back(f);
        // a chord through the engine: sounds, finite, below +6 dBFS
        Project p;
        p.global->tempo.set(120.0f);
        auto ch = addZest(p);
        ch.synth->loadState(pr.state);
        for (int k : {48, 60, 67}) p.pattern().notesFor(ch.id).push_back(mk(0, kPPQ * 4, k));
        auto out = render(p, 48000 * 3);
        CHECK(out != nullptr);
        if (!out) return;
        const float chord = std::max(peakOf(out->ch[0]), peakOf(out->ch[1]));
        CHECK_MSG(allFinite(out->ch[0]) && allFinite(out->ch[1]), "%s: non-finite chord", pr.name.c_str());
        CHECK_MSG(chord > 0.02f && chord < 2.0f, "%s: chord peaks at %.2f", pr.name.c_str(), chord);
        if (chord > loudestChord) { loudestChord = chord; loudestName = pr.name; }
        // state round trip: a second instance loaded from the saved state is the same instrument
        const Json saved = ch.synth->saveState();
        CHECK(saved["type"].str() == "zest");
        Project q;
        q.global->tempo.set(120.0f);
        auto ch2 = addZest(q);
        ch2.synth->loadState(Json::parse(saved.dump()));
        CHECK(ch2.synth->saveState() == saved);
        for (int k : {48, 60, 67}) q.pattern().notesFor(ch2.id).push_back(mk(0, kPPQ * 4, k));
        auto again = render(q, 48000 * 3);
        CHECK_MSG(again && identical(*out, *again), "%s: the reloaded state renders differently", pr.name.c_str());
    }
    // no two presets are the same sound
    double closest = 1e9;
    size_t ca = 0, cb = 0;
    for (size_t a = 0; a < features.size(); ++a)
        for (size_t b = a + 1; b < features.size(); ++b) {
            double d = 0.0;
            for (size_t i = 0; i < features[a].size(); ++i) d += std::min(900.0, (features[a][i] - features[b][i]) * (features[a][i] - features[b][i]));
            d = std::sqrt(d / (double)features[a].size());
            if (d < closest) { closest = d; ca = a; cb = b; }
        }
    printf("    %d presets; the two most alike are \"%s\" and \"%s\" (%.1f dB apart, rms over spectrum, envelope and width)\n", (int)presets.size(), names[ca].c_str(), names[cb].c_str(), closest);
    CHECK_MSG(closest > 3.0, "\"%s\" and \"%s\" are nearly the same sound (%.2f dB)", names[ca].c_str(), names[cb].c_str(), closest);
    printf("    the loudest three-note chord of any preset peaks at %.1f dBFS (\"%s\")\n", gainToDb(loudestChord), loudestName.c_str());
    // the presets use what the synth has
    int withMatrix = 0, withUnison = 0, withFx = 0, mono = 0;
    for (auto& pr : presets) {
        const Json& ps = pr.state["params"];
        withMatrix += ps["m1src"].i() != 0 ? 1 : 0;
        withUnison += ps["o1uni"].i() > 1 || ps["o2uni"].i() > 1 ? 1 : 0;
        withFx += ps["drvon"].i() || ps["choon"].i() || ps["dlyon"].i() || ps["revon"].i() ? 1 : 0;
        mono += ps["mode"].i() != 0 ? 1 : 0;
    }
    CHECK(withMatrix >= 12 && withUnison >= 8 && withFx >= 15 && mono >= 4);
}

TEST(zest_sounds_the_same_at_every_sample_rate) {
    // The same note of the same preset at 44.1, 48 and 96 kHz: the level, the balance of lows to highs and the way it
    // dies away must not depend on the rate (times, filters, noise colours, effects and smoothing are all in real units).
    Zest proto;
    const auto presets = proto.factoryPresets();
    for (const char* name : {"Init Saw", "Acid Squelch", "Stadium Saw Stack", "Warm Analog Pad", "Crystal Pluck", "Tine Electric Piano", "Brass Section", "Vowel Choir Pad", "Sync Sweep Lead", "Sub Drop Impact"}) {
        struct Seen { double level, tail, low, mid, high; };
        std::vector<Seen> seen;
        for (double sr : {48000.0, 44100.0, 96000.0}) {
            Rig r(sr);
            r.syn.loadState(presetNamed(presets, name)->state);
            r.playing = true;
            r.note(0, (int64)(sr * 3.0), 60);
            r.run((int64)(sr * 5.0) / 64 * 64);
            Seen s;
            s.level = db(rmsOf(r.L, 0, (size_t)(sr * 3.0)) + rmsOf(r.R, 0, (size_t)(sr * 3.0)));
            s.tail = db(rmsOf(r.L, (size_t)(sr * 3.1), (size_t)(sr * 3.4)) + 1e-6);
            // three bands over exactly the same three seconds at each rate (filtered in the time domain)
            auto band = [&](double lo, double hi) {
                std::vector<float> x = cut(r.L, 0, (size_t)(sr * 3.0));
                for (int pass = 0; pass < 2; ++pass) {
                    dsp::Biquad hp, lp;
                    hp.highpass(lo, 0.707, sr);
                    lp.lowpass(hi, 0.707, sr);
                    for (float& v : x) v = lp.process(hp.process(v));
                }
                return db(rmsOf(x) + 1e-7);
            };
            s.low = band(20.0, 400.0); s.mid = band(400.0, 3000.0); s.high = band(3000.0, 12000.0);
            seen.push_back(s);
        }
        for (size_t i = 1; i < seen.size(); ++i) {
            const double sr = i == 1 ? 44100.0 : 96000.0;
            CHECK_MSG(std::fabs(seen[i].level - seen[0].level) < 1.5, "%s at %.0f Hz: level %.2f dB against %.2f dB at 48 kHz", name, sr, seen[i].level, seen[0].level);
            if (seen[0].tail > -80.0) CHECK_MSG(std::fabs(seen[i].tail - seen[0].tail) < 3.0, "%s at %.0f Hz: tail %.2f dB against %.2f dB", name, sr, seen[i].tail, seen[0].tail);
            CHECK_MSG(std::fabs((seen[i].mid - seen[i].low) - (seen[0].mid - seen[0].low)) < 2.0, "%s at %.0f Hz: mids against lows %.2f dB, %.2f dB at 48 kHz", name, sr, seen[i].mid - seen[i].low, seen[0].mid - seen[0].low);
            if (seen[0].high - seen[0].mid > -50.0)
                CHECK_MSG(std::fabs((seen[i].high - seen[i].mid) - (seen[0].high - seen[0].mid)) < 2.5, "%s at %.0f Hz: highs against mids %.2f dB, %.2f dB at 48 kHz", name, sr, seen[i].high - seen[i].mid, seen[0].high - seen[0].mid);
        }
    }
}

TEST(zest_parameters_groups_and_state) {
    Zest c;
    std::vector<std::string> ids;
    for (auto& p : c.params()) {
        CHECK_MSG(std::find(ids.begin(), ids.end(), p->info.id) == ids.end(), "two parameters called %s", p->info.id.c_str());
        ids.push_back(p->info.id);
        CHECK(!p->info.name.empty() && p->info.max > p->info.min && p->info.def >= p->info.min && p->info.def <= p->info.max);
        CHECK(!p->display().empty());
        if (p->info.steps > 0 && !p->info.choices.empty()) CHECK((int)p->info.choices.size() == p->info.steps);
    }
    // the panel groups hold every parameter exactly once
    const auto groups = c.paramGroups();
    CHECK(groups.size() == 10);
    std::vector<std::string> grouped;
    for (auto& g : groups) {
        CHECK(!g.title.empty() && !g.ids.empty());
        for (auto& id : g.ids) {
            CHECK_MSG(c.param(id) != nullptr, "group %s lists %s, which does not exist", g.title.c_str(), id.c_str());
            CHECK_MSG(std::find(grouped.begin(), grouped.end(), id) == grouped.end(), "%s is in two groups", id.c_str());
            grouped.push_back(id);
        }
    }
    CHECK_MSG(grouped.size() == ids.size(), "%d parameters, %d of them in groups", (int)ids.size(), (int)grouped.size());
    CHECK(std::string(c.typeId()) == "zest" && std::string(c.displayName()) == "Zest" && c.isInstrument());
    // every parameter survives a save and a load; missing ones fall back to their defaults
    dsp::Noise rnd(5);
    for (auto& p : c.params()) p->setNorm(rnd.uni());
    const Json saved = Json::parse(c.saveState().dump());
    Zest d;
    d.loadState(saved);
    for (auto& p : c.params()) CHECK_MSG(d.param(p->info.id)->get() == p->get(), "%s came back as %g, was %g", p->info.id.c_str(), d.param(p->info.id)->get(), p->get());
    Json empty = Json::object();
    empty.set("type", "zest");
    empty.set("params", Json::object());
    d.loadState(empty);
    for (auto& p : d.params()) CHECK(p->get() == p->info.def);
    printf("    %d parameters in %d groups\n", (int)ids.size(), (int)groups.size());
}

// ------------------------------------------------------------------ the engine's guarantees

TEST(zest_render_is_identical_at_any_block_size_and_thread_count) {
    Zest proto;
    const auto presets = proto.factoryPresets();
    auto build = [&](Project& p) {
        p.global->tempo.set(133.0f);
        const char* names[] = {"Tempo Wobble Bass", "Stadium Saw Stack", "Robot Chatter", "Rhythm Gate Chords", "Sync Sweep Lead", "Dark Drone Pad"};
        for (int i = 0; i < 6; ++i) {
            auto ch = addZest(p, names[i]);
            ch.synth->loadState(presetNamed(presets, names[i])->state);
            auto& notes = p.pattern().notesFor(ch.id);
            if (i == 0 || i == 4) {       // a line with overlaps, a slide and portamento
                const int keys[] = {36, 48, 43, 39, 36, 51, 46, 36};
                for (int s = 0; s < 8; ++s) {
                    Note n = mk(s * 2 * kTicksPerStep + (s % 3) * 17, 2 * kTicksPerStep + (s % 2) * 200, keys[s] + (i == 4 ? 24 : 0));
                    n.modX = 0.3f + 0.08f * s; n.modY = 0.7f - 0.05f * s; n.pan = (s % 2) ? 0.4f : -0.4f; n.fine = (float)(s * 3 - 10); n.porta = s == 5;
                    notes.push_back(n);
                }
                Note slide = mk(9 * kTicksPerStep, 2 * kTicksPerStep, 55 + (i == 4 ? 24 : 0));
                slide.slide = true;
                notes.push_back(slide);
            } else {
                const int chord[] = {48, 55, 60, 63, 67, 72};
                for (int s = 0; s < 4; ++s)
                    for (int k = 0; k < 3 + i % 3; ++k) {
                        Note n = mk(s * 4 * kTicksPerStep + k * 31 + i * 7, 3 * kTicksPerStep + k * 60, chord[(k + s + i) % 6], 0.5f + 0.1f * k);
                        n.release = 0.2f + 0.2f * k; n.modX = 0.5f + 0.05f * (float)(k - 1);
                        notes.push_back(n);
                    }
            }
            sortNotes(notes);
            if (i == 2) { ch.live->porta.set(1); ch.live->portaTime.set(80.0f); }
            if (i == 3) ch.live->poly.set(4);
            ch.live->pan.set(-0.5f + 0.2f * i);
        }
    };
    std::shared_ptr<SampleData> ref;
    struct Case { int block, threads; } cases[] = {{64, 1}, {33, 1}, {4096, 1}, {64, 4}, {33, 4}, {4096, 4}, {1, 2}, {500, 8}};
    for (auto& cs : cases) {
        Project p;
        build(p);
        RenderOptions o;
        o.blockSize = cs.block;
        o.threads = cs.threads;
        o.loop = true;              // the pattern comes round twice: the transport jumps back in the middle
        o.lengthSamples = cs.block == 1 ? 30000 : 48000 * 4;
        const int64 before = Rt::allocationsInRt.load();
        auto out = renderOffline(p, o);
        CHECK_MSG(Rt::allocationsInRt.load() == before, "%lld allocations on the audio thread (block %d, threads %d)", (long long)(Rt::allocationsInRt.load() - before), cs.block, cs.threads);
        CHECK(out != nullptr);
        if (!out) return;
        if (!ref) { ref = out; CHECK(peakOf(out->ch[0]) > 0.2f && allFinite(out->ch[0])); continue; }
        if (cs.block == 1) {
            bool same = true;
            for (size_t i = 0; i < out->ch[0].size(); ++i) same = same && out->ch[0][i] == ref->ch[0][i] && out->ch[1][i] == ref->ch[1][i];
            CHECK_MSG(same, "block %d threads %d differs", cs.block, cs.threads);
        } else CHECK_MSG(identical(*ref, *out), "block %d threads %d differs", cs.block, cs.threads);
    }
    // and the same project rendered again from scratch gives the same file
    Project p;
    build(p);
    RenderOptions o;
    o.loop = true;
    o.lengthSamples = 48000 * 4;
    auto again = renderOffline(p, o);
    CHECK(again && identical(*ref, *again));
}

TEST(zest_never_allocates_on_the_audio_thread) {
    Zest proto;
    const auto presets = proto.factoryPresets();
    int64 counted = 0;
    int n = 0;
    for (auto& pr : presets) {
        Rig r(n % 2 ? 44100.0 : 48000.0);
        r.L.reserve(40000); r.R.reserve(40000);
        r.syn.loadState(pr.state);
        r.playing = (n % 3) != 0;
        uint32 ids[24];
        for (int i = 0; i < 24; ++i) ids[i] = r.on(i * 700, 36 + (i * 7) % 48, 0.3f + 0.03f * i);
        for (int i = 0; i < 24; ++i) if (i % 3) r.off(i * 700 + 3000, ids[i]);
        r.ev(9000, NoteEvent::KillNote, ids[3]);
        NoteEvent slide; slide.type = NoteEvent::SlideTo; slide.key = 70; slide.slideSamples = 2000.0f;
        r.evs.push_back({12000, slide});
        r.ev(20000, NoteEvent::AllOff);
        r.ev(24000, NoteEvent::Kill);
        r.evs.reserve(r.evs.size() + 8);
        const int64 before = Rt::allocationsInRt.load();
        r.run(32000, [&](int64 at) {        // parameters moving, effects and modes switching while it plays
            if (at == 6400) for (const char* fx : {"drvon", "choon", "dlyon", "revon"}) r.syn.param(fx)->set(1.0f - r.syn.param(fx)->get());
            if (at == 12800) r.syn.param("mode")->set((float)((n + 1) % 3));
            if (at == 16000) { r.syn.param("o1uni")->set(7); r.syn.param("o2uni")->set(7); r.syn.param("o2sync")->set(1); r.syn.param("froute")->set(1); r.syn.param("fbal")->set(0.5f); }
            r.tempo = 120.0 + (double)(at % 6400) / 640.0;
        });
        {
            Rt::Scope rt;
            r.syn.reset();
        }
        counted += Rt::allocationsInRt.load() - before;
        ++n;
    }
    CHECK_MSG(counted == 0, "%lld allocations inside process() or reset()", (long long)counted);
}

TEST(zest_survives_any_settings) {
    // Random settings of everything, at two sample rates, with notes of every kind: the output stays finite and bounded,
    // and when the notes and tails are over it is exactly silent (no denormal crawl, no DC, nothing left ringing).
    dsp::Noise rnd(20261016);
    float loudest = 0.0f;
    for (int trial = 0; trial < 36; ++trial) {
        Rig r(trial % 2 ? 96000.0 : 44100.0);
        for (auto& p : r.syn.params()) p->setNorm(rnd.uni());
        set(r.syn, "volume", 0);
        if (trial % 4 == 0) for (const char* id : {"f1reso", "f2reso", "f1drive", "f2drive", "dlyfb"}) r.syn.param(id)->setNorm(1.0f);
        if (trial % 6 == 1) for (const char* id : {"f1cutoff", "f2cutoff"}) r.syn.param(id)->setNorm(trial % 12 == 1 ? 0.0f : 1.0f);
        r.playing = trial % 3 == 0;
        r.tempo = 60.0 + 200.0 * rnd.uni();
        std::vector<uint32> ids;
        for (int i = 0; i < 14; ++i) {
            NoteEvent e;
            e.key = (int)(rnd.uni() * 127.0f);
            e.velocity = rnd.uni(); e.pan = rnd.next(); e.modX = rnd.uni(); e.modY = rnd.uni(); e.fineCents = rnd.next() * 1200.0f; e.release = rnd.uni();
            e.group = i % 3;
            if (i % 4 == 0) { e.glideFromKey = rnd.uni() * 127.0f; e.slideSamples = rnd.uni() * 20000.0f; }
            ids.push_back(r.onEv((int64)(rnd.uni() * 30000.0f), e));
        }
        for (size_t i = 0; i < ids.size(); i += 2) r.off(20000 + (int64)(rnd.uni() * 30000.0f), ids[i]);
        NoteEvent slide; slide.type = NoteEvent::SlideTo; slide.key = (int)(rnd.uni() * 127.0f); slide.slideSamples = rnd.uni() * 9000.0f; slide.group = 1;
        r.evs.push_back({25000, slide});
        r.ev(40000, NoteEvent::KillNote, ids[1]);
        r.run(60032, [&](int64 at) { r.bend = 24.0f * std::sin((float)at * 0.0003f); });
        CHECK_MSG(allFinite(r.L) && allFinite(r.R), "trial %d: non-finite output", trial);
        const float peak = std::max(peakOf(r.L), peakOf(r.R));
        loudest = std::max(loudest, peak);
        CHECK_MSG(peak < 30.0f, "trial %d: peak %.1f", trial, peak);
        // now wind it down: short release and short tails, everything let go
        set(r.syn, "e1rel", 30); set(r.syn, "revdecay", 0.5f); set(r.syn, "dlysync", 0); set(r.syn, "dlytime", 50); set(r.syn, "dlyfb", 0.5f);
        r.ev(r.pos + 100, trial % 2 ? NoteEvent::AllOff : NoteEvent::Kill);
        r.run((int64)(r.sr * 4.0));
        CHECK_MSG(allFinite(r.L) && allFinite(r.R), "trial %d: non-finite tail", trial);
        CHECK_MSG(r.syn.activeVoices() == 0, "trial %d: %d voices never ended", trial, r.syn.activeVoices());
        const size_t tail = r.L.size() - (size_t)(r.sr * 0.5);
        CHECK_MSG(peakOf(r.L, tail) == 0.0f && peakOf(r.R, tail) == 0.0f, "trial %d: not silent at the end (%.3g)", trial, std::max(peakOf(r.L, tail), peakOf(r.R, tail)));
    }
    printf("    loudest of 36 random patches (14 notes each, any velocity): %.1f dBFS\n", gainToDb(loudest));
}

// ------------------------------------------------------------------ speed

TEST(zest_worst_case_renders_far_faster_than_real_time) {
    // The heaviest patch there is: 16 notes, 7-voice unison on both oscillators with oscillator 2 hard synced (the
    // costliest oscillator path), sub and noise, both filters as driven 24 dB low-passes, all three LFOs and all eight
    // matrix slots working, all four effects on. One core.
#ifdef NDEBUG
    const double seconds = 10.0;
#else
    const double seconds = 1.0;         // the debug build only checks that it runs
#endif
    Zest proto;
    const auto presets = proto.factoryPresets();
    auto time = [&](bool worst, double& peak) {
        Project p;
        p.global->tempo.set(120.0f);
        auto ch = addZest(p);
        Zest& c = *ch.synth;
        c.loadState(presetNamed(presets, "Stadium Saw Stack")->state);
        if (worst) {
            set(c, "o1uni", 7); set(c, "o2uni", 7); set(c, "o2level", 0.8f); set(c, "o2sync", 1); set(c, "o2coarse", 7); set(c, "o2shape", Zest::Pulse);
            set(c, "sublevel", 0.5f); set(c, "subshape", 1); set(c, "noiselevel", 0.3f); set(c, "noisetype", 1);
            set(c, "f1type", Zest::LowPass24); set(c, "f2type", Zest::LowPass24); set(c, "f1cutoff", 3000); set(c, "f2cutoff", 5000); set(c, "fbal", 0.5f); set(c, "f1drive", 0.5f); set(c, "f2drive", 0.5f);
            const int src[8] = {Zest::SrcLfo1, Zest::SrcLfo2, Zest::SrcLfo3, Zest::SrcEnv2, Zest::SrcEnv3, Zest::SrcVelocity, Zest::SrcKey, Zest::SrcRandom};
            const int dst[8] = {Zest::DstPitch, Zest::DstCutoff1, Zest::DstPulseWidth, Zest::DstCutoff2, Zest::DstOsc2Pitch, Zest::DstResonance, Zest::DstPan, Zest::DstDetune};
            for (int i = 0; i < 8; ++i) {
                const std::string s = "m" + std::to_string(i + 1);
                set(c, (s + "src").c_str(), (float)src[i]); set(c, (s + "dst").c_str(), (float)dst[i]); set(c, (s + "amt").c_str(), 0.1f);
            }
            for (const char* fx : {"drvon", "choon", "dlyon", "revon"}) set(c, fx, 1);
            set(c, "poly", 16);
        }
        for (int i = 0; i < 16; ++i) p.pattern().notesFor(ch.id).push_back(mk(0, kPPQ * 4 * 8, 36 + i * 3));
        RenderOptions o;
        o.lengthSamples = (int64)(48000 * seconds);
        o.threads = 1;
        const auto t0 = std::chrono::steady_clock::now();
        auto out = renderOffline(p, o);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(out != nullptr);
        if (out) { peak = peakOf(out->ch[0]); CHECK(allFinite(out->ch[0]) && peak > 0.2); }
        return s;
    };
    double peak = 0.0;
    const double shipped = time(false, peak);
    const double worst = time(true, peak);
    printf("    16 notes of \"Stadium Saw Stack\" (7 + 7 unison, delay, reverb): %.1f s of sound in %.3f s, %.1fx real time\n", seconds, shipped, seconds / shipped);
    printf("    the worst case (7 + 7 unison with hard sync, sub, noise, two driven 24 dB filters, full matrix, all effects):\n");
    printf("        %.1f s of sound in %.3f s, %.1fx real time, %.1f %% of one core\n", seconds, worst, seconds / worst, 100.0 * worst / seconds);
#ifdef NDEBUG
    CHECK_MSG(worst < seconds * 0.4, "the worst case took %.2f s for %.0f s of sound", worst, seconds);
    CHECK_MSG(shipped < seconds * 0.25, "16 notes of the saw stack preset took %.2f s for %.0f s of sound", shipped, seconds);
#endif
}
