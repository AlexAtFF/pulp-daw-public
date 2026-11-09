// The listening half of the multisample engine: pitch, trimming, sustain loops, key mapping,
// and instrument files.
#include "pulp/Multisample.h"
#if __has_include("pulp/Flac.h")
#include "pulp/Flac.h"
#define PULP_HAVE_FLAC 1
#endif
#include <cctype>
#include <filesystem>
#include <tuple>
#if __has_include("pulp/Ogg.h")
#include "pulp/Ogg.h"
#endif
#if __has_include("pulp/Mp3.h")
#include "pulp/Mp3.h"
#endif

namespace pulp {

namespace fs = std::filesystem;
static fs::path u8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
static std::string str(const fs::path& p) { const auto u = p.generic_u8string(); return std::string(u.begin(), u.end()); }

// ------------------------------------------------------------------ reading audio files

std::shared_ptr<SampleData> readAiffMemory(const void* data, size_t size, std::string* error) {
    auto fail = [&](const char* m) -> std::shared_ptr<SampleData> { if (error) *error = m; return nullptr; };
    const unsigned char* d = (const unsigned char*)data;
    auto be16 = [&](size_t at) { return (unsigned)((d[at] << 8) | d[at + 1]); };
    auto be32 = [&](size_t at) { return ((unsigned)d[at] << 24) | ((unsigned)d[at + 1] << 16) | ((unsigned)d[at + 2] << 8) | (unsigned)d[at + 3]; };
    if (size < 12 || std::memcmp(d, "FORM", 4) != 0 || (std::memcmp(d + 8, "AIFF", 4) != 0 && std::memcmp(d + 8, "AIFC", 4) != 0)) return fail("not an AIFF file");
    const bool aifc = std::memcmp(d + 8, "AIFC", 4) == 0;
    unsigned channels = 0, bits = 0;
    size_t frames = 0, pcm = 0, pcmBytes = 0;
    double rate = 0.0;
    bool littleEndian = false;
    for (size_t pos = 12; pos + 8 <= size;) {
        size_t len = be32(pos + 4);
        const size_t body = pos + 8;
        if (body + len > size) len = size - body;
        if (std::memcmp(d + pos, "COMM", 4) == 0 && len >= 18) {
            channels = be16(body);
            frames = be32(body + 2);
            bits = be16(body + 6);
            // the sample rate is an 80-bit extended float
            const int exp = (int)(be16(body + 8) & 0x7FFF) - 16383;
            unsigned long long mant = 0;
            for (int i = 0; i < 8; ++i) mant = (mant << 8) | d[body + 10 + (size_t)i];
            rate = std::ldexp((double)mant, exp - 63);
            if (aifc && len >= 22) {
                if (std::memcmp(d + body + 18, "sowt", 4) == 0) littleEndian = true;
                else if (std::memcmp(d + body + 18, "NONE", 4) != 0) return fail("compressed AIFF files are not supported");
            }
        } else if (std::memcmp(d + pos, "SSND", 4) == 0 && len >= 8) {
            const size_t offset = be32(body);
            pcm = body + 8 + offset;
            pcmBytes = len - 8 - std::min<size_t>(offset, len - 8);
        }
        pos = body + len + (len & 1);
    }
    const unsigned bytes = (bits + 7) / 8;
    if (!pcm || channels == 0 || rate <= 0.0 || bytes < 1 || bytes > 4) return fail("AIFF file has no audio data");
    frames = std::min(frames, pcmBytes / ((size_t)bytes * channels));
    auto s = std::make_shared<SampleData>();
    s->sampleRate = rate;
    s->channels = channels > 1 ? 2 : 1;
    for (int c = 0; c < s->channels; ++c) s->ch[c].resize(frames);
    const double scale = 1.0 / (double)(1ull << (bytes * 8 - 1));
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < s->channels; ++c) {
            const unsigned char* p = d + pcm + (i * channels + (size_t)c) * bytes;
            long long v = 0;
            for (unsigned b = 0; b < bytes; ++b) v = (v << 8) | p[littleEndian ? bytes - 1 - b : b];
            if (v & (1ll << (bytes * 8 - 1))) v -= (1ll << (bytes * 8));
            s->ch[c][i] = (float)((double)v * scale);
        }
    return s;
}

std::shared_ptr<SampleData> readAudioFile(const std::string& path, std::string* error) {
    std::vector<unsigned char> b;
    if (!readFileBytes(path, b)) { if (error) *error = "cannot open " + path; return nullptr; }
    std::shared_ptr<SampleData> s;
    // Decided by what the file holds, not by its name. MP3 has no signature of its own, so it is tried last
    // of the known kinds, and an ID3 tag (which FLAC files can carry too) needs a second look.
    auto starts = [&](const char* magic, size_t n) { return b.size() >= n && std::memcmp(b.data(), magic, n) == 0; };
    const bool id3 = starts("ID3", 3);
#if __has_include("pulp/Mp3.h")
    const bool mp3 = looksLikeMp3(b.data(), b.size());
#else
    const bool mp3 = false;
#endif
    if (starts("FORM", 4)) s = readAiffMemory(b.data(), b.size(), error);
    else if (starts("fLaC", 4) || (id3 && !mp3)) {
#ifdef PULP_HAVE_FLAC
        s = readFlacMemory(b.data(), b.size(), error);
#else
        if (error) *error = "FLAC files cannot be read yet";
#endif
    } else if (starts("OggS", 4)) {
#if __has_include("pulp/Ogg.h")
        s = readOggMemory(b.data(), b.size(), error);
#else
        if (error) *error = "Ogg files cannot be read yet";
#endif
    } else if (starts("RIFF", 4) || starts("RF64", 4)) s = readWavMemory(b.data(), b.size(), error);
#if __has_include("pulp/Mp3.h")
    else if (mp3) s = readMp3Memory(b.data(), b.size(), error);
#endif
    else s = readWavMemory(b.data(), b.size(), error);
    if (s) s->path = path;
    return s;
}

namespace ms {

// ------------------------------------------------------------------ listening

static std::vector<float> monoOf(const SampleData& s) {
    std::vector<float> m((size_t)s.frames());
    if (s.channels > 1) for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (s.ch[0][i] + s.ch[1][i]);
    else m = s.ch[0];
    return m;
}

Range findSound(const SampleData& s, float belowPeakDb) {
    const int64 n = s.frames();
    Range r{0, n};
    if (n < 16) return r;
    const int block = std::max(8, (int)(s.sampleRate * 0.004));
    const int64 nb = (n + block - 1) / block;
    std::vector<float> level((size_t)nb, 0.0f);
    float top = 0.0f;
    for (int64 b = 0; b < nb; ++b) {
        float pk = 0.0f;
        for (int64 i = b * block; i < std::min(n, (b + 1) * block); ++i)
            for (int c = 0; c < s.channels; ++c) pk = std::max(pk, std::fabs(s.ch[c][(size_t)i]));
        level[(size_t)b] = pk;
        top = std::max(top, pk);
    }
    if (top <= 0.0f) return r;
    const float threshold = top * dbToGain(-belowPeakDb);
    int64 first = 0, last = nb - 1;
    while (first < nb && level[(size_t)first] < threshold) ++first;
    while (last > first && level[(size_t)last] < threshold) --last;
    // the very beginning of an attack is below any threshold: step back a little
    r.start = std::max<int64>(0, first * block - (int64)(s.sampleRate * 0.003));
    r.end = std::min(n, (last + 1) * block + (int64)(s.sampleRate * 0.02));
    return r;
}

static double rmsOf(const std::vector<float>& m, int64 a, int64 b) {
    a = std::max<int64>(0, a);
    b = std::min<int64>((int64)m.size(), b);
    if (b <= a) return 0.0;
    double sum = 0.0;
    for (int64 i = a; i < b; ++i) sum += (double)m[(size_t)i] * m[(size_t)i];
    return std::sqrt(sum / (double)(b - a));
}

bool holds(const SampleData& s, Range sound) {
    const int64 d = sound.end - sound.start;
    if ((double)d < s.sampleRate * 0.7) return false;
    const auto m = monoOf(s);
    const double early = rmsOf(m, sound.start + d / 4, sound.start + (int64)(d * 0.45));
    const double late = rmsOf(m, sound.start + (int64)(d * 0.6), sound.start + (int64)(d * 0.8));
    return early > 0.0 && late >= early * 0.5;     // within 6 dB
}

// YIN on one window. Returns the period in samples (0 if nothing periodic) and how clear it is.
static double yin(const float* x, int window, double sr, double* clarity) {
    const int maxLag = std::min(window / 2, (int)(sr / 25.0));
    const int minLag = std::max(2, (int)(sr / 5000.0));
    *clarity = 0.0;
    if (maxLag <= minLag + 2) return 0.0;
    const int span = window - maxLag;
    std::vector<double> d((size_t)maxLag + 1, 0.0);
    for (int lag = 1; lag <= maxLag; ++lag) {
        double sum = 0.0;
        for (int i = 0; i < span; ++i) { const double e = (double)x[i] - (double)x[i + lag]; sum += e * e; }
        d[(size_t)lag] = sum;
    }
    // cumulative mean normalised difference
    std::vector<double> c((size_t)maxLag + 1, 1.0);
    double run = 0.0;
    for (int lag = 1; lag <= maxLag; ++lag) {
        run += d[(size_t)lag];
        c[(size_t)lag] = run > 0.0 ? d[(size_t)lag] * lag / run : 1.0;
    }
    int best = -1;
    for (int lag = minLag; lag < maxLag; ++lag)
        if (c[(size_t)lag] < 0.12) {            // the first clear dip is the fundamental; follow it to its bottom
            while (lag + 1 < maxLag && c[(size_t)lag + 1] < c[(size_t)lag]) ++lag;
            best = lag;
            break;
        }
    if (best < 0) {
        best = minLag;
        for (int lag = minLag; lag < maxLag; ++lag) if (c[(size_t)lag] < c[(size_t)best]) best = lag;
    }
    auto refine = [&](int lag) {
        if (lag <= 1 || lag >= maxLag) return (double)lag;
        const double y0 = c[(size_t)lag - 1], y1 = c[(size_t)lag], y2 = c[(size_t)lag + 1];
        const double den = y0 - 2.0 * y1 + y2;
        return (double)lag + (std::fabs(den) > 1.0e-18 ? clampT(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0);
    };
    double period = refine(best);
    *clarity = clampT(1.0 - c[(size_t)best], 0.0, 1.0);
    // Many periods measured together give a finer answer than one. The count is doubled step by step, so the
    // estimate is always good enough to land on the right dip of the next, longer measurement.
    for (int k = 2; period * k < (double)(maxLag - 2); k *= 2) {
        const int centre = (int)std::lround(period * k);
        const int reach = std::max(1, (int)(period * 0.3));
        int top = centre;
        for (int lag = std::max(minLag, centre - reach); lag <= std::min(maxLag - 1, centre + reach); ++lag) if (c[(size_t)lag] < c[(size_t)top]) top = lag;
        if (c[(size_t)top] > c[(size_t)best] + 0.1) break;
        period = refine(top) / (double)k;
    }
    return period;
}

Pitch detectPitch(const SampleData& s) {
    Pitch out;
    const auto m = monoOf(s);
    float top = 0.0f;
    for (float v : m) top = std::max(top, std::fabs(v));
    if (top < 1.0e-6f) return out;          // silence has no pitch
    const Range sound = findSound(s);
    const int64 len = sound.end - sound.start;
    int window = 8192;
    while (window > 1024 && window > len) window /= 2;
    if (len < 512) return out;
    window = (int)std::min<int64>(window, len);
    // several windows over the first part of the note, past the attack; the middle answer wins
    std::vector<std::pair<double, double>> found;
    const int64 from = sound.start + std::min<int64>((int64)(s.sampleRate * 0.03), len / 10);
    const int64 to = std::max(from, sound.start + (int64)(len * 0.6) - window);
    for (int k = 0; k < 6; ++k) {
        const int64 at = std::min<int64>(from + (to - from) * k / 5, (int64)m.size() - window);
        if (at < 0) break;
        double clarity = 0.0;
        const double period = yin(m.data() + at, window, s.sampleRate, &clarity);
        if (period > 0.0) found.push_back({period, clarity});
        if (to == from) break;
    }
    if (found.empty()) return out;
    std::vector<std::pair<double, double>> clear;
    for (auto& f : found) if (f.second >= 0.6) clear.push_back(f);
    auto& use = clear.empty() ? found : clear;
    std::sort(use.begin(), use.end());
    const auto mid = use[use.size() / 2];
    out.hz = s.sampleRate / mid.first;
    out.midi = 69.0 + 12.0 * std::log2(out.hz / 440.0);
    out.clarity = mid.second;
    return out;
}

Loop findLoop(const SampleData& s, Range sound, double hz) {
    Loop out;
    const auto m = monoOf(s);
    const int64 d = sound.end - sound.start;
    const double period = hz > 0.0 ? s.sampleRate / hz : 0.0;
    // the steady part: after the attack has settled, before the release
    const int64 a = sound.start + std::max<int64>((int64)(d * 0.3), std::min<int64>(d / 2, (int64)(s.sampleRate * 0.25)));
    const int64 b = sound.start + (int64)(d * 0.85);
    if ((double)(b - a) < s.sampleRate * 0.2) return out;
    const int w = (int)clampT(period * 2.0, 256.0, 2048.0);
    auto match = [&](int64 p, int64 q) {       // how alike the audio around p and around q is
        double pq = 0.0, pp = 1.0e-12, qq = 1.0e-12;
        for (int i = -w; i < w; ++i) {
            const int64 ip = p + i, iq = q + i;
            if (ip < 0 || iq < 0 || ip >= (int64)m.size() || iq >= (int64)m.size()) continue;
            pq += (double)m[(size_t)ip] * m[(size_t)iq];
            pp += (double)m[(size_t)ip] * m[(size_t)ip];
            qq += (double)m[(size_t)iq] * m[(size_t)iq];
        }
        // alike in shape and in level
        return pq / std::sqrt(pp * qq) * std::min(pp, qq) / std::max(pp, qq);
    };
    const int64 reach = std::max<int64>(4, (int64)std::ceil(period > 0.0 ? period * 1.5 : 400.0));
    for (int k = 0; k < 4; ++k) {              // a few starting points: keep the pair of ends that agree best
        const int64 start = a + (b - a) * k / 12;
        int64 nominal = b - (b - a) * k / 16;
        if (period > 0.0) {
            // A whole number of periods that is also close to a whole number of samples: then the overtones
            // line up at the join as well as the fundamental does.
            const double most = std::floor((double)(nominal - start) / period);
            double bestCount = most, bestOff = 1.0;
            for (int j = 0; j < 48 && most - j > 4.0; ++j) {
                const double len = (most - j) * period, off = std::fabs(len - std::round(len));
                if (off < bestOff - 0.02) { bestOff = off; bestCount = most - j; }
            }
            nominal = start + (int64)std::llround(bestCount * period);
        }
        if ((double)(nominal - start) < s.sampleRate * 0.2) continue;
        for (int64 end = nominal - reach; end <= nominal + reach; ++end) {
            if (end + w >= (int64)m.size() || end <= start + w) continue;
            const double q = match(start, end);
            if (!out.found || q > out.match) { out.found = true; out.match = q; out.start = start; out.end = end; }
        }
    }
    if (!out.found) return out;
    // a long blend hides what the match could not: up to a quarter of a second, never more than the loop or the lead-in
    out.crossfade = std::min({(int64)(s.sampleRate * 0.25), (out.end - out.start) / 2, out.start - sound.start});
    out.crossfade = std::max<int64>(0, out.crossfade);
    return out;
}

std::shared_ptr<SampleData> bakeLoop(const SampleData& s, const Loop& loop) {
    auto o = std::make_shared<SampleData>();
    o->sampleRate = s.sampleRate;
    o->channels = s.channels;
    const int64 keep = std::min<int64>(s.frames(), loop.end + 8);
    for (int c = 0; c < s.channels; ++c) {
        auto& d = o->ch[c];
        d.assign(s.ch[c].begin(), s.ch[c].begin() + keep);
        const int64 xf = loop.crossfade;
        // ends that already agree are blended in level; ends that do not, in power
        const bool alike = loop.match > 0.7;
        for (int64 i = 0; i < xf; ++i) {
            const double t = (double)(i + 1) / (double)(xf + 1);
            const double gOut = alike ? 1.0 - t : std::cos(t * kPi * 0.5), gIn = alike ? t : std::sin(t * kPi * 0.5);
            const int64 e = loop.end - xf + i, b = loop.start - xf + i;
            if (e >= 0 && b >= 0 && e < keep) d[(size_t)e] = (float)(s.ch[c][(size_t)e] * gOut + s.ch[c][(size_t)b] * gIn);
        }
        // what follows the loop end is what follows the loop start, so interpolating across the jump is exact
        for (int64 i = 0; loop.end + i < keep && loop.start + i < s.frames(); ++i) d[(size_t)(loop.end + i)] = s.ch[c][(size_t)(loop.start + i)];
    }
    return o;
}

// ------------------------------------------------------------------ names

int noteFromName(const std::string& name, int middleC) {
    static const int semis[7] = {9, 11, 0, 2, 4, 5, 7};        // A B C D E F G
    int found = -1;
    const size_t n = name.size();
    for (size_t i = 0; i < n; ++i) {
        const char c = (char)std::toupper((unsigned char)name[i]);
        if (c < 'A' || c > 'G') continue;
        if (i > 0 && std::isalpha((unsigned char)name[i - 1])) continue;        // part of a word
        size_t j = i + 1;
        int acc = 0;
        if (j < n && (name[j] == '#' || name[j] == 's')) { acc = 1; ++j; }
        else if (j < n && name[j] == 'b') { acc = -1; ++j; }
        bool neg = false;
        if (j < n && name[j] == '-') { neg = true; ++j; }
        if (j >= n || !std::isdigit((unsigned char)name[j])) continue;
        const int octave = (name[j] - '0') * (neg ? -1 : 1);
        ++j;
        if (j < n && std::isalnum((unsigned char)name[j])) continue;            // "A440", "B12x"
        const int key = (octave - middleC) * 12 + 60 + semis[c - 'A'] + acc;
        if (key >= 0 && key <= 127) found = key;                                // the last one in the name wins
    }
    if (found >= 0) return found;
    // a bare three-digit MIDI number between separators: "_060_"
    for (size_t i = 0; i + 3 <= n; ++i) {
        if (!std::isdigit((unsigned char)name[i]) || !std::isdigit((unsigned char)name[i + 1]) || !std::isdigit((unsigned char)name[i + 2])) continue;
        if ((i > 0 && std::isalnum((unsigned char)name[i - 1])) || (i + 3 < n && std::isalnum((unsigned char)name[i + 3]))) continue;
        const int key = (name[i] - '0') * 100 + (name[i + 1] - '0') * 10 + (name[i + 2] - '0');
        if (key >= 0 && key <= 127) found = key;
    }
    return found;
}

std::string noteName(int key) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[((key % 12) + 12) % 12]) + std::to_string(key / 12);
}

// ------------------------------------------------------------------ building

std::string BuildReport::text() const {
    std::string out;
    for (auto& l : lines) {
        char b[400];
        snprintf(b, sizeof b, "%-40s %-4s %+6.1f cents  clarity %.2f  %s%s%s\n", l.file.c_str(), noteName(l.key).c_str(), l.cents, l.clarity,
                 l.looped ? "looped" : "one-shot", l.looped ? (" (match " + std::to_string(l.loopMatch).substr(0, 4) + ")").c_str() : "", l.note.empty() ? "" : ("  " + l.note).c_str());
        out += b;
    }
    return out;
}

const Zone* Instrument::pick(int key, int velocity, uint32 turn) const {
    const Zone* fallback = nullptr;
    for (const Zone& z : zones) {
        if (key < z.loKey || key > z.hiKey) continue;
        if (velocity < z.loVel || velocity > z.hiVel) { if (!fallback) fallback = &z; continue; }
        if (z.groupSize > 1 && (int)(turn % (uint32)z.groupSize) != z.turn) continue;
        return &z;
    }
    return fallback;
}

std::shared_ptr<Instrument> build(const std::vector<Note>& notes, const std::string& name, const BuildOptions& opt, BuildReport* report) {
    auto inst = std::make_shared<Instrument>();
    inst->name = name;
    struct Made { Zone z; int layer, take; double level; };
    std::vector<Made> made;
    int looped = 0;
    for (const Note& n : notes) {
        if (!n.sample || n.sample->frames() < 64) continue;
        const SampleData& src = *n.sample;
        BuildReport::Line line;
        line.file = n.file;
        const Range sound = opt.trim ? findSound(src) : Range{0, src.frames()};
        const Pitch pitch = detectPitch(src);
        const int hint = n.key >= 0 ? n.key : noteFromName(n.file, opt.middleC);
        // which key is it? Trust the ears when the note is clearly pitched, the label when it is not.
        int key = 60;
        const int heard = pitch.key();
        if (n.key >= 0) {
            // the source states the key outright: that settles it (bells and bars confuse any ear about the octave);
            // listening still corrects the tuning and reports a real disagreement
            key = n.key;
            if (pitch.hz > 0.0 && pitch.clarity >= 0.85 && (heard - key) % 12 != 0 && std::abs(heard - key) > 1) line.note = "stated as " + noteName(key) + ", sounds like " + noteName(heard);
        } else if (pitch.hz > 0.0 && pitch.clarity >= 0.6) {
            if (hint < 0) key = heard;
            else if (heard == hint) key = hint;
            else if ((heard - hint) % 12 == 0) {    // right note, another octave: sources number octaves differently
                key = pitch.clarity >= 0.85 ? heard : hint;
                if (key != hint) line.note = "labelled " + noteName(hint) + ", sounds an octave away";
            } else if (std::abs(heard - hint) <= 1) key = hint;
            else {
                key = pitch.clarity >= 0.85 ? heard : hint;
                line.note = "labelled " + noteName(hint) + ", sounds like " + noteName(heard);
            }
        } else {
            key = hint >= 0 ? hint : (pitch.hz > 0.0 ? heard : 60);
            line.note = "no clear pitch";
        }
        key = clampT(key, 0, 127);
        double cents = pitch.hz > 0.0 && pitch.clarity >= 0.6 ? (pitch.midi - key) * 100.0 : 0.0;
        if (std::fabs(cents) > 100.0) cents = 0.0;

        // cut out the sound itself, with a short fade at each cut
        auto cut = std::make_shared<SampleData>();
        cut->sampleRate = src.sampleRate;
        cut->channels = src.channels;
        const int64 len = sound.end - sound.start;
        const int64 fadeIn = sound.start > 0 ? std::min<int64>(len / 4, (int64)(src.sampleRate * 0.001)) : 0;
        const int64 fadeOut = sound.end < src.frames() ? std::min<int64>(len / 4, (int64)(src.sampleRate * 0.01)) : 0;
        for (int c = 0; c < src.channels; ++c) {
            cut->ch[c].assign(src.ch[c].begin() + sound.start, src.ch[c].begin() + sound.end);
            for (int64 i = 0; i < fadeIn; ++i) cut->ch[c][(size_t)i] *= (float)i / (float)fadeIn;
            for (int64 i = 0; i < fadeOut; ++i) cut->ch[c][(size_t)(len - 1 - i)] *= (float)i / (float)fadeOut;
        }
        Made m;
        m.layer = n.layer;
        m.take = n.take;
        m.z.root = key;
        m.z.cents = (float)cents;
        m.z.file = n.file;
        const Range whole{0, len};
        const bool sustained = opt.sustain == 1 || (opt.sustain < 0 && holds(*cut, whole));
        const auto mono = monoOf(*cut);
        if (sustained) {
            const Loop loop = findLoop(*cut, whole, pitch.clarity >= 0.6 ? pitch.hz : 0.0);
            if (loop.found) {
                m.z.sample = bakeLoop(*cut, loop);
                m.z.loop = true;
                m.z.loopStart = loop.start;
                m.z.loopEnd = loop.end;
                line.looped = true;
                line.loopMatch = loop.match;
                m.level = rmsOf(mono, loop.start, loop.end) * 1.41;       // as a peak-like figure
                ++looped;
            }
        }
        if (!m.z.sample) { m.z.sample = cut; m.level = cut->peak(); }
        line.key = key;
        line.cents = cents;
        line.clarity = pitch.clarity;
        if (report) report->lines.push_back(line);
        made.push_back(std::move(m));
    }
    if (made.empty()) return inst;
    std::stable_sort(made.begin(), made.end(), [](const Made& a, const Made& b) {
        return std::tie(a.z.root, a.layer, a.take) < std::tie(b.z.root, b.layer, b.take);
    });

    // keys: each recorded note takes the keys nearest to it
    std::vector<int> roots;
    for (auto& m : made) if (roots.empty() || roots.back() != m.z.root) roots.push_back(m.z.root);
    for (auto& m : made) {
        const size_t i = (size_t)(std::find(roots.begin(), roots.end(), m.z.root) - roots.begin());
        m.z.loKey = i == 0 ? 0 : (roots[i - 1] + roots[i]) / 2 + 1;
        m.z.hiKey = i + 1 == roots.size() ? 127 : (roots[i] + roots[i + 1]) / 2;
    }
    // velocity layers split the velocity range evenly; takes of one layer share a round-robin group
    int group = 0;
    for (size_t i = 0; i < made.size();) {
        size_t j = i;
        std::vector<int> layers;
        while (j < made.size() && made[j].z.root == made[i].z.root) { if (layers.empty() || layers.back() != made[j].layer) layers.push_back(made[j].layer); ++j; }
        for (size_t k = i; k < j;) {
            const int li = (int)(std::find(layers.begin(), layers.end(), made[k].layer) - layers.begin());
            size_t e = k;
            while (e < j && made[e].layer == made[k].layer) ++e;
            ++group;
            for (size_t t = k; t < e; ++t) {
                made[t].z.loVel = li * 128 / (int)layers.size();
                made[t].z.hiVel = (li + 1) * 128 / (int)layers.size() - 1;
                made[t].z.group = group;
                made[t].z.groupSize = (int)(e - k);
                made[t].z.turn = (int)(t - k);
            }
            k = e;
        }
        i = j;
    }
    // levels: bring every note to the typical level of the set (top velocity layers only set the reference)
    if (opt.matchLevels) {
        std::vector<double> levels;
        for (auto& m : made) if (m.level > 1.0e-6) levels.push_back(m.level);
        if (!levels.empty()) {
            std::sort(levels.begin(), levels.end());
            const double ref = levels[levels.size() / 2];
            for (auto& m : made) {
                // softer layers keep being softer: only notes of the loudest layer are evened out fully
                const bool top = m.z.hiVel == 127;
                m.z.gain = m.level > 1.0e-6 && top ? (float)clampT(ref / m.level, 0.25, 4.0) : 1.0f;
            }
            float peak = 0.0f;
            for (auto& m : made) peak = std::max(peak, m.z.sample->peak() * m.z.gain);
            if (peak > 0.0f) for (auto& m : made) m.z.gain *= 0.85f / peak;
        }
    }
    inst->sustain = looped * 2 > (int)made.size();
    for (auto& m : made) inst->zones.push_back(std::move(m.z));
    // settings that suit it: held sounds swell in and stop soon after the key is let go; struck sounds ring on
    Json p = Json::object();
    p.set("attack", inst->sustain ? 12.0f : 0.0f);
    p.set("decay", 600.0f);
    p.set("sustain", 1.0f);
    p.set("release", inst->sustain ? 220.0f : 450.0f);
    p.set("loop", inst->sustain ? 1.0f : 0.0f);
    inst->params = p;
    return inst;
}

// ------------------------------------------------------------------ instrument files

Json zonesToJson(const Instrument& inst) {
    Json arr = Json::array();
    for (auto& z : inst.zones) {
        Json zj = Json::object();
        zj.set("file", z.file);
        zj.set("root", z.root);
        zj.set("cents", z.cents);
        zj.set("lo", z.loKey); zj.set("hi", z.hiKey);
        zj.set("lovel", z.loVel); zj.set("hivel", z.hiVel);
        if (z.groupSize > 1) { zj.set("group", z.group); zj.set("of", z.groupSize); zj.set("turn", z.turn); }
        if (z.loop) { Json lp = Json::array(); lp.push((long long)z.loopStart); lp.push((long long)z.loopEnd); zj.set("loop", lp); }
        zj.set("gain", z.gain);
        arr.push(zj);
    }
    return arr;
}

std::shared_ptr<Instrument> instrumentFromJson(const Json& j, const std::string& baseDir, std::string* error) {
    auto inst = std::make_shared<Instrument>();
    inst->name = j["name"].str("");
    inst->category = j["category"].str("");
    inst->sustain = j["sustain"].b(false);
    inst->credit = j["credit"].str("");
    inst->params = j["params"];
    for (auto& zj : j["zones"].elements()) {
        Zone z;
        z.file = zj["file"].str("");
        z.sample = SampleCache::global().load(z.file, baseDir, error);
        if (!z.sample) continue;            // a missing sample leaves a gap in the keyboard, not a broken instrument
        z.root = clampT(zj["root"].i(60), 0, 127);
        z.cents = zj["cents"].f(0.0f);
        z.loKey = clampT(zj["lo"].i(0), 0, 127);
        z.hiKey = clampT(zj["hi"].i(127), 0, 127);
        z.loVel = clampT(zj["lovel"].i(0), 0, 127);
        z.hiVel = clampT(zj["hivel"].i(127), 0, 127);
        z.group = zj["group"].i(0);
        z.groupSize = std::max(1, zj["of"].i(1));
        z.turn = zj["turn"].i(0);
        const int64 frames = z.sample->frames();
        const int64 ls = zj["loop"].at(0).i64(), le = zj["loop"].at(1).i64();
        z.loop = le > ls + 8 && le <= frames;
        z.loopStart = z.loop ? ls : 0;
        z.loopEnd = z.loop ? le : 0;
        z.gain = clampT(zj["gain"].f(1.0f), 0.0f, 16.0f);
        inst->zones.push_back(std::move(z));
    }
    return inst;
}

bool saveInstrument(const Instrument& inst, const std::string& path, bool flac, std::string* error) {
    const fs::path file = u8p(path);
    const fs::path dir = file.parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
#ifndef PULP_HAVE_FLAC
    flac = false;
#endif
    Instrument out = inst;
    std::string stem = str(file.stem());
    for (size_t i = 0; i < out.zones.size(); ++i) {
        Zone& z = out.zones[i];
        std::string n = stem + " " + noteName(z.root);
        if (z.loVel > 0 || z.hiVel < 127) n += " v" + std::to_string(z.hiVel);
        if (z.groupSize > 1) n += " t" + std::to_string(z.turn + 1);
        n += flac ? ".flac" : ".wav";
        const std::string full = str(dir / u8p(n));
        bool ok;
#ifdef PULP_HAVE_FLAC
        ok = flac ? writeFlacFile(full, *z.sample, 24, error) : writeWavFile(full, *z.sample, WavFormat::Pcm24, false, error);
#else
        ok = writeWavFile(full, *z.sample, WavFormat::Pcm24, false, error);
#endif
        if (!ok) return false;
        z.file = n;
    }
    Json j = Json::object();
    j.set("format", "pulp-instrument");
    j.set("version", 1);
    j.set("name", inst.name);
    j.set("category", inst.category);
    j.set("sustain", inst.sustain);
    j.set("credit", inst.credit);
    j.set("params", inst.params.isNull() ? Json::object() : inst.params);
    j.set("zones", zonesToJson(out));
    if (!writeTextFile(path, j.dump(1))) { if (error) *error = "cannot write " + path; return false; }
    return true;
}

std::shared_ptr<Instrument> loadInstrument(const std::string& path, std::string* error) {
    if (path.size() > 4) {
        std::string ext = path.substr(path.size() - 4);
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".sfz") return loadSfz(path, error);
    }
    std::string text;
    if (!readTextFile(path, text)) { if (error) *error = "cannot open " + path; return nullptr; }
    std::string perr;
    const Json j = Json::parse(text, &perr);
    if (!perr.empty() || j["format"].str("") != "pulp-instrument") { if (error) *error = "not a PULP instrument file"; return nullptr; }
    auto inst = instrumentFromJson(j, str(u8p(path).parent_path()), error);
    if (inst && inst->zones.empty()) { if (error && error->empty()) *error = "the instrument's samples are missing"; return nullptr; }
    return inst;
}

} // namespace ms
} // namespace pulp
