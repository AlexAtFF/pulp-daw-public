#include "pulp/Zest.h"

namespace pulp {

namespace {
constexpr float kVoiceGain = 0.4f;      // one saw at full level and velocity 100 peaks near -8 dBFS
constexpr float kAtkTarget = 1.3f;      // the attack heads for this and stops at 1: a gently rounded rise
constexpr float kRelFloor = 0.001f;     // the release heads for just below zero, so it really gets there
constexpr float kDriveMin = 1.0e-3f;
constexpr double kLn2Over1200 = 0.00057762265046662105;

// ---- sine table ----

constexpr int kSinSize = 2048;
const float* sineTable() {
    static const struct Tab {
        float v[kSinSize + 2];
        Tab() { for (int i = 0; i < kSinSize + 2; ++i) v[i] = (float)std::sin(kTwoPi * (double)i / kSinSize); }
    } tab;
    return tab.v;
}

inline float tabSin(const float* tab, double t) {       // t in [0, 1)
    const double x = t * kSinSize;
    const int i = (int)x;
    const float f = (float)(x - (double)i);
    return tab[i] + f * (tab[i + 1] - tab[i]);
}

inline uint32 xorshift(uint32& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
inline float bipolar(uint32 x) { return (float)((double)x / 2147483648.0 - 1.0); }
inline uint32 hash32(uint32 x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

// ---- oscillators ----

// Values ramp linearly across the chunk: the first sample is one step on from where the last chunk ended and the
// last sample sits exactly on the target.
struct OscRamp {
    double dt, dts;         // phase increment
    float pw, pws;          // pulse width (pulse, twin saw)
    float gl, gls, gr, grs; // output gains
};

// Band-limited by polyBLEP (steps) and polyBLAMP (the triangle's corners), two samples wide.
void oscBlock(int shape, const float* tab, double& phase, const OscRamp& r, float* L, float* R, int m) {
    double ph = phase, dt = r.dt;
    float gl = r.gl, gr = r.gr, pw = r.pw;
    switch (shape) {
        case Zest::Sine:
            for (int s = 0; s < m; ++s) {
                dt += r.dts; gl += r.gls; gr += r.grs;
                const float o = tabSin(tab, ph);
                L[s] += o * gl; R[s] += o * gr;
                ph += dt; if (ph >= 1.0) ph -= 1.0;
            }
            break;
        case Zest::Triangle:
            for (int s = 0; s < m; ++s) {
                dt += r.dts; gl += r.gls; gr += r.grs;
                double v = ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph;
                const double c = dt * (4.0 / 3.0);      // slope change of 8 per cycle, times dt, over 6
                if (ph < dt) { const double d = 1.0 - ph / dt; v += c * d * d * d; }
                else if (ph > 1.0 - dt) { const double d = 1.0 - (1.0 - ph) / dt; v += c * d * d * d; }
                const double t2 = std::fabs(ph - 0.5);
                if (t2 < dt) { const double d = 1.0 - t2 / dt; v -= c * d * d * d; }
                const float o = (float)v;
                L[s] += o * gl; R[s] += o * gr;
                ph += dt; if (ph >= 1.0) ph -= 1.0;
            }
            break;
        case Zest::Saw:
            for (int s = 0; s < m; ++s) {
                dt += r.dts; gl += r.gls; gr += r.grs;
                double v = 2.0 * ph - 1.0;
                if (ph < dt) { const double d = 1.0 - ph / dt; v += d * d; }
                else if (ph > 1.0 - dt) { const double d = 1.0 - (1.0 - ph) / dt; v -= d * d; }
                const float o = (float)v;
                L[s] += o * gl; R[s] += o * gr;
                ph += dt; if (ph >= 1.0) ph -= 1.0;
            }
            break;
        case Zest::Pulse:
            for (int s = 0; s < m; ++s) {
                dt += r.dts; gl += r.gls; gr += r.grs; pw += r.pws;
                const double w = pw;
                double v = (ph < w ? 1.0 : -1.0) - (2.0 * w - 1.0);       // the mean is taken out so the width can move without a thump
                if (ph < dt) { const double d = 1.0 - ph / dt; v -= d * d; }
                else if (ph > 1.0 - dt) { const double d = 1.0 - (1.0 - ph) / dt; v += d * d; }
                double t2 = ph - w; if (t2 < 0.0) t2 += 1.0;
                if (t2 < dt) { const double d = 1.0 - t2 / dt; v += d * d; }
                else if (t2 > 1.0 - dt) { const double d = 1.0 - (1.0 - t2) / dt; v -= d * d; }
                const float o = (float)v;
                L[s] += o * gl; R[s] += o * gr;
                ph += dt; if (ph >= 1.0) ph -= 1.0;
            }
            break;
        default:        // twin saw: two saws, the second one "pulse width" of a cycle ahead
            for (int s = 0; s < m; ++s) {
                dt += r.dts; gl += r.gls; gr += r.grs; pw += r.pws;
                double t2 = ph + (double)pw; if (t2 >= 1.0) t2 -= 1.0;
                double v = ph + t2 - 1.0;
                if (ph < dt) { const double d = 1.0 - ph / dt; v += 0.5 * d * d; }
                else if (ph > 1.0 - dt) { const double d = 1.0 - (1.0 - ph) / dt; v -= 0.5 * d * d; }
                if (t2 < dt) { const double d = 1.0 - t2 / dt; v += 0.5 * d * d; }
                else if (t2 > 1.0 - dt) { const double d = 1.0 - (1.0 - t2) / dt; v -= 0.5 * d * d; }
                const float o = (float)v;
                L[s] += o * gl; R[s] += o * gr;
                ph += dt; if (ph >= 1.0) ph -= 1.0;
            }
            break;
    }
    phase = ph;
}

inline double naiveVal(int shape, const float* tab, double t, double w) {
    switch (shape) {
        case Zest::Sine: return tabSin(tab, t);
        case Zest::Triangle: return t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t;
        case Zest::Saw: return 2.0 * t - 1.0;
        case Zest::Pulse: return (t < w ? 1.0 : -1.0) - (2.0 * w - 1.0);
        default: { double t2 = t + w; if (t2 >= 1.0) t2 -= 1.0; return t + t2 - 1.0; }
    }
}

inline double naiveSlope(int shape, const float* tab, double t) {      // per cycle; zero where it is the same everywhere
    switch (shape) {
        case Zest::Sine: { double c = t + 0.25; if (c >= 1.0) c -= 1.0; return kTwoPi * tabSin(tab, c); }
        case Zest::Triangle: return t < 0.5 ? 4.0 : -4.0;
        default: return 0.0;
    }
}

// The integral of a shape from the start of its cycle to t. A synced oscillator is cut short part of the way through
// a cycle, which leaves it with a mean (DC) of this over the cycle length; knowing it, the mean can be taken out.
inline double naiveIntegral(int shape, double t, double w) {
    switch (shape) {
        case Zest::Sine: return (1.0 - std::cos(kTwoPi * t)) / kTwoPi;
        case Zest::Triangle: return t < 0.5 ? 2.0 * t * t - t : 3.0 * t - 2.0 * t * t - 1.0;
        case Zest::Saw: return t * t - t;
        case Zest::Pulse: return t < w ? 2.0 * t * (1.0 - w) : 2.0 * w * (1.0 - t);
        default: return t < 1.0 - w ? t * t + (w - 1.0) * t : t * t + (w - 2.0) * t + (1.0 - w);
    }
}

// Hard sync: the oscillator restarts whenever a hidden master (running at oscillator 1's pitch) wraps. Every step in
// the waveform between this sample and the next (its own edges and the restart) is found in time order and corrected
// on both sides: the part that belongs to the next sample is carried over. dc is the mean to take out.
void syncBlock(int shape, const float* tab, double& phase, double& mphase, float& carryRef, const OscRamp& r,
               double mdt, double mdts, double dc, double dcs, float* L, float* R, int m) {
    double ph = phase, mph = mphase, dt = r.dt, carry = carryRef;
    float gl = r.gl, gr = r.gr, pw = r.pw;
    for (int s = 0; s < m; ++s) {
        dt += r.dts; mdt += mdts; gl += r.gls; gr += r.grs; pw += r.pws; dc += dcs;
        const double w = pw;
        double v = naiveVal(shape, tab, ph, w) + carry - dc;
        double next = 0.0;
        double q[2] = {0, 0}, h[2] = {0, 0}, mu[2] = {0, 0};
        int nq = 0;
        switch (shape) {
            case Zest::Triangle: q[0] = 0.5; mu[0] = -8.0 * dt; q[1] = 1.0; mu[1] = 8.0 * dt; nq = 2; break;
            case Zest::Saw: q[0] = 1.0; h[0] = -2.0; nq = 1; break;
            case Zest::Pulse: q[0] = w; h[0] = -2.0; q[1] = 1.0; h[1] = 2.0; nq = 2; break;
            case Zest::TwinSaw: q[0] = 1.0 - w; h[0] = -1.0; q[1] = 1.0; h[1] = -1.0; nq = 2; break;
            default: break;
        }
        // a step of height hh and a corner of slope change mm, tau samples after this one
        auto event = [&](double tau, double hh, double mm) {
            const double a = 1.0 - tau;
            v += 0.5 * hh * a * a + mm * a * a * a * (1.0 / 6.0);
            next += -0.5 * hh * tau * tau + mm * tau * tau * tau * (1.0 / 6.0);
        };
        const bool restart = mph + mdt >= 1.0;
        const double tauM = restart ? (1.0 - mph) / mdt : 1.0;
        const double p1 = ph + dt * tauM;
        for (int i = 0; i < nq; ++i) {
            const double qq = q[i] > ph ? q[i] : q[i] + 1.0;
            if (qq <= p1) event((qq - ph) / dt, h[i], mu[i]);
        }
        if (restart) {
            double pr = p1; if (pr >= 1.0) pr -= 1.0;
            event(tauM, naiveVal(shape, tab, 0.0, w) - naiveVal(shape, tab, pr, w),
                  (naiveSlope(shape, tab, 0.0) - naiveSlope(shape, tab, pr)) * dt);
            const double p2 = dt * (1.0 - tauM);
            for (int i = 0; i < nq; ++i)
                if (q[i] > 0.0 && q[i] <= p2) event(tauM + q[i] / dt, h[i], mu[i]);
            ph = p2;
            mph = mph + mdt - 1.0;
        } else {
            ph = p1; if (ph >= 1.0) ph -= 1.0;
            mph += mdt;
        }
        carry = next;
        const float o = (float)v;
        L[s] += o * gl; R[s] += o * gr;
    }
    phase = ph; mphase = mph; carryRef = (float)carry;
}

// ---- modulators ----

inline float lfoValue(int shape, const float* tab, double cycles, uint32 seed) {
    const double whole = std::floor(cycles);
    double f = cycles - whole;
    if (!(f < 1.0)) f = 0.0;
    switch (shape) {
        case Zest::LfoSine: return tabSin(tab, f);
        case Zest::LfoTriangle: return (float)(f < 0.25 ? 4.0 * f : (f < 0.75 ? 2.0 - 4.0 * f : 4.0 * f - 4.0));
        case Zest::LfoSaw: return (float)(1.0 - 2.0 * f);
        case Zest::LfoSquare: return f < 0.5 ? 1.0f : -1.0f;
        default: return bipolar(hash32((uint32)(int64)whole * 2654435761u + seed));
    }
}

inline float envNext(Zest::Env& e, float aCoef, float dCoef, float sustain, float rCoef) {
    switch (e.stage) {
        case Zest::Env::Attack:
            e.value += (kAtkTarget - e.value) * aCoef;
            if (e.value >= 1.0f) { e.value = 1.0f; e.stage = Zest::Env::Decay; }
            break;
        case Zest::Env::Decay:
            e.value = sustain + (e.value - sustain) * dCoef;
            if (std::fabs(e.value - sustain) < 1.0e-5f) {       // arrived (and no denormals on the way to a zero sustain)
                e.value = sustain;
                if (sustain <= 0.0f) e.stage = Zest::Env::Idle;
            }
            break;
        case Zest::Env::Release:
            e.value = (e.value + kRelFloor) * rCoef - kRelFloor;
            if (e.value <= 0.0f) { e.value = 0.0f; e.stage = Zest::Env::Idle; }
            break;
        default: break;
    }
    return e.value;
}

// ---- saturation with first-order antiderivative anti-aliasing ----

inline double clipF(double u) { return std::fabs(u) <= 1.0 ? u - u * u * u * (1.0 / 3.0) : (u > 0.0 ? 2.0 / 3.0 : -2.0 / 3.0); }
inline double clipAD(double u) { const double a = std::fabs(u); return a <= 1.0 ? 0.5 * u * u - u * u * u * u * (1.0 / 12.0) : (2.0 / 3.0) * a - 0.25; }
inline double lnCosh(double u) { const double a = std::fabs(u); return a + std::log1p(std::exp(-2.0 * a)) - 0.69314718055994531; }
// One filter over a stereo chunk. Its coefficients move linearly, per sample, from where the last chunk left them
// to the targets, so a fast sweep does not zip. Zero-delay-feedback state variable filter (the same topology as
// dsp::Svf); the 24 dB low-pass is two of them in series with the resonance on the second.
void filterBlock(Zest::Filt& f, int type, const float* inL, const float* inR, float* outL, float* outR, int m,
                 float gT, float kT, float k2T, float driveT, float gainT) {
    const float inv = 1.0f / (float)m;
    const float gs = (gT - f.g) * inv, ks = (kT - f.k) * inv, k2s = (k2T - f.k2) * inv, ds = (driveT - f.drive) * inv, gns = (gainT - f.gain) * inv;
    float g = f.g, k = f.k, k2 = f.k2, dr = f.drive, gain = f.gain;
    const bool drv = driveT > kDriveMin || f.drive > kDriveMin;
    const float* in[2] = {inL, inR};
    float* out[2] = {outL, outR};
    if (drv && !f.driveOn)
        for (int c = 0; c < 2; ++c) { f.dx[c] = (double)in[c][0] * std::max(f.drive + ds, kDriveMin); f.dF[c] = clipAD(f.dx[c]); }
    f.driveOn = drv;
    const bool four = type == Zest::LowPass24;
    for (int s = 0; s < m; ++s) {
        g += gs; k += ks; k2 += k2s; dr += ds; gain += gns;
        const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        float b1 = 0.0f, b2 = 0.0f, b3 = 0.0f;
        if (four) { b1 = 1.0f / (1.0f + g * (g + k2)); b2 = g * b1; b3 = g * b2; }
        double da = 0.0, makeup = 1.0;
        if (drv) { da = std::max(dr, kDriveMin); makeup = std::sqrt(1.0 + da) / da; }
        for (int c = 0; c < 2; ++c) {
            float x = in[c][s];
            if (drv) {
                const double u = (double)x * da, du = u - f.dx[c], F = clipAD(u);
                const double y = std::fabs(du) > 1.0e-6 ? (F - f.dF[c]) / du : clipF(0.5 * (u + f.dx[c]));
                f.dx[c] = u; f.dF[c] = F;
                x = (float)(y * makeup);
            }
            float& ic1 = f.ic1[0][c];
            float& ic2 = f.ic2[0][c];
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            float y;
            switch (type) {
                case Zest::LowPass12: y = v2; break;
                case Zest::LowPass24: {
                    float& jc1 = f.ic1[1][c];
                    float& jc2 = f.ic2[1][c];
                    const float u3 = v2 - jc2;
                    const float u1 = b1 * jc1 + b2 * u3;
                    const float u2 = jc2 + b2 * jc1 + b3 * u3;
                    jc1 = 2.0f * u1 - jc1;
                    jc2 = 2.0f * u2 - jc2;
                    y = u2;
                    break;
                }
                case Zest::BandPass: y = v1; break;
                case Zest::HighPass: y = x - k * v1 - v2; break;
                default: y = x - k * v1; break;
            }
            out[c][s] = y * gain;
        }
    }
    f.g = gT; f.k = kT; f.k2 = k2T; f.drive = driveT; f.gain = gainT;
    for (int c = 0; c < 2; ++c) {
        flushDenormal(f.ic1[0][c]); flushDenormal(f.ic2[0][c]);
        if (four) { flushDenormal(f.ic1[1][c]); flushDenormal(f.ic2[1][c]); }
        else f.ic1[1][c] = f.ic2[1][c] = 0.0f;
    }
}

// Four-point (Catmull-Rom) read from a delay line, for the chorus.
inline float tapCubic(const dsp::DelayLine& d, float delay) {
    const int i = (int)delay;
    const float f = delay - (float)i;
    const float y0 = d.tap(i - 1), y1 = d.tap(i), y2 = d.tap(i + 1), y3 = d.tap(i + 2);
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

// Half-band decimator for the oversampled oscillators: two all-pass branches (elliptic, 80 dB stop band from
// 0.29 of the oversampled rate, flat to 0.21). Coefficients from the closed-form design, checked numerically.
const float kHalfband[5] = {0.0611063251f, 0.2211132431f, 0.4305337756f, 0.6510294999f, 0.8766858962f};

inline void decimate(float (*hx)[5], float (*hy)[5], const float* in, float* out, int m, int c) {
    float* x = hx[c];
    float* y = hy[c];
    for (int i = 0; i < m; ++i) {
        float p0 = in[2 * i + 1], p1 = in[2 * i];
        for (int s = 0; s < 5; s += 2) { const float o = kHalfband[s] * (p0 - y[s]) + x[s]; x[s] = p0; y[s] = o; p0 = o; }
        for (int s = 1; s < 5; s += 2) { const float o = kHalfband[s] * (p1 - y[s]) + x[s]; x[s] = p1; y[s] = o; p1 = o; }
        out[i] = 0.5f * (p0 + p1);
    }
    for (int s = 0; s < 5; ++s) { flushDenormal(x[s]); flushDenormal(y[s]); }
}

const double kDivBeats[Zest::NumDivisions] = {32.0, 16.0, 8.0, 4.0, 3.0, 2.0, 1.5, 1.0, 0.75, 2.0 / 3.0, 0.5, 0.375, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125};
const int kRevLen[8] = {2039, 2459, 2803, 3137, 3413, 3797, 4127, 4493};      // samples at 48 kHz
const int kRevAp[2][4] = {{113, 167, 241, 373}, {131, 191, 277, 419}};  // input diffusers, a side
constexpr float kRevApGain = 0.6f;

} // namespace

double Zest::divisionBeats(int division) { return kDivBeats[clampT(division, 0, (int)NumDivisions - 1)]; }

// ------------------------------------------------------------------ parameters

Zest::Zest() {
    const std::vector<std::string> shapes = {"Sine", "Triangle", "Saw", "Pulse", "Twin saw"};
    const std::vector<std::string> divisions = {"8 bars", "4 bars", "2 bars", "1 bar", "1/2 dotted", "1/2", "1/4 dotted", "1/4", "1/8 dotted",
                                                "1/4 triplet", "1/8", "1/16 dotted", "1/8 triplet", "1/16", "1/16 triplet", "1/32"};
    for (int i = 0; i < 2; ++i) {
        const std::string n = "o" + std::to_string(i + 1), d = "Osc " + std::to_string(i + 1) + " ";
        OscP& o = osc_[i];
        o.shape = addChoice((n + "shape").c_str(), (d + "shape").c_str(), shapes, Saw);
        o.coarse = add({n + "coarse", d + "coarse", -24, 24, 0, 1, 49, "st", {}});
        o.fine = addFloat((n + "fine").c_str(), (d + "fine").c_str(), -100, 100, 0, "ct");
        o.level = addFloat((n + "level").c_str(), (d + "level").c_str(), 0, 1, i == 0 ? 0.8f : 0.0f, "%");
        o.pan = addFloat((n + "pan").c_str(), (d + "pan").c_str(), -1, 1, 0, "");
        o.pw = addFloat((n + "pw").c_str(), (d + "pulse width").c_str(), 0.03f, 0.97f, 0.5f, "%");
        o.phase = addChoice((n + "phase").c_str(), (d + "phase").c_str(), {"Free", "Reset at note start"}, 0);
        o.voices = add({n + "uni", d + "unison voices", 1, (float)kUnison, 1, 1, kUnison, "", {}});
        o.detune = addFloat((n + "detune").c_str(), (d + "unison detune").c_str(), 0, 100, 20, "ct", 2.0f);
        o.spread = addFloat((n + "spread").c_str(), (d + "unison stereo spread").c_str(), 0, 1, 0.5f, "%");
        if (i == 1) pSync = addBool("o2sync", "Osc 2 hard sync to osc 1", false);
    }
    pSubShape = addChoice("subshape", "Sub shape", {"Sine", "Square"}, 0);
    pSubOct = addChoice("suboct", "Sub octave", {"One octave down", "Two octaves down"}, 0);
    pSubLevel = addFloat("sublevel", "Sub level", 0, 1, 0, "%");
    pNoiseType = addChoice("noisetype", "Noise colour", {"White", "Pink", "Dark"}, 0);
    pNoiseLevel = addFloat("noiselevel", "Noise level", 0, 1, 0, "%");
    const std::vector<std::string> ftypes = {"Low-pass 12 dB", "Low-pass 24 dB", "Band-pass", "High-pass", "Notch"};
    for (int i = 0; i < 2; ++i) {
        const std::string n = "f" + std::to_string(i + 1), d = "Filter " + std::to_string(i + 1) + " ";
        FiltP& f = filt_[i];
        f.type = addChoice((n + "type").c_str(), (d + "type").c_str(), ftypes, i == 0 ? LowPass24 : LowPass12);
        f.cutoff = addFloat((n + "cutoff").c_str(), (d + "cutoff").c_str(), 20, 20000, 20000, "Hz", 3.0f);
        f.reso = addFloat((n + "reso").c_str(), (d + "resonance").c_str(), 0, 1, i == 0 ? 0.1f : 0.0f, "%");
        f.env = addFloat((n + "env").c_str(), (d + "envelope amount").c_str(), -8, 8, 0, "oct");
        f.key = addFloat((n + "key").c_str(), (d + "key tracking").c_str(), 0, 1, 0, "%");
        f.drive = addFloat((n + "drive").c_str(), (d + "drive").c_str(), 0, 1, 0, "%");
    }
    pRoute = addChoice("froute", "Filter routing", {"Serial (1 into 2)", "Parallel"}, 0);
    pBalance = addFloat("fbal", "Filter balance (1 to 2)", 0, 1, 0, "%");
    const char* envId[3] = {"e1", "e2", "e3"};
    const char* envName[3] = {"Amp ", "Filter env ", "Mod env "};
    const float envDef[3][4] = {{2, 300, 0.8f, 200}, {2, 400, 0.3f, 300}, {2, 400, 0.0f, 300}};
    for (int i = 0; i < 3; ++i) {
        const std::string n = envId[i], d = envName[i];
        env_[i].a = addFloat((n + "atk").c_str(), (d + "attack").c_str(), 0, 10000, envDef[i][0], "ms", 3.0f);
        env_[i].d = addFloat((n + "dec").c_str(), (d + "decay").c_str(), 1, 10000, envDef[i][1], "ms", 3.0f);
        env_[i].s = addFloat((n + "sus").c_str(), (d + "sustain").c_str(), 0, 1, envDef[i][2], "%");
        env_[i].r = addFloat((n + "rel").c_str(), (d + "release").c_str(), 1, 10000, envDef[i][3], "ms", 3.0f);
    }
    const float lfoRate[3] = {5.0f, 0.5f, 2.0f};
    for (int i = 0; i < 3; ++i) {
        const std::string n = "l" + std::to_string(i + 1), d = "LFO " + std::to_string(i + 1) + " ";
        lfo_[i].shape = addChoice((n + "shape").c_str(), (d + "shape").c_str(), {"Sine", "Triangle", "Saw", "Square", "Sample and hold"}, 0);
        lfo_[i].sync = addBool((n + "sync").c_str(), (d + "tempo sync").c_str(), false);
        lfo_[i].rate = addFloat((n + "rate").c_str(), (d + "rate").c_str(), 0.01f, 40.0f, lfoRate[i], "Hz", 3.0f);
        lfo_[i].div = addChoice((n + "div").c_str(), (d + "synced length").c_str(), divisions, DivQuarter);
        lfo_[i].fade = addFloat((n + "fade").c_str(), (d + "fade-in").c_str(), 0, 5000, 0, "ms", 3.0f);
        lfo_[i].mode = addChoice((n + "mode").c_str(), (d + "mode").c_str(), {"Per note (restarts)", "Global (free running)"}, 0);
    }
    const std::vector<std::string> sources = {"None", "Filter envelope", "Mod envelope", "LFO 1", "LFO 2", "LFO 3", "Velocity", "Key", "Mod X", "Mod Y", "Random per note"};
    const std::vector<std::string> dests = {"None", "Pitch", "Osc 1 pitch", "Osc 2 pitch", "Pulse width", "Osc mix", "Noise level", "Filter 1 cutoff", "Filter 2 cutoff",
                                            "Resonance", "Filter balance", "Amplitude", "Pan", "Unison detune", "LFO 1 rate", "LFO 2 rate", "LFO 3 rate"};
    for (int i = 0; i < kSlots; ++i) {
        const std::string n = "m" + std::to_string(i + 1), d = "Slot " + std::to_string(i + 1) + " ";
        slot_[i].src = addChoice((n + "src").c_str(), (d + "source").c_str(), sources, SrcNone);
        slot_[i].dst = addChoice((n + "dst").c_str(), (d + "destination").c_str(), dests, DstNone);
        slot_[i].amt = addFloat((n + "amt").c_str(), (d + "amount").c_str(), -1, 1, 0, "");
    }
    pDrvOn = addBool("drvon", "Drive", false);
    pDrvAmt = addFloat("drvamt", "Drive amount", 0, 36, 12, "dB");
    pDrvTone = addFloat("drvtone", "Drive tone", 500, 20000, 9000, "Hz", 3.0f);
    pChoOn = addBool("choon", "Chorus", false);
    pChoRate = addFloat("chorate", "Chorus rate", 0.05f, 8.0f, 0.6f, "Hz", 2.0f);
    pChoDepth = addFloat("chodepth", "Chorus depth", 0, 1, 0.5f, "%");
    pChoMix = addFloat("chomix", "Chorus mix", 0, 1, 0.5f, "%");
    pDlyOn = addBool("dlyon", "Delay", false);
    pDlySync = addBool("dlysync", "Delay tempo sync", true);
    pDlyTime = addFloat("dlytime", "Delay time", 1, 2000, 350, "ms", 3.0f);
    pDlyDiv = addChoice("dlydiv", "Delay synced length", divisions, DivEighthDotted);
    pDlyFb = addFloat("dlyfb", "Delay feedback", 0, 0.95f, 0.4f, "%");
    pDlyMix = addFloat("dlymix", "Delay mix", 0, 1, 0.3f, "%");
    pDlyPing = addBool("dlyping", "Delay ping-pong", true);
    pRevOn = addBool("revon", "Reverb", false);
    pRevDecay = addFloat("revdecay", "Reverb decay", 0.2f, 12.0f, 2.2f, "s", 2.0f);
    pRevDamp = addFloat("revdamp", "Reverb damping", 0, 1, 0.5f, "%");
    pRevMix = addFloat("revmix", "Reverb mix", 0, 1, 0.25f, "%");
    pPoly = add({"poly", "Polyphony", 1, (float)kMaxPoly, 16, 1, kMaxPoly, "", {}});
    pMode = addChoice("mode", "Play mode", {"Poly", "Mono (retrigger)", "Legato"}, Poly);
    pGlide = addFloat("glide", "Glide time", 0, 2000, 0, "ms", 3.0f);
    pVelSens = addFloat("velsens", "Velocity to level", 0, 1, 0.7f, "%");
    pVelFilt = addFloat("velfilt", "Velocity to cutoff", 0, 1, 0, "%");
    pVolume = addFloat("volume", "Master volume", -48, 6, 0, "dB");
    // Unison layout: where each voice sits in the detune spread, in the stereo field, and where it starts its cycle
    // when the phase is reset. The detune positions bunch toward the centre, which is what gives a stack its body.
    static const float startPhase[kUnison] = {0.0f, 0.618f, 0.236f, 0.854f, 0.472f, 0.090f, 0.708f};
    for (int n = 1; n <= kUnison; ++n)
        for (int u = 0; u < n; ++u) {
            const float lin = n == 1 ? 0.0f : -1.0f + 2.0f * (float)u / (float)(n - 1);
            const float mag = std::fabs(lin);
            uniDet_[n][u] = (lin < 0.0f ? -1.0f : 1.0f) * std::pow(mag, 1.5f);
            const int pair = (int)std::lround(mag * (float)(n - 1) * 0.5f + ((n & 1) ? 0.0f : 0.5f));     // 0 = centre voice
            const float side = (lin < 0.0f ? -1.0f : 1.0f) * ((pair & 1) ? 1.0f : -1.0f);
            uniPan_[n][u] = mag == 0.0f ? 0.0f : side * (0.35f + 0.65f * mag);
            const int rank = mag == 0.0f ? 0 : clampT(2 * pair - ((n & 1) ? 1 : 2) + (lin > 0.0f ? 1 : 0), 0, kUnison - 1);
            uniPhase_[n][u] = startPhase[rank];
        }
    sinTab_ = sineTable();
}

std::vector<ParamGroup> Zest::paramGroups() const {
    std::vector<ParamGroup> g;
    g.push_back({"Oscillator 1", {"o1shape", "o1coarse", "o1fine", "o1level", "o1pan", "o1pw", "o1phase", "o1uni", "o1detune", "o1spread"}});
    g.push_back({"Oscillator 2", {"o2shape", "o2coarse", "o2fine", "o2level", "o2pan", "o2pw", "o2phase", "o2uni", "o2detune", "o2spread", "o2sync"}});
    g.push_back({"Sub and noise", {"subshape", "suboct", "sublevel", "noisetype", "noiselevel"}});
    g.push_back({"Filter 1", {"f1type", "f1cutoff", "f1reso", "f1env", "f1key", "f1drive"}});
    g.push_back({"Filter 2", {"f2type", "f2cutoff", "f2reso", "f2env", "f2key", "f2drive", "froute", "fbal"}});
    g.push_back({"Envelopes", {"e1atk", "e1dec", "e1sus", "e1rel", "e2atk", "e2dec", "e2sus", "e2rel", "e3atk", "e3dec", "e3sus", "e3rel"}});
    ParamGroup l{"LFOs", {}};
    for (int i = 1; i <= 3; ++i)
        for (const char* s : {"shape", "sync", "rate", "div", "fade", "mode"}) l.ids.push_back("l" + std::to_string(i) + s);
    g.push_back(l);
    ParamGroup m{"Modulation matrix", {}};
    for (int i = 1; i <= kSlots; ++i)
        for (const char* s : {"src", "dst", "amt"}) m.ids.push_back("m" + std::to_string(i) + s);
    g.push_back(m);
    g.push_back({"Effects", {"drvon", "drvamt", "drvtone", "choon", "chorate", "chodepth", "chomix", "dlyon", "dlysync", "dlytime", "dlydiv", "dlyfb", "dlymix", "dlyping",
                             "revon", "revdecay", "revdamp", "revmix"}});
    g.push_back({"Global", {"volume", "poly", "mode", "glide", "velsens", "velfilt"}});
    return g;
}

// ------------------------------------------------------------------ life cycle

void Zest::prepare(double sampleRate) {
    sr_ = sampleRate;
    sinTab_ = sineTable();
    for (auto& d : chorus_.d) d.prepare((int)(0.03 * sr_) + 16);
    int n = 1;
    while (n < (int)(4.0 * sr_) + 16) n <<= 1;
    for (auto& b : delay_.buf) b.assign((size_t)n, 0.0f);
    delay_.mask = n - 1;
    const double scale = sr_ / 48000.0;
    for (int i = 0; i < 8; ++i) {
        reverb_.len[i] = std::max(8, (int)(kRevLen[i] * scale)) | 1;
        reverb_.line[i].prepare(reverb_.len[i] + 4);
    }
    for (int c = 0; c < 2; ++c)
        for (int a = 0; a < 4; ++a) {
            reverb_.apLen[c][a] = std::max(4, (int)(kRevAp[c][a] * scale)) | 1;
            reverb_.ap[c][a].prepare(reverb_.apLen[c][a] + 4);
        }
    // Dark noise: white through one pole at 700 Hz. Pink noise: white through a bank of one-pole low-passes whose
    // corners are a factor of three apart, each weighted by one over the root of its corner; the sum falls at 3 dB an
    // octave from 15 Hz up. Both are scaled to be as loud as the white noise at any sample rate.
    darkCoef_ = (float)(1.0 - std::exp(-kTwoPi * 700.0 / sr_));
    darkGain_ = 1.0f / std::sqrt(darkCoef_ / (2.0f - darkCoef_));
    double weight[kPink], coef[kPink], hz = 15.0;
    for (int k = 0; k < kPink; ++k, hz *= 3.0) {
        coef[k] = 1.0 - std::exp(-kTwoPi * std::min(hz, 0.49 * sr_) / sr_);
        weight[k] = 1.0 / std::sqrt(hz / 15.0);
    }
    const double white = 1.0 / std::sqrt(hz / 15.0);        // the next corner up would be above the band: it is flat
    double power = 0.0;
    const int steps = 4096;
    for (int i = 0; i < steps; ++i) {                       // the mean of |H|^2 over the band is the power gain for white noise
        const double w = kPi * ((double)i + 0.5) / steps, cr = std::cos(w), ci = std::sin(w);
        double re = white, im = 0.0;
        for (int k = 0; k < kPink; ++k) {                   // c / (1 - (1 - c) e^-jw)
            const double a = 1.0 - coef[k], dr = 1.0 - a * cr, di = a * ci, d2 = dr * dr + di * di;
            re += weight[k] * coef[k] * dr / d2;
            im -= weight[k] * coef[k] * di / d2;
        }
        power += re * re + im * im;
    }
    const double norm = 1.0 / std::sqrt(power / steps);
    for (int k = 0; k < kPink; ++k) { pinkCoef_[k] = (float)coef[k]; pinkGain_[k] = (float)(weight[k] * norm); }
    pinkWhite_ = (float)(white * norm);
    reset();
}

void Zest::clearEffects() {
    for (int c = 0; c < 2; ++c) {
        drive_.x1[c] = drive_.F1[c] = 0.0; drive_.lp[c] = 0.0f;
        chorus_.d[c].clear();
        delay_.lp[c] = delay_.hp[c] = 0.0f;
        for (auto& a : reverb_.ap[c]) a.clear();
    }
    delay_.valid = 0;
    for (auto& l : reverb_.line) l.clear();
    for (auto& v : reverb_.lp) v = 0.0f;
}

void Zest::reset() {
    for (auto& v : voices_) v.active = false;
    heldCount_ = 0;
    monoVoice_ = -1;
    lastKey_ = -1.0f;
    ageCounter_ = 0;
    rng_ = 0x2F6E2B1u;
    primed_ = false;
    outPrimed_ = false;
    wasPlaying_ = false;
    for (int i = 0; i < 3; ++i) { glfoCyc_[i] = 0.0; glfoInc_[i] = 0.0; glfo_[i] = 0.0f; }
    chorus_.phase = 0.0;
    delay_.w = 0;
    drive_.running = chorus_.running = delay_.running = reverb_.running = false;
    drive_.wet.snap(0.0f); chorus_.wet.snap(0.0f); delay_.wet.snap(0.0f); reverb_.wet.snap(0.0f);
    clearEffects();
    idleSamples_ = 0;
    fxAsleep_ = true;
    fxWake_ = false;
    quantumPos_ = 0;
    std::memset(mixL_, 0, sizeof mixL_);
    std::memset(mixR_, 0, sizeof mixR_);
    voicesActive_.store(0, std::memory_order_relaxed);
}

// How long the effects ring after the last voice has stopped (to -120 dB).
double Zest::fxTailSeconds() const {
    double t = 0.05;
    if (pDlyOn->getBool()) {
        const double secs = pDlySync->getBool() ? divisionBeats(pDlyDiv->getInt()) * 60.0 / tempo_.load(std::memory_order_relaxed) : pDlyTime->get() * 0.001;
        const double fb = std::min(0.95f, pDlyFb->get());
        const double repeats = fb <= 0.001 ? 1.0 : std::min(300.0, 1.0 + std::log(1.0e-6) / std::log(fb));
        t += std::min(4.0, secs) * repeats;
    }
    if (pRevOn->getBool()) t += 2.0 * pRevDecay->get() + 0.2;
    return std::min(t, 90.0);
}

double Zest::tailSeconds() const { return env_[0].r->get() * 0.001 * 1.75 * 1.5 + fxTailSeconds(); }

void Zest::beginBlock(ProcessContext& ctx) {
    Snap& q = q_;
    const double tempo = ctx.tempo > 1.0 ? ctx.tempo : 120.0;
    tempo_.store(tempo, std::memory_order_relaxed);
    // Free-running things (a global LFO set in Hz, the chorus) are placed by the song position when the transport
    // starts or jumps, and run on from there: a render that starts at some bar gives what playback from that bar
    // gives. A tempo-synced global LFO is simply read off the song position all the time.
    const double tps = tempo * kPPQ / (60.0 * sr_);
    const bool started = ctx.playing && !wasPlaying_;
    tickJumped_ = ctx.playing && (started || std::fabs(ctx.tickPos - expectedTick_) > 2.0);
    wasPlaying_ = ctx.playing;
    expectedTick_ = ctx.tickPos + kQuantum * tps;
    const double songSeconds = ctx.tickPos * 60.0 / (tempo * kPPQ);
    if (started) chorus_.phase = songSeconds * (double)pChoRate->get();    // not on a jump: a loop coming round must not click the chorus
    float t[NumSm];
    for (int i = 0; i < 2; ++i) {
        const OscP& o = osc_[i];
        q.shape[i] = clampT(o.shape->getInt(), 0, (int)NumShapes - 1);
        q.coarse[i] = o.coarse->getInt();
        q.uni[i] = clampT(o.voices->getInt(), 1, kUnison);
        q.resetPhase[i] = o.phase->getInt() == 1;
        t[SmLevel1 + i] = o.level->get();
        t[SmPan1 + i] = o.pan->get();
        t[SmPw1 + i] = o.pw->get();
        t[SmFine1 + i] = o.fine->get();
        t[SmDetune1 + i] = o.detune->get();
        t[SmSpread1 + i] = o.spread->get();
        const FiltP& f = filt_[i];
        q.ftype[i] = f.type->getInt();
        t[SmCut1 + i] = std::log2(f.cutoff->get());
        t[SmReso1 + i] = f.reso->get();
        t[SmEnv1 + i] = f.env->get();
        t[SmKey1 + i] = f.key->get();
        t[SmDrive1 + i] = f.drive->get() * 4.0f;
    }
    q.sync = pSync->getBool();
    q.subShape = pSubShape->getInt();
    q.subOct = pSubOct->getInt() + 1;
    q.noiseType = pNoiseType->getInt();
    t[SmSub] = pSubLevel->get();
    t[SmNoise] = pNoiseLevel->get();
    q.parallel = pRoute->getInt() == 1;
    t[SmBalance] = pBalance->get();
    t[SmBend] = ctx.pitchBendSemis;
    bool envChanged = !primed_;
    for (int e = 0; e < 3; ++e) {
        const float a = env_[e].a->get(), d = env_[e].d->get(), r = env_[e].r->get();
        float* seen = envSeen_ + e * 3;
        if (!primed_ || a != seen[0] || d != seen[1] || r != seen[2]) {
            const double n = std::max(1.0, std::max((double)a, e == 0 ? 0.5 : 0.05) * 0.001 * sr_);
            q.envA[e] = (float)(1.0 - std::pow(1.0 - 1.0 / kAtkTarget, 1.0 / n));
            q.envD[e] = (float)std::exp(-4.6 / (std::max(1.0f, d) * 0.001 * sr_));
            q.envRelMs[e] = r;
            seen[0] = a; seen[1] = d; seen[2] = r;
            envChanged = true;
        }
        q.envS[e] = env_[e].s->get();
    }
    if (envChanged) ++envEpoch_;
    for (int l = 0; l < 3; ++l) {
        q.lfoShape[l] = lfo_[l].shape->getInt();
        q.lfoGlobal[l] = lfo_[l].mode->getInt() == 1;
        const bool sync = lfo_[l].sync->getBool();
        const double beats = divisionBeats(lfo_[l].div->getInt());
        const double hz = sync ? tempo / (60.0 * beats) : (double)lfo_[l].rate->get();
        q.lfoHz[l] = (float)hz;
        q.lfoFade[l] = (float)(lfo_[l].fade->get() * 0.001 * sr_);
        // While the song plays, a global LFO is placed by the song position, so an export matches playback.
        if (ctx.playing && sync) glfoCyc_[l] = ctx.tickPos / (beats * kPPQ);
        else if (tickJumped_) glfoCyc_[l] = songSeconds * hz;
        glfoInc_[l] = hz / sr_;
    }
    q.numSlots = 0;
    q.modXUsed = q.modYUsed = false;
    for (int i = 0; i < kSlots; ++i) {
        const int s = slot_[i].src->getInt(), d = slot_[i].dst->getInt();
        t[SmAmt0 + i] = slot_[i].amt->get();
        if (s <= SrcNone || s >= NumSources || d <= DstNone || d >= NumDests) continue;
        q.slotIdx[q.numSlots] = i; q.slotSrc[q.numSlots] = s; q.slotDst[q.numSlots] = d;
        ++q.numSlots;
        q.modXUsed = q.modXUsed || s == SrcModX;
        q.modYUsed = q.modYUsed || s == SrcModY;
    }
    q.mode = pMode->getInt();
    q.poly = clampT(pPoly->getInt(), 1, kMaxPoly);
    q.glideSamples = (float)(pGlide->get() * 0.001 * sr_);
    q.velSens = pVelSens->get();
    q.velFilt = pVelFilt->get();
    if (q.mode != lastMode_) { heldCount_ = 0; monoVoice_ = -1; lastMode_ = q.mode; }
    for (int i = 0; i < NumSm; ++i) { smStart_[i] = primed_ ? smTarget_[i] : t[i]; smTarget_[i] = t[i]; }
    primed_ = true;
    quantumPos_ = 0;
    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
}

// ------------------------------------------------------------------ notes

void Zest::setNoteValues(Voice& v, const NoteEvent& e) {
    v.velocity = clampT(e.velocity, 0.0f, 1.0f);
    const float vel = v.velocity / kDefaultVelocity;
    v.velGain = std::max(0.0f, 1.0f + (vel * vel - 1.0f) * q_.velSens);
    v.notePan = clampT(e.pan, -1.0f, 1.0f);
    v.modX = e.modX;
    v.modY = e.modY;
    v.relScale = 0.25f + 1.5f * clampT(e.release, 0.0f, 1.0f);
    v.envEpoch = 0;
}

void Zest::initVoice(Voice& v, const NoteEvent& e) {
    v = Voice{};
    v.active = true;
    v.id = e.id;
    v.age = ++ageCounter_;
    v.key = e.key;
    v.group = e.group;
    v.fine = e.fineCents;
    setNoteValues(v, e);
    v.rnd = bipolar(xorshift(rng_));
    if (e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) {              // the channel's portamento
        v.bend = e.glideFromKey - (float)e.key;
        v.bendStep = -v.bend / e.slideSamples;
    } else if (q_.glideSamples > 0.0f && q_.mode != Legato && lastKey_ >= 0.0f && lastKey_ != (float)e.key) {
        v.bend = lastKey_ - (float)e.key;
        v.bendStep = -v.bend / q_.glideSamples;
    }
    for (int o = 0; o < 2; ++o)
        for (int u = 0; u < kUnison; ++u) {
            const float r = (float)(xorshift(rng_) >> 8) * (1.0f / 16777216.0f);
            v.ph[o][u] = q_.resetPhase[o] ? (u < q_.uni[o] ? uniPhase_[q_.uni[o]][u] : 0.0f) : r;
        }
    for (int u = 0; u < kUnison; ++u) {
        const float r = (float)(xorshift(rng_) >> 8) * (1.0f / 16777216.0f);
        v.mph[u] = q_.resetPhase[1] ? (u < q_.uni[1] ? uniPhase_[q_.uni[1]][u] : 0.0f) : r;
    }
    const float r = (float)(xorshift(rng_) >> 8) * (1.0f / 16777216.0f);
    v.subPh = q_.resetPhase[0] ? 0.0 : (double)r;
    v.noise = xorshift(rng_) | 1u;
    v.lfoSeed = xorshift(rng_);
    for (auto& en : v.env) { en.stage = Env::Attack; en.value = 0.0f; }
}

// A sounding voice takes over another note (mono and legato modes).
void Zest::retarget(Voice& v, const NoteEvent& e, bool retrigger, bool glide) {
    const float now = (float)v.key + v.bend;        // where the pitch is at this moment
    v.id = e.id;
    v.key = e.key;
    v.group = e.group;
    v.fine = e.fineCents;
    v.bend = v.bendTarget = v.bendStep = 0.0f;
    const float time = (e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) ? e.slideSamples : (glide ? q_.glideSamples : 0.0f);
    if (time > 0.0f && now != (float)e.key) {
        v.bend = now - (float)e.key;
        v.bendStep = -v.bend / time;
    }
    if (retrigger) {
        setNoteValues(v, e);
        v.rnd = bipolar(xorshift(rng_));
        for (auto& en : v.env) en.stage = Env::Attack;      // from the level it is at: no click
    }
}

Zest::Voice* Zest::allocVoice() {
    const float fade = (float)(1.0 / (0.003 * sr_));
    const int limit = q_.mode == Poly ? q_.poly : 1;
    for (;;) {
        int sounding = 0;
        Voice* steal = nullptr;
        bool stealReleased = false;
        for (auto& c : voices_) {
            if (!c.active || c.killStep > 0.0f) continue;
            ++sounding;
            const bool released = c.env[0].stage == Env::Release;
            if (!steal || (released && !stealReleased) || (released == stealReleased && c.age < steal->age)) { steal = &c; stealReleased = released; }
        }
        if (sounding < limit || !steal) break;
        steal->killStep = fade;             // fades out over 3 ms while the new note starts
    }
    for (auto& c : voices_) if (!c.active) return &c;
    Voice* v = &voices_[0];                 // every slot is busy: take the one that has faded furthest
    for (auto& c : voices_) {
        const bool cFading = c.killStep > 0.0f, vFading = v->killStep > 0.0f;
        if ((cFading && !vFading) || (cFading == vFading && (cFading ? c.kill < v->kill : c.age < v->age))) v = &c;
    }
    return v;
}

void Zest::noteOn(const NoteEvent& e) {
    const int mode = q_.mode;
    if (mode != Poly) {
        if (heldCount_ == kHeld) { for (int i = 1; i < kHeld; ++i) held_[i - 1] = held_[i]; --heldCount_; }
        held_[heldCount_++] = e;
        Voice* mv = monoVoice_ >= 0 ? &voices_[monoVoice_] : nullptr;
        if (mv && mv->active && mv->killStep == 0.0f) {
            const bool keyDown = mv->env[0].stage != Env::Release;
            retarget(*mv, e, mode == Mono || !keyDown, mode == Mono || keyDown);
            lastKey_ = (float)e.key;
            return;
        }
    }
    Voice* v = allocVoice();
    initVoice(*v, e);
    if (mode != Poly) monoVoice_ = (int)(v - voices_);
    lastKey_ = (float)e.key;
}

void Zest::releaseVoice(Voice& v) {
    for (auto& en : v.env) if (en.stage != Env::Idle) en.stage = Env::Release;
}

void Zest::noteOff(uint32 id) {
    if (q_.mode != Poly) {
        int w = 0;
        for (int i = 0; i < heldCount_; ++i) if (held_[i].id != id) held_[w++] = held_[i];
        heldCount_ = w;
        Voice* mv = monoVoice_ >= 0 ? &voices_[monoVoice_] : nullptr;
        if (mv && mv->active && mv->killStep == 0.0f && mv->id == id && heldCount_ > 0) {
            NoteEvent back = held_[heldCount_ - 1];     // fall back to the note that is still held
            back.glideFromKey = -1.0f;
            retarget(*mv, back, q_.mode == Mono, true);
            return;
        }
    }
    for (auto& v : voices_) if (v.active && v.id == id) releaseVoice(v);
}

void Zest::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float fade = (float)(1.0 / (0.003 * sr_));
    switch (e.type) {
        case NoteEvent::On: noteOn(e); break;
        case NoteEvent::Off: noteOff(e.id); break;
        case NoteEvent::AllOff:
            heldCount_ = 0;
            for (auto& v : voices_) if (v.active) releaseVoice(v);
            break;
        case NoteEvent::Kill:
            heldCount_ = 0;
            for (auto& v : voices_) if (v.active && v.killStep == 0.0f) v.killStep = fade;
            break;
        case NoteEvent::KillNote: {
            int w = 0;
            for (int i = 0; i < heldCount_; ++i) if (held_[i].id != e.id) held_[w++] = held_[i];
            heldCount_ = w;
            for (auto& v : voices_) if (v.active && v.id == e.id && v.killStep == 0.0f) v.killStep = fade;
            break;
        }
        case NoteEvent::SlideTo:
            for (auto& v : voices_)
                if (v.active && v.group == e.group && v.env[0].stage != Env::Release) {
                    v.bendTarget = (float)(e.key - v.key);
                    v.bendStep = (v.bendTarget - v.bend) / std::max(1.0f, e.slideSamples);
                }
            break;
    }
}

// ------------------------------------------------------------------ rendering

void Zest::render(float*, float*, int n, ProcessContext& ctx) {
    int pos = quantumPos_;
    int left = std::min(n, kQuantum - pos);
    while (left > 0) {
        const int m = std::min(left, kCtl - (pos & (kCtl - 1)));
        renderChunk(pos, m);
        pos += m;
        left -= m;
    }
    quantumPos_ = pos;
    if (pos >= kQuantum) finishQuantum(ctx);
}

void Zest::renderChunk(int pos, int m) {
    const float frac = (float)(pos + m) * (1.0f / kQuantum);
    for (int i = 0; i < NumSm; ++i) cv_[i] = smStart_[i] + (smTarget_[i] - smStart_[i]) * frac;
    for (int l = 0; l < 3; ++l)
        glfo_[l] = lfoValue(q_.lfoShape[l], sinTab_, glfoCyc_[l] + (double)(pos + m) * glfoInc_[l], 0x51ED270Bu + (uint32)l);
    for (auto& v : voices_)
        if (v.active) renderVoice(v, mixL_ + pos, mixR_ + pos, m);
}

void Zest::renderVoice(Voice& v, float* outL, float* outR, int m) {
    const Snap& q = q_;
    const float* cv = cv_;
    const float* tab = sinTab_;
    const bool first = v.first;
    const float invM = 1.0f / (float)m;
    const double invSr = 1.0 / sr_;

    // ---- modulators: envelopes 2 and 3 and the LFOs, as they stand at the end of this chunk ----
    if (v.envEpoch != envEpoch_) {
        for (int e = 0; e < 3; ++e) v.rel[e] = (float)std::exp(-4.6 / (std::max(1.0f, q.envRelMs[e] * v.relScale) * 0.001 * sr_));
        v.envEpoch = envEpoch_;
    }
    for (int e = 1; e < 3; ++e)
        for (int s = 0; s < m; ++s) envNext(v.env[e], q.envA[e], q.envD[e], q.envS[e], v.rel[e]);
    v.ageSamples += (float)m;
    float src[NumSources];
    src[SrcNone] = 0.0f;
    src[SrcEnv2] = v.env[1].value;
    src[SrcEnv3] = v.env[2].value;
    for (int l = 0; l < 3; ++l) {
        float val;
        if (q.lfoGlobal[l]) val = glfo_[l];
        else {
            v.lfoCyc[l] += (double)m * q.lfoHz[l] * v.lfoRateMul[l] * invSr;
            val = lfoValue(q.lfoShape[l], tab, v.lfoCyc[l], v.lfoSeed + (uint32)l * 7919u);
        }
        const float fade = q.lfoFade[l] <= 0.0f ? 1.0f : std::min(1.0f, v.ageSamples / q.lfoFade[l]);
        src[SrcLfo1 + l] = val * fade;
    }
    src[SrcVelocity] = v.velocity;
    src[SrcKey] = clampT((float)(v.key - kRootKey) * (1.0f / 48.0f), -1.0f, 1.0f);
    src[SrcModX] = (v.modX - 0.5f) * 2.0f;
    src[SrcModY] = (v.modY - 0.5f) * 2.0f;
    src[SrcRandom] = v.rnd;
    float dst[NumDests] = {};
    for (int i = 0; i < q.numSlots; ++i) {
        float a = cv[SmAmt0 + q.slotIdx[i]];
        const int d = q.slotDst[i];
        if (d >= DstPitch && d <= DstOsc2Pitch) a = a * std::fabs(a) * 24.0f;      // semitones, with fine control near zero
        dst[d] += a * src[q.slotSrc[i]];
    }
    for (int l = 0; l < 3; ++l) {
        const float r = dst[DstLfo1Rate + l];
        v.lfoRateMul[l] = r == 0.0f ? 1.0f : std::exp2(4.0f * clampT(r, -2.0f, 2.0f));
    }

    // ---- pitch ----
    if (v.bendStep != 0.0f) {
        v.bend += v.bendStep * (float)m;
        if ((v.bendStep > 0.0f) == (v.bend >= v.bendTarget)) { v.bend = v.bendTarget; v.bendStep = 0.0f; }
    }
    const double base = (double)v.key + (double)v.bend + (double)cv[SmBend] + (double)v.fine * 0.01 + (double)dst[DstPitch];

    // ---- oscillators: rendered at twice the sample rate, then brought down through a half-band filter ----
    float osL[2 * kCtl], osR[2 * kCtl], bufL[kCtl], bufR[kCtl];
    const int m2 = 2 * m;
    const float invM2 = 0.5f * invM;
    for (int s = 0; s < m2; ++s) osL[s] = osR[s] = 0.0f;
    const float mix = clampT(dst[DstOscMix], -1.0f, 1.0f);
    const float level[2] = {cv[SmLevel1] * (1.0f - std::max(0.0f, mix)), cv[SmLevel2] * (1.0f + std::min(0.0f, mix))};
    double centre[2];
    for (int o = 0; o < 2; ++o)
        centre[o] = keyToHz(base + (double)q.coarse[o] + (double)cv[SmFine1 + o] * 0.01 + (double)dst[DstOsc1Pitch + o]) * invSr * 0.5;
    for (int o = 0; o < 2; ++o) {
        const int n = q.uni[o];
        const float g = level[o] / std::sqrt((float)n);
        if (g == 0.0f && !v.oscOn[o]) continue;
        const bool fresh = first || !v.oscOn[o];
        const bool sync = o == 1 && q.sync;
        const int nn = fresh ? n : std::max(n, v.prevN[o]);
        const double detune = (double)clampT(cv[SmDetune1 + o] + dst[DstDetune] * 100.0f, 0.0f, 200.0f) * kLn2Over1200;
        const float spread = cv[SmSpread1 + o], pan = cv[SmPan1 + o];
        const float pwT = clampT(cv[SmPw1 + o] + dst[DstPulseWidth] * 0.45f, 0.03f, 0.97f);
        if (fresh) v.pw[o] = pwT;
        OscRamp r;
        r.pw = v.pw[o];
        r.pws = (pwT - v.pw[o]) * invM2;
        double dcT = 0.0;
        if (sync) {         // every voice of the stack has the same ratio to its master, so the same mean
            const double ratio = std::max(1.0e-6, centre[1] / std::max(centre[0], 1.0e-12));
            dcT = naiveIntegral(q.shape[o], ratio - std::floor(ratio), (double)pwT) / ratio;
            if (fresh) v.syncDc = dcT;
        }
        const double dcs = (dcT - v.syncDc) * (double)invM2;
        for (int u = 0; u < nn; ++u) {
            float tl = 0.0f, tr = 0.0f;
            double dtT = v.dt[o][u], mdtT = v.mdt[u];
            if (u < n) {
                const float p = clampT(pan + spread * uniPan_[n][u], -1.0f, 1.0f);
                tl = g * (p > 0.0f ? 1.0f - p : 1.0f);
                tr = g * (p < 0.0f ? 1.0f + p : 1.0f);
                const double y = detune * (double)uniDet_[n][u];
                const double ratio = 1.0 + y * (1.0 + y * (0.5 + y * (1.0 / 6.0)));
                dtT = std::min(0.24, centre[o] * ratio);
                mdtT = std::min(0.24, centre[0] * ratio);
            }
            if (fresh || u >= v.prevN[o]) {
                v.dt[o][u] = dtT;
                if (o == 1) v.mdt[u] = mdtT;
                v.gl[o][u] = first ? tl : 0.0f;
                v.gr[o][u] = first ? tr : 0.0f;
            }
            r.dt = v.dt[o][u]; r.dts = (dtT - r.dt) * (double)invM2;
            r.gl = v.gl[o][u]; r.gls = (tl - r.gl) * invM2;
            r.gr = v.gr[o][u]; r.grs = (tr - r.gr) * invM2;
            if (sync) syncBlock(q.shape[o], tab, v.ph[o][u], v.mph[u], v.carry[u], r, v.mdt[u], (mdtT - v.mdt[u]) * (double)invM2, v.syncDc, dcs, osL, osR, m2);
            else oscBlock(q.shape[o], tab, v.ph[o][u], r, osL, osR, m2);
            v.dt[o][u] = dtT; v.gl[o][u] = tl; v.gr[o][u] = tr;
            if (o == 1) v.mdt[u] = mdtT;
        }
        v.pw[o] = pwT;
        v.prevN[o] = n;
        if (sync) v.syncDc = dcT;
        v.oscOn[o] = g != 0.0f;
    }
    decimate(v.hbX, v.hbY, osL, bufL, m, 0);
    decimate(v.hbX, v.hbY, osR, bufR, m, 1);

    // ---- sub oscillator ----
    {
        const float gT = cv[SmSub];
        if (gT != 0.0f || v.subGain != 0.0f) {
            const double dtT = std::min(0.45, keyToHz(base - 12.0 * q.subOct) * invSr);
            if (first || v.subGain == 0.0f) { v.subDt = dtT; if (first) v.subGain = gT; }
            double ph = v.subPh, dt = v.subDt;
            const double dts = (dtT - dt) * (double)invM;
            float gg = v.subGain;
            const float ggs = (gT - gg) * invM;
            if (q.subShape == 0) {
                for (int s = 0; s < m; ++s) {
                    dt += dts; gg += ggs;
                    const float o = tabSin(tab, ph) * gg;
                    bufL[s] += o; bufR[s] += o;
                    ph += dt; if (ph >= 1.0) ph -= 1.0;
                }
            } else {
                for (int s = 0; s < m; ++s) {
                    dt += dts; gg += ggs;
                    double x = ph < 0.5 ? 1.0 : -1.0;
                    if (ph < dt) { const double d = 1.0 - ph / dt; x -= d * d; }
                    else if (ph > 1.0 - dt) { const double d = 1.0 - (1.0 - ph) / dt; x += d * d; }
                    double t2 = ph - 0.5; if (t2 < 0.0) t2 += 1.0;
                    if (t2 < dt) { const double d = 1.0 - t2 / dt; x += d * d; }
                    else if (t2 > 1.0 - dt) { const double d = 1.0 - (1.0 - t2) / dt; x -= d * d; }
                    const float o = (float)x * gg;
                    bufL[s] += o; bufR[s] += o;
                    ph += dt; if (ph >= 1.0) ph -= 1.0;
                }
            }
            v.subPh = ph; v.subDt = dtT; v.subGain = gT;
        }
    }

    // ---- noise ----
    {
        const float gT = clampT(cv[SmNoise] + dst[DstNoise], 0.0f, 1.0f);
        if (gT != 0.0f || v.noiseGain != 0.0f) {
            if (first) v.noiseGain = gT;
            float gg = v.noiseGain;
            const float ggs = (gT - gg) * invM;
            uint32 ns = v.noise;
            float* b[2] = {bufL, bufR};
            for (int s = 0; s < m; ++s) {
                gg += ggs;
                for (int c = 0; c < 2; ++c) {
                    const float w = bipolar(xorshift(ns));
                    float o;
                    if (q.noiseType == 0) o = w;
                    else if (q.noiseType == 1) {
                        float* p = v.pink[c];
                        o = w * pinkWhite_;
                        for (int k = 0; k < kPink; ++k) { p[k] += pinkCoef_[k] * (w - p[k]); o += p[k] * pinkGain_[k]; }
                    } else {
                        v.dark[c] += darkCoef_ * (w - v.dark[c]);
                        o = v.dark[c] * darkGain_;
                    }
                    b[c][s] += o * gg;
                }
            }
            v.noise = ns; v.noiseGain = gT;
            for (int c = 0; c < 2; ++c) { flushDenormal(v.dark[c]); for (float& p : v.pink[c]) flushDenormal(p); }
        }
    }

    // ---- filters ----
    const float keyOct = (float)(base - (double)kRootKey) * (1.0f / 12.0f);
    const float xOct = q.modXUsed ? 0.0f : (v.modX - 0.5f) * 10.0f;
    const float resoAdd = (q.modYUsed ? 0.0f : (v.modY - 0.5f) * 2.0f) + dst[DstResonance];
    const float velOct = q.velFilt * 5.0f * (v.velocity - kDefaultVelocity);
    const float maxOct = (float)std::log2(std::min(20000.0, 0.45 * sr_));
    float gT[2], kT[2], k2T[2], gainT[2], driveT[2];
    for (int f = 0; f < 2; ++f) {
        const float oct = cv[SmCut1 + f] + cv[SmEnv1 + f] * v.env[1].value + cv[SmKey1 + f] * keyOct + xOct + velOct + 8.0f * dst[DstCutoff1 + f];
        const float hz = std::exp2(clampT(oct, 4.0f, maxOct));
        gT[f] = (float)std::tan(kPi * (double)hz * invSr);
        const float reso = clampT(cv[SmReso1 + f] + resoAdd, 0.0f, 1.0f);
        const float comp = 1.0f / (1.0f + 0.7f * reso * reso);          // a ringing filter is louder: take some of it back
        if (q.ftype[f] == LowPass24) {
            kT[f] = 1.0f / 0.541f;
            k2T[f] = 1.0f / (1.307f * std::exp2(reso * 4.1f));          // Butterworth at zero resonance, Q 22 at full
            gainT[f] = comp;
        } else {
            kT[f] = k2T[f] = 1.0f / (0.7071f * std::exp2(reso * 5.0f)); // Q 0.71 to 22.6
            gainT[f] = q.ftype[f] == BandPass ? comp * std::sqrt(kT[f]) * 1.189f : comp;
        }
        driveT[f] = cv[SmDrive1 + f];
    }
    const float bal = clampT(cv[SmBalance] + dst[DstBalance], 0.0f, 1.0f);
    const float w1T = q.parallel ? std::min(1.0f, 2.0f - 2.0f * bal) : 1.0f - bal;
    const float w2T = q.parallel ? std::min(1.0f, 2.0f * bal) : bal;
    if (first) { v.w1 = w1T; v.w2 = w2T; }
    const bool need2 = w2T > 0.0f || v.w2 > 0.0f;
    float y1L[kCtl], y1R[kCtl], y2L[kCtl], y2R[kCtl];
    if (first) { Filt& f = v.f[0]; f.g = gT[0]; f.k = kT[0]; f.k2 = k2T[0]; f.gain = gainT[0]; f.drive = driveT[0]; }
    filterBlock(v.f[0], q.ftype[0], bufL, bufR, y1L, y1R, m, gT[0], kT[0], k2T[0], driveT[0], gainT[0]);
    if (need2) {
        if (!v.f2On) { v.f[1] = Filt{}; Filt& f = v.f[1]; f.g = gT[1]; f.k = kT[1]; f.k2 = k2T[1]; f.gain = gainT[1]; f.drive = driveT[1]; }
        filterBlock(v.f[1], q.ftype[1], q.parallel ? bufL : y1L, q.parallel ? bufR : y1R, y2L, y2R, m, gT[1], kT[1], k2T[1], driveT[1], gainT[1]);
    }
    v.f2On = need2;

    // ---- amplifier ----
    const float amp = kVoiceGain * v.velGain * std::max(0.0f, 1.0f + dst[DstAmp]);
    const float p = clampT(v.notePan + dst[DstPan], -1.0f, 1.0f);
    const float tl = amp * (p > 0.0f ? 1.0f - p : 1.0f), tr = amp * (p < 0.0f ? 1.0f + p : 1.0f);
    if (first) { v.outL = tl; v.outR = tr; }
    float gl = v.outL, gr = v.outR, w1 = v.w1, w2 = v.w2;
    const float gls = (tl - gl) * invM, grs = (tr - gr) * invM, w1s = (w1T - w1) * invM, w2s = (w2T - w2) * invM;
    v.outL = tl; v.outR = tr; v.w1 = w1T; v.w2 = w2T;
    v.first = false;
    const float aC = q.envA[0], dC = q.envD[0], sus = q.envS[0], rC = v.rel[0];
    for (int s = 0; s < m; ++s) {
        gl += gls; gr += grs; w1 += w1s; w2 += w2s;
        const float e = envNext(v.env[0], aC, dC, sus, rC) * v.kill;
        float l = y1L[s] * w1, r = y1R[s] * w1;
        if (need2) { l += y2L[s] * w2; r += y2R[s] * w2; }
        outL[s] += l * e * gl;
        outR[s] += r * e * gr;
        if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) { v.active = false; return; } }
        if (v.env[0].stage == Env::Idle) { v.active = false; return; }
    }
}

// ------------------------------------------------------------------ effects and output

void Zest::finishQuantum(ProcessContext& ctx) {
    float* L = mixL_;
    float* R = mixR_;
    bool silent = true;
    for (int i = 0; i < kQuantum && silent; ++i) silent = L[i] == 0.0f && R[i] == 0.0f;
    if (!silent) { idleSamples_ = 0; fxWake_ = fxAsleep_; fxAsleep_ = false; }
    else idleSamples_ += kQuantum;
    if (!fxAsleep_ && idleSamples_ > 0 && (double)idleSamples_ > fxTailSeconds() * sr_) {
        // the tails have died away: clear them out so the output is exactly silent and costs nothing
        clearEffects();
        drive_.running = chorus_.running = delay_.running = reverb_.running = false;
        drive_.wet.snap(0.0f); chorus_.wet.snap(0.0f); delay_.wet.snap(0.0f); reverb_.wet.snap(0.0f);
        fxAsleep_ = true;
    }
    const float* tab = sinTab_;
    const float sr = (float)sr_;
    if (!fxAsleep_) {
        // ---- drive: tanh, antialiased with its antiderivative, then a tone low-pass ----
        {
            FxDrive& d = drive_;
            const bool on = pDrvOn->getBool();
            if (on || d.wet.cur > 0.0f) {
                const float g = dbToGain(pDrvAmt->get());
                if (!d.running) {
                    for (int c = 0; c < 2; ++c) { d.x1[c] = d.F1[c] = 0.0; d.lp[c] = 0.0f; }
                    d.gain.snap(g);
                    d.wet.snap(fxWake_ && on ? 1.0f : 0.0f);
                    d.running = true;
                }
                const float mk0 = std::pow(d.gain.cur, -0.7f), mk1 = std::pow(g, -0.7f);
                d.wet.target(on ? 1.0f : 0.0f, kQuantum);
                d.gain.target(g, kQuantum);
                const float lpc = (float)(1.0 - std::exp(-kTwoPi * (double)pDrvTone->get() / sr_));
                float* io[2] = {L, R};
                for (int i = 0; i < kQuantum; ++i) {
                    const float gg = d.gain.next(), w = d.wet.next();
                    const float mk = mk0 + (mk1 - mk0) * (float)(i + 1) * (1.0f / kQuantum);
                    for (int c = 0; c < 2; ++c) {
                        const float in = io[c][i];
                        const double x = (double)in * gg, dx = x - d.x1[c], F = lnCosh(x);
                        const double y = std::fabs(dx) > 1.0e-7 ? (F - d.F1[c]) / dx : std::tanh(0.5 * (x + d.x1[c]));
                        d.x1[c] = x; d.F1[c] = F;
                        d.lp[c] += lpc * ((float)y - d.lp[c]);
                        io[c][i] = in + w * (d.lp[c] * mk - in);
                    }
                }
                for (float& s : d.lp) flushDenormal(s);
                if (!on && d.wet.cur <= 0.0f) d.running = false;
            } else d.running = false;
        }

        // ---- chorus: two modulated taps a side, the right side moving against the left ----
        {
            FxChorus& c = chorus_;
            const bool on = pChoOn->getBool();
            if (on || c.wet.cur > 0.0f) {
                const float depth = pChoDepth->get() * 0.0035f * sr;
                const float mixT = on ? pChoMix->get() : 0.0f;
                if (!c.running) {
                    c.d[0].clear(); c.d[1].clear();
                    c.depth.snap(depth);
                    c.wet.snap(fxWake_ ? mixT : 0.0f);
                    c.running = true;
                }
                c.wet.target(mixT, kQuantum);
                c.depth.target(depth, kQuantum);
                const double inc = (double)pChoRate->get() / sr_;
                const float base1 = 0.0070f * sr, base2 = 0.0115f * sr;
                double ph = c.phase - std::floor(c.phase);
                if (!(ph < 1.0)) ph = 0.0;
                for (int i = 0; i < kQuantum; ++i) {
                    const float w = c.wet.next(), dp = c.depth.next();
                    c.d[0].push(L[i]);
                    c.d[1].push(R[i]);
                    double p2 = ph + 0.31; if (p2 >= 1.0) p2 -= 1.0;
                    const float a = tabSin(tab, ph), b = tabSin(tab, p2);
                    const float wl = 0.5f * (tapCubic(c.d[0], base1 + dp * a) + tapCubic(c.d[0], base2 + 1.3f * dp * b));
                    const float wr = 0.5f * (tapCubic(c.d[1], base1 - dp * a) + tapCubic(c.d[1], base2 - 1.3f * dp * b));
                    L[i] = L[i] * (1.0f - 0.293f * w) + wl * 0.707f * w;
                    R[i] = R[i] * (1.0f - 0.293f * w) + wr * 0.707f * w;
                    ph += inc; if (ph >= 1.0) ph -= 1.0;
                }
                c.phase = ph;
                if (!on && c.wet.cur <= 0.0f) c.running = false;
            } else {
                c.running = false;
                c.phase += (double)kQuantum * (double)pChoRate->get() / sr_;
                c.phase -= std::floor(c.phase);
            }
        }

        // ---- delay ----
        {
            FxDelay& d = delay_;
            const bool on = pDlyOn->getBool();
            if (on || d.wet.cur > 0.0f) {
                const double secs = pDlySync->getBool() ? divisionBeats(pDlyDiv->getInt()) * 60.0 / tempo_.load(std::memory_order_relaxed) : (double)pDlyTime->get() * 0.001;
                const float target = (float)clampT(std::round(secs * sr_), 1.0, std::min((double)(d.mask - 8), 4.0 * sr_));     // 4 s at most
                const float fbT = std::min(0.95f, pDlyFb->get()), mixT = on ? pDlyMix->get() : 0.0f;
                if (!d.running) {
                    d.valid = 0;
                    d.time = target;
                    for (int c = 0; c < 2; ++c) d.lp[c] = d.hp[c] = 0.0f;
                    d.fb.snap(fbT);
                    d.wet.snap(fxWake_ ? mixT : 0.0f);
                    d.running = true;
                }
                d.wet.target(mixT, kQuantum);
                d.fb.target(fbT, kQuantum);
                const bool ping = pDlyPing->getBool();
                const float lpc = (float)(1.0 - std::exp(-kTwoPi * 5500.0 / sr_)), hpc = (float)(1.0 - std::exp(-kTwoPi * 110.0 / sr_));
                const float slew = (float)(1.0 - std::exp(-1.0 / (0.06 * sr_)));
                const int64 size = (int64)d.mask + 1;
                for (int i = 0; i < kQuantum; ++i) {
                    if (d.time != target) {
                        d.time += (target - d.time) * slew;
                        if (std::fabs(target - d.time) < 0.01f) d.time = target;
                    }
                    const int di = (int)d.time;
                    const float fr = d.time - (float)di;
                    float wet[2];
                    for (int c = 0; c < 2; ++c) {
                        const float a = di <= d.valid ? d.buf[c][(size_t)((d.w - di) & d.mask)] : 0.0f;
                        float rd = a;
                        if (fr > 0.0f) {
                            const float b = di + 1 <= d.valid ? d.buf[c][(size_t)((d.w - di - 1) & d.mask)] : 0.0f;
                            rd = a + (b - a) * fr;
                        }
                        d.lp[c] += lpc * (rd - d.lp[c]);
                        d.hp[c] += hpc * (d.lp[c] - d.hp[c]);
                        wet[c] = d.lp[c] - d.hp[c];
                    }
                    const float fb = d.fb.next(), w = d.wet.next();
                    if (ping) {
                        d.buf[0][(size_t)d.w] = 0.5f * (L[i] + R[i]) + fb * wet[1];
                        d.buf[1][(size_t)d.w] = fb * wet[0];
                    } else {
                        d.buf[0][(size_t)d.w] = L[i] + fb * wet[0];
                        d.buf[1][(size_t)d.w] = R[i] + fb * wet[1];
                    }
                    d.w = (d.w + 1) & d.mask;
                    if (d.valid < size) ++d.valid;
                    L[i] += wet[0] * w;
                    R[i] += wet[1] * w;
                }
                for (int c = 0; c < 2; ++c) { flushDenormal(d.lp[c]); flushDenormal(d.hp[c]); }
                if (!on && d.wet.cur <= 0.0f) d.running = false;
            } else d.running = false;
        }

        // ---- reverb: eight delay lines in a feedback network behind four diffusers a side ----
        {
            FxReverb& r = reverb_;
            const bool on = pRevOn->getBool();
            if (on || r.wet.cur > 0.0f) {
                const float mixT = on ? pRevMix->get() : 0.0f;
                if (!r.running) {
                    for (auto& l : r.line) l.clear();
                    for (auto& ch : r.ap) for (auto& a : ch) a.clear();
                    for (float& s : r.lp) s = 0.0f;
                    r.wet.snap(fxWake_ ? mixT : 0.0f);
                    r.running = true;
                }
                r.wet.target(mixT, kQuantum);
                const double rt = std::max(0.2f, pRevDecay->get());
                float gain[8];
                for (int j = 0; j < 8; ++j) gain[j] = (float)std::pow(10.0, -3.0 * (double)r.len[j] / (rt * sr_));
                const double dampHz = 16000.0 * std::pow(0.09, (double)pRevDamp->get());
                const float dc = (float)(1.0 - std::exp(-kTwoPi * std::min(dampHz, 0.45 * sr_) / sr_));
                for (int i = 0; i < kQuantum; ++i) {
                    float x[2] = {L[i], R[i]};
                    for (int c = 0; c < 2; ++c)
                        for (int a = 0; a < 4; ++a) {
                            dsp::DelayLine& ap = r.ap[c][a];
                            const float wd = ap.tap(r.apLen[c][a] - 1);
                            const float wn = x[c] + kRevApGain * wd;
                            x[c] = wd - kRevApGain * wn;
                            ap.push(wn);
                        }
                    float o[8], sum = 0.0f;
                    for (int j = 0; j < 8; ++j) {
                        const float t = r.line[j].tap(r.len[j] - 1);
                        r.lp[j] += dc * (t - r.lp[j]);
                        o[j] = r.lp[j] * gain[j];
                        sum += o[j];
                    }
                    sum *= 0.25f;
                    for (int j = 0; j < 8; ++j) r.line[j].push(x[j & 1] + o[j] - sum);
                    // the diffused input is heard at once (the lines only answer after 40 ms), then the network takes over
                    const float w = r.wet.next() * 0.22f;
                    L[i] += (o[0] - o[2] + o[4] - o[6] + 1.5f * x[0]) * w;
                    R[i] += (o[1] - o[3] + o[5] - o[7] + 1.5f * x[1]) * w;
                }
                for (float& s : r.lp) flushDenormal(s);
                if (!on && r.wet.cur <= 0.0f) r.running = false;
            } else r.running = false;
        }
    } else {
        chorus_.phase += (double)kQuantum * (double)pChoRate->get() / sr_;
        chorus_.phase -= std::floor(chorus_.phase);
    }

    // ---- master volume and output ----
    const float vdb = pVolume->get();
    const float vol = vdb <= -47.9f ? 0.0f : dbToGain(vdb);
    if (!outPrimed_) volume_.snap(vol);
    volume_.target(vol, kQuantum);
    outPrimed_ = true;
    fxWake_ = false;
    float* oL = ctx.io[0];
    float* oR = ctx.io[1];
    for (int i = 0; i < kQuantum; ++i) {
        const float g = volume_.next();
        oL[i] += L[i] * g;
        oR[i] += R[i] * g;
        L[i] = R[i] = 0.0f;
    }
    scope_.push(oL, oR, kQuantum);
    for (int l = 0; l < 3; ++l) glfoCyc_[l] += (double)kQuantum * glfoInc_[l];
}

// ------------------------------------------------------------------ presets

std::vector<Preset> Zest::factoryPresets() const {
    // Matrix amounts: pitch destinations are amount * |amount| * 24 semitones (0.7071 is an octave, 0.08 a light
    // vibrato), cutoff destinations are amount * 8 octaves, everything else is the plain range of the control.
    const float On = 1.0f;
    return buildPresets(*this, {
        {"Init Saw", {}},

        // ---- basses ----
        {"Round Sub Bass", {{"o1shape", Sine}, {"o1coarse", -12}, {"o1level", 0.9f}, {"o1phase", 1}, {"o2shape", Triangle}, {"o2coarse", -12}, {"o2level", 0.35f}, {"o2phase", 1},
                            {"f1cutoff", 1800}, {"f1env", 1.5f}, {"f1drive", 0.25f}, {"e2dec", 200}, {"e2sus", 0}, {"e1atk", 2}, {"e1dec", 400}, {"e1sus", 0.85f}, {"e1rel", 120},
                            {"mode", Legato}, {"glide", 60}, {"velfilt", 0.3f}, {"volume", -2.5f}}},
        {"Reese Growl", {{"o1coarse", -12}, {"o1uni", 2}, {"o1detune", 24}, {"o1spread", 0.25f}, {"o2coarse", -12}, {"o2fine", 12}, {"o2uni", 2}, {"o2detune", 30}, {"o2spread", 0.4f}, {"o2level", 0.7f},
                         {"sublevel", 0.35f}, {"f1cutoff", 650}, {"f1reso", 0.22f}, {"f1key", 0.3f}, {"f1drive", 0.35f},
                         {"l2rate", 0.13f}, {"l2mode", 1}, {"m1src", SrcLfo2}, {"m1dst", DstCutoff1}, {"m1amt", 0.09f},
                         {"l3rate", 0.21f}, {"l3mode", 1}, {"m2src", SrcLfo3}, {"m2dst", DstDetune}, {"m2amt", 0.12f},
                         {"e1atk", 4}, {"e1sus", 1}, {"e1rel", 160}, {"mode", Mono}, {"glide", 45}, {"choon", On}, {"chomix", 0.3f}, {"chorate", 0.35f}, {"volume", -4.5f}}},
        {"Acid Squelch", {{"o1level", 0.9f}, {"o1phase", 1}, {"f1cutoff", 300}, {"f1reso", 0.74f}, {"f1env", 4.3f}, {"f1key", 0.4f}, {"e2atk", 0}, {"e2dec", 210}, {"e2sus", 0.15f}, {"e2rel", 150},
                          {"e1dec", 500}, {"e1sus", 0.7f}, {"e1rel", 90}, {"mode", Legato}, {"glide", 55}, {"velfilt", 0.55f}, {"drvon", On}, {"drvamt", 14}, {"drvtone", 6500}, {"volume", -1}}},
        {"Pick Bass", {{"o1shape", Pulse}, {"o1pw", 0.32f}, {"o1phase", 1}, {"sublevel", 0.5f}, {"f1cutoff", 420}, {"f1reso", 0.18f}, {"f1env", 3.8f}, {"f1key", 0.35f}, {"f1drive", 0.2f},
                       {"e2dec", 130}, {"e2sus", 0}, {"e1atk", 1}, {"e1dec", 420}, {"e1sus", 0.25f}, {"e1rel", 110}, {"velfilt", 0.5f}, {"volume", -2.5f}}},
        {"Tempo Wobble Bass", {{"o1coarse", -12}, {"o2shape", Pulse}, {"o2coarse", -12}, {"o2pw", 0.4f}, {"o2fine", -9}, {"o2level", 0.6f}, {"sublevel", 0.4f},
                               {"f1cutoff", 480}, {"f1reso", 0.42f}, {"f1drive", 0.4f}, {"l1shape", LfoTriangle}, {"l1sync", On}, {"l1div", DivEighth},
                               {"m1src", SrcLfo1}, {"m1dst", DstCutoff1}, {"m1amt", 0.3f}, {"m2src", SrcModX}, {"m2dst", DstLfo1Rate}, {"m2amt", 0.5f},     // mod X: a quarter up doubles the wobble
                               {"e1sus", 1}, {"e1rel", 120}, {"mode", Mono}, {"glide", 30}, {"drvon", On}, {"drvamt", 9}, {"volume", -4}}},
        {"Sync Bite Bass", {{"o1coarse", -12}, {"o1level", 0}, {"o2sync", On}, {"o2coarse", 10}, {"o2level", 0.9f}, {"o2phase", 1}, {"o1phase", 1}, {"sublevel", 0.45f},
                            {"m1src", SrcEnv3}, {"m1dst", DstOsc2Pitch}, {"m1amt", 0.62f}, {"e3atk", 0}, {"e3dec", 260}, {"e3sus", 0},
                            {"f1cutoff", 2200}, {"f1reso", 0.15f}, {"f1env", 1.5f}, {"e2dec", 300}, {"e1sus", 0.8f}, {"e1rel", 100}, {"mode", Mono}, {"volume", -5}}},

        // ---- leads ----
        {"Sync Sweep Lead", {{"o1level", 0}, {"o2sync", On}, {"o2level", 0.9f}, {"o2coarse", 5}, {"m1src", SrcEnv3}, {"m1dst", DstOsc2Pitch}, {"m1amt", 0.78f}, {"e3atk", 0}, {"e3dec", 900}, {"e3sus", 0.25f},
                             {"l2rate", 0.3f}, {"m2src", SrcLfo2}, {"m2dst", DstOsc2Pitch}, {"m2amt", 0.3f}, {"l1rate", 5.5f}, {"l1fade", 500}, {"m3src", SrcLfo1}, {"m3dst", DstPitch}, {"m3amt", 0.075f},
                             {"f1type", LowPass12}, {"f1cutoff", 9000}, {"e1sus", 0.85f}, {"e1rel", 180}, {"mode", Legato}, {"glide", 50},
                             {"dlyon", On}, {"dlyfb", 0.35f}, {"dlymix", 0.22f}, {"revon", On}, {"revmix", 0.15f}, {"volume", -4.5f}}},
        {"Stadium Saw Stack", {{"o1uni", 7}, {"o1detune", 30}, {"o1spread", 0.9f}, {"o2uni", 7}, {"o2detune", 42}, {"o2spread", 1}, {"o2level", 0.55f},
                              {"f1type", LowPass12}, {"f1cutoff", 7500}, {"f1env", 1.2f}, {"e2dec", 600}, {"e2sus", 0.5f}, {"f2type", HighPass}, {"f2cutoff", 140}, {"fbal", 1},
                              {"e1atk", 4}, {"e1dec", 600}, {"e1sus", 0.8f}, {"e1rel", 350}, {"revon", On}, {"revmix", 0.22f}, {"revdecay", 2.6f}, {"dlyon", On}, {"dlymix", 0.15f}, {"volume", -3.5f}}},
        {"Hollow Square Lead", {{"o1shape", Pulse}, {"o2shape", Pulse}, {"o2coarse", 12}, {"o2pw", 0.35f}, {"o2level", 0.25f}, {"l1rate", 5.6f}, {"l1fade", 450},
                                {"m1src", SrcLfo1}, {"m1dst", DstPitch}, {"m1amt", 0.08f}, {"f1type", LowPass12}, {"f1cutoff", 3800}, {"f1reso", 0.2f}, {"f1env", 1.0f}, {"e2dec", 300},
                                {"e1atk", 3}, {"e1sus", 0.85f}, {"e1rel", 140}, {"mode", Legato}, {"glide", 45}, {"dlyon", On}, {"dlymix", 0.2f}, {"volume", -6}}},
        {"Breathy Flute Lead", {{"o1shape", Triangle}, {"o2shape", Sine}, {"o2coarse", 12}, {"o2level", 0.15f}, {"noisetype", 1}, {"m1src", SrcEnv3}, {"m1dst", DstNoise}, {"m1amt", 0.22f},
                                {"e3atk", 5}, {"e3dec", 220}, {"e3sus", 0.25f}, {"f1type", LowPass12}, {"f1cutoff", 2600}, {"f1key", 0.5f}, {"l1rate", 5.0f}, {"l1fade", 500},
                                {"m2src", SrcLfo1}, {"m2dst", DstPitch}, {"m2amt", 0.06f}, {"m3src", SrcLfo1}, {"m3dst", DstAmp}, {"m3amt", 0.08f},
                                {"e1atk", 45}, {"e1dec", 300}, {"e1sus", 0.85f}, {"e1rel", 180}, {"revon", On}, {"revmix", 0.2f}, {"volume", -3.5f}}},
        {"PWM Solo Lead", {{"o1shape", Pulse}, {"o2shape", Pulse}, {"o2fine", 9}, {"o2level", 0.6f}, {"l2rate", 0.75f}, {"m1src", SrcLfo2}, {"m1dst", DstPulseWidth}, {"m1amt", 0.7f},
                           {"f1cutoff", 2600}, {"f1reso", 0.25f}, {"f1env", 1.8f}, {"f1key", 0.4f}, {"e2dec", 500}, {"e2sus", 0.4f}, {"l1rate", 5.3f}, {"l1fade", 600},
                           {"m2src", SrcLfo1}, {"m2dst", DstPitch}, {"m2amt", 0.07f}, {"e1sus", 0.9f}, {"e1rel", 160}, {"mode", Legato}, {"glide", 40},
                           {"dlyon", On}, {"dlymix", 0.18f}, {"volume", -11}}},
        {"Twin Saw Screamer", {{"o1shape", TwinSaw}, {"o1uni", 3}, {"o1detune", 14}, {"l2rate", 0.4f}, {"m1src", SrcLfo2}, {"m1dst", DstPulseWidth}, {"m1amt", 0.6f},
                               {"f1cutoff", 1800}, {"f1reso", 0.5f}, {"f1env", 2.5f}, {"f1key", 0.6f}, {"f1drive", 0.5f}, {"e2dec", 450}, {"e2sus", 0.35f},
                               {"e1sus", 0.9f}, {"e1rel", 150}, {"mode", Mono}, {"glide", 35}, {"drvon", On}, {"drvamt", 16}, {"drvtone", 7000}, {"dlyon", On}, {"dlymix", 0.2f}, {"volume", -1}}},

        // ---- pads ----
        {"Warm Analog Pad", {{"o1uni", 3}, {"o1detune", 16}, {"o1spread", 0.7f}, {"o2fine", -7}, {"o2uni", 3}, {"o2detune", 20}, {"o2level", 0.65f}, {"o2spread", 0.9f},
                             {"f1cutoff", 900}, {"f1reso", 0.12f}, {"f1env", 2.2f}, {"f1key", 0.3f}, {"e2atk", 1200}, {"e2dec", 2500}, {"e2sus", 0.55f}, {"e2rel", 1500},
                             {"e1atk", 600}, {"e1dec", 1000}, {"e1sus", 0.85f}, {"e1rel", 1600}, {"l2rate", 0.18f}, {"l2mode", 1}, {"m1src", SrcLfo2}, {"m1dst", DstCutoff1}, {"m1amt", 0.06f},
                             {"choon", On}, {"chorate", 0.4f}, {"revon", On}, {"revdecay", 3.5f}, {"revmix", 0.3f}, {"volume", -3.5f}}},
        {"Vowel Choir Pad", {{"o1uni", 5}, {"o1detune", 14}, {"o1spread", 0.8f}, {"noisetype", 1}, {"noiselevel", 0.05f}, {"froute", 1}, {"fbal", 0.5f},
                             {"f1type", BandPass}, {"f1cutoff", 700}, {"f1reso", 0.45f}, {"f1key", 0.25f}, {"f2type", BandPass}, {"f2cutoff", 1150}, {"f2reso", 0.45f}, {"f2key", 0.25f},
                             {"l3rate", 0.1f}, {"l3mode", 1}, {"m1src", SrcLfo3}, {"m1dst", DstCutoff2}, {"m1amt", 0.05f}, {"m2src", SrcRandom}, {"m2dst", DstCutoff1}, {"m2amt", 0.03f},
                             {"l1rate", 5.0f}, {"l1fade", 800}, {"m3src", SrcLfo1}, {"m3dst", DstPitch}, {"m3amt", 0.05f},
                             {"e1atk", 500}, {"e1sus", 0.9f}, {"e1rel", 1200}, {"choon", On}, {"revon", On}, {"revdecay", 4.5f}, {"revmix", 0.35f}, {"volume", -2}}},
        {"PWM String Machine", {{"o1shape", Pulse}, {"o1uni", 2}, {"o1detune", 10}, {"o1spread", 0.6f}, {"o2shape", Pulse}, {"o2fine", 6}, {"o2level", 0.5f},
                                {"l1rate", 0.9f}, {"m1src", SrcLfo1}, {"m1dst", DstPulseWidth}, {"m1amt", 0.6f}, {"f1type", LowPass12}, {"f1cutoff", 3400}, {"f1reso", 0.08f},
                                {"f2type", HighPass}, {"f2cutoff", 200}, {"fbal", 1}, {"e1atk", 140}, {"e1dec", 500}, {"e1sus", 0.8f}, {"e1rel", 500},
                                {"choon", On}, {"chorate", 0.9f}, {"chodepth", 0.7f}, {"chomix", 0.6f}, {"revon", On}, {"revmix", 0.18f}, {"volume", -6}}},
        {"Slow Bloom Pad", {{"o1uni", 5}, {"o1detune", 22}, {"o1spread", 1}, {"o2shape", Triangle}, {"o2coarse", 12}, {"o2level", 0.4f},
                            {"f1cutoff", 220}, {"f1reso", 0.3f}, {"f1env", 5}, {"e2atk", 3500}, {"e2dec", 4000}, {"e2sus", 0.7f}, {"e2rel", 2500},
                            {"e1atk", 900}, {"e1sus", 1}, {"e1rel", 2500}, {"m1src", SrcEnv3}, {"m1dst", DstDetune}, {"m1amt", 0.15f}, {"e3atk", 3000}, {"e3sus", 1},
                            {"revon", On}, {"revdecay", 6}, {"revmix", 0.4f}, {"dlyon", On}, {"dlymix", 0.15f}, {"dlydiv", DivQuarter}, {"volume", -6}}},
        {"Dark Drone Pad", {{"o1coarse", -12}, {"o1uni", 3}, {"o1detune", 12}, {"o2shape", Pulse}, {"o2coarse", -12}, {"o2pw", 0.3f}, {"o2level", 0.6f}, {"o2fine", 5},
                            {"noisetype", 2}, {"noiselevel", 0.25f}, {"f1cutoff", 320}, {"f1reso", 0.4f}, {"f1key", 0.2f},
                            {"l3rate", 0.07f}, {"l3mode", 1}, {"m1src", SrcLfo3}, {"m1dst", DstCutoff1}, {"m1amt", 0.2f}, {"l2shape", LfoSampleHold}, {"l2rate", 0.6f},
                            {"m2src", SrcLfo2}, {"m2dst", DstPan}, {"m2amt", 0.35f}, {"m3src", SrcRandom}, {"m3dst", DstCutoff1}, {"m3amt", 0.05f},
                            {"e1atk", 2000}, {"e1sus", 1}, {"e1rel", 3500}, {"revon", On}, {"revdecay", 9}, {"revdamp", 0.6f}, {"revmix", 0.45f}, {"volume", -11}}},
        {"Shimmer Air Pad", {{"o1coarse", 12}, {"o1uni", 7}, {"o1detune", 26}, {"o1spread", 1}, {"o1level", 0.7f}, {"noiselevel", 0.08f},
                             {"f1type", HighPass}, {"f1cutoff", 900}, {"f2cutoff", 9000}, {"fbal", 1}, {"l2rate", 0.25f}, {"l2mode", 1}, {"m1src", SrcLfo2}, {"m1dst", DstCutoff2}, {"m1amt", 0.08f},
                             {"e1atk", 800}, {"e1sus", 0.9f}, {"e1rel", 2000}, {"choon", On}, {"dlyon", On}, {"dlymix", 0.25f}, {"dlydiv", DivQuarterDotted}, {"dlyfb", 0.5f},
                             {"revon", On}, {"revdecay", 5}, {"revmix", 0.4f}, {"volume", -2}}},

        // ---- plucks ----
        {"Crystal Pluck", {{"o2shape", Pulse}, {"o2coarse", 12}, {"o2level", 0.35f}, {"o2pw", 0.4f}, {"f1cutoff", 380}, {"f1reso", 0.32f}, {"f1env", 5.2f}, {"f1key", 0.6f},
                           {"e2atk", 0}, {"e2dec", 190}, {"e2sus", 0}, {"e2rel", 190}, {"e1atk", 0}, {"e1dec", 500}, {"e1sus", 0}, {"e1rel", 450}, {"velfilt", 0.5f},
                           {"dlyon", On}, {"dlymix", 0.28f}, {"dlyfb", 0.45f}, {"revon", On}, {"revmix", 0.2f}, {"volume", -1.5f}}},
        {"Trance Stack Pluck", {{"o1uni", 7}, {"o1detune", 28}, {"o1spread", 0.9f}, {"o2coarse", 12}, {"o2uni", 3}, {"o2level", 0.4f},
                                   {"f1type", LowPass12}, {"f1cutoff", 600}, {"f1reso", 0.2f}, {"f1env", 4.8f}, {"f1key", 0.4f}, {"e2atk", 0}, {"e2dec", 240}, {"e2sus", 0.05f},
                                   {"e1atk", 0}, {"e1dec", 350}, {"e1sus", 0.1f}, {"e1rel", 300}, {"dlyon", On}, {"dlymix", 0.25f}, {"revon", On}, {"revmix", 0.22f}, {"volume", 1.5f}}},
        {"Wooden Mallet", {{"o1shape", Sine}, {"o1level", 0.9f}, {"o1phase", 1}, {"o2shape", Triangle}, {"o2coarse", 24}, {"o2level", 0.18f}, {"o2phase", 1},
                           {"noisetype", 1}, {"m1src", SrcEnv3}, {"m1dst", DstNoise}, {"m1amt", 0.5f}, {"e3atk", 0}, {"e3dec", 18}, {"e3sus", 0},
                           {"f1type", LowPass12}, {"f1cutoff", 3000}, {"f1env", 2}, {"f1key", 0.7f}, {"e2atk", 0}, {"e2dec", 80}, {"e2sus", 0},
                           {"e1atk", 0}, {"e1dec", 650}, {"e1sus", 0}, {"e1rel", 500}, {"revon", On}, {"revmix", 0.15f}, {"revdecay", 1.2f}, {"volume", 0}}},
        {"Muted String Pluck", {{"o1shape", Pulse}, {"o1pw", 0.22f}, {"o2level", 0.3f}, {"f1cutoff", 500}, {"f1reso", 0.15f}, {"f1env", 3.4f}, {"f1key", 0.75f}, {"f1drive", 0.2f},
                                {"e2atk", 0}, {"e2dec", 85}, {"e2sus", 0}, {"e1atk", 0}, {"e1dec", 260}, {"e1sus", 0}, {"e1rel", 150},
                                {"m1src", SrcRandom}, {"m1dst", DstPan}, {"m1amt", 0.25f}, {"volume", -2}}},

        // ---- keys ----
        {"Tine Electric Piano", {{"o1shape", Triangle}, {"o1phase", 1}, {"o2shape", Sine}, {"o2coarse", 24}, {"o2level", 0.1f}, {"o2phase", 1},
                                 {"f1type", LowPass12}, {"f1cutoff", 1500}, {"f1env", 2.4f}, {"f1key", 0.5f}, {"velfilt", 0.7f}, {"velsens", 0.9f}, {"e2dec", 600}, {"e2sus", 0.1f},
                                 {"e1atk", 1}, {"e1dec", 1800}, {"e1sus", 0.12f}, {"e1rel", 300}, {"l3shape", LfoTriangle}, {"l3rate", 4.8f}, {"l3mode", 1},
                                 {"m1src", SrcLfo3}, {"m1dst", DstPan}, {"m1amt", 0.4f}, {"choon", On}, {"chomix", 0.3f}, {"volume", 0.5f}}},
        {"Drawbar Organ", {{"o1shape", Pulse}, {"o1level", 0.6f}, {"o2shape", Sine}, {"o2coarse", 12}, {"o2level", 0.5f}, {"sublevel", 0.6f},
                           {"f1type", LowPass12}, {"f1cutoff", 5200}, {"e1atk", 3}, {"e1dec", 100}, {"e1sus", 1}, {"e1rel", 50}, {"velsens", 0},
                           {"choon", On}, {"chorate", 5.8f}, {"chodepth", 0.25f}, {"chomix", 0.6f}, {"drvon", On}, {"drvamt", 6}, {"volume", -6.5f}}},
        {"Funk Clav", {{"o1shape", Pulse}, {"o1pw", 0.18f}, {"o1phase", 1}, {"f1cutoff", 800}, {"f1reso", 0.38f}, {"f1env", 3.5f}, {"f1key", 0.8f}, {"e2atk", 0}, {"e2dec", 150}, {"e2sus", 0.1f},
                       {"f2type", HighPass}, {"f2cutoff", 300}, {"fbal", 1}, {"e1atk", 0}, {"e1dec", 700}, {"e1sus", 0.25f}, {"e1rel", 70}, {"velfilt", 0.8f}, {"velsens", 0.9f}, {"volume", -4}}},
        {"Glass Bell Keys", {{"o1shape", Sine}, {"o2shape", Sine}, {"o2coarse", 19}, {"o2fine", 14}, {"o2level", 0.35f}, {"f1type", LowPass12}, {"f1cutoff", 9000}, {"f1env", 1}, {"e2dec", 900}, {"e2sus", 0},
                             {"e1atk", 0}, {"e1dec", 2600}, {"e1sus", 0}, {"e1rel", 2200}, {"velfilt", 0.4f}, {"dlyon", On}, {"dlymix", 0.15f}, {"revon", On}, {"revdecay", 4}, {"revmix", 0.35f}, {"volume", -3.5f}}},

        // ---- strings and brass ----
        {"Ensemble Strings", {{"o1uni", 5}, {"o1detune", 15}, {"o1spread", 0.85f}, {"o2coarse", 12}, {"o2uni", 3}, {"o2detune", 12}, {"o2level", 0.3f}, {"o2spread", 0.7f},
                              {"f1type", LowPass12}, {"f1cutoff", 4200}, {"f1reso", 0.08f}, {"f1key", 0.3f}, {"f2type", HighPass}, {"f2cutoff", 220}, {"fbal", 1},
                              {"e1atk", 280}, {"e1dec", 600}, {"e1sus", 0.85f}, {"e1rel", 650}, {"l1rate", 5.4f}, {"l1fade", 700}, {"m1src", SrcLfo1}, {"m1dst", DstPitch}, {"m1amt", 0.06f},
                              {"m2src", SrcVelocity}, {"m2dst", DstCutoff1}, {"m2amt", 0.12f}, {"velsens", 0.5f},
                              {"choon", On}, {"chorate", 0.5f}, {"chomix", 0.45f}, {"revon", On}, {"revdecay", 2.8f}, {"revmix", 0.28f}, {"volume", -4}}},
        {"Brass Section", {{"o1uni", 3}, {"o1detune", 9}, {"o1spread", 0.5f}, {"o2fine", 6}, {"o2level", 0.6f}, {"f1cutoff", 380}, {"f1env", 3.6f}, {"f1key", 0.45f},
                           {"e2atk", 65}, {"e2dec", 420}, {"e2sus", 0.7f}, {"e2rel", 200}, {"e1atk", 30}, {"e1dec", 300}, {"e1sus", 0.9f}, {"e1rel", 170}, {"velfilt", 0.7f},
                           {"l1rate", 5.2f}, {"l1fade", 600}, {"m1src", SrcLfo1}, {"m1dst", DstPitch}, {"m1amt", 0.05f}, {"m2src", SrcRandom}, {"m2dst", DstPitch}, {"m2amt", 0.04f},
                           {"revon", On}, {"revmix", 0.15f}, {"volume", -7.5f}}},
        {"Fifth Horn Stab", {{"o2coarse", 7}, {"o2level", 0.6f}, {"subshape", 1}, {"sublevel", 0.3f}, {"f1cutoff", 600}, {"f1reso", 0.2f}, {"f1env", 3}, {"e2atk", 10}, {"e2dec", 300}, {"e2sus", 0.2f},
                             {"e1dec", 450}, {"e1sus", 0.3f}, {"e1rel", 200}, {"choon", On}, {"chomix", 0.35f}, {"dlyon", On}, {"dlymix", 0.18f}, {"volume", -1.5f}}},

        // ---- for arpeggios and sequences ----
        {"Sequencer Blip", {{"o1shape", Pulse}, {"o1pw", 0.3f}, {"o1phase", 1}, {"o2coarse", 12}, {"o2level", 0.2f}, {"f1cutoff", 900}, {"f1reso", 0.3f}, {"f1env", 3.2f}, {"f1key", 0.5f},
                            {"e2atk", 0}, {"e2dec", 110}, {"e2sus", 0}, {"e1atk", 0}, {"e1dec", 190}, {"e1sus", 0}, {"e1rel", 130},
                            {"m1src", SrcRandom}, {"m1dst", DstPan}, {"m1amt", 0.6f}, {"m2src", SrcRandom}, {"m2dst", DstCutoff1}, {"m2amt", 0.08f},
                            {"dlyon", On}, {"dlyfb", 0.5f}, {"dlymix", 0.3f}, {"volume", -1.5f}}},
        {"Rhythm Gate Chords", {{"o1uni", 3}, {"o1detune", 18}, {"o1spread", 0.8f}, {"o2coarse", -12}, {"o2level", 0.5f}, {"l1shape", LfoSquare}, {"l1sync", On}, {"l1div", DivSixteenth}, {"l1mode", 1},
                                {"m1src", SrcLfo1}, {"m1dst", DstAmp}, {"m1amt", 0.85f}, {"l2sync", On}, {"l2div", Div2Bars}, {"l2mode", 1}, {"m2src", SrcLfo2}, {"m2dst", DstCutoff1}, {"m2amt", 0.15f},
                                {"f1type", LowPass12}, {"f1cutoff", 2400}, {"f1env", 2}, {"e2dec", 400}, {"e1sus", 0.9f}, {"e1rel", 250},
                                {"dlyon", On}, {"dlymix", 0.2f}, {"revon", On}, {"revmix", 0.2f}, {"volume", -7.5f}}},

        // ---- effects ----
        {"Wind Over Wires", {{"o1level", 0}, {"noisetype", 1}, {"noiselevel", 1}, {"f1type", BandPass}, {"f1cutoff", 700}, {"f1reso", 0.55f}, {"f1key", 0.6f},
                             {"l3rate", 0.11f}, {"l3mode", 1}, {"m1src", SrcLfo3}, {"m1dst", DstCutoff1}, {"m1amt", 0.22f}, {"l2shape", LfoSampleHold}, {"l2rate", 3},
                             {"m2src", SrcLfo2}, {"m2dst", DstCutoff1}, {"m2amt", 0.05f}, {"l1rate", 0.23f}, {"m3src", SrcLfo1}, {"m3dst", DstPan}, {"m3amt", 0.5f},
                             {"e1atk", 1500}, {"e1sus", 1}, {"e1rel", 3000}, {"revon", On}, {"revdecay", 6}, {"revmix", 0.4f}, {"volume", 0}}},
        {"Laser Zap", {{"o1phase", 1}, {"o2sync", On}, {"o2shape", Pulse}, {"o2level", 0.7f}, {"o2coarse", 12}, {"o2phase", 1}, {"m1src", SrcEnv3}, {"m1dst", DstPitch}, {"m1amt", 1},
                       {"e3atk", 0}, {"e3dec", 260}, {"e3sus", 0}, {"m2src", SrcEnv3}, {"m2dst", DstCutoff1}, {"m2amt", 0.4f}, {"f1cutoff", 500}, {"f1reso", 0.5f},
                       {"e1atk", 0}, {"e1dec", 380}, {"e1sus", 0}, {"e1rel", 200}, {"dlyon", On}, {"dlysync", 0}, {"dlytime", 130}, {"dlyfb", 0.55f}, {"dlymix", 0.3f}, {"volume", -1.5f}}},
        {"Robot Chatter", {{"o1shape", Pulse}, {"l1shape", LfoSampleHold}, {"l1sync", On}, {"l1div", DivSixteenth}, {"m1src", SrcLfo1}, {"m1dst", DstPitch}, {"m1amt", 0.7f},
                           {"m2src", SrcLfo1}, {"m2dst", DstCutoff1}, {"m2amt", 0.3f}, {"f1cutoff", 1400}, {"f1reso", 0.55f}, {"e1sus", 0.9f}, {"e1rel", 120},
                           {"dlyon", On}, {"dlymix", 0.25f}, {"volume", -8}}},
        {"Rising Tension", {{"o1uni", 7}, {"o1detune", 40}, {"o1spread", 1}, {"noiselevel", 0.3f}, {"m1src", SrcEnv3}, {"m1dst", DstPitch}, {"m1amt", 0.7071f}, {"e3atk", 8000}, {"e3sus", 1},
                            {"f1cutoff", 200}, {"f1reso", 0.3f}, {"f1env", 6}, {"e2atk", 8000}, {"e2sus", 1}, {"l1rate", 2}, {"m2src", SrcEnv3}, {"m2dst", DstLfo1Rate}, {"m2amt", 0.5f},
                            {"m3src", SrcLfo1}, {"m3dst", DstAmp}, {"m3amt", 0.5f}, {"e1atk", 3000}, {"e1sus", 1}, {"e1rel", 400}, {"revon", On}, {"revdecay", 4}, {"revmix", 0.3f}, {"volume", -6.5f}}},
        {"Sub Drop Impact", {{"o1shape", Sine}, {"o1level", 1}, {"o1phase", 1}, {"m1src", SrcEnv3}, {"m1dst", DstPitch}, {"m1amt", 0.9f}, {"e3atk", 0}, {"e3dec", 180}, {"e3sus", 0},
                             {"noisetype", 2}, {"m2src", SrcEnv3}, {"m2dst", DstNoise}, {"m2amt", 0.6f}, {"f1drive", 0.5f}, {"e1atk", 0}, {"e1dec", 1200}, {"e1sus", 0}, {"e1rel", 600},
                             {"drvon", On}, {"drvamt", 10}, {"revon", On}, {"revmix", 0.12f}, {"volume", 0}}},
    });
}

} // namespace pulp
