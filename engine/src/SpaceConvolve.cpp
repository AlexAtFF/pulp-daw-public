// Spaces: convolution reverb.
//
// The convolution is partitioned three ways. The first 1024 samples of the impulse response run in
// blocks of 64 (one FFT pair per quantum, no delay at all); samples 1024 to 8192 run in blocks of 512;
// everything after that in blocks of 4096. A long block has twice its own length to get its work done
// before its output is due (its part of the impulse response starts 2 x block late), and that work is cut
// into equal slices, one per quantum, so no quantum carries a spike. Every block is an overlap-save
// frequency-domain delay line: one forward FFT, a multiply-accumulate over the stored input spectra, one
// inverse FFT.
//
// The decay and length trims are envelopes over the impulse response. They are applied at run time as a
// gain per partition; the long partitions also keep the spectrum of (partition x linear ramp), so the
// envelope is piecewise linear and continuous rather than a staircase. When a trim moves, the block is
// computed with the old and the new gains and crossfaded.
#include "pulp/Space.h"
#include "pulp/Stretch.h"

namespace pulp {
namespace space {

namespace {

constexpr int kStages = 3;
constexpr int kBlock[kStages] = {64, 512, 4096};
constexpr int kOffset[kStages] = {0, 1024, 8192};
constexpr float kImpulseEnergy = 0.25f;     // every impulse response is scaled to this energy (-6 dB)

// Complex radix-2 FFT with tables. forward() is unscaled; inverse() is unscaled too (divide by n).
struct Fft {
    int n = 0;
    std::vector<int> rev;
    std::vector<float> tr, ti;      // the pass with half-length h reads its twiddles from index h - 1
    explicit Fft(int size) : n(size) {
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        rev.resize((size_t)n);
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t)i] = r;
        }
        tr.resize((size_t)n);
        ti.resize((size_t)n);
        for (int h = 1; h < n; h <<= 1)
            for (int k = 0; k < h; ++k) {
                const double a = -kPi * (double)k / (double)h;
                tr[(size_t)(h - 1 + k)] = (float)std::cos(a);
                ti[(size_t)(h - 1 + k)] = (float)std::sin(a);
            }
    }
    void forward(float* __restrict re, float* __restrict im) const {
        const int* rv = rev.data();
        for (int i = 0; i < n; ++i) {
            const int j = rv[i];
            if (i < j) { const float a = re[i]; re[i] = re[j]; re[j] = a; const float b = im[i]; im[i] = im[j]; im[j] = b; }
        }
        for (int s = 0; s < n; s += 2) {        // first pass: the twiddle is 1
            const float ar = re[s], br = re[s + 1], ai = im[s], bi = im[s + 1];
            re[s] = ar + br; re[s + 1] = ar - br;
            im[s] = ai + bi; im[s + 1] = ai - bi;
        }
        for (int h = 2; h < n; h <<= 1) {
            const float* __restrict wr = tr.data() + (h - 1);
            const float* __restrict wi = ti.data() + (h - 1);
            for (int s = 0; s < n; s += 2 * h) {
                float* __restrict ar = re + s;
                float* __restrict br = re + s + h;
                float* __restrict ai = im + s;
                float* __restrict bi = im + s + h;
                for (int k = 0; k < h; ++k) {
                    const float xr = br[k] * wr[k] - bi[k] * wi[k];
                    const float xi = br[k] * wi[k] + bi[k] * wr[k];
                    br[k] = ar[k] - xr; bi[k] = ai[k] - xi;
                    ar[k] += xr; ai[k] += xi;
                }
            }
        }
    }
    void inverse(float* re, float* im) const { forward(im, re); }
};

const Fft& fftFor(int stage) {
    static const Fft f0(2 * kBlock[0]), f1(2 * kBlock[1]), f2(2 * kBlock[2]);
    return stage == 0 ? f0 : (stage == 1 ? f1 : f2);
}

// acc += g * (x * h), all complex, split into real and imaginary arrays
inline void mac(float* __restrict ar, float* __restrict ai, const float* __restrict xr, const float* __restrict xi,
                const float* __restrict hr, const float* __restrict hi, float g, int n) {
    for (int i = 0; i < n; ++i) {
        const float pr = xr[i] * hr[i] - xi[i] * hi[i];
        const float pi = xr[i] * hi[i] + xi[i] * hr[i];
        ar[i] += g * pr;
        ai[i] += g * pi;
    }
}

} // namespace

// An impulse response ready to convolve with: immutable, shared between instances.
struct Kernel {
    std::string name;
    int nch = 1, length = 0;
    double rt60 = 1.0;                              // seconds, estimated; the decay trim scales it
    std::vector<float> td[2];                       // the impulse response itself
    int parts[kStages] = {0, 0, 0};
    std::vector<float> re[kStages][2], im[kStages][2];      // spectrum of each partition, bins 0..block
    std::vector<float> rre[kStages][2], rim[kStages][2];    // spectrum of partition x ramp (stages 1 and 2)
};

namespace {

std::shared_ptr<const Kernel> buildKernel(std::vector<float> l, std::vector<float> r, const std::string& name, double sr) {
    auto k = std::make_shared<Kernel>();
    k->name = name;
    k->nch = r.empty() ? 1 : 2;
    k->length = (int)l.size();
    k->td[0] = std::move(l);
    k->td[1] = std::move(r);
    const int len = k->length;
    for (int s = 0; s < kStages; ++s) {
        const int B = kBlock[s], bins = B + 1, off = kOffset[s];
        const int end = s + 1 < kStages ? std::min(len, kOffset[s + 1]) : len;
        const int P = end > off ? (end - off + B - 1) / B : 0;
        k->parts[s] = P;
        if (P == 0) continue;
        const Fft& fft = fftFor(s);
        std::vector<float> wr((size_t)(2 * B)), wi((size_t)(2 * B));
        for (int ch = 0; ch < k->nch; ++ch) {
            const float* h = k->td[ch].data();
            k->re[s][ch].assign((size_t)P * bins, 0.0f);
            k->im[s][ch].assign((size_t)P * bins, 0.0f);
            if (s > 0) { k->rre[s][ch].assign((size_t)P * bins, 0.0f); k->rim[s][ch].assign((size_t)P * bins, 0.0f); }
            for (int p = 0; p < P; ++p) {
                const int from = off + p * B, count = std::max(0, std::min(B, end - from));
                for (int pass = 0; pass < (s > 0 ? 2 : 1); ++pass) {
                    std::fill(wr.begin(), wr.end(), 0.0f);
                    std::fill(wi.begin(), wi.end(), 0.0f);
                    float* w = wr.data();
                    if (pass == 0) for (int i = 0; i < count; ++i) w[i] = h[from + i];
                    else for (int i = 0; i < count; ++i) w[i] = h[from + i] * ((float)i / (float)B);
                    fft.forward(wr.data(), wi.data());
                    float* dr = (pass == 0 ? k->re[s][ch] : k->rre[s][ch]).data() + (size_t)p * bins;
                    float* di = (pass == 0 ? k->im[s][ch] : k->rim[s][ch]).data() + (size_t)p * bins;
                    std::memcpy(dr, wr.data(), sizeof(float) * (size_t)bins);
                    std::memcpy(di, wi.data(), sizeof(float) * (size_t)bins);
                }
            }
        }
    }
    // Decay time from the backward-integrated energy: -5 to -35 dB, doubled.
    const double seconds = (double)len / sr;
    double total = 0.0;
    for (int ch = 0; ch < k->nch; ++ch) for (float v : k->td[ch]) total += (double)v * v;
    double rt = seconds;
    if (total > 0.0 && len > 8) {
        double run = total;
        int t5 = -1, t35 = -1;
        for (int i = 0; i < len; ++i) {
            const double db = 10.0 * std::log10(run / total + 1e-30);
            if (t5 < 0 && db <= -5.0) t5 = i;
            if (db <= -35.0) { t35 = i; break; }
            for (int ch = 0; ch < k->nch; ++ch) { const double v = k->td[ch][(size_t)i]; run -= v * v; }
            if (run < 0.0) run = 0.0;
        }
        if (t5 >= 0 && t35 > t5) rt = 2.0 * (double)(t35 - t5) / sr;
    }
    k->rt60 = clampT(rt, 0.5 * seconds, 2.0 * seconds);
    return k;
}

void normaliseEnergy(std::vector<float>& l, std::vector<float>& r) {
    double e = 0.0;
    for (float v : l) e += (double)v * v;
    if (!r.empty()) { for (float v : r) e += (double)v * v; e *= 0.5; }
    if (e <= 1e-20) return;
    const float g = (float)std::sqrt((double)kImpulseEnergy / e);
    for (float& v : l) v *= g;
    for (float& v : r) v *= g;
}

// ------------------------------------------------------------------ the generated impulse responses

enum Kind { Room, Plate, Spring, Reverse, Gate };

struct IrSpec {
    const char* name;
    int kind;
    float seconds;                  // length
    float rtLow, rtMid, rtHigh;     // decay time below 250 Hz, in the middle, above 4 kHz
    float room[3];                  // width, depth, height in metres for the early reflections (0 = none)
    float early;                    // energy of the early reflections relative to the tail
    float mixMs;                    // how long the echo density takes to build up
    float riseMs;                   // how fast the tail comes in
    float bright;                   // overall high cut, Hz
    float lowCut;                   // Hz
    float corrLow;                  // how alike the two sides are in the lows
    uint32 seed;
};

const IrSpec kSpecs[Spaces::kBuiltIn] = {
    {"Small room",   Room,    0.50f, 0.42f, 0.36f, 0.22f, {3.4f, 4.1f, 2.5f},    1.20f,  14.0f,  3.0f, 12000.0f,  90.0f, 0.70f, 101u},
    {"Medium room",  Room,    0.85f, 0.75f, 0.62f, 0.38f, {5.2f, 6.8f, 2.9f},    0.90f,  25.0f,  6.0f, 11000.0f,  70.0f, 0.60f, 202u},
    {"Large room",   Room,    1.50f, 1.25f, 1.05f, 0.60f, {9.0f, 12.5f, 4.2f},   0.70f,  45.0f, 10.0f, 10000.0f,  60.0f, 0.60f, 303u},
    {"Tiled room",   Room,    1.30f, 0.85f, 1.05f, 0.85f, {2.6f, 3.3f, 2.4f},    1.00f,  10.0f,  2.0f, 16000.0f, 150.0f, 0.50f, 404u},
    {"Chamber hall", Room,    2.20f, 1.90f, 1.60f, 0.90f, {14.0f, 22.0f, 8.0f},  0.50f,  70.0f, 15.0f,  9000.0f,  50.0f, 0.55f, 505u},
    {"Concert hall", Room,    3.00f, 2.60f, 2.30f, 1.20f, {22.0f, 36.0f, 16.0f}, 0.40f, 110.0f, 25.0f,  8000.0f,  40.0f, 0.50f, 606u},
    {"Cathedral",    Room,    6.00f, 6.00f, 5.00f, 2.00f, {30.0f, 70.0f, 28.0f}, 0.25f, 220.0f, 60.0f,  6000.0f,  40.0f, 0.40f, 707u},
    {"Plate",        Plate,   2.40f, 2.10f, 1.90f, 1.25f, {0.0f, 0.0f, 0.0f},    0.00f,   1.0f,  1.5f, 14000.0f, 130.0f, 0.30f, 808u},
    {"Spring",       Spring,  2.60f, 2.20f, 2.20f, 1.00f, {0.0f, 0.0f, 0.0f},    0.00f,   1.0f,  1.0f,  4600.0f,  90.0f, 0.00f, 909u},
    {"Ambience",     Room,    0.28f, 0.20f, 0.18f, 0.13f, {4.5f, 5.5f, 2.7f},    2.00f,  12.0f,  2.0f, 13000.0f, 120.0f, 0.60f, 1010u},
    {"Reversed",     Reverse, 1.10f, 1.80f, 1.65f, 1.30f, {0.0f, 0.0f, 0.0f},    0.00f,   6.0f,  1.0f, 10000.0f, 100.0f, 0.50f, 1111u},
    {"Gated",        Gate,    0.34f, 9.00f, 8.00f, 5.00f, {6.0f, 8.0f, 3.5f},    0.25f,   8.0f,  2.0f, 12000.0f, 110.0f, 0.50f, 1212u},
};

// First to third order reflections of a box-shaped room, heard by a spaced pair of microphones.
// The direct sound is left out (the dry signal is the direct sound), so time zero is its arrival.
void earlyReflections(const IrSpec& sp, double sr, std::vector<float>& L, std::vector<float>& R, int& firstSample) {
    firstSample = 0;
    const double W = sp.room[0], D = sp.room[1], H = sp.room[2];
    if (W <= 0.0) return;
    const double src[3] = {0.31 * W, 0.68 * D, 0.42 * H};
    const double mic[3] = {0.57 * W, 0.27 * D, 0.36 * H};
    const double half = std::min(0.45, 0.12 + 0.012 * W);        // half the distance between the microphones
    const double dims[3] = {W, D, H};
    const double c = 343.0, wall = 0.84;
    const double d0 = std::sqrt((src[0] - mic[0]) * (src[0] - mic[0]) + (src[1] - mic[1]) * (src[1] - mic[1]) + (src[2] - mic[2]) * (src[2] - mic[2]));
    const int len = std::min((int)L.size(), (int)(0.7 * sr));
    std::vector<float> buf[3][2];
    for (auto& o : buf) for (auto& b : o) b.assign((size_t)len, 0.0f);
    dsp::Noise jitter(sp.seed * 31u + 7u);
    int first = len;
    for (int nx = -2; nx <= 2; ++nx) for (int ux = 0; ux < 2; ++ux)
    for (int ny = -2; ny <= 2; ++ny) for (int uy = 0; uy < 2; ++uy)
    for (int nz = -2; nz <= 2; ++nz) for (int uz = 0; uz < 2; ++uz) {
        const int order = std::abs(2 * nx - ux) + std::abs(2 * ny - uy) + std::abs(2 * nz - uz);
        if (order < 1 || order > 3) continue;
        const int nn[3] = {nx, ny, nz}, uu[3] = {ux, uy, uz};
        double img[3];
        for (int a = 0; a < 3; ++a) img[a] = 2.0 * nn[a] * dims[a] + (uu[a] ? -src[a] : src[a]);
        const double rough = 0.8 + 0.4 * (double)jitter.uni();       // walls are not perfect mirrors
        for (int m = 0; m < 2; ++m) {
            const double mx = mic[0] + (m ? half : -half);
            const double dx = img[0] - mx, dy = img[1] - mic[1], dz = img[2] - mic[2];
            const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const double pos = (dist - d0) / c * sr;
            if (pos < 0.0003 * sr || pos >= (double)(len - 2)) continue;
            const double lateral = dx / dist;
            const double g = std::pow(wall, order) * d0 / dist * rough * (1.0 + 0.35 * (m ? lateral : -lateral));
            const int i0 = (int)pos;
            const double f = pos - (double)i0;
            buf[order - 1][m][(size_t)i0] += (float)(g * (1.0 - f));
            buf[order - 1][m][(size_t)i0 + 1] += (float)(g * f);
            first = std::min(first, i0);
        }
    }
    static const double cut[3] = {15000.0, 9000.0, 5500.0};     // each bounce loses some treble
    for (int o = 0; o < 3; ++o)
        for (int m = 0; m < 2; ++m) {
            dsp::OnePole lp;
            lp.setHz(std::min(cut[o], sr * 0.45), sr);
            float* dst = (m ? R : L).data();
            const float* b = buf[o][m].data();
            for (int i = 0; i < len; ++i) dst[i] += lp.lp(b[i]);
        }
    firstSample = first < len ? first : 0;
}

// The late field: noise in three bands, each with its own decay time, sparse at first and dense later.
void lateField(const IrSpec& sp, double sr, int onset, std::vector<float>& L, std::vector<float>& R) {
    const int N = (int)L.size();
    dsp::Noise nc(sp.seed * 2654435761u + 1u), nl(sp.seed * 40503u + 17u), nr(sp.seed * 69069u + 29u), gate(sp.seed * 7919u + 3u);
    dsp::OnePole lowC, low[2], high[2];
    lowC.setHz(250.0, sr);
    for (int ch = 0; ch < 2; ++ch) { low[ch].setHz(250.0, sr); high[ch].setHz(4000.0, sr); }
    const double kL = std::pow(10.0, -3.0 / (sp.rtLow * sr)), kM = std::pow(10.0, -3.0 / (sp.rtMid * sr)), kH = std::pow(10.0, -3.0 / (sp.rtHigh * sr));
    double eL = std::pow(kL, onset), eM = std::pow(kM, onset), eH = std::pow(kH, onset);
    const float cl = sp.corrLow, ci = std::sqrt(1.0f - cl * cl);
    const double riseK = std::exp(-1000.0 / ((double)sp.riseMs * sr));
    double fall = 1.0;                                           // 1 - rise
    float* out[2] = {L.data(), R.data()};
    for (int n = onset; n < N; ++n) {
        const double ms = (double)(n - onset) * 1000.0 / sr;
        const double q = (ms + 1.5) / (double)sp.mixMs;
        const float dens = (float)clampT(q * q, 0.004, 1.0);
        const float amp = std::min(1.0f / std::sqrt(dens), 2.5f);
        const float common = gate.uni() < dens ? nc.next() * amp : 0.0f;
        const float lc = lowC.lp(common);
        const float rise = (float)(1.0 - fall);
        for (int ch = 0; ch < 2; ++ch) {
            const float w = gate.uni() < dens ? (ch ? nr : nl).next() * amp : 0.0f;
            const float lo = low[ch].lp(w), mid = high[ch].lp(w);
            const float v = (cl * lc + ci * lo) * 2.2f * (float)eL + (mid - lo) * (float)eM + (w - mid) * 0.9f * (float)eH;
            out[ch][n] = v * rise;
        }
        eL *= kL; eM *= kM; eH *= kH;
        fall *= riseK;
    }
}

// One spring: a loop of delay, loss and a chain of stretched all-pass sections, which makes every echo
// a chirp that gets longer each time round. Only the initial condition drives it.
void springLine(std::vector<float>& out, double sr, double loopMs, float feedback, int sections, float coef, float level) {
    const int N = (int)out.size();
    const int T = std::max(8, (int)(loopMs * 0.001 * sr));
    const int stretch = std::max(1, (int)std::lround(sr / (2.0 * 4300.0)));      // dispersion sits below about 4.3 kHz
    std::vector<float> loop((size_t)T, 0.0f), ap((size_t)sections * stretch, 0.0f);
    loop[(size_t)(T / 2)] = 1.0f;
    const float a = (float)(1.0 - std::exp(-kTwoPi * 4500.0 / sr));
    float lp1 = 0.0f, lp2 = 0.0f;
    int li = 0, ai = 0;
    float* s = ap.data();
    float* o = out.data();
    float* lb = loop.data();
    for (int n = 0; n < N; ++n) {
        float v = lb[li];
        lp1 += a * (v - lp1);
        lp2 += a * (lp1 - lp2);
        v = lp2;
        for (int m = 0; m < sections; ++m) {
            float& z = s[m * stretch + ai];
            const float y = coef * v + z;
            z = v - coef * y;
            v = y;
        }
        if (++ai == stretch) ai = 0;
        o[n] += v * level;
        lb[li] = v * feedback;
        if (++li == T) li = 0;
    }
}

void fadeEnd(std::vector<float>& x, int samples) {
    const int N = (int)x.size();
    samples = std::min(samples, N);
    for (int i = 0; i < samples; ++i) x[(size_t)(N - 1 - i)] *= (float)(0.5 - 0.5 * std::cos(kPi * (double)i / (double)samples));
}

double energyOf(const std::vector<float>& a, const std::vector<float>& b) {
    double e = 0.0;
    for (float v : a) e += (double)v * v;
    for (float v : b) e += (double)v * v;
    return e;
}

void makeImpulse(int index, double sr, std::vector<float>& L, std::vector<float>& R) {
    const IrSpec& sp = kSpecs[index];
    const int N = std::max(256, (int)(sp.seconds * sr));
    L.assign((size_t)N, 0.0f);
    R.assign((size_t)N, 0.0f);
    if (sp.kind == Spring) {
        springLine(L, sr, 33.1, -0.90f, 64, 0.62f, 1.0f);
        springLine(L, sr, 41.3, -0.89f, 72, 0.60f, 0.8f);
        springLine(R, sr, 36.7, -0.90f, 68, 0.61f, 1.0f);
        springLine(R, sr, 45.9, -0.89f, 76, 0.63f, 0.8f);
        // a little of each tank in the other side, as two transducers on one chassis would give
        for (int i = 0; i < N; ++i) { const float l = L[(size_t)i], r = R[(size_t)i]; L[(size_t)i] = l + 0.25f * r; R[(size_t)i] = r + 0.25f * l; }
    } else {
        std::vector<float> el((size_t)N, 0.0f), er((size_t)N, 0.0f);
        int first = 0;
        if (sp.kind == Room || sp.kind == Gate) earlyReflections(sp, sr, el, er, first);
        lateField(sp, sr, first, L, R);
        if (sp.kind == Plate) {         // a few ringing modes of the sheet
            dsp::Noise m(sp.seed + 99u);
            for (int k = 0; k < 16; ++k) {
                const double hz = 150.0 * std::pow(3200.0 / 150.0, (double)m.uni());
                const double tau = sp.rtMid * (0.5 + 0.7 * m.uni()) / 6.908;
                const double ph = kTwoPi * m.uni();
                const float aL = 0.06f * m.next(), aR = 0.06f * m.next();
                const double dec = std::exp(-1.0 / (tau * sr));
                double e = 1.0;
                for (int i = 0; i < N; ++i) {
                    const float v = (float)(e * std::sin(kTwoPi * hz * (double)i / sr + ph));
                    L[(size_t)i] += aL * v;
                    R[(size_t)i] += aR * v;
                    e *= dec;
                }
            }
        }
        const double eTail = energyOf(L, R), eEarly = energyOf(el, er);
        if (eEarly > 0.0 && eTail > 0.0) {
            const float g = (float)std::sqrt((double)sp.early * eTail / eEarly);
            for (int i = 0; i < N; ++i) { L[(size_t)i] += g * el[(size_t)i]; R[(size_t)i] += g * er[(size_t)i]; }
        }
    }
    // overall tone
    for (int ch = 0; ch < 2; ++ch) {
        dsp::Biquad hi, hi2, lo;
        hi.lowpass(std::min((double)sp.bright, sr * 0.45), 0.6, sr);
        hi2.lowpass(std::min((double)sp.bright, sr * 0.45), 0.8, sr);
        lo.highpass(sp.lowCut, 0.7, sr);
        for (float& v : (ch ? R : L)) {
            v = lo.process(hi.process(v));
            if (sp.kind == Spring) v = hi2.process(v);
        }
    }
    if (sp.kind == Reverse) {
        std::reverse(L.begin(), L.end());
        std::reverse(R.begin(), R.end());
        const int in = (int)(0.03 * sr);
        for (int i = 0; i < in && i < N; ++i) { const float g = (float)i / (float)in; L[(size_t)i] *= g; R[(size_t)i] *= g; }
        fadeEnd(L, (int)(0.003 * sr));
        fadeEnd(R, (int)(0.003 * sr));
    } else if (sp.kind == Gate) {
        fadeEnd(L, (int)(0.015 * sr));
        fadeEnd(R, (int)(0.015 * sr));
    } else {
        fadeEnd(L, (int)(0.02 * sr));
        fadeEnd(R, (int)(0.02 * sr));
    }
    normaliseEnergy(L, R);
}

} // namespace

// The twelve generated impulse responses at one sample rate, built once and shared by every instance.
struct KernelSet {
    double sr = 0.0;
    std::shared_ptr<const Kernel> k[Spaces::kBuiltIn];
    int maxParts[kStages] = {0, 0, 0};
};

namespace {

std::shared_ptr<const KernelSet> builtIns(double sr) {
    static std::mutex m;
    static std::vector<std::shared_ptr<const KernelSet>> cache;        // most recently used first; two rates are kept (playback and export)
    std::lock_guard<std::mutex> g(m);
    for (size_t i = 0; i < cache.size(); ++i)
        if (cache[i]->sr == sr) {
            auto hit = cache[i];
            cache.erase(cache.begin() + (std::ptrdiff_t)i);
            cache.insert(cache.begin(), hit);
            return hit;
        }
    auto set = std::make_shared<KernelSet>();
    set->sr = sr;
    for (int i = 0; i < Spaces::kBuiltIn; ++i) {
        std::vector<float> l, r;
        makeImpulse(i, sr, l, r);
        set->k[i] = buildKernel(std::move(l), std::move(r), kSpecs[i].name, sr);
        for (int s = 0; s < kStages; ++s) set->maxParts[s] = std::max(set->maxParts[s], set->k[i]->parts[s]);
    }
    cache.insert(cache.begin(), set);
    if (cache.size() > 2) cache.pop_back();
    return set;
}

// The envelope the trims put over the impulse response, as a function of the sample index.
struct Env {
    double k = 0.0;                 // decay trim: exp(-k t)
    double cut = 1e30, fade = 1.0;  // length trim: full until cut - fade, nothing from cut on
    inline float at(double t) const {
        double g = k > 0.0 ? std::exp(-k * t) : 1.0;
        if (t > cut - fade) g *= t >= cut ? 0.0 : (cut - t) / fade;
        return g < 1.0e-9 ? 0.0f : (float)g;       // below -180 dB a partition is skipped (and nothing goes denormal)
    }
};

} // namespace

// The moving parts of one convolver. Owned by a Rig; only the audio thread touches it once published.
struct ConvState {
    struct Stage {
        int B = 64, S = 1, cap = 1;
        const Fft* fft = nullptr;
        std::vector<float> in[2];               // the last two blocks of input
        std::vector<float> fr[2], fi[2];        // input spectra, newest at head
        std::vector<float> wr[2], wi[2];        // FFT work
        std::vector<float> vr[2], vi[2];        // FFT work for the previous gains while a trim moves
        std::vector<float> ar[2], ai[2], br[2], bi[2];
        std::vector<float> out[2];
        std::vector<float> gain[2], delta[2];   // per-partition gain and ramp, [cur] and the set before
        int cur = 0, pos = 0, head = 0, filled = 0, readPos = 0, slot = -1;
        bool dual = false, haveGains = false;
    } st[kStages];

    void allocate(const int* caps) {
        for (int s = 0; s < kStages; ++s) {
            Stage& g = st[s];
            g.B = kBlock[s];
            g.S = g.B / kQuantum;
            g.cap = std::max(1, caps[s]);
            g.fft = &fftFor(s);
            const size_t B = (size_t)g.B, bins = B + 1;
            for (int ch = 0; ch < 2; ++ch) {
                g.in[ch].assign(2 * B, 0.0f);
                g.fr[ch].assign((size_t)g.cap * bins, 0.0f);
                g.fi[ch].assign((size_t)g.cap * bins, 0.0f);
                g.wr[ch].assign(2 * B, 0.0f); g.wi[ch].assign(2 * B, 0.0f);
                g.vr[ch].assign(2 * B, 0.0f); g.vi[ch].assign(2 * B, 0.0f);
                g.ar[ch].assign(bins, 0.0f); g.ai[ch].assign(bins, 0.0f);
                g.br[ch].assign(bins, 0.0f); g.bi[ch].assign(bins, 0.0f);
                g.out[ch].assign(B, 0.0f);
            }
            for (int i = 0; i < 2; ++i) { g.gain[i].assign((size_t)g.cap, 1.0f); g.delta[i].assign((size_t)g.cap, 0.0f); }
        }
        reset();
    }

    // Audio thread. The stored spectra are not cleared: `filled` says how many of them count.
    void reset() {
        for (auto& g : st) {
            for (int ch = 0; ch < 2; ++ch) {
                std::fill(g.in[ch].begin(), g.in[ch].end(), 0.0f);
                std::fill(g.out[ch].begin(), g.out[ch].end(), 0.0f);
            }
            g.pos = g.head = g.filled = g.readPos = 0;
            g.slot = -1;
            g.dual = g.haveGains = false;
        }
    }

    void setGains(Stage& g, int s, const Kernel& k, const Env& env) {
        const int P = std::min(k.parts[s], g.cap), nxt = g.cur ^ 1;
        float* ga = g.gain[nxt].data();
        float* gd = g.delta[nxt].data();
        const double B = g.B, off = kOffset[s];
        if (s == 0) {
            for (int p = 0; p < P; ++p) { ga[p] = env.at(off + (p + 0.5) * B); gd[p] = 0.0f; }
        } else {
            float a = env.at(off);
            for (int p = 0; p < P; ++p) {
                const float b = env.at(off + (p + 1) * B);
                ga[p] = a;
                gd[p] = b - a;
                a = b;
            }
        }
        bool changed = false;
        if (g.haveGains) {
            const float* oa = g.gain[g.cur].data();
            const float* od = g.delta[g.cur].data();
            for (int p = 0; p < P && !changed; ++p) changed = oa[p] != ga[p] || od[p] != gd[p];
        }
        g.dual = changed;
        g.cur = nxt;
        g.haveGains = true;
    }

    void forward(Stage& g, int ch) {
        g.fft->forward(g.wr[ch].data(), g.wi[ch].data());
        const size_t bins = (size_t)g.B + 1;
        std::memcpy(g.fr[ch].data() + (size_t)g.head * bins, g.wr[ch].data(), sizeof(float) * bins);
        std::memcpy(g.fi[ch].data() + (size_t)g.head * bins, g.wi[ch].data(), sizeof(float) * bins);
    }

    void clearAcc(Stage& g) {
        for (int ch = 0; ch < 2; ++ch) {
            std::fill(g.ar[ch].begin(), g.ar[ch].end(), 0.0f); std::fill(g.ai[ch].begin(), g.ai[ch].end(), 0.0f);
            if (g.dual) { std::fill(g.br[ch].begin(), g.br[ch].end(), 0.0f); std::fill(g.bi[ch].begin(), g.bi[ch].end(), 0.0f); }
        }
    }

    void accumulate(Stage& g, int s, const Kernel& k, int ch, int p0, int p1) {
        const int bins = g.B + 1, kc = ch < k.nch ? ch : 0;
        p1 = std::min(p1, std::min(std::min(k.parts[s], g.cap), g.filled));
        const float* ga = g.gain[g.cur].data();
        const float* gd = g.delta[g.cur].data();
        const float* oa = g.gain[g.cur ^ 1].data();
        const float* od = g.delta[g.cur ^ 1].data();
        const float* hr = k.re[s][kc].data();
        const float* hi = k.im[s][kc].data();
        const float* rr = s > 0 ? k.rre[s][kc].data() : nullptr;
        const float* ri = s > 0 ? k.rim[s][kc].data() : nullptr;
        for (int p = p0; p < p1; ++p) {
            int idx = g.head - p;
            if (idx < 0) idx += g.cap;
            const float* xr = g.fr[ch].data() + (size_t)idx * bins;
            const float* xi = g.fi[ch].data() + (size_t)idx * bins;
            const size_t o = (size_t)p * bins;
            if (ga[p] != 0.0f) mac(g.ar[ch].data(), g.ai[ch].data(), xr, xi, hr + o, hi + o, ga[p], bins);
            if (gd[p] != 0.0f) mac(g.ar[ch].data(), g.ai[ch].data(), xr, xi, rr + o, ri + o, gd[p], bins);
            if (g.dual) {
                if (oa[p] != 0.0f) mac(g.br[ch].data(), g.bi[ch].data(), xr, xi, hr + o, hi + o, oa[p], bins);
                if (od[p] != 0.0f) mac(g.br[ch].data(), g.bi[ch].data(), xr, xi, rr + o, ri + o, od[p], bins);
            }
        }
    }

    static void spectrumToTime(const Fft& fft, int B, const float* ar, const float* ai, float* wr, float* wi) {
        std::memcpy(wr, ar, sizeof(float) * (size_t)(B + 1));
        std::memcpy(wi, ai, sizeof(float) * (size_t)(B + 1));
        for (int i = 1; i < B; ++i) { wr[2 * B - i] = ar[i]; wi[2 * B - i] = -ai[i]; }
        fft.inverse(wr, wi);
    }

    void inverse(Stage& g, int ch) {
        spectrumToTime(*g.fft, g.B, g.ar[ch].data(), g.ai[ch].data(), g.wr[ch].data(), g.wi[ch].data());
        if (g.dual) spectrumToTime(*g.fft, g.B, g.br[ch].data(), g.bi[ch].data(), g.vr[ch].data(), g.vi[ch].data());
    }

    // The second half of the inverse transform is the block's output.
    void emit(Stage& g, int ch, float* dst, bool add) {
        const int B = g.B;
        const float scale = 1.0f / (float)(2 * B);
        const float* y = g.wr[ch].data() + B;
        if (g.dual) {
            const float* old = g.vr[ch].data() + B;
            const float step = 1.0f / (float)B;
            for (int i = 0; i < B; ++i) {
                const float t = (float)(i + 1) * step;
                const float v = (old[i] + (y[i] - old[i]) * t) * scale;
                dst[i] = add ? dst[i] + v : v;
            }
        } else if (add) {
            for (int i = 0; i < B; ++i) dst[i] += y[i] * scale;
        } else {
            for (int i = 0; i < B; ++i) dst[i] = y[i] * scale;
        }
    }

    // One slice of a long block's work.
    void runSlot(Stage& g, int s, const Kernel& k, const Env& env) {
        const int S = g.S, F = S >= 32 ? 2 : 1, M = S - 2 * F, P = k.parts[s], j = g.slot;
        if (j < F) {
            if (j == 0) { g.head = g.head + 1 == g.cap ? 0 : g.head + 1; g.filled = std::min(g.filled + 1, g.cap); }
            if (F == 2) forward(g, j); else { forward(g, 0); forward(g, 1); }
            if (j == F - 1) { setGains(g, s, k, env); clearAcc(g); }
        } else if (j < F + M) {
            const int m = j - F, p0 = (int)((int64)P * m / M), p1 = (int)((int64)P * (m + 1) / M);
            accumulate(g, s, k, 0, p0, p1);
            accumulate(g, s, k, 1, p0, p1);
        } else {
            if (F == 2) inverse(g, j - F - M); else { inverse(g, 0); inverse(g, 1); }
            if (j == S - 1) { emit(g, 0, g.out[0].data(), false); emit(g, 1, g.out[1].data(), false); }
        }
        g.slot = j == S - 1 ? -1 : j + 1;
    }

    // One quantum: 64 samples in, 64 samples of x * h out.
    void run(const Kernel& k, const Env& env, const float* const* in, float* const* out) {
        const size_t qb = sizeof(float) * kQuantum;
        std::memset(out[0], 0, qb);
        std::memset(out[1], 0, qb);
        if (k.parts[0] > 0) {       // the head of the impulse response: computed here and now
            Stage& g = st[0];
            g.head = g.head + 1 == g.cap ? 0 : g.head + 1;
            g.filled = std::min(g.filled + 1, g.cap);
            for (int ch = 0; ch < 2; ++ch) {
                std::memcpy(g.in[ch].data() + kQuantum, in[ch], qb);
                std::memcpy(g.wr[ch].data(), g.in[ch].data(), 2 * qb);
                std::memset(g.wi[ch].data(), 0, 2 * qb);
                forward(g, ch);
                std::memcpy(g.in[ch].data(), in[ch], qb);
            }
            setGains(g, 0, k, env);
            clearAcc(g);
            for (int ch = 0; ch < 2; ++ch) {
                accumulate(g, 0, k, ch, 0, k.parts[0]);
                inverse(g, ch);
                emit(g, ch, out[ch], true);
            }
        }
        for (int s = 1; s < kStages; ++s) {
            if (k.parts[s] == 0) continue;
            Stage& g = st[s];
            const size_t B = (size_t)g.B;
            for (int ch = 0; ch < 2; ++ch) {
                const float* y = g.out[ch].data() + g.readPos;
                float* o = out[ch];
                for (int i = 0; i < kQuantum; ++i) o[i] += y[i];
            }
            g.readPos += kQuantum;
            if (g.readPos == g.B) g.readPos = 0;
            if (g.slot >= 0) runSlot(g, s, k, env);
            for (int ch = 0; ch < 2; ++ch) std::memcpy(g.in[ch].data() + B + (size_t)g.pos, in[ch], qb);
            g.pos += kQuantum;
            if (g.pos == g.B) {     // a block is complete: its work starts in the next quantum
                for (int ch = 0; ch < 2; ++ch) {
                    std::memcpy(g.wr[ch].data(), g.in[ch].data(), sizeof(float) * 2 * B);
                    std::memset(g.wi[ch].data(), 0, sizeof(float) * 2 * B);
                    std::memcpy(g.in[ch].data(), g.in[ch].data() + B, sizeof(float) * B);
                }
                g.pos = 0;
                g.slot = 0;
            }
        }
    }
};

// What the audio thread is handed: the impulse responses and the state to run them with.
struct Rig {
    std::shared_ptr<const KernelSet> set;
    std::shared_ptr<const Kernel> user;
    std::unique_ptr<ConvState> state;
    const Kernel* kernel(int index) const {
        if (index >= 0 && index < Spaces::kBuiltIn) return set ? set->k[index].get() : nullptr;
        return user.get();
    }
};

} // namespace space

using space::Kernel;
using space::Rig;

// ------------------------------------------------------------------ Spaces

namespace { constexpr int kFadeQuanta = 4; }

Spaces::Spaces() {
    std::vector<std::string> names;
    for (int i = 0; i <= kBuiltIn; ++i) names.push_back(impulseName(i));
    pImpulse = addChoice("impulse", "Impulse", names, 5);
    pPre = addFloat("predelay", "Pre-delay", 0, 250, 0, "ms");
    pDecay = addFloat("decay", "Decay trim", 0.1f, 1, 1, "%");
    pLength = addFloat("length", "Length", 0.05f, 1, 1, "%");
    pLowCut = addFloat("lowcut", "Low cut", 20, 2000, 20, "Hz", 3.0f);
    pHighCut = addFloat("highcut", "High cut", 1000, 20000, 20000, "Hz", 3.0f);
    pWidth = addFloat("width", "Stereo width", 0, 2, 1, "%");
    pMix = addFloat("mix", "Dry / wet", 0, 1, 0.25f, "%");
}

Spaces::~Spaces() = default;

const char* Spaces::impulseName(int index) {
    return index >= 0 && index < kBuiltIn ? space::kSpecs[index].name : "User file";
}

void Spaces::rebuildUserLocked() {
    userKernel_.reset();
    userDirty_ = false;
    if (!userSource_ || userSource_->frames() < 1 || userSource_->sampleRate <= 0.0) return;
    const SampleData& src = *userSource_;
    const int64 maxIn = (int64)(kMaxImpulseSeconds * src.sampleRate);
    const bool truncated = src.frames() > maxIn;
    const size_t take = (size_t)std::min(src.frames(), maxIn);
    const int chans = src.channels > 1 ? 2 : 1;
    std::vector<float> l, r;
    float gain = 1.0f;
    if (std::fabs(src.sampleRate - sr_) > 1e-6) {
        SampleData cutIn;
        cutIn.sampleRate = src.sampleRate;
        cutIn.channels = chans;
        for (int c = 0; c < chans; ++c) cutIn.ch[c].assign(src.ch[c].begin(), src.ch[c].begin() + (std::ptrdiff_t)take);
        auto rs = stretch::resample(cutIn, sr_ / src.sampleRate);
        l = std::move(rs->ch[0]);
        if (chans > 1) r = std::move(rs->ch[1]);
        gain = (float)(src.sampleRate / sr_);       // the same response sampled more finely has more, smaller samples
    } else {
        l.assign(src.ch[0].begin(), src.ch[0].begin() + (std::ptrdiff_t)take);
        if (chans > 1) r.assign(src.ch[1].begin(), src.ch[1].begin() + (std::ptrdiff_t)take);
    }
    const size_t maxOut = (size_t)(kMaxImpulseSeconds * sr_);
    if (l.size() > maxOut) l.resize(maxOut);
    if (r.size() > maxOut) r.resize(maxOut);
    if (truncated) { space::fadeEnd(l, (int)(0.02 * sr_)); if (!r.empty()) space::fadeEnd(r, (int)(0.02 * sr_)); }
    if (userNormalize_) space::normaliseEnergy(l, r);
    else if (gain != 1.0f) { for (float& v : l) v *= gain; for (float& v : r) v *= gain; }
    userKernel_ = space::buildKernel(std::move(l), std::move(r), "User file", sr_);
}

void Spaces::publishLocked() {
    if (!prepared_) return;
    auto rig = std::make_shared<Rig>();
    rig->set = space::builtIns(sr_);
    rig->user = userKernel_;
    int caps[space::kStages];
    for (int s = 0; s < space::kStages; ++s) caps[s] = std::max(rig->set->maxParts[s], userKernel_ ? userKernel_->parts[s] : 0);
    rig->state = std::make_unique<space::ConvState>();
    rig->state->allocate(caps);
    rig_.set(rig);
    gen_.fetch_add(1, std::memory_order_release);
}

void Spaces::prepare(double sr) {
    {
        std::lock_guard<std::mutex> g(ctl_);
        if (sr != sr_ || !prepared_) userDirty_ = true;         // the user's impulse has to be resampled for this rate
        sr_ = sr;
        prepared_ = true;
        if (userDirty_) rebuildUserLocked();
        publishLocked();
    }
    for (auto& d : pre_) d.prepare((int)(0.26 * sr) + 64);
    rt_ = nullptr;
    kernel_ = nullptr;
    active_ = -1;
    reset();
}

void Spaces::reset() {
    if (rt_ && rt_->state) rt_->state->reset();
    for (auto& d : pre_) d.clear();
    for (int c = 0; c < 2; ++c) { lc_[c].reset(); hc_[c].reset(); }
    lcSeen_ = hcSeen_ = -1.0f;
    primed_ = false;
    idle_ = true;
    zeroRun_ = (int64)1 << 40;
    fadeOut_ = 0;
    fade_.snap(1.0f);
}

bool Spaces::setImpulse(SamplePtr data, bool normalize, std::string* error) {
    if (data && (data->frames() < 1 || data->sampleRate <= 0.0)) {
        if (error) *error = "the impulse response is empty";
        return false;
    }
    std::lock_guard<std::mutex> g(ctl_);
    userSource_ = std::move(data);
    userNormalize_ = normalize;
    userPath_ = userSource_ ? userSource_->path : std::string();
    userDirty_ = true;
    if (prepared_) { rebuildUserLocked(); publishLocked(); }
    return true;
}

bool Spaces::loadImpulse(const std::string& path, std::string* error) {
    auto s = readAudioFile(path, error);
    if (!s) return false;
    if (s->frames() < 1) { if (error) *error = "the file holds no audio"; return false; }
    s->path = path;
    if (!setImpulse(s, true, error)) return false;
    pImpulse->set((float)kBuiltIn);
    return true;
}

std::string Spaces::impulsePath() const {
    std::lock_guard<std::mutex> g(ctl_);
    return userPath_;
}

SamplePtr Spaces::impulseData(int index) const {
    if (index < 0) index = pImpulse->getInt();
    std::shared_ptr<const Kernel> k;
    double sr;
    {
        std::lock_guard<std::mutex> g(ctl_);
        if (!prepared_) return nullptr;
        sr = sr_;
        if (index >= kBuiltIn) k = userKernel_;
    }
    if (index < kBuiltIn) k = space::builtIns(sr)->k[index];
    if (!k) return nullptr;
    auto out = std::make_shared<SampleData>();
    out->sampleRate = sr;
    out->channels = k->nch;
    out->ch[0] = k->td[0];
    if (k->nch > 1) out->ch[1] = k->td[1];
    return out;
}

Json Spaces::saveExtra() const {
    std::lock_guard<std::mutex> g(ctl_);
    if (userPath_.empty()) return Json();
    Json j = Json::object();
    j.set("ir", userPath_);
    return j;
}

void Spaces::loadExtra(const Json& j, const std::string& projectDir) {
    const std::string path = j["ir"].str("");
    std::shared_ptr<SampleData> s;
    if (!path.empty()) {
        s = readAudioFile(SampleCache::global().resolve(path, projectDir));
        if (s) s->path = path;
    }
    std::lock_guard<std::mutex> g(ctl_);
    if (path.empty() && !userSource_ && userPath_.empty()) return;      // nothing to change
    userSource_ = std::move(s);
    userPath_ = path;               // kept even when the file is missing, so saving again does not lose it
    userNormalize_ = true;
    userDirty_ = true;
    if (prepared_) { rebuildUserLocked(); publishLocked(); }
}

double Spaces::tailSeconds() const {
    auto rig = rig_.get();
    const Kernel* k = rig ? rig->kernel(pImpulse->getInt()) : nullptr;
    double len;
    {
        std::lock_guard<std::mutex> g(ctl_);
        len = k ? (double)k->length / sr_ : 0.0;
    }
    // the length trim lets the partition it cuts into finish its fade: up to 4096 samples more
    return pPre->get() * 0.001 + len * pLength->get() + 0.15;
}

void Spaces::process(ProcessContext& c) {
    const int n = c.numSamples;
    float* L = c.io[0];
    float* R = c.io[1];

    // ---- which impulse response: a change fades the reverb out, swaps, and fades it back in ----
    const int want = clampT(pImpulse->getInt(), 0, (int)kBuiltIn);
    const uint32 gen = gen_.load(std::memory_order_acquire);
    bool swapNow = false, fadeIn = false;
    if (!rt_) swapNow = true;
    else if (fadeOut_ > 0) { if (--fadeOut_ == 0) { swapNow = true; fadeIn = true; } }
    else if (gen != seenGen_ || want != active_) {
        if (idle_ || !kernel_) swapNow = true;
        else { fadeOut_ = kFadeQuanta; fade_.target(0.0f, kFadeQuanta * n); }
    }
    if (swapNow) {
        if (!rt_ || gen != seenGen_) { seenGen_ = gen; rt_ = rig_.rtAcquire(); }
        active_ = want;
        kernel_ = rt_ ? rt_->kernel(want) : nullptr;
        if (rt_ && rt_->state) rt_->state->reset();
        for (int ch = 0; ch < 2; ++ch) { lc_[ch].reset(); hc_[ch].reset(); }
        if (fadeIn) { fade_.snap(0.0f); fade_.target(1.0f, kFadeQuanta * n); }
        else fade_.snap(1.0f);
    }

    const float mixT = pMix->get(), widthT = pWidth->get() - 1.0f;
    const int preT = (int)std::lround(clampT(pPre->get() * 0.001 * sr_, 0.0, (double)(pre_[0].mask - 8)));
    if (!primed_) { mix_.snap(mixT); width_.snap(widthT); preA_ = preT; preFade_ = 0; primed_ = true; }
    mix_.target(mixT, n);
    width_.target(widthT, n);

    // ---- silence: once the input has been zero for longer than everything can ring, do nothing ----
    bool silent = true;
    for (int i = 0; i < n; ++i) if (std::fabs(L[i]) > 1.0e-18f || std::fabs(R[i]) > 1.0e-18f) { silent = false; break; }
    zeroRun_ = silent ? std::min<int64>(zeroRun_ + n, (int64)1 << 40) : 0;
    if (!silent) idle_ = false;
    else if (!idle_) {
        const int64 ring = (int64)pre_[0].mask + 1 + (kernel_ ? kernel_->length : 0) + 3 * 4096 + 1024;
        if (zeroRun_ >= ring) {
            idle_ = true;
            if (rt_ && rt_->state) rt_->state->reset();
            for (int ch = 0; ch < 2; ++ch) { lc_[ch].reset(); hc_[ch].reset(); }
        }
    }
    if (idle_) {            // input and reverb are both silent: the output is the (silent) input
        mix_.snap(mixT);
        width_.snap(widthT);
        preA_ = preT;
        preFade_ = 0;
        if (fadeOut_ == 0) fade_.snap(1.0f);
        return;
    }

    // ---- pre-delay ----
    float x[2][kQuantum], w[2][kQuantum];
    const int preLen = std::max(8, (int)(0.02 * sr_));
    if (preFade_ == 0 && preA_ != preT) { preB_ = preT; preFade_ = preLen; }
    for (int i = 0; i < n; ++i) {
        pre_[0].push(silent ? 0.0f : L[i]);
        pre_[1].push(silent ? 0.0f : R[i]);
        x[0][i] = pre_[0].tap(preA_);
        x[1][i] = pre_[1].tap(preA_);
        if (preFade_ > 0) {     // crossfade to the new time
            const float t = (float)(preLen - preFade_ + 1) / (float)preLen;
            x[0][i] += (pre_[0].tap(preB_) - x[0][i]) * t;
            x[1][i] += (pre_[1].tap(preB_) - x[1][i]) * t;
            if (--preFade_ == 0) preA_ = preB_;
        }
    }

    // ---- convolution ----
    if (kernel_ && rt_ && rt_->state && n == kQuantum) {
        space::Env env;
        const float decay = pDecay->get(), length = pLength->get();
        if (decay < 0.999f) env.k = 6.9078 * (1.0 / decay - 1.0) / (kernel_->rt60 * sr_);
        if (length < 0.999f) { env.cut = (double)length * kernel_->length; env.fade = std::max(64.0, 0.1 * env.cut); }
        const float* in[2] = {x[0], x[1]};
        float* out[2] = {w[0], w[1]};
        rt_->state->run(*kernel_, env, in, out);
    } else {
        std::memset(w, 0, sizeof w);
    }

    // ---- tone, width, mix ----
    const float lcHz = pLowCut->get(), hcHz = pHighCut->get();
    const bool lcOn = lcHz > 20.5f, hcOn = hcHz < 19990.0f;
    if (lcHz != lcSeen_) { lcSeen_ = lcHz; for (auto& b : lc_) b.highpass(lcHz, 0.7071, sr_); }
    if (hcHz != hcSeen_) { hcSeen_ = hcHz; for (auto& b : hc_) b.lowpass(hcHz, 0.7071, sr_); }
    if (!lcOn) for (auto& b : lc_) b.reset();
    if (!hcOn) for (auto& b : hc_) b.reset();
    for (int i = 0; i < n; ++i) {
        float wl = w[0][i], wr = w[1][i];
        if (lcOn) { wl = lc_[0].process(wl); wr = lc_[1].process(wr); }
        if (hcOn) { wl = hc_[0].process(wl); wr = hc_[1].process(wr); }
        const float side = 0.5f * (wl - wr) * width_.next();        // width 1 adds nothing, 0 leaves the middle
        wl += side;
        wr -= side;
        const float f = fade_.next();
        float dg, wg;
        space::mixGains(mix_.next(), dg, wg);
        L[i] = L[i] * dg + wl * f * wg;
        R[i] = R[i] * dg + wr * f * wg;
    }
    for (int ch = 0; ch < 2; ++ch) { space::settle(lc_[ch]); space::settle(hc_[ch]); }
}

std::vector<Preset> Spaces::factoryPresets() const {
    return buildPresets(*this, {
        {"Concert hall", {}},
        {"Small room", {{"impulse", 0}, {"mix", 0.2f}}},
        {"Drum room", {{"impulse", 1}, {"lowcut", 120}, {"mix", 0.22f}}},
        {"Live room, wide", {{"impulse", 2}, {"width", 1.4f}, {"mix", 0.25f}}},
        {"Tiled bathroom", {{"impulse", 3}, {"highcut", 12000}, {"mix", 0.2f}}},
        {"Chamber strings", {{"impulse", 4}, {"predelay", 18}, {"lowcut", 150}, {"mix", 0.3f}}},
        {"Short hall", {{"impulse", 5}, {"decay", 0.55f}, {"predelay", 12}, {"mix", 0.25f}}},
        {"Cathedral wash", {{"impulse", 6}, {"predelay", 40}, {"lowcut", 200}, {"mix", 0.35f}}},
        {"Vocal plate", {{"impulse", 7}, {"predelay", 30}, {"lowcut", 250}, {"highcut", 11000}, {"mix", 0.22f}}},
        {"Spring twang", {{"impulse", 8}, {"lowcut", 180}, {"mix", 0.3f}}},
        {"Tight ambience", {{"impulse", 9}, {"mix", 0.3f}}},
        {"Reverse swell", {{"impulse", 10}, {"mix", 0.4f}}},
        {"Gated snare", {{"impulse", 11}, {"lowcut", 150}, {"mix", 0.4f}}},
        {"Dark hall send", {{"impulse", 5}, {"highcut", 4500}, {"lowcut", 120}, {"predelay", 25}, {"mix", 1.0f}}},
        {"Cut-off cathedral", {{"impulse", 6}, {"length", 0.25f}, {"mix", 0.3f}}},
    });
}

std::vector<ParamGroup> Spaces::paramGroups() const {
    return {{"Impulse", {"impulse", "predelay", "decay", "length"}},
            {"Tone", {"lowcut", "highcut", "width"}},
            {"Output", {"mix"}}};
}

} // namespace pulp
