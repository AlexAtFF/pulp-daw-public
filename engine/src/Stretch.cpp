#include "pulp/Stretch.h"
#include "pulp/Dsp.h"

namespace pulp::stretch {

const char* modeName(int mode) {
    static const char* names[NumModes] = {"Resample (pitch follows speed)", "Stretch: rhythmic (drums, loops)", "Stretch: tonal (vocals, chords)"};
    return names[clampT(mode, 0, (int)NumModes - 1)];
}

namespace {

std::shared_ptr<SampleData> like(const SampleData& in, int64 frames) {
    auto out = std::make_shared<SampleData>();
    out->sampleRate = in.sampleRate;
    out->channels = in.channels;
    for (int c = 0; c < in.channels; ++c) out->ch[c].assign((size_t)std::max<int64>(0, frames), 0.0f);
    return out;
}

double bessel0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; if (term < 1e-12 * sum) break; }
    return sum;
}

// Windowed sinc, tabulated over [0, kZero] zero crossings.
struct SincTable {
    static constexpr int kZero = 16, kPer = 512;
    std::vector<float> t;
    SincTable() {
        t.resize((size_t)kZero * kPer + 2);
        const double beta = 9.0, i0 = bessel0(beta);
        for (size_t i = 0; i < t.size(); ++i) {
            const double x = (double)i / kPer;
            const double s = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double u = std::min(1.0, x / kZero);
            t[i] = (float)(s * bessel0(beta * std::sqrt(1.0 - u * u)) / i0);
        }
    }
    inline float at(double x) const {       // x in zero crossings, >= 0
        const double p = x * kPer;
        const size_t i = (size_t)p;
        if (i + 1 >= t.size()) return 0.0f;
        const float f = (float)(p - (double)i);
        return t[i] + (t[i + 1] - t[i]) * f;
    }
};
const SincTable& sincTable() { static const SincTable s; return s; }

std::vector<float> hann(int n) {
    std::vector<float> w((size_t)n);
    for (int i = 0; i < n; ++i) w[(size_t)i] = (float)(0.5 - 0.5 * std::cos(kTwoPi * i / n));
    return w;
}

} // namespace

std::shared_ptr<SampleData> resample(const SampleData& in, double ratio) {
    ratio = clampT(ratio, 1.0 / 64.0, 64.0);
    const int64 n = in.frames();
    const int64 outN = std::max<int64>(n > 0 ? 1 : 0, (int64)std::llround((double)n * ratio));
    auto out = like(in, outN);
    if (n == 0) return out;
    if (std::fabs(ratio - 1.0) < 1.0e-12) { for (int c = 0; c < in.channels; ++c) out->ch[c] = in.ch[c]; return out; }
    const auto& tab = sincTable();
    const double step = 1.0 / ratio;                            // input frames per output frame
    const double fc = std::min(1.0, ratio) * 0.96;              // reading faster than recorded needs a lower cutoff
    const double half = SincTable::kZero / fc;
    for (int c = 0; c < in.channels; ++c) {
        const float* src = in.ch[c].data();
        float* dst = out->ch[c].data();
        for (int64 j = 0; j < outN; ++j) {
            const double x = (double)j * step;
            const int64 k0 = std::max<int64>(0, (int64)std::ceil(x - half)), k1 = std::min<int64>(n - 1, (int64)std::floor(x + half));
            double sum = 0.0, wsum = 0.0;
            for (int64 k = k0; k <= k1; ++k) {
                const double w = tab.at(std::fabs(x - (double)k) * fc);
                sum += w * src[k];
                wsum += w;
            }
            // dividing by the weight sum keeps the level exact, also at the two ends of the file
            const int64 full0 = (int64)std::ceil(x - half), full1 = (int64)std::floor(x + half);
            if (full0 < 0 || full1 > n - 1) {       // near an edge: use the weight a whole kernel would have
                double all = 0.0;
                for (int64 k = full0; k <= full1; ++k) all += tab.at(std::fabs(x - (double)k) * fc);
                wsum = all;
            }
            dst[j] = wsum != 0.0 ? (float)(sum / wsum) : 0.0f;
        }
    }
    return out;
}

// Where the drum hits are: places where the level jumps well above what came just before.
static std::vector<int64> findHits(const std::vector<float>& mono, double sr) {
    std::vector<int64> hits;
    const int64 n = (int64)mono.size();
    const int B = std::max(16, (int)(sr * 0.0015));
    const int64 nb = n / B;
    if (nb < 2) return hits;
    std::vector<float> e((size_t)nb);
    float top = 0.0f;
    for (int64 b = 0; b < nb; ++b) {
        double sum = 0.0;
        for (int i = 0; i < B; ++i) { const double v = mono[(size_t)(b * B + i)]; sum += v * v; }
        e[(size_t)b] = (float)std::sqrt(sum / B);
        top = std::max(top, e[(size_t)b]);
    }
    if (top <= 0.0f) return hits;
    int64 last = -(int64)sr;
    for (int64 b = 0; b < nb; ++b) {
        double before = 0.0;
        const int64 from = std::max<int64>(0, b - 12);
        for (int64 k = from; k < b; ++k) before += e[(size_t)k];
        if (b > from) before /= (double)(b - from);
        if (e[(size_t)b] <= 4.0 * before + 0.02 * top) continue;
        // the first sample of the block that is clearly part of the hit
        float pk = 0.0f;
        for (int i = 0; i < B; ++i) pk = std::max(pk, std::fabs(mono[(size_t)(b * B + i)]));
        int64 pos = b * B;
        for (int i = 0; i < B; ++i) if (std::fabs(mono[(size_t)(b * B + i)]) > 0.3f * pk) { pos = b * B + i; break; }
        if ((double)(pos - last) < sr * 0.045) continue;
        hits.push_back(pos);
        last = pos;
    }
    return hits;
}

// Time-domain stretch for drums and loops. Every hit, with a few tens of milliseconds after it,
// is copied across untouched at its new place; the time in between is made longer or shorter by
// waveform-similarity overlap-add: frames of about 20 ms laid down at a fixed spacing, each taken
// from near where the stretch says it should come from, at the offset that lines up best with
// what is already written.
std::shared_ptr<SampleData> rhythmic(const SampleData& in, double ratio) {
    ratio = clampT(ratio, 1.0 / 16.0, 16.0);
    const int64 n = in.frames();
    const int64 outN = std::max<int64>(n > 0 ? 1 : 0, (int64)std::llround((double)n * ratio));
    auto out = like(in, outN);
    if (n == 0) return out;
    if (std::fabs(ratio - 1.0) < 1.0e-12) { for (int c = 0; c < in.channels; ++c) out->ch[c] = in.ch[c]; return out; }
    const int hop = std::max(32, (int)(in.sampleRate * 0.010) & ~1);        // synthesis hop; frames are two hops long
    const int tol = std::max(16, (int)(in.sampleRate * 0.008));             // search range each way
    const int win = hop * 2;
    const auto w = hann(win);
    std::vector<float> mono((size_t)n);
    for (int64 i = 0; i < n; ++i) mono[(size_t)i] = in.channels > 1 ? 0.5f * (in.ch[0][(size_t)i] + in.ch[1][(size_t)i]) : in.ch[0][(size_t)i];
    auto at = [&](int64 i) { return i >= 0 && i < n ? mono[(size_t)i] : 0.0f; };

    // The time map from output to input: straight lines between knots. A locked stretch has slope one.
    struct Knot { double o, i; bool locked; };
    std::vector<Knot> knots;
    knots.push_back({0.0, 0.0, false});
    const auto hits = findHits(mono, in.sampleRate);
    const double post = in.sampleRate * 0.030;
    for (size_t h = 0; h < hits.size(); ++h) {
        const double t = (double)hits[h], o = t * ratio;
        const double nextT = h + 1 < hits.size() ? (double)hits[h + 1] : (double)n;
        const double after = std::min({post, (nextT - t) * ratio - hop * 1.5, nextT - t - hop, (double)outN - o, (double)n - t});
        if (after < hop * 0.5) continue;                 // hits too close together to keep apart
        const double o0 = std::max(o - hop, knots.back().o), i0 = t - (o - o0);
        if (i0 < knots.back().i) continue;
        if (o0 <= knots.back().o && i0 <= knots.back().i) knots.back().locked = true;       // starts where the last knot is
        else knots.push_back({o0, i0, true});
        knots.push_back({o + after, t + after, false});
    }
    if ((double)outN > knots.back().o + 0.5) knots.push_back({(double)outN, (double)n, false});
    size_t seg = 0;

    int64 prev = 0;         // input position of the previous frame's centre
    for (int64 m = 0; m * hop < outN + hop; ++m) {
        const double o = (double)(m * hop);
        while (seg + 2 < knots.size() && o >= knots[seg + 1].o) ++seg;
        const Knot& k0 = knots[seg];
        const Knot& k1 = knots[std::min(seg + 1, knots.size() - 1)];
        const double span = k1.o - k0.o;
        const double nominalF = span > 0.0 ? k0.i + (o - k0.o) * (k1.i - k0.i) / span : k0.i;
        const int64 nominal = (int64)std::llround(nominalF);
        int64 pos = nominal;
        if (m > 0 && !(k0.locked && o < k1.o)) {
            const int64 natural = prev + hop;           // what would follow the previous frame with no stretch at all
            // the overlap is the first half of the new frame against the second half of the natural continuation
            auto score = [&](int64 cand) {
                double s = 0.0, e = 1.0e-9;
                for (int i = -hop; i < 0; i += 2) { const double v = at(cand + i); s += v * (double)at(natural + i); e += v * v; }
                return s / std::sqrt(e);        // by shape, not by loudness
            };
            int64 best = nominal;
            double bestScore = -1.0e30;
            const int64 lo = std::max<int64>(-hop, nominal - tol), hi = std::min<int64>(n + hop, nominal + tol);
            for (int64 cand = lo; cand <= hi; cand += 2) {
                const double sc = score(cand);
                if (sc > bestScore) { bestScore = sc; best = cand; }
            }
            for (int64 cand : {best - 1, best + 1}) {
                const double sc = score(cand);
                if (sc > bestScore) { bestScore = sc; best = cand; }
            }
            // when the natural continuation is itself within reach, nothing beats it
            if (natural >= nominal - tol && natural <= nominal + tol) best = natural;
            pos = best;
        }
        prev = pos;
        const int64 o0 = m * hop - hop;
        for (int c = 0; c < in.channels; ++c) {
            const float* src = in.ch[c].data();
            float* dst = out->ch[c].data();
            for (int i = 0; i < win; ++i) {
                const int64 oi = o0 + i, si = pos - hop + i;
                if (oi < 0 || oi >= outN || si < 0 || si >= n) continue;
                dst[oi] += w[(size_t)i] * src[si];
            }
        }
    }
    return out;
}

// Phase vocoder with identity phase locking: every spectral peak keeps turning at its own
// frequency across the new hop size, and the bins around a peak keep their phase relative to it.
std::shared_ptr<SampleData> tonal(const SampleData& in, double ratio) {
    ratio = clampT(ratio, 1.0 / 16.0, 16.0);
    const int64 n = in.frames();
    const int64 outN = std::max<int64>(n > 0 ? 1 : 0, (int64)std::llround((double)n * ratio));
    auto out = like(in, outN);
    if (n == 0) return out;
    const int N = in.sampleRate >= 32000.0 ? 4096 : 2048;
    const int hs = N / 4, bins = N / 2 + 1;
    const auto w = hann(N);
    const float norm = 1.0f / 1.5f;                             // four overlapping squared Hann windows add up to 1.5
    std::vector<float> re((size_t)N), im((size_t)N), mag((size_t)bins), ph((size_t)bins), lastPh((size_t)bins), synPh((size_t)bins);
    std::vector<double> peakPh((size_t)bins);
    std::vector<int> peakOf((size_t)bins);
    for (int c = 0; c < in.channels; ++c) {
        const float* src = in.ch[c].data();
        float* dst = out->ch[c].data();
        int64 lastA = 0;
        for (int64 m = 0; m * hs < outN + N / 2; ++m) {
            const int64 a = (int64)std::llround((double)(m * hs) / ratio);         // input position of the frame centre
            for (int i = 0; i < N; ++i) {
                const int64 s = a - N / 2 + i;
                re[(size_t)i] = s >= 0 && s < n ? src[s] * w[(size_t)i] : 0.0f;
                im[(size_t)i] = 0.0f;
            }
            dsp::fft(re.data(), im.data(), N, false);
            for (int k = 0; k < bins; ++k) {
                mag[(size_t)k] = std::sqrt(re[(size_t)k] * re[(size_t)k] + im[(size_t)k] * im[(size_t)k]);
                ph[(size_t)k] = std::atan2(im[(size_t)k], re[(size_t)k]);
            }
            if (m == 0) synPh = ph;
            else {
                const double ha = (double)(a - lastA);
                // peaks: bins that stand above their two neighbours on each side
                int lastPeak = -1;
                for (int k = 0; k < bins; ++k) {
                    const float v = mag[(size_t)k];
                    const bool isPeak = (k < 1 || v > mag[(size_t)k - 1]) && (k < 2 || v > mag[(size_t)k - 2])
                                     && (k + 1 >= bins || v >= mag[(size_t)k + 1]) && (k + 2 >= bins || v >= mag[(size_t)k + 2]);
                    if (!isPeak) continue;
                    // every bin goes with its nearest peak
                    const int from = lastPeak < 0 ? 0 : (lastPeak + k) / 2 + 1;
                    for (int j = from; j <= k; ++j) peakOf[(size_t)j] = k;
                    if (lastPeak >= 0) for (int j = lastPeak + 1; j < from; ++j) peakOf[(size_t)j] = lastPeak;
                    lastPeak = k;
                }
                if (lastPeak < 0) { for (int k = 0; k < bins; ++k) peakOf[(size_t)k] = k; }
                else for (int j = lastPeak + 1; j < bins; ++j) peakOf[(size_t)j] = lastPeak;
                // advance the peaks, then lock everything else to them
                for (int k = 0; k < bins; ++k) {
                    if (peakOf[(size_t)k] != k) continue;
                    const double omega = kTwoPi * k / N;
                    double d = (double)ph[(size_t)k] - (double)lastPh[(size_t)k] - omega * ha;
                    d -= kTwoPi * std::round(d / kTwoPi);
                    const double inst = ha > 0.0 ? omega + d / ha : omega;
                    peakPh[(size_t)k] = (double)synPh[(size_t)k] + inst * hs;
                }
                for (int k = 0; k < bins; ++k) {
                    const int p = peakOf[(size_t)k];
                    synPh[(size_t)k] = (float)std::remainder(peakPh[(size_t)p] + ((double)ph[(size_t)k] - (double)ph[(size_t)p]), kTwoPi);
                }
            }
            lastPh = ph;
            lastA = a;
            for (int k = 0; k < bins; ++k) {
                re[(size_t)k] = mag[(size_t)k] * std::cos(synPh[(size_t)k]);
                im[(size_t)k] = mag[(size_t)k] * std::sin(synPh[(size_t)k]);
            }
            for (int k = 1; k < N / 2; ++k) { re[(size_t)(N - k)] = re[(size_t)k]; im[(size_t)(N - k)] = -im[(size_t)k]; }
            dsp::fft(re.data(), im.data(), N, true);
            const int64 o0 = m * hs - N / 2;
            for (int i = 0; i < N; ++i) {
                const int64 o = o0 + i;
                if (o >= 0 && o < outN) dst[o] += re[(size_t)i] * w[(size_t)i] * norm;
            }
        }
    }
    return out;
}

double lengthRatio(int mode, double timeRatio, double pitchSemis) {
    if (mode == Resample) return timeRatio / std::pow(2.0, pitchSemis / 12.0);
    return timeRatio;
}

std::shared_ptr<SampleData> process(const SampleData& in, int mode, double timeRatio, double pitchSemis) {
    const double p = std::pow(2.0, pitchSemis / 12.0);
    if (mode == Resample) return resample(in, timeRatio / p);
    // stretch to length * p, then read it back p times faster: the length is right and the pitch has moved
    const double r = timeRatio * p;
    std::shared_ptr<SampleData> s = mode == Rhythmic ? rhythmic(in, r) : tonal(in, r);
    if (std::fabs(p - 1.0) < 1.0e-9) return s;
    auto out = resample(*s, 1.0 / p);
    const int64 want = std::max<int64>(1, (int64)std::llround((double)in.frames() * timeRatio));
    for (int c = 0; c < out->channels; ++c) out->ch[c].resize((size_t)want, 0.0f);
    return out;
}

double detectTempo(const SampleData& s, double* confidence) {
    if (confidence) *confidence = 0.0;
    const int64 n = s.frames();
    if (n < (int64)(s.sampleRate * 1.0)) return 0.0;
    const int N = s.sampleRate > 60000.0 ? 2048 : 1024, H = N / 4, bins = N / 2;
    const double envRate = s.sampleRate / H;
    const int64 frames = (n - N) / H;
    if (frames < 64) return 0.0;
    const auto w = hann(N);
    std::vector<float> re((size_t)N), im((size_t)N), prev((size_t)bins, 0.0f), flux((size_t)frames, 0.0f);
    for (int64 m = 0; m < frames; ++m) {
        for (int i = 0; i < N; ++i) {
            const size_t at = (size_t)(m * H + i);
            const float v = s.channels > 1 ? 0.5f * (s.ch[0][at] + s.ch[1][at]) : s.ch[0][at];
            re[(size_t)i] = v * w[(size_t)i];
            im[(size_t)i] = 0.0f;
        }
        dsp::fft(re.data(), im.data(), N, false);
        float f = 0.0f;
        for (int k = 1; k < bins; ++k) {
            const float c = std::log(1.0f + 100.0f * std::sqrt(re[(size_t)k] * re[(size_t)k] + im[(size_t)k] * im[(size_t)k]));
            if (c > prev[(size_t)k]) f += c - prev[(size_t)k];
            prev[(size_t)k] = c;
        }
        flux[(size_t)m] = m == 0 ? 0.0f : f;
    }
    // keep what stands above the local average
    {
        const int span = std::max(4, (int)(envRate * 0.25));
        std::vector<float> avg((size_t)frames);
        double run = 0.0;
        for (int64 m = 0; m < frames; ++m) {
            run += flux[(size_t)m];
            if (m >= 2 * span) run -= flux[(size_t)(m - 2 * span)];
            avg[(size_t)m] = (float)(run / std::min<int64>(m + 1, 2 * span));
        }
        for (int64 m = 0; m < frames; ++m) {
            const int64 a = std::min<int64>(frames - 1, m + span);
            flux[(size_t)m] = std::max(0.0f, flux[(size_t)m] - avg[(size_t)a]);
        }
    }
    {
        double mean = 0.0;
        for (int64 m = 0; m < frames; ++m) mean += flux[(size_t)m];
        mean /= (double)frames;
        for (int64 m = 0; m < frames; ++m) flux[(size_t)m] -= (float)mean;
    }
    auto acf = [&](int64 lag) {
        if (lag <= 0 || lag >= frames) return 0.0;
        double sum = 0.0;
        for (int64 m = 0; m + lag < frames; ++m) sum += (double)flux[(size_t)m] * (double)flux[(size_t)(m + lag)];
        return sum / (double)(frames - lag);
    };
    double zero = 0.0;
    for (int64 m = 0; m < frames; ++m) zero += (double)flux[(size_t)m] * (double)flux[(size_t)m];
    zero /= (double)frames;
    if (zero <= 1.0e-12) return 0.0;

    const int64 lagMin = (int64)std::floor(envRate * 60.0 / 200.0), lagMax = std::min<int64>(frames / 2, (int64)std::ceil(envRate * 60.0 / 60.0));
    if (lagMax <= lagMin + 2) return 0.0;
    std::vector<double> ac((size_t)(lagMax * 4 + 8), 0.0);
    for (int64 l = lagMin; l < (int64)ac.size(); ++l) ac[(size_t)l] = acf(l);
    int64 best = -1;
    double bestScore = 0.0;
    for (int64 l = lagMin; l <= lagMax; ++l) {
        const double bpm = envRate * 60.0 / (double)l;
        // a beat that repeats also shows up at two and four beats; most music sits near 120
        double sc = ac[(size_t)l] + 0.5 * ac[(size_t)(2 * l)] + 0.25 * ac[(size_t)(4 * l)];
        const double oct = std::log2(bpm / 120.0);
        sc *= std::exp(-0.5 * (oct / 0.9) * (oct / 0.9));
        if (sc > bestScore) { bestScore = sc; best = l; }
    }
    if (best < 0) return 0.0;
    // refine on the longest multiple of the beat that the file can hold: more beats, finer resolution
    double beatLag = (double)best;
    for (int mult = 1; mult <= 8; mult *= 2) {
        const int64 c = (int64)std::llround(beatLag * mult);
        if (c + 3 >= frames * 3 / 4) break;
        int64 top = c;
        double topV = -1.0;
        for (int64 l = std::max<int64>(1, c - 2); l <= c + 2; ++l) { const double v = acf(l); if (v > topV) { topV = v; top = l; } }
        const double y0 = acf(top - 1), y1 = topV, y2 = acf(top + 1);
        const double den = y0 - 2.0 * y1 + y2;
        const double frac = std::fabs(den) > 1.0e-18 ? clampT(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
        beatLag = ((double)top + frac) / (double)mult;
    }
    double bpm = envRate * 60.0 / beatLag;
    const double conf = clampT(ac[(size_t)best] / zero, 0.0, 1.0);
    if (confidence) *confidence = conf;
    // a real pulse repeats after two beats as well as after one; a chance bump in noise does not
    if (conf < 0.1 || ac[(size_t)(2 * best)] < 0.4 * ac[(size_t)best]) { if (confidence) *confidence = 0.0; return 0.0; }
    // a file cut to a whole number of bars is a loop: its length gives the tempo exactly
    const double seconds = (double)n / s.sampleRate;
    const double beats = seconds * bpm / 60.0;
    const double whole = std::round(beats);
    if (whole >= 1.0 && std::fabs(beats - whole) / whole < 0.02 && ((int)whole % 4 == 0 || whole <= 2.0)) bpm = 60.0 * whole / seconds;
    return clampT(bpm, 30.0, 400.0);
}

} // namespace pulp::stretch
