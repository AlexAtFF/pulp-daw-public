// What the creative effects share: a seeded hash and a tape that can be read back at any speed.
#include "pulp/Creative.h"

namespace pulp::crt {

uint32 hash32(uint32 a, uint32 b, uint32 c) {
    uint32 h = a * 0x9E3779B1u + 0x7F4A7C15u;
    h ^= b + 0x85EBCA6Bu + (h << 6) + (h >> 2);
    h *= 0xC2B2AE35u;
    h ^= c + 0x27D4EB2Fu + (h << 6) + (h >> 2);
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

namespace {
constexpr int kZeros = 8;           // zero crossings each side of the kernel at normal speed
constexpr int kPerZero = 512;
constexpr int kTable = kZeros * kPerZero;
constexpr double kMaxStretch = 4.0; // the kernel widens with the speed up to this; faster than that aliases

inline double blackman(double u) { return 0.42 + 0.5 * std::cos(kPi * u) + 0.08 * std::cos(kTwoPi * u); }   // 1 at 0, 0 at +-1
inline double sinc(double x) { return std::fabs(x) < 1.0e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

struct SincTable {
    float v[kTable + 2];
    SincTable() {
        for (int i = 0; i <= kTable + 1; ++i) {
            const double x = (double)i / kPerZero;
            v[i] = x >= kZeros ? 0.0f : (float)(sinc(x) * blackman(x / kZeros));
        }
    }
};
const SincTable& sincTable() { static const SincTable t; return t; }
}

void warmTables() { (void)sincTable(); }

void Tape::prepare(double seconds, double sampleRate) {
    int64 size = 1024;
    while ((double)size < seconds * sampleRate + 128.0) size <<= 1;
    for (auto& b : buf) b.assign((size_t)size, 0.0f);
    mask = size - 1;
    n = 0;
    validFrom = 0;
    warmTables();
}

void Tape::read(double delay, double rate, float& l, float& r) const {
    if (n <= 0) { l = r = 0.0f; return; }
    double D = clampT(delay, 0.0, maxDelay());          // before the first sample there is silence (see at())
    const double whole = std::round(D);
    if (std::fabs(D - whole) < 1.0e-7) D = whole;       // a hair off a sample is that sample
    const double up = std::ceil(D);
    const int64 idx = n - 1 - (int64)up;                // the sample at or just before the read point
    const double frac = up - D;                         // the read point is idx + frac
    const double ar = std::fabs(rate);
    if (frac == 0.0 && ar <= 1.0) { l = at(0, idx); r = at(1, idx); return; }
    if (D <= 1.0) {                                     // between the two newest samples
        const float a0 = at(0, idx), a1 = at(1, idx);
        l = a0 + (at(0, idx + 1) - a0) * (float)frac;
        r = a1 + (at(1, idx + 1) - a1) * (float)frac;
        return;
    }
    const double k = clampT(ar, 1.0, kMaxStretch);
    const double half = kZeros * k;
    double al = 0.0, arr = 0.0, norm = 0.0;
    if (D >= half) {                                    // the whole kernel fits behind the newest sample
        const float* tab = sincTable().v;
        const double scale = kPerZero / k;
        const int jlo = (int)std::ceil(frac - half), jhi = (int)std::floor(frac + half);
        for (int j = jlo; j <= jhi; ++j) {
            const double x = std::fabs((double)j - frac) * scale;
            const int xi = (int)x;
            const double w = tab[xi] + (tab[xi + 1] - tab[xi]) * (x - xi);
            al += w * at(0, idx + j);
            arr += w * at(1, idx + j);
            norm += w;
        }
    } else if (ar <= 1.0) {                             // close to the newest sample at normal speed or slower: four points
        auto herm = [&](int ch) {
            const float y0 = at(ch, idx - 1), y1 = at(ch, idx), y2 = at(ch, idx + 1), y3 = at(ch, idx + 2);
            const float t = (float)frac;
            const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            return ((c3 * t + c2) * t + c1) * t + y1;
        };
        l = herm(0);
        r = herm(1);
        return;
    } else {                                            // close to the newest sample and fast: a shorter kernel that stops there
        const double W = D;
        const int jlo = (int)std::ceil(frac - W), jhi = (int)std::floor(frac + W);
        for (int j = jlo; j <= jhi; ++j) {
            const double d = (double)j - frac;
            const double w = sinc(d / k) * blackman(d / W);
            al += w * at(0, idx + j);
            arr += w * at(1, idx + j);
            norm += w;
        }
    }
    if (norm < 1.0e-9) { l = at(0, idx); r = at(1, idx); return; }
    l = (float)(al / norm);
    r = (float)(arr / norm);
}

} // namespace pulp::crt
