#pragma once
// Helpers for the Sheaf tests: clean patches for measuring, and measures of where the partials are
// and of anything that is not a partial.
#include "FmwtTestUtil.h"
#include "pulp/Sheaf.h"

namespace sheaft {
using namespace pulp;
using namespace tu;
using namespace ft;

inline double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-12)); }

// A clean tone: one spectrum, no filter, a flat envelope, nothing moving.
inline std::shared_ptr<Sheaf> plainSheaf(int shape = Sheaf::Saw, int partials = 256) {
    auto s = std::make_shared<Sheaf>();
    s->spec[0].shape->set((float)shape);
    s->pFilterType->set((float)Sheaf::FilterOff);
    s->pAttack->set(0.0f);
    s->pSustain->set(1.0f);
    s->pPartials->set((float)partials);
    return s;
}
// Every partial at the same level: a saw (1/k) tilted up by 6.02 dB per octave.
inline void flatten(Sheaf& s) { s.spec[0].shape->set((float)Sheaf::Saw); s.spec[0].tilt->set(6.0206f); }

// One held note, driven straight into the plugin. Returns the left channel.
inline std::vector<float> hold(Sheaf& s, int key, double sr, int64 samples, std::vector<float>* right = nullptr, float vel = kDefaultVelocity) {
    Drive d(s, sr);
    d.on(0, key, 1, vel);
    d.run(samples);
    if (right) *right = d.R;
    return d.L;
}

// The frequencies a note's partials should be on: ratio(k) * f0 for k = 1.., below 98% of half the sample rate.
inline std::vector<double> harmonicsOf(double f0, double sr, int count, double stiff = 0.0) {
    std::vector<double> out;
    for (int k = 1; k <= count; ++k) {
        const double hz = f0 * Sheaf::stretched((double)k, stiff);
        if (hz >= 0.98 * 0.5 * sr) break;
        out.push_back(hz);
    }
    return out;
}

// The exact frequency of the steady partial near each guess, from how its phase moves between two
// windows a hop apart (both transformed once, so hundreds of partials cost two FFTs).
inline std::vector<double> lineFrequencies(const std::vector<float>& d, size_t start, double sr, const std::vector<double>& guess, int n = 32768, int hop = 2048) {
    std::vector<float> re1((size_t)n), im1((size_t)n, 0.0f), re2((size_t)n), im2((size_t)n, 0.0f);
    for (int i = 0; i < n; ++i) {
        const double x = kTwoPi * i / n;
        const double w = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
        re1[(size_t)i] = (float)(w * d[start + (size_t)i]);
        re2[(size_t)i] = (float)(w * d[start + (size_t)hop + (size_t)i]);
    }
    dsp::fft(re1.data(), im1.data(), n);
    dsp::fft(re2.data(), im2.data(), n);
    std::vector<double> out;
    for (double g : guess) {
        const int b = (int)std::lround(g * n / sr);
        if (b < 2 || b >= n / 2 - 2) { out.push_back(0.0); continue; }
        const double p1 = std::atan2((double)im1[(size_t)b], (double)re1[(size_t)b]), p2 = std::atan2((double)im2[(size_t)b], (double)re2[(size_t)b]);
        double dp = p2 - p1 - kTwoPi * (double)b * hop / n;
        dp -= kTwoPi * std::round(dp / kTwoPi);
        out.push_back(((double)b + dp * n / (kTwoPi * hop)) * sr / n);
    }
    return out;
}

// The strongest thing in the spectrum that is not within `guard` bins of a frequency the sound is
// meant to contain, in dB below the strongest partial. For a sound made only of its partials this is
// the noise floor; an alias, a click or a wrong partial shows up here.
// (Measured in double precision through a seven-term Blackman-Harris window, whose own side lobes are
// 180 dB down, so the floor it reports is the synthesizer's and not the measurement's.)
inline void fftDouble(std::vector<double>& re, std::vector<double>& im) {
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -kTwoPi / (double)len;
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k) {
                const double wr = std::cos(ang * (double)k), wi = std::sin(ang * (double)k);
                const size_t a = i + k, b = i + k + len / 2;
                const double xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
            }
    }
}
inline std::vector<double> spectrumFine(const std::vector<float>& d, size_t start, int n) {
    static const double a[7] = {0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803, 0.01081174209837, 0.00077658482522, 0.00001388721735};
    std::vector<double> re((size_t)n), im((size_t)n, 0.0);
    double ws = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = kTwoPi * i / n;
        double w = a[0];
        for (int k = 1; k < 7; ++k) w += (k & 1 ? -1.0 : 1.0) * a[k] * std::cos(k * x);
        re[(size_t)i] = w * d[start + (size_t)i];
        ws += w;
    }
    fftDouble(re, im);
    std::vector<double> m((size_t)n / 2);
    for (int i = 0; i < n / 2; ++i) m[(size_t)i] = 2.0 * std::hypot(re[(size_t)i], im[(size_t)i]) / ws;
    return m;
}
inline double strayDb(const std::vector<float>& d, size_t start, int n, double sr, const std::vector<double>& expected, int guard = 12, double* whereHz = nullptr) {
    const auto m = spectrumFine(d, start, n);
    std::vector<char> ok((size_t)n / 2, 0);
    for (double hz : expected) {
        const int b = (int)std::lround(hz * n / sr);
        for (int k = std::max(0, b - guard); k <= std::min(n / 2 - 1, b + guard); ++k) ok[(size_t)k] = 1;
    }
    double strongest = 0.0, stray = 0.0;
    for (int b = 0; b < n / 2; ++b) {
        strongest = std::max(strongest, m[(size_t)b]);
        if (b < guard || ok[(size_t)b]) continue;
        if (m[(size_t)b] > stray) { stray = m[(size_t)b]; if (whereHz) *whereHz = b * sr / n; }
    }
    if (strongest < 1.0e-9) return -200.0;
    return db(stray / strongest);
}

// The amplitude of the partial at exactly hz, through the same seven-term window (a sine of amplitude A
// reads A; a partial 14 bins or more away adds nothing down to -180 dB).
inline double ampFine(const std::vector<float>& d, size_t start, int n, double hz, double sr) {
    static std::vector<double> win;
    static double wsum = 0.0;
    if ((int)win.size() != n) {
        static const double a[7] = {0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803, 0.01081174209837, 0.00077658482522, 0.00001388721735};
        win.assign((size_t)n, 0.0);
        wsum = 0.0;
        for (int i = 0; i < n; ++i) {
            const double x = kTwoPi * i / n;
            double w = a[0];
            for (int k = 1; k < 7; ++k) w += (k & 1 ? -1.0 : 1.0) * a[k] * std::cos(k * x);
            win[(size_t)i] = w;
            wsum += w;
        }
    }
    // a rotating phasor instead of a sine and cosine per sample
    const double cw = std::cos(kTwoPi * hz / sr), sw = std::sin(kTwoPi * hz / sr);
    double c = 1.0, s = 0.0, re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
        const double x = win[(size_t)i] * d[start + (size_t)i];
        re += x * c;
        im -= x * s;
        const double nc = c * cw - s * sw;
        s = s * cw + c * sw;
        c = nc;
    }
    return 2.0 * std::hypot(re, im) / wsum;
}

// Level of harmonic k of f0 in dB relative to harmonic 1.
inline double harmonicDb(const std::vector<float>& d, size_t start, int n, double f0, int k, double sr) {
    return db(ampAt(d, start, n, f0 * k, sr) / ampAt(d, start, n, f0, sr));
}

inline void loadPreset(Sheaf& s, const std::string& name) {
    for (auto& pr : s.factoryPresets()) if (pr.name == name) { s.loadState(pr.state); return; }
    CHECK_MSG(false, "no preset %s", name.c_str());
}

// A recording of one note for the analyser: harmonic k has amplitude amp[k-1] * env(k, t).
inline std::shared_ptr<SampleData> harmonicNote(double hz, double seconds, double sr, const std::vector<double>& amp,
                                                const std::function<double(int, double)>& env, int channels = 1) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = channels;
    const int n = (int)(seconds * sr);
    for (int c = 0; c < channels; ++c) s->ch[c].assign((size_t)n, 0.0f);
    for (int i = 0; i < n; ++i) {
        const double t = i / sr;
        double v = 0.0;
        for (size_t k = 1; k <= amp.size(); ++k) {
            if (hz * (double)k > 0.45 * sr || amp[k - 1] == 0.0) continue;
            v += amp[k - 1] * (env ? env((int)k, t) : 1.0) * std::sin(kTwoPi * hz * (double)k * t + 0.7 * (double)k);
        }
        for (int c = 0; c < channels; ++c) s->ch[c][(size_t)i] = (float)v;
    }
    return s;
}

} // namespace sheaft
