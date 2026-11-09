// What the tone effects share: the oversampling filter tables.
#include "pulp/Tone.h"

namespace pulp::tonefx {

namespace {

double bessel0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 80; ++k) {
        const double h = x / (2.0 * k);
        term *= h * h;
        sum += term;
        if (term < 1.0e-18 * sum) break;
    }
    return sum;
}

// Kaiser-windowed low-pass, n taps, cut-off in cycles per sample. An even n puts the centre between two taps.
void kaiserLowpass(double* h, int n, double fc, double beta) {
    const double centre = (n - 1) / 2.0;
    const double norm = bessel0(beta);
    for (int i = 0; i < n; ++i) {
        const double m = i - centre;
        const double sinc = std::fabs(m) < 1.0e-9 ? 2.0 * fc : std::sin(kTwoPi * fc * m) / (kPi * m);
        const double r = m / centre;
        h[i] = sinc * bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / norm;
        // Where the sinc crosses zero the arithmetic leaves 1e-17 instead of nothing. Two such taps in a row would
        // turn the last of a sound into denormal numbers, so they are made the zeros they stand for.
        if (std::fabs(h[i]) < 1.0e-12) h[i] = 0.0;
    }
}

struct AllTables {
    OsTables t[3][5];
    AllTables() {
        static const int factors[3] = {2, 4, 8};
        const double beta = 8.5;        // about 85 dB down outside the pass band
        double h[260];
        for (int fi = 0; fi < 3; ++fi) {
            const int R = factors[fi];
            const int nUp = 32 * R + 1;
            kaiserLowpass(h, nUp, 0.5 / R, beta);
            for (int m = 0; m < 5; ++m) {
                OsTables& o = t[fi][m];
                o.factor = R;
                o.halfDelays = m;
                std::memset(o.up, 0, sizeof o.up);
                std::memset(o.down, 0, sizeof o.down);
                for (int p = 0; p < R; ++p) {
                    double sum = 0.0;
                    for (int k = 0; k < 33; ++k) { const int idx = p + R * k; if (idx < nUp) sum += h[idx]; }
                    // every phase passes DC exactly, so a steady level comes back as itself
                    for (int k = 0; k < 33; ++k) { const int idx = p + R * k; o.up[p][k] = idx < nUp ? (float)(h[idx] / sum) : 0.0f; }
                }
            }
            for (int m = 0; m < 5; ++m) {
                OsTables& o = t[fi][m];
                o.nDown = 32 * R + 1 - m;
                double d[260];
                kaiserLowpass(d, o.nDown, 0.5 / R, beta);
                double sum = 0.0;
                for (int i = 0; i < o.nDown; ++i) sum += d[i];
                for (int i = 0; i < o.nDown; ++i) o.down[i] = (float)(d[i] / sum);
            }
        }
    }
};

} // namespace

const OsTables* osTables(int factor, int halfDelays) {
    static const AllTables all;
    const int fi = factor == 2 ? 0 : (factor == 8 ? 2 : 1);
    return &all.t[fi][clampT(halfDelays, 0, 4)];
}

} // namespace pulp::tonefx
