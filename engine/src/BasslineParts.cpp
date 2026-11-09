#include "pulp/BasslineParts.h"

namespace pulp::bl {

namespace {
// Fifth-order Lagrange weights for a delay of d samples from the first of six taps (2 <= d <= 3 is
// the middle span, where the gain never exceeds one).
inline void lagrange(double d, double* h) {
    for (int k = 0; k < 6; ++k) {
        double v = 1.0;
        for (int j = 0; j < 6; ++j) if (j != k) v *= (d - (double)j) / (double)(k - j);
        h[k] = v;
    }
}
// Gain and phase delay (in samples) of those weights at angular frequency w.
inline void lagrangeAt(const double* h, double w, double& gain, double& delay) {
    double re = 0.0, im = 0.0;
    for (int k = 0; k < 6; ++k) { re += h[k] * std::cos(w * k); im -= h[k] * std::sin(w * k); }
    gain = std::hypot(re, im);
    delay = -std::atan2(im, re) / w;
}
// One-pole low-pass y = (1 - p) x + p y1: gain and phase delay at w.
inline double poleGain(double p, double w) { return (1.0 - p) / std::sqrt(1.0 - 2.0 * p * std::cos(w) + p * p); }
inline double poleDelay(double p, double w) { return std::atan2(p * std::sin(w), 1.0 - p * std::cos(w)) / w; }
// The largest pole whose gain at w is still at least g (g < 1).
inline double poleFor(double g, double w) {
    const double g2 = g * g, c = std::cos(w), a = 1.0 - g2, b = 1.0 - g2 * c;
    if (a < 1.0e-15) return 0.0;
    const double disc = b * b - a * a;
    return clampT((b - std::sqrt(std::max(0.0, disc))) / a, 0.0, 0.995);
}
// All-pass (a + z^-1) / (1 + a z^-1): phase delay at w.
inline double allpassDelay(double a, double w) { return 1.0 - 2.0 * std::atan2(a * std::sin(w), 1.0 + a * std::cos(w)) / w; }
}

void StringLoop::retune(const StringSetting& s, double sr) {
    const int size = mask + 1;
    // The shortest loop is the four all-pass stages, the interpolator and the write itself.
    const double P = clampT(sr / std::max(1.0, s.hz), 8.5, (double)(size - 12));
    period = P;
    const double w0 = kTwoPi / P;
    const double f0 = sr / P;
    // What the whole loop must multiply the fundamental by each time round.
    const double target = std::exp(-kLn1000 / (f0 * std::max(0.005, s.t60)));

    // Stiffness: each all-pass delays low frequencies by more than a sample and the top by exactly one,
    // so upper partials come round early and sound sharp. The extra delay is a share of the period, which
    // keeps the character the same from key to key, and it is held back where the period is too short.
    const double room = std::max(0.0, 0.45 * (P - 10.0));
    const double extra = std::min(room, s.stiffness * s.stiffness * 0.5 * P) / 4.0;     // per stage, at DC
    const double a = -extra / (2.0 + extra);                                              // (1 - d) / (1 + d) with d = 1 + extra
    const double apDelay = 4.0 * allpassDelay(a, w0);

    // Loss of the high partials: a one-pole low-pass whose loss at 2 kHz, per second, is what was asked
    // for at every key (so a low string is not left ringing bright). It is never allowed to take more
    // from the fundamental than the decay time permits.
    const double wRef = kTwoPi * 2000.0 / sr;
    const double q = std::max(0.0, s.highLoss) / (f0 * (1.0 - std::cos(wRef)));     // p / (1 - p)^2
    double p = q < 1.0e-9 ? 0.0 : ((2.0 * q + 1.0) - std::sqrt(4.0 * q + 1.0)) / (2.0 * q);
    p = clampT(p, 0.0, 0.995);

    double hd[6], gInt = 1.0, dInt = 2.5, g = 1.0;
    int m = 0;
    for (int pass = 0; pass < 2; ++pass) {
        p = std::min(p, poleFor(std::min(1.0, target / gInt), w0));
        g = poleGain(p, w0);
        // tap k of the line is k + 1 samples old; the interpolator adds d more.
        const double need = P - apDelay - poleDelay(p, w0) - 1.0;
        m = std::max(0, (int)std::floor(need - 2.0));     // leaves 2 <= d < 3, the interpolator's middle span
        double d = clampT(need - (double)m, 2.0, 3.0);
        // The interpolator's phase delay is not quite d: correct for it. d must stay within 2..3, where
        // the interpolator never has gain above one at any frequency (outside it would boost the top,
        // and a loop that is nearly lossless would then grow); if the correction pushes it out, the
        // whole-sample tap moves instead.
        for (int it = 0; it < 4; ++it) {
            lagrange(d, hd);
            lagrangeAt(hd, w0, gInt, dInt);
            d += need - (double)m - dInt;
            if (d < 2.0 && m > 0) { --m; d += 1.0; }
            else if (d > 3.0) { ++m; d -= 1.0; }
            d = clampT(d, 2.0, 3.0);
        }
        lagrange(d, hd);
        lagrangeAt(hd, w0, gInt, dInt);
    }
    tap = std::min(m, size - 10);
    for (int k = 0; k < 6; ++k) h[k] = (float)hd[k];
    pole = (float)p;
    apA = (float)a;
    gain = (float)clampT(target / (g * gInt), 0.0, 0.99999);
}

} // namespace pulp::bl
