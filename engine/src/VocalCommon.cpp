// Shared pieces of the vocal effects: scales, an FFT with tables, the pitch detector and the grain shifter.
#include "pulp/Vocal.h"

namespace pulp {
namespace vocal {

// ------------------------------------------------------------------ scales

std::vector<std::string> keyNames() { return {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}; }
std::vector<std::string> scaleNames() {
    return {"Chromatic", "Major", "Minor", "Pentatonic major", "Pentatonic minor", "Blues", "Dorian", "Mixolydian", "Harmonic minor"};
}

unsigned scaleMask(int scale, int key) {
    static const unsigned kSets[NumScales] = {
        0xFFF,                                                           // chromatic
        1u | 1u << 2 | 1u << 4 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 11, // major
        1u | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 10, // natural minor
        1u | 1u << 2 | 1u << 4 | 1u << 7 | 1u << 9,                      // pentatonic major
        1u | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 10,                     // pentatonic minor
        1u | 1u << 3 | 1u << 5 | 1u << 6 | 1u << 7 | 1u << 10,           // blues
        1u | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 10, // dorian
        1u | 1u << 2 | 1u << 4 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 10, // mixolydian
        1u | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 11, // harmonic minor
    };
    const unsigned set = kSets[clampT(scale, 0, (int)NumScales - 1)];
    key = ((key % 12) + 12) % 12;
    unsigned out = 0;
    for (int i = 0; i < 12; ++i) if (set & (1u << i)) out |= 1u << ((i + key) % 12);
    return out;
}

static inline bool inMask(int note, unsigned mask) { return (mask >> (((note % 12) + 12) % 12)) & 1u; }

double nearestNote(double key, unsigned mask) {
    if ((mask & 0xFFFu) == 0) return key;
    const int base = (int)std::floor(key);
    double best = key, bestDist = 1.0e9;
    for (int n = base - 6; n <= base + 7; ++n) {
        if (!inMask(n, mask)) continue;
        const double d = std::fabs(key - (double)n);
        if (d < bestDist) { bestDist = d; best = (double)n; }
    }
    return best;
}

int stepNote(int note, int steps, unsigned mask) {
    if ((mask & 0xFFFu) == 0) return note + steps;
    while (steps > 0) { do ++note; while (!inMask(note, mask)); --steps; }
    while (steps < 0) { do --note; while (!inMask(note, mask)); ++steps; }
    return note;
}

int latencyFor(double sr) { return (int)std::lround(0.018 * sr); }

// ------------------------------------------------------------------ FFT

void Fft::prepare(int n) {
    n_ = n;
    rev_.assign((size_t)n, 0);
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        rev_[(size_t)i] = j;
    }
    cos_.assign((size_t)(n / 2), 0.0f);
    sin_.assign((size_t)(n / 2), 0.0f);
    for (int k = 0; k < n / 2; ++k) {
        cos_[(size_t)k] = (float)std::cos(kTwoPi * k / n);
        sin_[(size_t)k] = (float)std::sin(kTwoPi * k / n);
    }
}

void Fft::run(float* re, float* im, bool inverse) const {
    const int n = n_;
    const int* rev = rev_.data();
    for (int i = 1; i < n; ++i) {
        const int j = rev[i];
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    const float* ct = cos_.data();
    const float* st = sin_.data();
    const float sign = inverse ? 1.0f : -1.0f;
    for (int len = 2, step = n / 2; len <= n; len <<= 1, step >>= 1) {
        const int half = len / 2;
        for (int i = 0; i < n; i += len) {
            float* ar = re + i; float* ai = im + i;
            float* br = ar + half; float* bi = ai + half;
            for (int k = 0; k < half; ++k) {
                const float wr = ct[k * step], wi = sign * st[k * step];
                const float xr = br[k] * wr - bi[k] * wi, xi = br[k] * wi + bi[k] * wr;
                br[k] = ar[k] - xr; bi[k] = ai[k] - xi;
                ar[k] += xr; ai[k] += xi;
            }
        }
    }
    if (inverse) {
        const float s = 1.0f / (float)n;
        for (int i = 0; i < n; ++i) { re[i] *= s; im[i] *= s; }
    }
}

// ------------------------------------------------------------------ pitch detector

namespace {
inline double dot(const float* a, const float* b, int n) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += (double)a[i] * (double)b[i];
        s1 += (double)a[i + 1] * (double)b[i + 1];
        s2 += (double)a[i + 2] * (double)b[i + 2];
        s3 += (double)a[i + 3] * (double)b[i + 3];
    }
    for (; i < n; ++i) s0 += (double)a[i] * (double)b[i];
    return (s0 + s1) + (s2 + s3);
}
constexpr int kDecRing = 1024;
constexpr double kFmin[2] = {60.0, 110.0}, kFmax[2] = {600.0, 1200.0};
}

void PitchDetector::prepare(double sr, int monoRingSize) {
    sr_ = sr;
    dec_ = sr < 64000.0 ? 4 : (sr < 128000.0 ? 8 : 16);
    const double fd = sr / dec_;
    centre_ = (int)std::lround(kCentreSeconds * sr);
    defaultT_ = std::floor(sr * 0.005);
    int maxWin = 0, maxTau = 0, maxFine = 0;
    for (int r = 0; r < 2; ++r) {
        tmax_[r] = (int)std::ceil(fd / kFmin[r]) + 1;
        tmin_[r] = std::max(2, (int)std::floor(fd / kFmax[r]));
        win_[r] = std::min(kDecRing - 8, (int)(2.2 * tmax_[r]) + 2);
        fineMin_[r] = std::max(8, (int)std::floor(sr / kFmax[r] * 0.9));
        fineMax_[r] = (int)std::ceil(sr / kFmin[r] * 1.08) + dec_ + 2;
        maxWin = std::max(maxWin, win_[r]);
        maxTau = std::max(maxTau, tmax_[r]);
        maxFine = std::max(maxFine, fineMax_[r]);
    }
    const int maxM = (int)(1.25 * sr / kFmin[0]) + 8;
    decBuf_.assign((size_t)kDecRing, 0.0f);
    w_.assign((size_t)maxWin + 8, 0.0f);
    sq_.assign((size_t)maxWin + 9, 0.0);
    nsdf_.assign((size_t)maxTau + 8, 0.0f);
    lin_.assign((size_t)(maxM + maxFine + 16), 0.0f);
    psq_.assign((size_t)(maxM + maxFine + 17), 0.0);
    vals_.assign((size_t)maxFine + 16, 0.0);
    const double fc = 0.28 * fd;
    lp_[0].lowpass(fc, 0.54119610, sr);
    lp_[1].lowpass(fc, 1.30656296, sr);
    mono_.prepare(std::max(monoRingSize, maxM + maxFine + centre_ + 64));
    reset();
}

void PitchDetector::reset() {
    std::fill(decBuf_.begin(), decBuf_.end(), 0.0f);
    lp_[0].reset(); lp_[1].reset();
    mono_.clear();
    decW_ = decPhase_ = hopCount_ = 0;
    n_ = 0;
    count_ = 0;
    lastVoiced_ = false;
    lastPeriod_ = 0.0;
}

void PitchDetector::push(float x) {
    mono_.d[n_ & mono_.mask] = x;
    ++n_;
    const float y = lp_[1].process(lp_[0].process(x));
    if (++decPhase_ >= dec_) {
        decPhase_ = 0;
        decBuf_[(size_t)decW_] = y;
        decW_ = (decW_ + 1) & (kDecRing - 1);
        if (++hopCount_ >= kHopDec) { hopCount_ = 0; analyse(); }
    }
}

void PitchDetector::analyse() {
    const int r = range_, W = win_[r], tmin = tmin_[r], tmax = tmax_[r];
    float* w = w_.data();
    double* sq = sq_.data();
    for (int i = 0; i < W; ++i) w[i] = decBuf_[(size_t)((decW_ - W + i) & (kDecRing - 1))];
    sq[0] = 0.0;
    for (int i = 0; i < W; ++i) sq[i + 1] = sq[i] + (double)w[i] * (double)w[i];

    Estimate e;
    e.time = (double)(n_ - centre_);
    e.period = lastPeriod_ > 0.0 ? lastPeriod_ : defaultT_;
    double tauC = 0.0, clarC = 0.0, clarF = 0.0, fine = 0.0;

    if (sq[W] > 1.0e-10 * W) {          // louder than -100 dBFS
        float* ns = nsdf_.data();
        for (int tau = tmin - 1; tau <= tmax + 1; ++tau) {
            const double m = sq[W - tau] + (sq[W] - sq[tau]);
            ns[tau] = m > 1.0e-20 ? (float)(2.0 * dot(w, w + tau, W - tau) / m) : 0.0f;
        }
        // every local maximum is a candidate period; the first one nearly as strong as the strongest is the pitch
        constexpr int kMaxPeaks = 64;
        int pkTau[kMaxPeaks], nPeaks = 0;
        float maxV = 0.0f;
        for (int tau = tmin; tau <= tmax; ++tau) {
            const float v = ns[tau];
            if (v > 0.0f && v > ns[tau - 1] && v >= ns[tau + 1]) {
                if (nPeaks < kMaxPeaks) pkTau[nPeaks++] = tau;
                maxV = std::max(maxV, v);
            }
        }
        int pick = -1;
        if (nPeaks > 0) {
            if (lastVoiced_) {          // stay with the period we were following if it is still a good candidate
                double bestRel = 0.12;
                for (int p = 0; p < nPeaks; ++p) {
                    const double rel = std::fabs(pkTau[p] * (double)dec_ - lastPeriod_) / lastPeriod_;
                    if (rel < bestRel && ns[pkTau[p]] >= 0.8f * maxV) { bestRel = rel; pick = pkTau[p]; }
                }
            }
            if (pick < 0)
                for (int p = 0; p < nPeaks; ++p) if (ns[pkTau[p]] >= 0.9f * maxV) { pick = pkTau[p]; break; }
        }
        if (pick > 0) {
            const double a = ns[pick - 1], b = ns[pick], c = ns[pick + 1], den = a - 2.0 * b + c;
            const double d = den < -1.0e-12 ? clampT(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
            tauC = pick + d;
            clarC = std::min(1.0, b - 0.25 * (a - c) * d);
        }
    }

    if (tauC > 0.0 && clarC >= 0.3) {
        // second pass at the full rate: two segments one lag apart, about two and a quarter periods in all
        const double T0 = tauC * dec_;
        const int lo = std::max(fineMin_[r], (int)std::floor(T0 * 0.94) - dec_);
        const int hi = std::min(fineMax_[r], (int)std::ceil(T0 * 1.06) + dec_);
        fine = T0;
        if (hi >= lo + 2) {
            const int maxM = (int)lin_.size() - fineMax_[r] - 16;
            const int M = clampT((int)std::lround(1.25 * T0), 24, maxM);
            const int span = M + hi + 2;
            int64 a0 = n_ - centre_ - (M + (int64)std::lround(T0)) / 2;
            if (a0 + span > n_) a0 = n_ - span;
            float* lin = lin_.data();
            double* psq = psq_.data();
            const float* md = mono_.d;
            const int64 mm = mono_.mask;
            psq[0] = 0.0;
            for (int i = 0; i < span; ++i) {
                lin[i] = md[(a0 + i) & mm];
                psq[i + 1] = psq[i] + (double)lin[i] * (double)lin[i];
            }
            const double e0 = psq[M];
            double* vals = vals_.data();
            const int nv = hi - lo + 3;                     // lags lo-1 .. hi+1
            for (int i = 0; i < nv; ++i) vals[i] = -2.0;
            auto at = [&](int tau) {
                double& v = vals[tau - lo + 1];
                if (v < -1.5) {
                    const double m = e0 + (psq[tau + M] - psq[tau]);
                    v = m > 1.0e-20 ? 2.0 * dot(lin, lin + tau, M) / m : 0.0;
                }
                return v;
            };
            const int step = (hi - lo) > 28 ? 2 : 1;
            int best = lo;
            double bestV = -2.0;
            for (int tau = lo; tau <= hi; tau += step) { const double v = at(tau); if (v > bestV) { bestV = v; best = tau; } }
            if (step == 2) {
                int b0 = best;
                for (int tau = std::max(lo, b0 - 1); tau <= std::min(hi, b0 + 1); ++tau) { const double v = at(tau); if (v > bestV) { bestV = v; best = tau; } }
            }
            const double a = at(best - 1), b = bestV, c = at(best + 1), den = a - 2.0 * b + c;
            const double d = den < -1.0e-12 ? clampT(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
            fine = best + d;
            clarF = std::min(1.0, b - 0.25 * (a - c) * d);
            e.time = (double)a0 + 0.5 * ((double)M + fine);
        }
    }

    const double thrC = lastVoiced_ ? 0.5 : 0.7, thrF = lastVoiced_ ? 0.45 : 0.6;
    e.voiced = fine > 0.0 && clarC >= thrC && clarF >= thrF;
    e.clarity = (float)std::min(clarC, clarF);
    if (e.voiced) { e.period = fine; lastPeriod_ = fine; }
    lastVoiced_ = e.voiced;
    if (count_ > 0) e.time = std::max(e.time, est_[(size_t)((count_ - 1) % kMaxEst)].time + 1.0);
    est_[(size_t)(count_ % kMaxEst)] = e;
    ++count_;
}

bool PitchDetector::periodAt(double time, double& period) const {
    period = defaultT_;
    if (count_ == 0) return false;
    const int64 oldest = std::max<int64>(0, count_ - kMaxEst);
    int64 i = count_ - 1;
    while (i > oldest && est_[(size_t)(i % kMaxEst)].time > time) --i;
    const Estimate& a = est_[(size_t)(i % kMaxEst)];
    if (i == count_ - 1 || a.time > time) {             // after the newest estimate, or before the oldest one kept
        period = a.period > 0.0 ? a.period : defaultT_;
        return a.voiced;
    }
    const Estimate& b = est_[(size_t)((i + 1) % kMaxEst)];
    const double al = (time - a.time) / (b.time - a.time);
    const bool voiced = al < 0.5 ? a.voiced : b.voiced;
    if (a.voiced && b.voiced) {
        const double ratio = b.period / a.period;
        period = (ratio > 0.8 && ratio < 1.25) ? a.period * std::pow(ratio, al) : (al < 0.5 ? a.period : b.period);
    } else if (voiced) {
        period = a.voiced ? a.period : b.period;
    } else {
        period = defaultT_;
    }
    return voiced;
}

// ------------------------------------------------------------------ grain shifter

struct GrainShifter::Tables {
    float sinc[kPhases + 1][kTaps];
    float hann[kWin + 2];
    // Blackman-windowed sinc, eight taps, for a read position `frac` past tap 3
    static void coefs(double frac, float* c) {
        double v[kTaps], sum = 0.0;
        for (int k = 0; k < kTaps; ++k) {
            const double d = (double)(k - 3) - frac, x = kPi * d;
            const double s = std::fabs(d) < 1.0e-9 ? 1.0 : std::sin(x) / x;
            const double wn = 0.42 + 0.5 * std::cos(kPi * d / 4.0) + 0.08 * std::cos(kTwoPi * d / 4.0);
            v[k] = s * wn;
            sum += v[k];
        }
        for (int k = 0; k < kTaps; ++k) c[k] = (float)(v[k] / sum);
    }
    Tables() {
        for (int p = 0; p <= kPhases; ++p) coefs((double)p / kPhases, sinc[p]);
        for (int i = 0; i < kWin + 2; ++i) hann[i] = (float)(0.5 - 0.5 * std::cos(kPi * (double)std::min(i, (int)kWin) / kWin));
    }
};

const GrainShifter::Tables& GrainShifter::tables() {
    static const Tables t;
    return t;
}

void GrainShifter::prepare(double sr, int lookahead, float* const* hist, int64 mask, int nch) {
    tab_ = &tables();
    nch_ = clampT(nch, 1, 2);
    hist_[0] = hist[0];
    hist_[1] = nch_ > 1 ? hist[1] : hist[0];
    mask_ = mask;
    minT_ = sr / 1400.0;
    maxT_ = sr / 55.0;
    lookahead_ = (double)lookahead;
    prevSpacing_ = prevT_ = std::floor(sr * 0.005);
    scratch_.assign((size_t)(2.2 * maxT_) + 64, 0.0f);
    scratchSq_.assign(scratch_.size() + 1, 0.0);
    reset(0.0);
}

void GrainShifter::reset(double startTime) {
    n_ = 0;
    nonIdent_ = 0;
    hasPrev_ = false;
    prevIdent_ = true;
    prevUnity_ = true;
    prevVoiced_ = false;
    delta_ = 0.0;
    lastMark_ = startTime;
    nextMark_ = startTime;
    spawnAt_ = startTime;
    prevSpacing_ = prevT_;
}

double GrainShifter::localPeriod(double estimate, int64 newest) {
    const double T = clampT(estimate, minT_, maxT_);
    if (!hasPrev_) return T;
    const int64 base = (int64)std::floor(lastMark_ - delta_);       // where the previous grain was read around
    const int L0 = (int)std::lround(T), r = std::max(2, (int)(0.04 * T) + 1), W = std::max(16, (int)T);
    const int span = W + L0 + r + 2;
    if (base + L0 + r + 2 > newest || span > (int)scratch_.size()) return T;
    float* sc = scratch_.data();
    double* sq = scratchSq_.data();
    const int64 first = base - W + 1;
    sq[0] = 0.0;
    for (int i = 0; i < span; ++i) {
        float v = hist_[0][(first + i) & mask_];
        if (nch_ > 1) v += hist_[1][(first + i) & mask_];
        sc[i] = v;
        sq[i + 1] = sq[i] + (double)v * (double)v;
    }
    const double e0 = sq[W];
    if (e0 < 1.0e-12) return T;
    // the period that ended at the previous read point against the one that ends a lag later
    double vals[3] = {0.0, 0.0, 0.0}, bestV = -2.0, bestA = 0.0, bestC = 0.0;
    int best = 0;
    for (int L = L0 - r - 1; L <= L0 + r + 1; ++L) {
        const double m = e0 + (sq[L + W] - sq[L]);
        const double v = m > 1.0e-20 ? 2.0 * dot(sc, sc + L, W) / m : 0.0;
        vals[0] = vals[1]; vals[1] = vals[2]; vals[2] = v;
        // vals[1] is lag L-1: an inner lag with both neighbours known
        if (L - 1 >= L0 - r && L - 1 <= L0 + r && vals[1] > bestV) { bestV = vals[1]; best = L - 1; bestA = vals[0]; bestC = vals[2]; }
    }
    if (bestV < 0.7 || !(bestV >= bestA && bestV >= bestC)) return T;       // unclear, or no peak inside the range
    const double den = bestA - 2.0 * bestV + bestC;
    const double d = den < -1.0e-12 ? clampT(0.5 * (bestA - bestC) / den, -0.5, 0.5) : 0.0;
    return clampT(best + d, minT_, maxT_);
}

// Where, in the period that ends at `end`, the voice's pulse sits: the place where the energy of the positive
// half of the wave, summed over a short stretch, is greatest. Also gives that energy at the position `at`
// (taken modulo the period).
int64 GrainShifter::findPulse(int64 end, int period, int half, double at, double& peakEnergy, double& energyAt) {
    const int count = period + 2 * half + 1;
    const int64 first = end - period - half;
    double* sq = scratchSq_.data();
    sq[0] = 0.0;
    for (int i = 0; i < count; ++i) {
        float v = hist_[0][(first + i) & mask_];
        if (nch_ > 1) v += hist_[1][(first + i) & mask_];
        sq[i + 1] = sq[i] + (v > 0.0f ? (double)v * (double)v : 0.0);
    }
    int best = 0;
    peakEnergy = -1.0;
    for (int k = 0; k < period; ++k) {
        const double e = sq[k + 2 * half + 1] - sq[k];
        if (e > peakEnergy) { peakEnergy = e; best = k; }
    }
    int64 k0 = ((int64)std::llround(at) - (end - period)) % period;
    if (k0 < 0) k0 += period;
    energyAt = sq[k0 + 2 * half + 1] - sq[k0];
    return end - period + best;
}

// Grains are best cut around the voice's pulses: a grain centred between two pulses holds half of each, and
// its spectrum has dips between the harmonics, exactly where a moved harmonic lands. jump: go straight to
// the pulse (a shifted stretch is starting); otherwise creep towards it if the read point has wandered.
void GrainShifter::centreOnPulse(double s, double T, int64 newest, bool jump) {
    const int Ti = (int)T, half = std::max(1, Ti / 12);
    if (Ti + 2 * half + 2 >= (int)scratchSq_.size()) return;
    const double a0 = s - delta_;
    const int64 end = std::min((int64)std::floor(a0 + 0.5 * T), newest - half - 1);
    double peak = 0.0, here = 0.0;
    const int64 p = findPulse(end, Ti, half, a0, peak, here);
    if (peak <= 1.0e-12) return;
    if (jump) { delta_ = s - (double)p; return; }
    double e = (double)p - a0;
    e -= T * std::round(e / T);
    // (a creep is a small detune while it lasts: at most 0.2 %, 3.5 cents)
    if (std::fabs(e) > 0.125 * T && peak > 1.3 * here) delta_ -= clampT(0.006 * e, -0.002 * T, 0.002 * T);
}

void GrainShifter::spawn(const GrainCtl& c, int64 newest) {
    const double s = nextMark_;
    const double T = clampT(c.period, minT_, maxT_);
    double P = 1.0, F = 1.0;
    if (c.voiced) { P = clampT(c.pitch, 0.25, 4.0); F = clampT(c.formant, 0.25, 4.0); }
    double O = P / F;                       // how many grains overlap, relative to a plain crossfade
    if (O > 8.0) { O = 8.0; F = P / 8.0; }
    else if (O < 0.125) { O = 0.125; F = P * 8.0; }
    const bool unity = P == 1.0 && F == 1.0;

    if (!c.voiced) {
        delta_ = 0.0;
    } else if (unity && prevUnity_) {
        if (c.centre && prevIdent_) {
            centreOnPulse(s, T, newest, true);      // a voiced stretch begins and will be shifted: start on the pulse
        } else if (delta_ != 0.0 && !c.centre) {
            // nothing is being shifted: the read point drifts back onto the input, after which the output is the input
            const double st = 0.002 * T;
            delta_ = std::fabs(delta_) <= st ? 0.0 : delta_ - (delta_ > 0.0 ? st : -st);
        }
    } else {
        delta_ += prevSpacing_ - T;         // the read point moves on by one input period per grain
        if (!unity) centreOnPulse(s, T, newest, prevIdent_);
    }
    const double Lh = !hasPrev_ ? T / F : (prevIdent_ ? prevSpacing_ : O * prevSpacing_);
    const double spacing = T / P;
    const double Rh = O * spacing;
    if (c.voiced) {
        // keep every read inside the input that exists: at most `lookahead_` ahead of the output position
        const double reach = F > 1.0 ? Rh * (F - 1.0) : Lh * (1.0 - F);
        double dlo = std::max(-0.5 * T, reach - lookahead_);
        // and, when shifting, keep the next grain's period measurable (it looks one period past this read point)
        if (!unity) dlo = std::max(dlo, 1.04 * T + 8.0 + std::max(0.0, O - 1.0) * spacing - lookahead_);
        int guard = 0;
        while (delta_ < dlo && guard++ < 64) delta_ += T;           // repeat a period
        while (delta_ >= dlo + T && guard++ < 64) delta_ -= T;      // skip a period
    }

    Grain g;
    g.centre = s;
    g.start = s - Lh;
    g.end = s + Rh;
    g.invL = 1.0 / Lh;
    g.invR = 1.0 / Rh;
    g.first = !hasPrev_;
    g.rate = F;
    g.src = s - delta_;
    g.gain = O == 1.0 ? 1.0f : (float)(1.0 / std::sqrt(O));
    const bool identNow = delta_ == 0.0 && F == 1.0 && O == 1.0;
    g.ident = identNow && prevIdent_;
    g.fixed = F == 1.0;
    g.intTap = true;
    g.ioff = 0;
    if (g.fixed) {
        const double off = -delta_, fl = std::floor(off), fr = off - fl;
        g.ioff = (int64)fl;
        g.intTap = fr == 0.0;
        if (!g.intTap) Tables::coefs(fr, g.coef);
    }
    if (n_ < kMaxGrains) {
        g_[n_++] = g;
        if (!g.ident) ++nonIdent_;
    }
    hasPrev_ = true;
    prevIdent_ = identNow;
    prevUnity_ = unity;
    prevVoiced_ = c.voiced;
    prevSpacing_ = spacing;
    prevT_ = T;
    lastMark_ = s;
    nextMark_ = s + spacing;
    spawnAt_ = nextMark_ - (identNow ? 1.0 : O) * spacing;
}

void GrainShifter::render(int64 t, float* out) {
    float* const* hist = hist_;
    const int64 mask = mask_;
    const int nch = nch_;
    const double td = (double)t;
    const bool mixed = nonIdent_ > 0;
    const Tables& tb = *tab_;
    float acc[2] = {0.0f, 0.0f};
    for (int i = 0; i < n_;) {
        Grain& g = g_[i];
        if (td >= g.end) {
            if (!g.ident) --nonIdent_;
            g_[i] = g_[--n_];
            continue;
        }
        if (mixed) {
            float w = 1.0f;
            double u = 1.0;
            if (td < g.centre) { if (!g.first) u = (td - g.start) * g.invL; }
            else u = (g.end - td) * g.invR;
            if (u <= 0.0) { ++i; continue; }
            if (u < 1.0) {
                const double x = u * kWin;
                const int k = (int)x;
                const float f = (float)(x - k);
                w = tb.hann[k] + f * (tb.hann[k + 1] - tb.hann[k]);
            }
            w *= g.gain;
            if (g.fixed) {
                const int64 idx = t + g.ioff;
                if (g.intTap) {
                    for (int ch = 0; ch < nch; ++ch) acc[ch] += w * hist[ch][idx & mask];
                } else {
                    for (int ch = 0; ch < nch; ++ch) {
                        const float* h = hist[ch];
                        float sum = 0.0f;
                        for (int k = 0; k < kTaps; ++k) sum += g.coef[k] * h[(idx - 3 + k) & mask];
                        acc[ch] += w * sum;
                    }
                }
            } else {
                const double pos = g.src + (td - g.centre) * g.rate, fl = std::floor(pos);
                const int64 idx = (int64)fl;
                const double ph = (pos - fl) * kPhases;
                const int pi = (int)ph;
                const float pf = (float)(ph - pi);
                const float* c0 = tb.sinc[pi];
                const float* c1 = tb.sinc[pi + 1];
                float cf[kTaps];
                for (int k = 0; k < kTaps; ++k) cf[k] = c0[k] + pf * (c1[k] - c0[k]);
                for (int ch = 0; ch < nch; ++ch) {
                    const float* h = hist[ch];
                    float sum = 0.0f;
                    for (int k = 0; k < kTaps; ++k) sum += cf[k] * h[(idx - 3 + k) & mask];
                    acc[ch] += w * sum;
                }
            }
        }
        ++i;
    }
    if (mixed) for (int ch = 0; ch < nch; ++ch) out[ch] = acc[ch];
    else for (int ch = 0; ch < nch; ++ch) out[ch] = hist[ch][t & mask];
}

} // namespace vocal
} // namespace pulp
