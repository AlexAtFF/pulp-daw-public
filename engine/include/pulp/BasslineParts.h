#pragma once
// Parts shared by the bass and drum instruments (Squelch, Twine, Low Four, Thud, Clatter): a decay
// that lands on zero, a 2x up / down sampler pair with no reported latency, voice allocation that
// never clicks when it steals, and a plucked-string loop whose tuning and decay are set analytically.
#include "pulp/Dsp.h"
#include "pulp/Effects.h"
#include "pulp/Plugin.h"

namespace pulp::bl {

constexpr double kLn1000 = 6.907755278982137;       // 60 dB as a natural logarithm

// Per-sample multiplier of an exponential that falls 60 dB in the given time.
inline float t60Coef(double seconds, double rate) { return (float)std::exp(-kLn1000 / (std::max(1.0e-4, seconds) * rate)); }
// One-pole smoothing coefficient for a time constant.
inline float tauCoef(double seconds, double rate) { return seconds <= 0.0 ? 1.0f : (float)(1.0 - std::exp(-1.0 / (seconds * rate))); }

// An exponential decay that really reaches zero and stays there: it aims a little below zero, so there
// is no step when a voice ends and no denormal tail.
struct Fall {
    float v = 0.0f;
    static constexpr float kUnder = 3.0e-5f;
    inline float next(float coef) {
        v = (v + kUnder) * coef - kUnder;
        if (v < 0.0f) v = 0.0f;
        return v;
    }
};

// Per-note pan as the native synths do it: unity in the middle, the far side fades out.
inline void noteGains(float pan, float& l, float& r) {
    pan = clampT(pan, -1.0f, 1.0f);
    l = pan > 0.0f ? 1.0f - pan : 1.0f;
    r = pan < 0.0f ? 1.0f + pan : 1.0f;
}

// Two-sample polynomial band-limited step residual. t is the phase (0..1), dt the phase increment.
inline double polyBlep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

// Eighth-order Butterworth low-pass at 0.42 of the base rate, running at twice the base rate. It is what
// stands between 2x processing and the base rate in both directions. Minimum phase: nothing is reported
// as latency and a note still starts on its own sample.
struct HalfBand {
    dsp::Biquad s[4];
    void prepare(double baseRate) {
        static const double q[4] = {0.50979558, 0.60134489, 0.89997622, 2.56291545};
        for (int i = 0; i < 4; ++i) { s[i].lowpass(0.42 * baseRate, q[i], 2.0 * baseRate); s[i].reset(); }
    }
    void reset() { for (auto& b : s) b.reset(); }
    inline float process(float x) {
        double y = x;
        for (auto& b : s) {
            const double o = b.b0 * y + b.z1;
            b.z1 = b.b1 * y - b.a1 * o + b.z2;
            b.z2 = b.b2 * y - b.a2 * o;
            y = o;
        }
        return (float)y;
    }
};
// 2x -> 1x: feed both samples, keep the second output.
struct Down2 {
    HalfBand f;
    void prepare(double baseRate) { f.prepare(baseRate); }
    void reset() { f.reset(); }
    inline float process(float a, float b) { f.process(a); return f.process(b); }
};
// 1x -> 2x: zero stuffing, then the same low-pass.
struct Up2 {
    HalfBand f;
    void prepare(double baseRate) { f.prepare(baseRate); }
    void reset() { f.reset(); }
    inline void process(float x, float& a, float& b) { a = f.process(2.0f * x); b = f.process(0.0f); }
};

// Picks the voice a new note plays on. At most maxSounding voices sound at once: when that is reached
// the oldest is faded out (killStep per sample) and the note takes a spare voice, so a steal never
// clicks. Only when every spare is itself still fading is the most faded one taken over.
// V needs: bool active; uint32 age; float kill, killStep.
template <class V, int N>
V* claimVoice(V (&voices)[N], int maxSounding, float killStep) {
    int sounding = 0;
    V* oldest = nullptr;
    V* spare = nullptr;
    for (auto& v : voices) {
        if (!v.active) { if (!spare) spare = &v; continue; }
        if (v.killStep == 0.0f) { ++sounding; if (!oldest || v.age < oldest->age) oldest = &v; }
    }
    if (sounding >= maxSounding && oldest) oldest->killStep = killStep;
    if (spare) return spare;
    V* best = &voices[0];
    for (auto& v : voices) if (v.kill < best->kill) best = &v;
    return best;
}

// ---------------------------------------------------------------------------------------------
// A plucked string: a delay loop holding one period, read with fifth-order Lagrange interpolation,
// four all-pass stages for stiffness, and a one-pole low-pass for the loss of high partials.
//
// retune() works everything out at the fundamental: the phase delay of every filter in the loop is
// taken off the delay line, so the first partial sits on the requested frequency at any sample rate,
// and the loop gain is set so that partial falls 60 dB in exactly t60 seconds, whatever the key.
struct StringSetting {
    double hz = 110.0;          // fundamental
    double t60 = 4.0;           // seconds for the fundamental to fall 60 dB
    double highLoss = 1.0;      // extra decay rate (1/s) of a partial at 2 kHz: how fast the brightness goes
    double stiffness = 0.0;     // 0..1: stretches the upper partials sharp
};

struct StringLoop {
    float* buf = nullptr;
    int mask = 0, w = 0;
    int tap = 0;                // whole samples of delay before the interpolator
    float h[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    float pole = 0.0f, gain = 0.0f, apA = 0.0f;
    float lp = 0.0f, apZ[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    double period = 100.0;      // samples, after clamping to what the loop can hold

    void attach(float* memory, int size) { buf = memory; mask = size - 1; w = 0; }
    void clear() {
        if (buf) std::memset(buf, 0, sizeof(float) * (size_t)(mask + 1));
        w = 0;
        lp = 0.0f;
        apZ[0] = apZ[1] = apZ[2] = apZ[3] = 0.0f;
    }
    void retune(const StringSetting& s, double sr);
    // One sample: what comes round the loop plus the excitation. Returns the string's output.
    inline float tick(float excitation) {
        const int r = w - 1 - tap;
        float x = h[0] * buf[(size_t)(r & mask)] + h[1] * buf[(size_t)((r - 1) & mask)] + h[2] * buf[(size_t)((r - 2) & mask)]
                + h[3] * buf[(size_t)((r - 3) & mask)] + h[4] * buf[(size_t)((r - 4) & mask)] + h[5] * buf[(size_t)((r - 5) & mask)];
        for (int k = 0; k < 4; ++k) {
            const float y = apA * x + apZ[k];
            apZ[k] = x - apA * y;
            x = y;
        }
        lp += (1.0f - pole) * (x - lp);
        float y = gain * lp + excitation;
        flushDenormal(y);
        buf[(size_t)w] = y;
        w = (w + 1) & mask;
        return y;
    }
    // The string as it was `delay` samples ago (for pickup positions); delay >= 0.
    inline float past(float delay) const {
        const int d = (int)delay;
        const float f = delay - (float)d;
        const float a = buf[(size_t)((w - 1 - d) & mask)], b = buf[(size_t)((w - 2 - d) & mask)];
        return a + (b - a) * f;
    }
};

// Shared storage for the delay lines of a bank of strings, sized for the lowest note at this rate.
struct StringMemory {
    std::vector<float> data;
    int size = 0;
    void prepare(double sr, int strings) {
        size = 1;
        while (size < (int)(sr / 15.0) + 16) size <<= 1;
        data.assign((size_t)size * (size_t)strings, 0.0f);
    }
    float* line(int index) { return data.data() + (size_t)size * (size_t)index; }
};

} // namespace pulp::bl
