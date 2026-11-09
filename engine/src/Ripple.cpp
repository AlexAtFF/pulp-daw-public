#include "pulp/Ripple.h"
#include <deque>
#include <functional>

namespace pulp {

namespace {

constexpr int kCtl = 32;                    // control period in samples, counted per voice from its first sample
constexpr int kHarm = Ripple::kFrameSize / 2;   // a 2048-sample frame is analysed up to harmonic 1023; the fullest level plays 512 of them

using Wavetable = Ripple::Wavetable;

// One frame as a spectrum: x(p) = sum over h of s[h] sin(2 pi h p) + c[h] cos(2 pi h p).
struct Spec {
    std::vector<double> s, c;
    Spec() : s((size_t)kHarm, 0.0), c((size_t)kHarm, 0.0) {}
};

// The frame with only its first `harmonics` partials, as `size` samples (left in re).
void synth(const Spec& sp, int harmonics, int size, std::vector<float>& re, std::vector<float>& im) {
    re.assign((size_t)size, 0.0f);
    im.assign((size_t)size, 0.0f);
    const int top = std::min(std::min(harmonics, size / 2 - 1), kHarm - 1);
    for (int h = 1; h <= top; ++h) {
        re[(size_t)h] = (float)(0.5 * size * sp.c[(size_t)h]);
        im[(size_t)h] = (float)(-0.5 * size * sp.s[(size_t)h]);
        re[(size_t)(size - h)] = re[(size_t)h];
        im[(size_t)(size - h)] = -im[(size_t)h];
    }
    dsp::fft(re.data(), im.data(), size, true);
}

// The spectrum of one cycle given as n samples (n a power of two). DC is dropped.
Spec analyse(const float* x, int n) {
    std::vector<float> re(x, x + n), im((size_t)n, 0.0f);
    dsp::fft(re.data(), im.data(), n);
    Spec sp;
    for (int h = 1; h < std::min(kHarm, n / 2); ++h) {
        sp.c[(size_t)h] = 2.0 * re[(size_t)h] / n;
        sp.s[(size_t)h] = -2.0 * im[(size_t)h] / n;
    }
    return sp;
}

// Turns frames into a playable table: levels them (each frame to a peak of 1, or the whole table
// to its loudest frame) and renders every frame at every level.
std::shared_ptr<Wavetable> assemble(std::vector<Spec>& frames, const std::string& name, bool levelEachFrame) {
    auto t = std::make_shared<Wavetable>();
    t->name = name;
    t->frames = (int)frames.size();
    std::vector<float> re, im;
    std::vector<double> peaks;
    double loudest = 0.0;
    for (auto& f : frames) {
        synth(f, Wavetable::mipHarmonics(0), Wavetable::mipSize(0), re, im);     // levelled as the fullest level plays it
        double pk = 0.0;
        for (float v : re) pk = std::max(pk, (double)std::fabs(v));
        peaks.push_back(pk);
        loudest = std::max(loudest, pk);
    }
    for (size_t i = 0; i < frames.size(); ++i) {
        const double div = levelEachFrame ? peaks[i] : loudest;
        if (div < 1.0e-9) continue;
        for (int h = 0; h < kHarm; ++h) { frames[i].s[(size_t)h] /= div; frames[i].c[(size_t)h] /= div; }
    }
    size_t total = 0;
    for (int m = 0; m < Ripple::kMips; ++m) { t->mipBase[m] = total; total += frames.size() * (size_t)(Wavetable::mipSize(m) + Wavetable::kGuard); }
    t->data.assign(total, 0.0f);
    for (int m = 0; m < Ripple::kMips; ++m) {
        const int size = Wavetable::mipSize(m);
        for (size_t f = 0; f < frames.size(); ++f) {
            synth(frames[f], Wavetable::mipHarmonics(m), size, re, im);
            float* d = t->data.data() + t->mipBase[m] + f * (size_t)(size + Wavetable::kGuard) + 2;
            std::copy(re.begin(), re.end(), d);
            d[-2] = re[(size_t)size - 2];
            d[-1] = re[(size_t)size - 1];
            for (int k = 0; k < 3; ++k) d[size + k] = re[(size_t)k];
        }
    }
    return t;
}

// ---- tables from audio ----

std::shared_ptr<Wavetable> fromCycles(const float* cycles, int numFrames, const std::string& name) {
    numFrames = clampT(numFrames, 1, Ripple::kMaxFrames);
    std::vector<Spec> f;
    for (int i = 0; i < numFrames; ++i) f.push_back(analyse(cycles + (size_t)i * Ripple::kFrameSize, Ripple::kFrameSize));
    return assemble(f, name, false);
}

std::shared_ptr<Wavetable> fromAudio(const SampleData& audio, const std::string& name, std::string* error) {
    const int64 n = audio.frames();
    if (n < 2) { if (error) *error = "the file holds no audio"; return nullptr; }
    const float* x = audio.left();
    if (n % Ripple::kFrameSize == 0) return fromCycles(x, (int)std::min<int64>(n / Ripple::kFrameSize, Ripple::kMaxFrames), name);
    // One cycle of some other length: take its harmonics directly (very long cycles are thinned first).
    std::vector<double> cyc;
    const int64 cap = 16384;
    if (n <= cap) cyc.assign(x, x + n);
    else {
        cyc.resize((size_t)cap);
        for (int64 i = 0; i < cap; ++i) {
            const double p = (double)i * (double)n / (double)cap;
            const int64 a = (int64)p;
            const double fr = p - (double)a;
            cyc[(size_t)i] = x[a] + (x[(a + 1) % n] - x[a]) * fr;
        }
    }
    const int len = (int)cyc.size();
    std::vector<Spec> f(1);
    for (int h = 1; h < kHarm && h <= (len - 1) / 2; ++h) {
        const double w = kTwoPi * h / len, wr = std::cos(w), wi = std::sin(w);
        double cr = 1.0, ci = 0.0, re = 0.0, im = 0.0;
        for (int i = 0; i < len; ++i) {
            re += cyc[(size_t)i] * cr;
            im += cyc[(size_t)i] * ci;
            const double nr = cr * wr - ci * wi;
            ci = cr * wi + ci * wr;
            cr = nr;
        }
        f[0].c[(size_t)h] = 2.0 * re / len;
        f[0].s[(size_t)h] = 2.0 * im / len;
    }
    return assemble(f, name, false);
}

// ---- the built-in tables ----

// A table whose frames are written straight into the spectrum. t runs 0..1 across the frames.
Ripple::TablePtr spectral(const char* name, int frames, const std::function<void(Spec&, double t, int index)>& fill) {
    std::vector<Spec> f((size_t)frames);
    for (int i = 0; i < frames; ++i) fill(f[(size_t)i], frames > 1 ? (double)i / (frames - 1) : 0.0, i);
    return assemble(f, name, true);
}
// A table whose frames are a function of the phase x (0..1). It is sampled four times finer than a
// frame and only the harmonics a frame can hold are kept, so shapes with sharp corners do not alias.
Ripple::TablePtr timed(const char* name, int frames, const std::function<double(double x, double t)>& fn) {
    const int n = 8192;
    std::vector<Spec> f;
    std::vector<float> x((size_t)n);
    for (int i = 0; i < frames; ++i) {
        const double t = frames > 1 ? (double)i / (frames - 1) : 0.0;
        for (int k = 0; k < n; ++k) x[(size_t)k] = (float)fn((double)k / n, t);
        f.push_back(analyse(x.data(), n));
    }
    return assemble(f, name, true);
}

struct Builtins { std::vector<Ripple::TablePtr> t; };

Builtins buildBuiltins() {
    Builtins b;
    auto add = [&](Ripple::TablePtr t) { b.t.push_back(std::move(t)); };
    const int H = kHarm;

    // 0: sine, triangle, saw, square, narrow pulse
    add(spectral("Basic Shapes", 5, [&](Spec& sp, double, int i) {
        for (int h = 1; h < H; ++h) {
            const bool odd = h % 2 == 1;
            switch (i) {
                case 0: if (h == 1) sp.s[1] = 1.0; break;
                case 1: if (odd) sp.s[(size_t)h] = (((h - 1) / 2) % 2 ? -1.0 : 1.0) / ((double)h * h); break;
                case 2: sp.s[(size_t)h] = 1.0 / h; break;
                case 3: if (odd) sp.s[(size_t)h] = 1.0 / h; break;
                default: sp.c[(size_t)h] = std::sin(kPi * h * 0.125) / h; break;
            }
        }
    }));
    // 1: a pulse from square to a thin spike
    add(spectral("Pulse Width", 24, [&](Spec& sp, double t, int) {
        const double d = 0.5 * std::pow(0.06, t);
        for (int h = 1; h < H; ++h) sp.c[(size_t)h] = std::sin(kPi * h * d) / h;
    }));
    // 2: a saw through a resonant low-pass that opens
    add(spectral("Harmonic Sweep", 24, [&](Spec& sp, double t, int) {
        const double hc = 1.5 * std::pow(200.0, t);
        for (int h = 1; h < H; ++h) {
            const double r = h / hc;
            sp.s[(size_t)h] = (1.0 / h) / std::sqrt((1.0 - r * r) * (1.0 - r * r) + r * r * 0.25);
        }
    }));
    // 3: the fundamental and one overtone that climbs the harmonic series
    add(spectral("Overtone Ladder", 16, [&](Spec& sp, double, int i) {
        sp.s[1] = 1.0;
        sp.s[(size_t)(i + 2)] = 0.9;
    }));
    // 4: square (odd harmonics) to saw (all) to a saw with its odd harmonics thinned
    add(spectral("Odd Even", 9, [&](Spec& sp, double, int i) {
        const double even = i <= 4 ? i / 4.0 : 1.0, odd = i <= 4 ? 1.0 : 1.0 - (i - 4) * 0.2;
        for (int h = 1; h < H; ++h) sp.s[(size_t)h] = (h % 2 ? (h == 1 ? 1.0 : odd) : even) / h;
    }));
    // 5: A - E - I - O - U
    add(spectral("Vowels", 17, [&](Spec& sp, double t, int) {
        static const double F[5][3] = {{800, 1150, 2900}, {400, 1600, 2700}, {270, 2140, 2950}, {450, 800, 2830}, {325, 700, 2700}};
        static const double bw[3] = {80, 100, 140}, gain[3] = {1.0, 0.5, 0.25};
        const double p = t * 4.0;
        const int a = std::min(3, (int)p);
        const double fr = p - a;
        for (int h = 1; h < H; ++h) {
            const double hz = h * 110.0;
            double g = 0.03;
            for (int k = 0; k < 3; ++k) {
                const double fk = F[a][k] + (F[a + 1][k] - F[a][k]) * fr, x = (hz - fk) / bw[k];
                g += gain[k] / (1.0 + x * x);
            }
            sp.s[(size_t)h] = g / h;
        }
    }));
    // 6: one resonance travelling up through a saw
    add(spectral("Formant Sweep", 24, [&](Spec& sp, double t, int) {
        const double hc = 2.0 * std::pow(24.0, t);
        for (int h = 1; h < H; ++h) {
            const double x = (h - hc) / (0.1 * hc + 0.6);
            sp.s[(size_t)h] = (0.2 + 5.0 / (1.0 + x * x)) / h;
        }
    }));
    // 7..9: a sine phase-modulated more and more, by itself, by its octave, and by two high partials
    add(timed("FM Bright", 24, [](double x, double t) { return std::sin(kTwoPi * x + 7.0 * t * std::sin(kTwoPi * x)); }));
    add(timed("FM Hollow", 24, [](double x, double t) { return std::sin(kTwoPi * x + 6.0 * t * std::sin(2.0 * kTwoPi * x)); }));
    add(timed("FM Clang", 24, [](double x, double t) {
        return std::sin(kTwoPi * x + 3.5 * t * std::sin(3.0 * kTwoPi * x) + 2.1 * t * std::sin(7.0 * kTwoPi * x));
    }));
    // 10: sparse, wide-spaced partials that come in one after another
    add(spectral("Bell Tower", 12, [&](Spec& sp, double t, int) {
        static const int part[10] = {1, 4, 7, 11, 15, 20, 26, 33, 41, 50};
        for (int k = 0; k < 10; ++k) {
            const double a = (k == 0 ? 1.0 : 0.8) * std::exp(-k * 2.5 * std::pow(1.0 - t, 1.5));
            (k % 2 ? sp.c : sp.s)[(size_t)part[k]] = a;
        }
    }));
    // 11: organ registrations, from one stop to all of them
    add(spectral("Drawbars", 9, [&](Spec& sp, double, int i) {
        static const int harm[7] = {1, 2, 3, 4, 5, 6, 8};
        static const int reg[9][7] = {{8, 0, 0, 0, 0, 0, 0}, {8, 4, 0, 0, 0, 0, 0}, {8, 6, 4, 0, 0, 0, 0}, {8, 8, 0, 6, 0, 0, 0}, {8, 8, 6, 6, 0, 0, 0},
                                      {8, 6, 5, 7, 3, 0, 0}, {8, 8, 8, 8, 4, 4, 0}, {8, 8, 8, 8, 8, 8, 8}, {6, 8, 4, 8, 6, 8, 8}};
        for (int k = 0; k < 7; ++k) sp.s[(size_t)harm[k]] = reg[i][k] / 8.0;
    }));
    // 12: a saw restarted by the cycle, running up to eight times as fast
    add(timed("Saw Sync", 24, [](double x, double t) { const double p = x * (1.0 + 7.0 * t); return 2.0 * (p - std::floor(p)) - 1.0; }));
    // 13: a sine restarted by the cycle and faded across it
    add(timed("Sine Sync", 24, [](double x, double t) {
        const double w = std::cos(kPi * x * 0.5);
        return std::sin(kTwoPi * x * (1.0 + 11.0 * t)) * w * w;
    }));
    // 14: a sine folded back on itself
    add(timed("Folded Sine", 24, [](double x, double t) { return std::sin((0.5 + 11.5 * t * t) * std::sin(kTwoPi * x)); }));
    // 15: a cosine whose first half is squeezed into less and less of the cycle
    add(timed("Phase Bend", 24, [](double x, double t) {
        const double d = 0.5 * std::pow(0.06, t);
        const double p = x < d ? 0.5 * x / d : 0.5 + 0.5 * (x - d) / (1.0 - d);
        return -std::cos(kTwoPi * p);
    }));
    // 16: a sine held in fewer and fewer steps
    add(timed("Stairs", 16, [](double x, double t) {
        static const int steps[16] = {64, 48, 32, 24, 20, 16, 14, 12, 10, 9, 8, 7, 6, 5, 4, 3};
        const int s = steps[clampT((int)std::lround(t * 15.0), 0, 15)];
        return std::sin(kTwoPi * (std::floor(x * s) + 0.5) / s);
    }));
    // 17: random partials over a strong fundamental, a different draw per frame, brighter along the table
    add(spectral("Digital Grit", 16, [&](Spec& sp, double, int i) {
        dsp::Noise rnd(0xC0FFEEu + (uint32)i * 7919u);
        const int top = std::min(H - 1, (int)(16.0 * std::pow(2.0, i / 3.0)));
        for (int h = 1; h <= top; ++h) {
            const double a = std::pow((double)h, -0.6);
            sp.s[(size_t)h] = a * rnd.next();
            sp.c[(size_t)h] = a * rnd.next();
        }
        sp.s[1] += 1.5;
    }));
    // 18: a saw through a comb whose teeth slide (the last frame leads back into the first)
    add(spectral("Comb Shift", 16, [&](Spec& sp, double, int i) {
        const double t = i / 16.0;
        for (int h = 1; h < H; ++h) {
            const double tooth = 0.5 + 0.5 * std::cos(kTwoPi * (h / 7.0 - t));
            sp.s[(size_t)h] = (0.08 + tooth * tooth * tooth) / h;
        }
    }));
    // 19: the fundamental and a small cluster of high partials that climbs
    add(spectral("Glass Harmonics", 12, [&](Spec& sp, double t, int) {
        const double centre = 5.0 + 22.0 * t;
        sp.s[1] = 1.0;
        for (int h = 2; h < 64; ++h) { const double x = (h - centre) / 2.5; sp.s[(size_t)h] = 0.7 * std::exp(-x * x); }
    }));
    // 20: octaves stacked one at a time
    add(spectral("Octave Stack", 6, [&](Spec& sp, double, int i) {
        for (int k = 0; k <= i; ++k) (k % 2 ? sp.c : sp.s)[(size_t)1 << k] = std::pow(2.0, -0.5 * k);
    }));
    // 21: a sine with a band of random high partials growing over it, a different draw per frame
    add(spectral("Breath Noise", 12, [&](Spec& sp, double t, int i) {
        dsp::Noise rnd(0xB4EA7u + (uint32)i * 104729u);
        sp.s[1] = 1.0;
        for (int h = 12; h <= 300; ++h) {
            const double x = (std::log2((double)h) - 6.0) / 1.2, a = t * 0.35 * std::exp(-x * x);
            sp.s[(size_t)h] = a * rnd.next();
            sp.c[(size_t)h] = a * rnd.next();
        }
    }));
    return b;
}

const Builtins& builtins() { static const Builtins b = buildBuiltins(); return b; }

std::string oscId(int o, const char* what) { return "o" + std::to_string(o + 1) + what; }

// Reads the two neighbouring frames of a table at one level for every unison voice of an
// oscillator and adds the result, times a gain that ramps from xf by xfStep per sample.
void oscRun(const Wavetable& t, int mip, int unison, uint32* phase, uint32* inc, const std::int32_t* incStep,
            float* gl, const float* glStep, float* gr, const float* grStep, float& pos, float posStep,
            float* L, float* R, int m, float xf, float xfStep) {
    const int bits = Wavetable::mipBits(mip), size = 1 << bits, stride = size + Wavetable::kGuard, fracBits = 32 - bits;
    const uint32 fracMask = (1u << fracBits) - 1u;
    const float fracScale = 1.0f / (float)(1u << fracBits);
    const float* base = t.frame(mip, 0);
    const int last = t.frames - 1;
    for (int i = 0; i < m; ++i) {
        pos += posStep;
        const float fp = pos * (float)last;
        const int ia = std::max(0, std::min((int)fp, last - 1));
        const float tt = last > 0 ? fp - (float)ia : 0.0f;
        const float* fa = base + (size_t)ia * (size_t)stride;
        const float* fb = last > 0 ? fa + stride : fa;
        float l = 0.0f, r = 0.0f;
        for (int u = 0; u < unison; ++u) {
            const uint32 p = phase[u];
            phase[u] = p + inc[u];
            inc[u] += (uint32)incStep[u];
            const uint32 idx = p >> fracBits;
            // six-point Lagrange weights for the samples at -2 .. 3 around the read point
            const float x = (float)(p & fracMask) * fracScale;
            const float xa = x + 2.0f, xb = x + 1.0f, xd = x - 1.0f, xe = x - 2.0f, xg = x - 3.0f;
            const float ab = xa * xb, cd = x * xd, eg = xe * xg;
            const float w0 = xb * cd * eg * (-1.0f / 120.0f), w1 = xa * cd * eg * (1.0f / 24.0f), w2 = ab * xd * eg * (-1.0f / 12.0f);
            const float w3 = ab * x * eg * (1.0f / 12.0f), w4 = ab * cd * xg * (-1.0f / 24.0f), w5 = ab * cd * xe * (1.0f / 120.0f);
            const float* a = fa + idx - 2;
            const float* b = fb + idx - 2;
            const float wa = w0 * a[0] + w1 * a[1] + w2 * a[2] + w3 * a[3] + w4 * a[4] + w5 * a[5];
            const float wb = w0 * b[0] + w1 * b[1] + w2 * b[2] + w3 * b[3] + w4 * b[4] + w5 * b[5];
            const float w = wa + (wb - wa) * tt;
            gl[u] += glStep[u];
            gr[u] += grStep[u];
            l += w * gl[u];
            r += w * gr[u];
        }
        xf += xfStep;
        L[i] += l * xf;
        R[i] += r * xf;
    }
}

} // namespace

int Ripple::numBuiltinTables() { return (int)builtins().t.size(); }
const Ripple::Wavetable& Ripple::builtinTable(int index) { return *builtins().t[(size_t)clampT(index, 0, numBuiltinTables() - 1)]; }

Ripple::TablePtr Ripple::makeTable(const float* cycles, int numFrames, const std::string& name) { return fromCycles(cycles, numFrames, name); }
Ripple::TablePtr Ripple::makeTable(const SampleData& audio, const std::string& name, std::string* error) { return fromAudio(audio, name, error); }

Ripple::Ripple() {
    std::vector<std::string> tables;
    for (auto& t : builtins().t) tables.push_back(t->name);
    tables.push_back("User table");
    for (int o = 0; o < 2; ++o) {
        const std::string d = "Osc " + std::to_string(o + 1) + " ";
        auto id = [&](const char* w) { return oscId(o, w); };
        OscParams& p = osc[o];
        p.table = addChoice(id("table").c_str(), (d + "wavetable").c_str(), tables, o == 0 ? 0 : 1);
        p.pos = addFloat(id("pos").c_str(), (d + "position").c_str(), 0, 1, o == 0 ? 0.5f : 0.0f, "%");
        ParamInfo ci;
        ci.id = id("coarse"); ci.name = d + "coarse"; ci.min = -36; ci.max = 36; ci.def = 0; ci.steps = 73; ci.unit = "st";
        p.coarse = add(ci);
        p.fine = addFloat(id("fine").c_str(), (d + "fine").c_str(), -100, 100, 0, "ct");
        p.level = addFloat(id("level").c_str(), (d + "level").c_str(), 0, 1, o == 0 ? 0.8f : 0.0f, "%");
        p.pan = addFloat(id("pan").c_str(), (d + "pan").c_str(), -1, 1, 0, "");
        ParamInfo ui;
        ui.id = id("uni"); ui.name = d + "unison voices"; ui.min = 1; ui.max = (float)kUnison; ui.def = 1; ui.steps = kUnison;
        p.uni = add(ui);
        p.detune = addFloat(id("detune").c_str(), (d + "unison detune").c_str(), 0, 100, 15, "ct");
        p.spread = addFloat(id("spread").c_str(), (d + "unison spread").c_str(), 0, 1, 0.5f, "%");
        p.phase = addFloat(id("phase").c_str(), (d + "phase randomisation").c_str(), 0, 1, 0, "%");
        p.posEnv = addFloat(id("posenv").c_str(), (d + "position from mod envelope").c_str(), -1, 1, 0, "");
        p.posLfo[0] = addFloat(id("poslfo1").c_str(), (d + "position from LFO 1").c_str(), -1, 1, 0, "");
        p.posLfo[1] = addFloat(id("poslfo2").c_str(), (d + "position from LFO 2").c_str(), -1, 1, 0, "");
        p.posY = addFloat(id("posy").c_str(), (d + "position from mod Y").c_str(), -1, 1, 1, "");
    }
    pSubWave = addChoice("subwave", "Sub shape", {"Sine", "Triangle", "Square", "Saw"}, 0);
    pSubOct = addChoice("suboct", "Sub octave", {"One below", "Two below"}, 0);
    pSubLevel = addFloat("sublevel", "Sub level", 0, 1, 0, "%");
    pNoise = addFloat("noise", "Noise level", 0, 1, 0, "%");
    pNoiseColor = addFloat("noisecolor", "Noise colour", 100, 20000, 8000, "Hz", 3.0f);

    pFType = addChoice("ftype", "Filter type", {"Off", "Low pass 12", "Low pass 24", "Band pass", "High pass", "Notch"}, 1);
    pCutoff = addFloat("cutoff", "Cutoff", 20, 20000, 20000, "Hz", 3.0f);
    pReso = addFloat("reso", "Resonance", 0, 1, 0.1f, "%");
    pFEnv = addFloat("fenv", "Filter envelope amount", -8, 8, 0, "oct");
    pFKey = addFloat("fkey", "Filter key tracking", 0, 1, 0, "%");
    pFVel = addFloat("fvel", "Filter velocity", 0, 1, 0, "%");
    pFAttack = addFloat("fattack", "Filter attack", 0, 10000, 1, "ms", 3.0f);
    pFDecay = addFloat("fdecay", "Filter decay", 1, 20000, 300, "ms", 3.0f);
    pFSustain = addFloat("fsustain", "Filter sustain", 0, 1, 0.3f, "%");
    pFRelease = addFloat("frelease", "Filter release", 1, 20000, 200, "ms", 3.0f);

    pAttack = addFloat("attack", "Attack", 0, 10000, 2, "ms", 3.0f);
    pDecay = addFloat("decay", "Decay", 1, 20000, 400, "ms", 3.0f);
    pSustain = addFloat("sustain", "Sustain", 0, 1, 0.8f, "%");
    pRelease = addFloat("release", "Release", 1, 20000, 200, "ms", 3.0f);
    pVelSens = addFloat("velsens", "Velocity sensitivity", 0, 1, 1, "%");

    pMAttack = addFloat("mattack", "Mod envelope attack", 0, 10000, 1, "ms", 3.0f);
    pMDecay = addFloat("mdecay", "Mod envelope decay", 1, 20000, 500, "ms", 3.0f);
    pMSustain = addFloat("msustain", "Mod envelope sustain", 0, 1, 0, "%");
    pMRelease = addFloat("mrelease", "Mod envelope release", 1, 20000, 300, "ms", 3.0f);
    pMEnvPitch = addFloat("menvpitch", "Mod envelope to pitch", -48, 48, 0, "st");

    const float defRate[2] = {1.0f, 0.25f};
    for (int k = 0; k < 2; ++k) {
        const std::string n = "lfo" + std::to_string(k + 1), d = "LFO " + std::to_string(k + 1) + " ";
        pLfoWave[k] = addChoice((n + "wave").c_str(), (d + "shape").c_str(), parts::lfoNames(), k);
        pLfoRate[k] = addFloat((n + "rate").c_str(), (d + "rate").c_str(), 0.01f, 40, defRate[k], "Hz", 3.0f);
        pLfoDelay[k] = addFloat((n + "delay").c_str(), (d + "delay").c_str(), 0, 5000, 0, "ms", 3.0f);
        pLfoPitch[k] = addFloat((n + "pitch").c_str(), (d + "to pitch").c_str(), 0, 1200, 0, "ct", 3.0f);
        pLfoCutoff[k] = addFloat((n + "cutoff").c_str(), (d + "to cutoff").c_str(), -4, 4, 0, "oct");
        pLfoAmp[k] = addFloat((n + "amp").c_str(), (d + "to level").c_str(), 0, 1, 0, "%");
    }
    ParamInfo vi;
    vi.id = "voices"; vi.name = "Polyphony"; vi.min = 1; vi.max = (float)kMaxPoly; vi.def = 16; vi.steps = kMaxPoly;
    pVoices = add(vi);
    pGlide = addFloat("glide", "Glide", 0, 2000, 0, "ms", 3.0f);
    pVolume = addFloat("volume", "Level", 0, 1, 0.5f, "%");

    pChorus = addFloat("chorus", "Chorus mix", 0, 1, 0, "%");
    pChRate = addFloat("chrate", "Chorus rate", 0.05f, 5, 0.6f, "Hz", 2.0f);
    pChDepth = addFloat("chdepth", "Chorus depth", 0, 1, 0.5f, "%");
    pDelay = addFloat("delay", "Delay mix", 0, 1, 0, "%");
    pDTime = addFloat("dtime", "Delay time", 20, 1000, 300, "ms", 2.0f);
    pDFeed = addFloat("dfeed", "Delay feedback", 0, 0.9f, 0.35f, "%");
}

std::vector<ParamGroup> Ripple::paramGroups() const {
    std::vector<ParamGroup> g;
    for (int o = 0; o < 2; ++o) {
        ParamGroup pg;
        pg.title = "Oscillator " + std::to_string(o + 1);
        for (const char* w : {"table", "pos", "posenv", "poslfo1", "poslfo2", "posy", "coarse", "fine", "level", "pan", "uni", "detune", "spread", "phase"})
            pg.ids.push_back(oscId(o, w));
        g.push_back(pg);
    }
    g.push_back({"Sub and noise", {"subwave", "suboct", "sublevel", "noise", "noisecolor"}});
    g.push_back({"Filter", {"ftype", "cutoff", "reso", "fenv", "fkey", "fvel", "fattack", "fdecay", "fsustain", "frelease"}});
    g.push_back({"Amplitude", {"attack", "decay", "sustain", "release", "velsens"}});
    g.push_back({"Mod envelope", {"mattack", "mdecay", "msustain", "mrelease", "menvpitch"}});
    g.push_back({"LFO 1", {"lfo1wave", "lfo1rate", "lfo1delay", "lfo1pitch", "lfo1cutoff", "lfo1amp"}});
    g.push_back({"LFO 2", {"lfo2wave", "lfo2rate", "lfo2delay", "lfo2pitch", "lfo2cutoff", "lfo2amp"}});
    g.push_back({"Effects", {"chorus", "chrate", "chdepth", "delay", "dtime", "dfeed"}});
    g.push_back({"Global", {"voices", "glide", "volume"}});
    return g;
}

// ---- the user wavetable ----

void Ripple::setWavetable(TablePtr table) {
    user_.collect();            // let go of tables the audio thread has moved off since the last swap
    auto old = user_.get();
    auto s = std::make_shared<UserSlot>();
    s->prev = old ? old->cur : nullptr;
    s->cur = std::move(table);
    {
        std::lock_guard<std::mutex> g(pathM_);
        path_ = s->cur ? s->cur->path : std::string();
    }
    user_.set(s);
}
Ripple::TablePtr Ripple::wavetable() const {
    auto s = user_.get();
    return s ? s->cur : nullptr;
}
std::string Ripple::wavetablePath() const {
    std::lock_guard<std::mutex> g(pathM_);
    return path_;
}

bool Ripple::loadWavetable(const std::string& path, std::string* error) {
    if (path.empty()) { if (error) *error = "no file given"; return false; }
    auto audio = readAudioFile(SampleCache::global().resolve(path), error);
    if (!audio) return false;
    std::string name = path;
    const size_t cut = name.find_last_of("/\\");
    if (cut != std::string::npos) name = name.substr(cut + 1);
    auto t = fromAudio(*audio, name, error);
    if (!t) return false;
    t->path = path;
    setWavetable(t);
    return true;
}

Json Ripple::saveExtra() const {
    Json j = Json::object();
    j.set("wavetable", wavetablePath());
    return j;
}

void Ripple::loadExtra(const Json& j, const std::string& projectDir) {
    const std::string path = j["wavetable"].str("");
    if (path.empty()) { if (wavetable() || !wavetablePath().empty()) setWavetable(nullptr); return; }
    auto cur = wavetable();
    if (cur && cur->path == path) return;
    std::string err;
    auto audio = readAudioFile(SampleCache::global().resolve(path, projectDir), &err);
    const size_t cut = path.find_last_of("/\\");
    auto t = audio ? fromAudio(*audio, cut == std::string::npos ? path : path.substr(cut + 1), &err) : nullptr;
    if (!t) {
        // the file is missing or unreadable: play the fallback, but keep the reference so saving again does not lose it
        setWavetable(nullptr);
        std::lock_guard<std::mutex> g(pathM_);
        path_ = path;
        return;
    }
    t->path = path;
    setWavetable(t);
}

const Ripple::Wavetable* Ripple::resolve(int table, uint32 serial) const {
    const auto& b = builtins().t;
    if (table >= 0 && table < (int)b.size()) return b[(size_t)table].get();
    const Wavetable* t = nullptr;
    // a voice that has not caught up with a swap yet keeps reading the table it was on
    if (slot_) t = (serial + 1 == slotSerial_) ? slot_->prev.get() : slot_->cur.get();
    return t ? t : b[0].get();
}

// ---- audio ----

void Ripple::prepare(double sampleRate) {
    sr_ = sampleRate;
    // Harmonics may run up to here. Past half the sample rate they fold back, but only into the
    // band above 19 kHz, so nothing lands where it can be heard.
    maxHz_ = std::max(0.5 * sampleRate, sampleRate - 19000.0);
    for (int c = 0; c < 2; ++c) {
        chorus_[c].prepare((int)(0.05 * sampleRate));
        delay_[c].prepare((int)(1.1 * sampleRate));
    }
    dlDampCoef_ = (float)(1.0 - std::exp(-kTwoPi * 6000.0 / sampleRate));
    reset();
}

void Ripple::reset() {
    for (auto& v : voices_) v.active = false;
    voicesActive_.store(0, std::memory_order_relaxed);
    for (int c = 0; c < 2; ++c) { chorus_[c].clear(); delay_[c].clear(); dlDamp_[c] = 0.0f; }
    chPhase_ = 0.0f;
    lastKey_ = -1.0f;
    primed_ = false;
}

double Ripple::tailSeconds() const {
    if (pDelay->get() <= 0.0f) return 0.0;
    const double feed = std::max(0.01, (double)pDFeed->get());
    return std::min(20.0, 0.001 * pDTime->get() * (1.0 + std::log(0.001) / std::log(feed)));
}

void Ripple::beginBlock(ProcessContext& ctx) {
    slot_ = user_.rtAcquire();
    if (slot_ != lastSlot_) { ++slotSerial_; lastSlot_ = slot_; }
    hostBend_ = ctx.pitchBendSemis;
    Snap& S = snap_;
    for (int o = 0; o < 2; ++o) {
        const OscParams& p = osc[o];
        OscSnap& s = S.osc[o];
        s.table = p.table->getInt();
        s.pos = p.pos->get();
        s.ratio = (float)std::pow(2.0, ((double)p.coarse->get() + (double)p.fine->get() * 0.01) / 12.0);
        s.level = p.level->get();
        s.phaseRand = p.phase->get();
        s.posEnv = p.posEnv->get();
        s.posLfo[0] = p.posLfo[0]->get();
        s.posLfo[1] = p.posLfo[1]->get();
        s.posY = p.posY->get();
        s.uni = clampT(p.uni->getInt(), 1, kUnison);
        const float detune = p.detune->get(), spread = p.spread->get(), pan = p.pan->get();
        for (int u = 0; u < kUnison; ++u) {
            // unison voices are spread evenly either side of the note, a little wider towards the edges, and alternate left and right
            const float x = s.uni > 1 && u < s.uni ? -1.0f + 2.0f * (float)u / (float)(s.uni - 1) : 0.0f;
            s.uniRatio[u] = x == 0.0f ? 1.0f : std::pow(2.0f, detune * x * (0.6f + 0.4f * std::fabs(x)) / 1200.0f);
            s.uniPan[u] = clampT(pan + spread * std::fabs(x) * (u % 2 ? 1.0f : -1.0f), -1.0f, 1.0f);
        }
    }
    S.subWave = pSubWave->getInt();
    S.subRatio = pSubOct->getInt() == 1 ? 0.25f : 0.5f;
    S.subLevel = pSubLevel->get();
    S.noise = pNoise->get();
    S.noiseCoef = (float)(1.0 - std::exp(-kTwoPi * std::min((double)pNoiseColor->get(), 0.45 * sr_) / sr_));
    S.ftype = pFType->getInt();
    S.cutoff = pCutoff->get();
    S.k = parts::svfK(pReso->get());
    S.fenv = pFEnv->get();
    S.fkey = pFKey->get();
    S.fvel = pFVel->get();
    Param* envs[3][4] = {{pAttack, pDecay, pSustain, pRelease}, {pFAttack, pFDecay, pFSustain, pFRelease}, {pMAttack, pMDecay, pMSustain, pMRelease}};
    for (int e = 0; e < 3; ++e) for (int k = 0; k < 4; ++k) S.env[e][k] = envs[e][k]->get();
    S.menvPitch = pMEnvPitch->get();
    for (int k = 0; k < 2; ++k) {
        S.lfo[k].wave = pLfoWave[k]->getInt();
        S.lfo[k].rate = pLfoRate[k]->get();
        S.lfo[k].delay = pLfoDelay[k]->get();
        S.lfo[k].pitch = pLfoPitch[k]->get();
        S.lfo[k].cutoff = pLfoCutoff[k]->get();
        S.lfo[k].amp = pLfoAmp[k]->get();
    }
    S.voices = clampT(pVoices->getInt(), 1, kMaxPoly);
    S.glideMs = pGlide->get();
    S.velSens = pVelSens->get();

    const float vol = pVolume->get() * 0.7f, ch = pChorus->get(), dl = pDelay->get(), fb = pDFeed->get();
    chRate_ = pChRate->get();
    chDepth_ = pChDepth->get();
    dlTarget_ = (float)(0.001 * pDTime->get() * sr_);
    if (!primed_) { volume_.snap(vol); chMix_.snap(ch); dlMix_.snap(dl); dlFeed_.snap(fb); dlTime_ = dlTarget_; primed_ = true; }
    volume_.target(vol, kQuantum);
    chMix_.target(ch, kQuantum);
    dlMix_.target(dl, kQuantum);
    dlFeed_.target(fb, kQuantum);

    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
}

void Ripple::startVoice(const NoteEvent& e) {
    const Snap& S = snap_;
    const float killStep = (float)(1.0 / (0.003 * sr_));
    for (;;) {      // polyphony: fade out the oldest sounding voices until there is room
        int living = 0;
        Voice* oldest = nullptr;
        for (auto& c : voices_)
            if (c.active && c.killStep == 0.0f) { ++living; if (!oldest || c.age < oldest->age) oldest = &c; }
        if (living < S.voices || !oldest) break;
        oldest->killStep = killStep;
    }
    Voice* v = nullptr;
    for (auto& c : voices_) if (!c.active) { v = &c; break; }
    if (!v) {
        for (auto& c : voices_) if (c.killStep > 0.0f && (!v || c.kill < v->kill)) v = &c;
        if (!v) { v = &voices_[0]; for (auto& c : voices_) if (c.age < v->age) v = &c; }
    }
    v->active = true;
    v->released = false;
    v->id = e.id;
    v->age = ++ageCounter_;
    v->key = e.key;
    v->group = e.group;
    v->vel = e.velocity / kDefaultVelocity;
    v->pan = e.pan;
    v->fine = e.fineCents;
    v->modX = e.modX;
    v->modY = e.modY;
    v->relScale = 0.25f + 1.5f * e.release;
    v->bend = v->bendTarget = v->bendStep = 0.0f;
    if (e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) {
        v->bend = e.glideFromKey - (float)e.key;
        v->bendStep = -v->bend / e.slideSamples;
    } else if (S.glideMs > 0.0f && lastKey_ >= 0.0f && lastKey_ != (float)e.key) {
        v->bend = lastKey_ - (float)e.key;
        v->bendStep = -v->bend / std::max(1.0f, (float)(S.glideMs * 0.001 * sr_));
    }
    lastKey_ = (float)e.key;
    v->kill = 1.0f;
    v->killStep = 0.0f;
    v->time = 0.0;
    v->ctlLeft = 0;
    v->rng = dsp::Noise(0x7A5E7u + e.id * 2654435761u);
    v->noise = dsp::Noise(0x1234567u + e.id * 40503u);
    v->noiseZ = 0.0f;
    for (int k = 0; k < 2; ++k) { v->lfoPhase[k] = 0.0f; v->lfoRnd[k] = v->rng.next(); }
    for (int o = 0; o < 2; ++o) {
        OscVoice& ov = v->osc[o];
        for (int u = 0; u < kUnison; ++u) {
            ov.phase[u] = (uint32)((double)v->rng.uni() * (double)S.osc[o].phaseRand * 4294967295.0);
            ov.gl[u] = ov.gr[u] = ov.glStep[u] = ov.grStep[u] = 0.0f;
            ov.inc[u] = 0;
            ov.incStep[u] = 0;
        }
        ov.uni = ov.run = 0;
    }
    v->sub.phase = 0.0;
    v->aenv.set(S.env[0][0], S.env[0][1], S.env[0][2], S.env[0][3] * v->relScale, sr_);
    v->fenv.set(S.env[1][0], S.env[1][1], S.env[1][2], S.env[1][3] * v->relScale, sr_ / kCtl);
    v->menv.set(S.env[2][0], S.env[2][1], S.env[2][2], S.env[2][3] * v->relScale, sr_ / kCtl);
    v->aenv.start();
    v->fenv.start();
    v->menv.start();
    v->svf[0].reset();
    v->svf[1].reset();
    control(*v, true);
}

void Ripple::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * sr_));
    auto release = [](Voice& v) { v.released = true; v.aenv.release(); v.fenv.release(); v.menv.release(); };
    switch (e.type) {
        case NoteEvent::On: startVoice(e); break;
        case NoteEvent::Off:
            for (auto& v : voices_) if (v.active && v.id == e.id) release(v);
            break;
        case NoteEvent::AllOff:
            for (auto& v : voices_) if (v.active) release(v);
            break;
        case NoteEvent::Kill:
            for (auto& v : voices_) if (v.active && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::KillNote:
            for (auto& v : voices_) if (v.active && v.id == e.id && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::SlideTo:
            for (auto& v : voices_)
                if (v.active && v.group == e.group && !v.released) {
                    v.bendTarget = (float)(e.key - v.key);
                    v.bendStep = (v.bendTarget - v.bend) / std::max(1.0f, e.slideSamples);
                }
            break;
    }
}

// Works out where every ramp of a voice should be one control period from now (or, for a new voice,
// where it starts). Control points are counted from the note's own first sample.
void Ripple::control(Voice& v, bool snap) {
    const Snap& S = snap_;
    if (!snap) {
        v.time += kCtl;
        if (v.bendStep != 0.0f) {
            v.bend += v.bendStep * (float)kCtl;
            if ((v.bendStep > 0.0f) == (v.bend >= v.bendTarget)) { v.bend = v.bendTarget; v.bendStep = 0.0f; }
        }
    }
    const float tMs = (float)(v.time * 1000.0 / sr_);
    float lv[2], cents = 0.0f, cutOct = 0.0f, trem = 1.0f;
    for (int k = 0; k < 2; ++k) {
        const LfoSnap& l = S.lfo[k];
        if (!snap) {
            v.lfoPhase[k] += l.rate * (float)kCtl / (float)sr_;
            if (v.lfoPhase[k] >= 1.0f) { v.lfoPhase[k] -= std::floor(v.lfoPhase[k]); v.lfoRnd[k] = v.rng.next(); }
        }
        const float fade = clampT((tMs - l.delay) / (0.5f * l.delay + 10.0f), 0.0f, 1.0f);
        const float x = parts::lfoShape(l.wave, v.lfoPhase[k], v.lfoRnd[k]);
        lv[k] = x * fade;
        cents += lv[k] * l.pitch;
        cutOct += lv[k] * l.cutoff;
        trem *= 1.0f - l.amp * fade * 0.5f * (1.0f - x);
    }
    const float me = v.menv.next(), fe = v.fenv.next();
    const double semis = (double)v.key + (double)v.bend + (double)hostBend_ + (double)(v.fine + cents) * 0.01 + (double)(me * S.menvPitch);
    const double base = keyToHz(semis);
    const float rampK = 1.0f / (float)kCtl;
    auto ramp = [&](float& cur, float& step, float target) {
        if (snap) { cur = target; step = 0.0f; }
        else step = (target - cur) * rampK;
    };

    for (int o = 0; o < 2; ++o) {
        const OscSnap& os = S.osc[o];
        OscVoice& ov = v.osc[o];
        const int U = os.level > 0.0f ? os.uni : 0;
        const double f0 = base * os.ratio, fmax = U > 0 ? f0 * os.uniRatio[U - 1] : f0;
        // an oscillator whose fundamental would pass half the sample rate is faded out instead
        const float nyq = clampT((float)((0.49 * sr_ - fmax) / (0.04 * sr_)), 0.0f, 1.0f);
        const float lvl = U > 0 ? os.level * nyq / std::sqrt((float)U) : 0.0f;
        for (int u = 0; u < kUnison; ++u) {
            float tl = 0.0f, tr = 0.0f;
            if (u < U) {
                parts::panGains(os.uniPan[u], tl, tr);
                tl *= lvl;
                tr *= lvl;
                const uint32 incT = (uint32)(std::min(f0 * os.uniRatio[u], 0.49 * sr_) / sr_ * 4294967296.0);
                if (snap || u >= ov.uni) { ov.inc[u] = incT; ov.incStep[u] = 0; }       // a unison voice that joins starts at pitch
                else ov.incStep[u] = (std::int32_t)(((int64)incT - (int64)ov.inc[u]) / kCtl);
            } else ov.incStep[u] = 0;
            ramp(ov.gl[u], ov.glStep[u], tl);
            ramp(ov.gr[u], ov.grStep[u], tr);
        }
        ov.run = snap ? U : std::max(U, ov.uni);        // voices that were dropped fade out over this period
        ov.uni = U;
        ramp(ov.pos, ov.posStep, clampT(os.pos + os.posEnv * me + os.posLfo[0] * lv[0] + os.posLfo[1] * lv[1] + os.posY * (v.modY - 0.5f) * 2.0f, 0.0f, 1.0f));

        // The level whose harmonics all fit under the limit at this pitch.
        int mip = ov.mip;
        if (U > 0) {
            const double fit = maxHz_ / fmax;
            mip = 0;
            while (mip < kMips - 1 && (double)Wavetable::mipHarmonics(mip) > fit) ++mip;
        }
        if (snap) {
            ov.table = os.table;
            ov.mip = mip;
            ov.xfade = false;
        } else {
            const bool swapped = os.table >= numBuiltinTables() && ov.serial != slotSerial_;
            ov.xfade = os.table != ov.table || mip != ov.mip || swapped;
            if (ov.xfade) {     // read the old picture too over this period and cross-fade
                ov.fromTable = ov.table;
                ov.fromMip = ov.mip;
                ov.fromSerial = ov.serial;
                ov.xf = 0.0f;
                ov.table = os.table;
                ov.mip = mip;
            }
        }
        ov.serial = slotSerial_;
    }

    ramp(v.subG, v.subGStep, S.subLevel);
    ramp(v.subDt, v.subDtStep, (float)std::min(0.45, base * S.subRatio / sr_));
    ramp(v.noiseG, v.noiseGStep, S.noise);

    // Filter: envelope, key tracking, velocity, LFOs and mod X (five octaves either way) move the cutoff.
    const float oct = S.fenv * fe + S.fkey * ((float)(v.key - kRootKey) + v.bend) / 12.0f + (v.modX - 0.5f) * 10.0f
                    + S.fvel * 3.0f * (v.vel - 1.0f) + cutOct;
    const double fc = clampT((double)S.cutoff * std::pow(2.0, (double)oct), 20.0, 0.45 * sr_);
    ramp(v.g, v.gStep, parts::svfG(fc, sr_));
    ramp(v.k, v.kStep, S.k);

    const float velGain = std::max(0.0f, 1.0f + (v.vel * v.vel - 1.0f) * S.velSens);
    float pl, pr;
    parts::panGains(v.pan, pl, pr);
    ramp(v.outL, v.outLStep, velGain * trem * pl);
    ramp(v.outR, v.outRStep, velGain * trem * pr);
}

void Ripple::renderOsc(OscVoice& o, float* L, float* R, int m) {
    if (o.run <= 0) { o.pos += o.posStep * (float)m; return; }
    const Wavetable* to = resolve(o.table, o.serial);
    if (!o.xfade) {
        oscRun(*to, o.mip, o.run, o.phase, o.inc, o.incStep, o.gl, o.glStep, o.gr, o.grStep, o.pos, o.posStep, L, R, m, 1.0f, 0.0f);
        return;
    }
    // The old table or level fades out on a copy of the oscillator's state, the new one fades in on the real state.
    const Wavetable* from = resolve(o.fromTable, o.fromSerial);
    OscVoice old = o;
    const float step = 1.0f / (float)kCtl;
    oscRun(*from, o.fromMip, old.run, old.phase, old.inc, old.incStep, old.gl, old.glStep, old.gr, old.grStep, old.pos, old.posStep, L, R, m, 1.0f - o.xf, -step);
    oscRun(*to, o.mip, o.run, o.phase, o.inc, o.incStep, o.gl, o.glStep, o.gr, o.grStep, o.pos, o.posStep, L, R, m, o.xf, step);
    o.xf += step * (float)m;
}

void Ripple::renderVoice(Voice& v, float* L, float* R, int n) {
    const Snap& S = snap_;
    int pos = 0;
    while (pos < n && v.active) {
        if (v.ctlLeft == 0) { control(v, false); v.ctlLeft = kCtl; }
        const int m = std::min(n - pos, v.ctlLeft);
        float bl[kCtl], br[kCtl];
        std::memset(bl, 0, sizeof(float) * (size_t)m);
        std::memset(br, 0, sizeof(float) * (size_t)m);
        renderOsc(v.osc[0], bl, br, m);
        renderOsc(v.osc[1], bl, br, m);

        if (v.subG != 0.0f || v.subGStep != 0.0f) {
            for (int s = 0; s < m; ++s) {
                v.subDt += v.subDtStep;
                v.subG += v.subGStep;
                const float x = v.sub.next(S.subWave, (double)v.subDt) * v.subG;
                bl[s] += x;
                br[s] += x;
            }
        } else v.subDt += v.subDtStep * (float)m;
        if (v.noiseG != 0.0f || v.noiseGStep != 0.0f) {
            for (int s = 0; s < m; ++s) {
                v.noiseG += v.noiseGStep;
                v.noiseZ += S.noiseCoef * (v.noise.next() - v.noiseZ);
                const float x = v.noiseZ * v.noiseG;
                bl[s] += x;
                br[s] += x;
            }
        }

        if (S.ftype != 0) {
            const float cl = (S.ftype == 1 || S.ftype == 2 || S.ftype == 5) ? 1.0f : 0.0f, ch = (S.ftype == 4 || S.ftype == 5) ? 1.0f : 0.0f;
            const bool band = S.ftype == 3, steep = S.ftype == 2;
            for (int s = 0; s < m; ++s) {
                v.g += v.gStep;
                v.k += v.kStep;
                v.svf[0].process(v.g, v.k, cl, band ? v.k : 0.0f, ch, bl[s], br[s]);
                if (steep) v.svf[1].process(v.g, 1.4142f, 1.0f, 0.0f, 0.0f, bl[s], br[s]);
            }
            v.svf[0].flush();
            v.svf[1].flush();
        } else {
            v.g += v.gStep * (float)m;
            v.k += v.kStep * (float)m;
            v.svf[0].reset();
            v.svf[1].reset();
        }

        float* oL = L + pos;
        float* oR = R + pos;
        for (int s = 0; s < m; ++s) {
            const float a = v.aenv.next() * v.kill;
            if (v.killStep > 0.0f) v.kill = std::max(0.0f, v.kill - v.killStep);
            v.outL += v.outLStep;
            v.outR += v.outRStep;
            oL[s] += bl[s] * a * v.outL;
            oR[s] += br[s] * a * v.outR;
        }
        v.ctlLeft -= m;
        pos += m;
        if (!v.aenv.active() || v.kill <= 0.0f) v.active = false;
    }
}

void Ripple::render(float* L, float* R, int n, ProcessContext&) {
    float bl[kQuantum], br[kQuantum];
    std::memset(bl, 0, sizeof(float) * (size_t)n);
    std::memset(br, 0, sizeof(float) * (size_t)n);
    for (auto& v : voices_) if (v.active) renderVoice(v, bl, br, n);

    const float chStep = chRate_ / (float)sr_, chBase = (float)(0.012 * sr_), chSwing = (float)(0.005 * sr_) * chDepth_;
    for (int s = 0; s < n; ++s) {
        float l = bl[s], r = br[s];
        // chorus: two modulated taps per side, a quarter cycle apart between left and right
        const float mix = chMix_.next();
        chPhase_ += chStep;
        if (chPhase_ >= 1.0f) chPhase_ -= 1.0f;
        const float a = (float)kTwoPi * chPhase_;
        const float wl = 0.5f * (chorus_[0].tapFrac(chBase + chSwing * std::sin(a)) + chorus_[0].tapFrac(chBase * 1.6f + chSwing * std::sin(a + 2.1f)));
        const float wr = 0.5f * (chorus_[1].tapFrac(chBase + chSwing * std::sin(a + 1.5708f)) + chorus_[1].tapFrac(chBase * 1.6f + chSwing * std::sin(a + 3.67f)));
        chorus_[0].push(l);
        chorus_[1].push(r);
        const float norm = 1.0f / (1.0f + 0.5f * mix);
        l = (l + mix * wl) * norm;
        r = (r + mix * wr) * norm;
        // delay: the time glides to a new setting, the repeats lose a little top each time round
        const float dmix = dlMix_.next(), feed = dlFeed_.next();
        dlTime_ += (dlTarget_ - dlTime_) * 0.001f;
        const float el = delay_[0].tapFrac(dlTime_ - 1.0f), er = delay_[1].tapFrac(dlTime_ - 1.0f);
        dlDamp_[0] += dlDampCoef_ * (el - dlDamp_[0]);
        dlDamp_[1] += dlDampCoef_ * (er - dlDamp_[1]);
        delay_[0].push(l + feed * dlDamp_[0]);
        delay_[1].push(r + feed * dlDamp_[1]);
        l += dmix * el;
        r += dmix * er;
        const float vol = volume_.next();
        L[s] += l * vol;
        R[s] += r * vol;
    }
    flushDenormal(dlDamp_[0]);
    flushDenormal(dlDamp_[1]);
    scope_.push(L, R, n);
}

// ------------------------------------------------------------------ presets

namespace {

const char* intern(const std::string& s) {
    static std::deque<std::string> pool;
    for (auto& p : pool) if (p == s) return p.c_str();
    pool.push_back(s);
    return pool.back().c_str();
}

// The built-in tables by number, in the order buildBuiltins() adds them.
namespace wt {
enum { Basic, PulseWidth, HarmonicSweep, OvertoneLadder, OddEven, Vowels, FormantSweep, FmBright, FmHollow, FmClang, BellTower,
       Drawbars, SawSync, SineSync, FoldedSine, PhaseBend, Stairs, DigitalGrit, CombShift, GlassHarmonics, OctaveStack, BreathNoise, Count };
}

struct Patch {
    PresetDef d;
    explicit Patch(const char* name) { d.name = name; }
    Patch& set(const char* id, float v) { d.values.push_back({id, v}); return *this; }
    Patch& o(int n, const char* what, float v) { d.values.push_back({intern(oscId(n - 1, what)), v}); return *this; }
    Patch& osc(int n, int table, float pos, float level) { return o(n, "table", (float)table).o(n, "pos", pos).o(n, "level", level); }
    Patch& tune(int n, float coarse, float fine) { return o(n, "coarse", coarse).o(n, "fine", fine); }
    Patch& uni(int n, int voices, float detune, float spread) { return o(n, "uni", (float)voices).o(n, "detune", detune).o(n, "spread", spread).o(n, "phase", 1.0f); }
    // how the position moves: from the mod envelope, LFO 1 and LFO 2
    Patch& move(int n, float env, float lfo1, float lfo2) { return o(n, "posenv", env).o(n, "poslfo1", lfo1).o(n, "poslfo2", lfo2); }
    Patch& amp(float a, float dc, float s, float r) { return set("attack", a).set("decay", dc).set("sustain", s).set("release", r); }
    Patch& filter(int type, float cutoff, float reso) { return set("ftype", (float)type).set("cutoff", cutoff).set("reso", reso); }
    Patch& fenv(float amount, float a, float dc, float s, float r) { return set("fenv", amount).set("fattack", a).set("fdecay", dc).set("fsustain", s).set("frelease", r); }
    Patch& menv(float a, float dc, float s, float r) { return set("mattack", a).set("mdecay", dc).set("msustain", s).set("mrelease", r); }
    Patch& lfo(int k, int wave, float rate) {
        const std::string n = "lfo" + std::to_string(k);
        return set(intern(n + "wave"), (float)wave).set(intern(n + "rate"), rate);
    }
    Patch& sub(int wave, float level) { return set("subwave", (float)wave).set("sublevel", level); }
    Patch& chorus(float mix) { return set("chorus", mix); }
    Patch& delay(float mix, float ms, float feed) { return set("delay", mix).set("dtime", ms).set("dfeed", feed); }
    operator PresetDef() const { return d; }
};

// Filter types: 1 low pass 12, 2 low pass 24, 3 band pass, 4 high pass, 5 notch.
// LFO shapes: 0 sine, 1 triangle, 2 saw up, 3 saw down, 4 square, 5 random steps.
std::vector<PresetDef> buildDefs() {
    std::vector<PresetDef> p;
    p.push_back({"Default", {}});

    // ---- pads
    p.push_back(Patch("Tidal Pad").osc(1, wt::HarmonicSweep, 0.3f, 0.7f).uni(1, 5, 18, 0.8f).move(1, 0, 0.3f, 0)
        .osc(2, wt::Vowels, 0.3f, 0.45f).uni(2, 3, 12, 0.6f).move(2, 0, 0, 0.4f)
        .lfo(1, 1, 0.18f).lfo(2, 0, 0.11f).filter(1, 3500, 0.15f).fenv(1.5f, 900, 2500, 0.6f, 1500)
        .amp(600, 2000, 0.8f, 1800).chorus(0.5f));
    p.push_back(Patch("Vowel Drift").osc(1, wt::Vowels, 0.1f, 0.8f).uni(1, 3, 10, 0.7f).move(1, 0.4f, 0.45f, 0)
        .menv(3000, 4000, 0.6f, 2000).lfo(1, 0, 0.09f).filter(1, 6000, 0.1f).amp(400, 1500, 0.85f, 1500).chorus(0.35f));
    p.push_back(Patch("Glass Cathedral").osc(1, wt::GlassHarmonics, 0.15f, 0.7f).uni(1, 3, 8, 0.8f).move(1, 0.6f, 0, 0.15f)
        .osc(2, wt::BellTower, 0.5f, 0.35f).tune(2, 12, 4).move(2, 0, 0, 0.3f)
        .menv(2500, 4000, 0.5f, 2500).lfo(2, 1, 0.07f).amp(900, 3000, 0.8f, 2500).chorus(0.4f).delay(0.25f, 420, 0.45f).set("volume", 0.68f));
    p.push_back(Patch("Comb Phaser Pad").osc(1, wt::CombShift, 0.5f, 0.75f).uni(1, 4, 10, 0.7f).move(1, 0, 0.5f, 0)
        .osc(2, wt::Basic, 0.5f, 0.35f).tune(2, -12, 0).lfo(1, 2, 0.2f)
        .filter(1, 5000, 0.1f).amp(500, 2000, 0.85f, 1400).chorus(0.3f).set("volume", 0.72f));
    p.push_back(Patch("Slow Fold").osc(1, wt::FoldedSine, 0.05f, 0.8f).uni(1, 2, 7, 0.6f).move(1, 0.7f, 0, 0.1f)
        .sub(0, 0.4f).menv(4000, 5000, 0.7f, 3000).lfo(2, 0, 0.13f)
        .filter(2, 2500, 0.2f).fenv(2.0f, 2500, 4000, 0.7f, 2500).amp(800, 2000, 0.9f, 2500).chorus(0.3f));
    p.push_back(Patch("Choir of Wires").osc(1, wt::FormantSweep, 0.4f, 0.75f).uni(1, 5, 22, 0.9f).move(1, 0, 0.25f, 0)
        .osc(2, wt::OddEven, 0.4f, 0.3f).tune(2, 12, 0).move(2, 0, 0, 0.4f)
        .lfo(1, 0, 0.3f).lfo(2, 1, 0.08f).filter(1, 7000, 0.1f).amp(350, 1500, 0.85f, 900).chorus(0.4f));

    // ---- basses
    p.push_back(Patch("Rip Bass").osc(1, wt::SawSync, 0.1f, 0.8f).tune(1, -12, 0).move(1, 0.7f, 0, 0).sub(0, 0.6f)
        .menv(0, 250, 0, 150).filter(2, 900, 0.25f).fenv(3.0f, 0, 220, 0.1f, 120).amp(1, 400, 0.7f, 90).set("volume", 0.35f));
    p.push_back(Patch("Wobble Bass").osc(1, wt::FmBright, 0.35f, 0.75f).tune(1, -12, 0).move(1, 0, 0.4f, 0)
        .osc(2, wt::Basic, 0.75f, 0.4f).tune(2, -24, 0).lfo(1, 0, 3.0f)
        .filter(2, 500, 0.35f).set("lfo1cutoff", 2.5f).amp(2, 300, 1.0f, 80).set("volume", 0.27f));
    p.push_back(Patch("Reese Undertow").osc(1, wt::Basic, 0.5f, 0.7f).tune(1, -12, 0).uni(1, 3, 25, 0.5f)
        .osc(2, wt::PulseWidth, 0.2f, 0.45f).tune(2, -12, 0).move(2, 0, 0.3f, 0).lfo(1, 1, 0.4f)
        .filter(2, 700, 0.2f).set("fkey", 0.3f).amp(3, 500, 0.9f, 140));
    p.push_back(Patch("Formant Bass").osc(1, wt::FormantSweep, 0.15f, 0.8f).tune(1, -12, 0).move(1, 0.5f, 0, 0).sub(2, 0.35f)
        .menv(0, 300, 0.1f, 150).filter(1, 2500, 0.15f).amp(1, 500, 0.75f, 100).set("volume", 0.33f));
    p.push_back(Patch("Sub Pulse").osc(1, wt::PulseWidth, 0.05f, 0.7f).tune(1, -12, 0).move(1, 0, 0.3f, 0).sub(0, 0.7f)
        .lfo(1, 1, 1.2f).filter(2, 350, 0.2f).fenv(2.0f, 0, 150, 0.2f, 100).amp(1, 300, 0.85f, 80));
    p.push_back(Patch("Slap Table Bass").osc(1, wt::FmHollow, 0.15f, 0.8f).tune(1, -12, 0).move(1, 0.8f, 0, 0)
        .osc(2, wt::FmBright, 0.2f, 0.35f).tune(2, -12, 0).move(2, 0.5f, 0, 0)
        .menv(0, 120, 0, 100).filter(1, 3000, 0.1f).fenv(2.0f, 0, 200, 0.2f, 120).amp(1, 500, 0.5f, 90).set("fvel", 0.5f));

    // ---- leads
    p.push_back(Patch("Sync Scream").osc(1, wt::SawSync, 0.2f, 0.8f).uni(1, 2, 8, 0.4f).move(1, 0.5f, 0, 0)
        .menv(0, 600, 0.3f, 300).filter(1, 9000, 0.1f).amp(2, 300, 0.85f, 150)
        .lfo(1, 0, 5.5f).set("lfo1delay", 300).set("lfo1pitch", 12).set("glide", 60).delay(0.18f, 330, 0.3f).set("volume", 0.64f));
    p.push_back(Patch("Hollow Reed Lead").osc(1, wt::FmHollow, 0.35f, 0.8f).move(1, 0.15f, 0, 0.1f)
        .menv(30, 400, 0.5f, 200).lfo(2, 0, 0.4f).filter(1, 4000, 0.15f).fenv(1.0f, 20, 300, 0.6f, 200).amp(15, 300, 0.9f, 140)
        .lfo(1, 0, 5.2f).set("lfo1delay", 350).set("lfo1pitch", 10));
    p.push_back(Patch("Stair Lead").osc(1, wt::Stairs, 0.5f, 0.7f).move(1, -0.4f, 0, 0)
        .osc(2, wt::PulseWidth, 0.3f, 0.3f).tune(2, 12, 0).menv(0, 200, 0, 100).amp(0, 100, 0.8f, 40).set("volume", 0.66f));
    p.push_back(Patch("Laser Fold Lead").osc(1, wt::FoldedSine, 0.3f, 0.8f).uni(1, 3, 14, 0.6f).move(1, 0.6f, 0, 0)
        .menv(0, 400, 0.2f, 200).filter(1, 8000, 0.1f).amp(1, 400, 0.8f, 160).delay(0.25f, 280, 0.35f).set("volume", 0.39f));
    p.push_back(Patch("Vox Lead").osc(1, wt::Vowels, 0.25f, 0.85f).move(1, 0, 0.5f, 0).lfo(1, 1, 0.8f)
        .filter(1, 5000, 0.1f).amp(10, 300, 0.9f, 160).set("glide", 80).set("lfo2wave", 0).set("lfo2rate", 5.3f).set("lfo2delay", 400).set("lfo2pitch", 9));

    // ---- keys
    p.push_back(Patch("Drawbar Keys").osc(1, wt::Drawbars, 0.6f, 0.75f)
        .osc(2, wt::Drawbars, 0.3f, 0.3f).tune(2, 12, 0).amp(3, 200, 1.0f, 60).set("velsens", 0.2f).chorus(0.6f).set("chrate", 2.2f));
    p.push_back(Patch("Tine Table").osc(1, wt::BellTower, 0.15f, 0.6f).move(1, 0.5f, 0, 0)
        .osc(2, wt::Basic, 0.0f, 0.7f).menv(0, 500, 0, 300).amp(1, 1800, 0, 400).set("fvel", 0.5f)
        .filter(1, 6000, 0.05f).fenv(1.5f, 0, 600, 0, 300).chorus(0.25f));
    p.push_back(Patch("Glass Keys").osc(1, wt::GlassHarmonics, 0.1f, 0.6f).move(1, 0.4f, 0, 0)
        .osc(2, wt::Basic, 0.25f, 0.6f).menv(0, 700, 0, 400).amp(1, 2200, 0.15f, 500).chorus(0.3f).set("volume", 0.7f));
    p.push_back(Patch("Clang Keys").osc(1, wt::FmClang, 0.1f, 0.8f).move(1, 0.5f, 0, 0)
        .osc(2, wt::Basic, 0.0f, 0.4f).tune(2, -12, 0).menv(0, 350, 0, 250).amp(1, 1500, 0.1f, 350).set("fvel", 0.4f).filter(1, 9000, 0.05f));

    // ---- plucks
    p.push_back(Patch("Harmonic Pluck").osc(1, wt::HarmonicSweep, 0.0f, 0.8f).uni(1, 3, 10, 0.7f).move(1, 0.9f, 0, 0)
        .menv(0, 220, 0, 200).amp(0, 500, 0, 400).delay(0.3f, 330, 0.35f).set("volume", 0.7f));
    p.push_back(Patch("Octave Pluck").osc(1, wt::OctaveStack, 0.0f, 0.8f).move(1, 0.8f, 0, 0)
        .osc(2, wt::Basic, 0.5f, 0.35f).menv(0, 180, 0, 150).filter(1, 3000, 0.15f).fenv(3.0f, 0, 150, 0, 150).amp(0, 600, 0, 350));
    p.push_back(Patch("Bent Pluck").osc(1, wt::PhaseBend, 0.0f, 0.8f).move(1, 0.9f, 0, 0)
        .menv(0, 260, 0, 200).filter(2, 2000, 0.2f).fenv(2.5f, 0, 240, 0, 200).amp(0, 700, 0, 400));
    p.push_back(Patch("Bell Pluck").osc(1, wt::BellTower, 0.9f, 0.8f).move(1, -0.7f, 0, 0)
        .menv(0, 900, 0, 700).amp(0, 2500, 0, 1500).delay(0.2f, 380, 0.4f).set("volume", 0.64f));

    // ---- textures
    p.push_back(Patch("Digital Rain").osc(1, wt::DigitalGrit, 0.5f, 0.6f).move(1, 0, 0.5f, 0)
        .osc(2, wt::CombShift, 0.5f, 0.4f).tune(2, 12, 0).move(2, 0, 0, 0.5f)
        .lfo(1, 5, 9.0f).lfo(2, 2, 0.25f).filter(4, 400, 0.1f).amp(300, 1500, 0.8f, 2000).chorus(0.3f).delay(0.4f, 250, 0.5f).set("volume", 0.9f));
    p.push_back(Patch("Breathing Machine").osc(1, wt::BreathNoise, 0.3f, 0.7f).move(1, 0, 0.4f, 0)
        .osc(2, wt::OvertoneLadder, 0.5f, 0.45f).move(2, 0, 0, 0.5f).set("noise", 0.12f).set("noisecolor", 3000)
        .lfo(1, 0, 0.15f).lfo(2, 2, 0.07f).filter(1, 6000, 0.15f).set("lfo1cutoff", 1.0f).amp(1500, 2000, 0.9f, 3000).chorus(0.3f).set("volume", 0.94f));
    p.push_back(Patch("Overtone Singer").osc(1, wt::OvertoneLadder, 0.5f, 0.8f).uni(1, 2, 6, 0.5f).move(1, 0, 0.5f, 0)
        .sub(0, 0.3f).lfo(1, 1, 0.12f).amp(400, 1500, 0.9f, 1200).delay(0.3f, 450, 0.4f));
    p.push_back(Patch("Shifting Static").osc(1, wt::Stairs, 0.5f, 0.7f).move(1, 0, 0.5f, 0)
        .osc(2, wt::DigitalGrit, 0.3f, 0.4f).move(2, 0, 0, 0.3f)
        .lfo(1, 5, 6.0f).lfo(2, 0, 0.2f).filter(1, 2200, 0.45f).set("lfo2cutoff", 1.5f).amp(200, 1000, 0.85f, 1500).delay(0.3f, 180, 0.55f));
    p.push_back(Patch("Resonant Riser").osc(1, wt::SineSync, 0.0f, 0.8f).uni(1, 3, 9, 0.6f).move(1, 1.0f, 0, 0)
        .osc(2, wt::Basic, 0.0f, 0.4f).tune(2, -12, 0).menv(1500, 1500, 0.3f, 800).amp(50, 1000, 0.9f, 800).chorus(0.3f).delay(0.25f, 300, 0.4f));
    return p;
}

} // namespace

std::vector<Preset> Ripple::factoryPresets() const {
    static const std::vector<PresetDef> defs = buildDefs();
    return buildPresets(*this, defs);
}

} // namespace pulp
