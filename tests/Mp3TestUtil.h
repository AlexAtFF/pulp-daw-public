#pragma once
// Helpers shared by the MP3 test files: test signals, a signal-to-noise measure, and a reader of
// frame headers and side information written independently of the codec (so the tests can say what a
// stream contains without asking the decoder).
#include "Util.h"
#include "pulp/Mp3.h"
#include <map>

namespace mp3t {
using namespace pulp;

// A small deterministic generator, so every run tests the same signals.
struct Rand {
    uint32 s;
    explicit Rand(uint32 seed) : s(seed) {}
    uint32 next() { s = s * 1664525u + 1013904223u; return s >> 8; }
    float uni() { return (float)next() / 8388608.0f - 1.0f; }        // -1 to just under 1
};

inline std::shared_ptr<SampleData> blank(int channels, size_t frames, double sr = 44100.0) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = channels;
    for (int c = 0; c < channels; ++c) s->ch[c].assign(frames, 0.0f);
    return s;
}

enum class Sig { Sine, Sweep, Noise, Music, Silence, Clicks, NearMono };
inline const char* sigName(Sig k) {
    static const char* n[] = {"sine", "sweep", "noise", "music-like", "silence", "clicks", "near-mono music"};
    return n[(int)k];
}

// A music-like mix for one channel: notes with harmonics that start at different times and die away,
// three drum-like hits (noise bursts with most of their energy under 6 kHz) and a quiet noise floor.
inline void musicInto(std::vector<float>& out, double sr, uint32 seed, double detune) {
    Rand rng(seed);
    const size_t n = out.size();
    const double seconds = (double)n / sr;
    static const double notes[6] = {110.0, 164.81, 220.0, 277.18, 329.63, 440.0};
    for (int k = 0; k < 6; ++k) {
        const double f0 = notes[k] * detune, start = (0.02 + 0.13 * k) * seconds;
        for (int h = 1; h <= 14; ++h) {
            if (f0 * h > 12000.0) break;
            const double amp = 0.16 / h, phase = 0.37 * h + k;
            for (size_t i = (size_t)(start * sr); i < n; ++i) {
                const double t = (double)i / sr - start;
                out[i] += (float)(amp * std::exp(-t / 0.35) * (1.0 - std::exp(-t / 0.003)) * std::sin(kTwoPi * f0 * h * t + phase));
            }
        }
    }
    for (int hit = 0; hit < 3; ++hit) {
        const size_t at = (size_t)((0.22 + 0.27 * hit) * seconds * sr);
        float low = 0.0f;
        for (size_t i = at; i < n && i < at + (size_t)(0.06 * sr); ++i) {
            low += 0.55f * (rng.uni() - low);                   // one-pole low-pass, about 6 kHz at 44.1 kHz
            out[i] += 0.45f * low * (float)std::exp(-(double)(i - at) / (0.012 * sr));
        }
    }
    for (size_t i = 0; i < n; ++i) out[i] += 0.0007f * rng.uni();
}

// Where click number k of the Clicks signal starts.
inline size_t clickAt(int k, double sr) { return (size_t)((0.12 + 0.23 * k) * sr) + 97u * (size_t)k; }

inline std::shared_ptr<SampleData> signal(Sig kind, int channels, size_t frames, double sr = 44100.0, uint32 seed = 1) {
    auto s = blank(channels, frames, sr);
    Rand rng(seed);
    const double seconds = (double)frames / sr;
    for (int c = 0; c < channels; ++c) {
        auto& d = s->ch[c];
        switch (kind) {
        case Sig::Sine:
            for (size_t i = 0; i < frames; ++i) d[i] = 0.5f * (float)std::sin(kTwoPi * (c ? 554.37 : 440.0) * (double)i / sr);
            break;
        case Sig::Sweep: {                                       // 30 Hz to 14 kHz (or 0.4 of the rate), rising by octaves; under the encoder's low-pass at every bitrate
            const double f0 = 30.0, f1 = std::min(14000.0, 0.4 * sr);
            double phase = c * 0.5;
            for (size_t i = 0; i < frames; ++i) {
                phase += kTwoPi * f0 * std::pow(f1 / f0, (double)i / (double)frames) / sr;
                d[i] = 0.5f * (float)std::sin(phase);
            }
            break;
        }
        case Sig::Noise:
            for (size_t i = 0; i < frames; ++i) d[i] = 0.4f * rng.uni();
            break;
        case Sig::Music:
            musicInto(d, sr, seed + 77u * (uint32)c, c ? 1.003 : 1.0);
            break;
        case Sig::Silence:
            break;
        case Sig::Clicks: {                                      // silence with short sharp bursts: the test for pre-echo
            for (int k = 0; k * 0.23 + 0.12 < seconds; ++k) {
                const size_t at = clickAt(k, sr), len = (size_t)(0.02 * sr);
                if (at + len + 8 >= frames) break;
                // noise smoothed three times over three samples: next to nothing is left above 17 kHz, where the
                // encoder's low-pass would otherwise be what the measurement sees
                std::vector<float> burst(len + 8, 0.0f);
                for (size_t i = 6; i < len + 6; ++i) burst[i] = 1.6f * rng.uni() * (float)std::exp(-(double)(i - 6) / (0.003 * sr));
                for (int pass = 0; pass < 3; ++pass)
                    for (size_t i = len + 7; i >= 2; --i) burst[i] = (burst[i] + burst[i - 1] + burst[i - 2]) * (1.0f / 3.0f);
                for (size_t i = 0; i < len + 8; ++i) d[at + i] = burst[i];
            }
            break;
        }
        case Sig::NearMono:
            break;                                               // filled in below
        }
    }
    if (kind == Sig::NearMono) {                                 // one piece of music in both channels, plus a small difference
        musicInto(s->ch[0], sr, seed, 1.0);
        if (channels > 1) {
            for (size_t i = 0; i < frames; ++i) {
                const float side = 0.01f * (float)std::sin(kTwoPi * 700.0 * (double)i / sr);
                s->ch[1][i] = s->ch[0][i] - side;
                s->ch[0][i] += side;
            }
        }
    }
    return s;
}

// Signal-to-noise ratio in dB of what came back against what went in, over all channels and the
// frames both have. 200 when nothing differs.
inline double snrDb(const SampleData& src, const SampleData& got, size_t from = 0, size_t to = (size_t)-1) {
    double sig = 0.0, err = 0.0;
    for (int c = 0; c < std::min(src.channels, got.channels); ++c) {
        const size_t n = std::min({src.ch[c].size(), got.ch[c].size(), to});
        for (size_t i = from; i < n; ++i) {
            const double a = src.ch[c][i], e = a - (double)got.ch[c][i];
            sig += a * a;
            err += e * e;
        }
    }
    if (err <= 0.0) return 200.0;
    if (sig <= 0.0) return -200.0;
    return 10.0 * std::log10(sig / err);
}

inline bool allFinite(const SampleData& s) {
    for (int c = 0; c < s.channels; ++c)
        for (float v : s.ch[c]) if (!std::isfinite(v)) return false;
    return true;
}

// ------------------------------------------------------------------ reading frames without the codec

struct GranuleInfo {
    int part23 = 0, bigValues = 0, globalGain = 0, compress = 0;
    bool switching = false, mixed = false, preflag = false, scale = false, count1Table = false;
    int blockType = 0;
    int table[3] = {0, 0, 0};
    int subblockGain[3] = {0, 0, 0};
    int region0 = 0, region1 = 0;
};
struct FrameInfo {
    size_t pos = 0;
    int bytes = 0;
    int version = 1;                // 1, 2, or 25 for MPEG-2.5
    int bitrateIndex = 0, kbps = 0, rate = 0;
    bool padding = false, crc = false;
    int mode = 0, modeExt = 0, channels = 0;
    int sideBytes = 0, samples = 0;
    int mainDataBegin = 0;
    int scfsi[2] = {0, 0};
    int granules = 0;
    GranuleInfo gr[2][2];
};

struct BitsIn {
    const unsigned char* p;
    size_t pos = 0;
    unsigned get(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; ++i, ++pos) v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1u);
        return v;
    }
};

// Reads the header at pos. False if it is not a Layer III header. bytes stays 0 for free format.
inline bool readHeader(const unsigned char* d, size_t size, size_t pos, FrameInfo& f) {
    static const int br1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int br2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    static const int rates[3] = {44100, 48000, 32000};
    if (pos + 4 > size || d[pos] != 0xFF || (d[pos + 1] & 0xE0) != 0xE0) return false;
    const int v = (d[pos + 1] >> 3) & 3, layer = (d[pos + 1] >> 1) & 3, bri = d[pos + 2] >> 4, sri = (d[pos + 2] >> 2) & 3;
    if (v == 1 || layer != 1 || bri == 15 || sri == 3) return false;
    f.pos = pos;
    f.version = v == 3 ? 1 : (v == 2 ? 2 : 25);
    f.bitrateIndex = bri;
    f.kbps = (v == 3 ? br1 : br2)[bri];
    f.rate = rates[sri] / (v == 3 ? 1 : (v == 2 ? 2 : 4));
    f.crc = (d[pos + 1] & 1) == 0;
    f.padding = (d[pos + 2] & 2) != 0;
    f.mode = d[pos + 3] >> 6;
    f.modeExt = (d[pos + 3] >> 4) & 3;
    f.channels = f.mode == 3 ? 1 : 2;
    f.samples = v == 3 ? 1152 : 576;
    f.sideBytes = v == 3 ? (f.channels == 1 ? 17 : 32) : (f.channels == 1 ? 9 : 17);
    f.bytes = bri ? (v == 3 ? 144000 : 72000) * f.kbps / f.rate + (f.padding ? 1 : 0) : 0;
    f.granules = v == 3 ? 2 : 1;
    return true;
}

inline void readSideInfo(const unsigned char* d, FrameInfo& f) {
    BitsIn b{d + f.pos + 4 + (f.crc ? 2 : 0)};
    const bool lsf = f.version != 1;
    f.mainDataBegin = (int)b.get(lsf ? 8 : 9);
    b.get(lsf ? (f.channels == 1 ? 1 : 2) : (f.channels == 1 ? 5 : 3));
    if (!lsf) for (int c = 0; c < f.channels; ++c) f.scfsi[c] = (int)b.get(4);
    for (int g = 0; g < f.granules; ++g)
        for (int c = 0; c < f.channels; ++c) {
            GranuleInfo& G = f.gr[g][c];
            G = GranuleInfo();
            G.part23 = (int)b.get(12);
            G.bigValues = (int)b.get(9);
            G.globalGain = (int)b.get(8);
            G.compress = (int)b.get(lsf ? 9 : 4);
            G.switching = b.get(1) != 0;
            if (G.switching) {
                G.blockType = (int)b.get(2);
                G.mixed = b.get(1) != 0;
                G.table[0] = (int)b.get(5);
                G.table[1] = (int)b.get(5);
                for (int i = 0; i < 3; ++i) G.subblockGain[i] = (int)b.get(3);
            } else {
                for (int i = 0; i < 3; ++i) G.table[i] = (int)b.get(5);
                G.region0 = (int)b.get(4);
                G.region1 = (int)b.get(3);
            }
            if (!lsf) G.preflag = b.get(1) != 0;
            G.scale = b.get(1) != 0;
            G.count1Table = b.get(1) != 0;
        }
}

// The frames of a stream that starts at a frame (after an ID3v2 tag if there is one) and has nothing
// between its frames. Free format frames are measured by looking for the next header like the first.
inline std::vector<FrameInfo> readFrames(const unsigned char* d, size_t size) {
    std::vector<FrameInfo> out;
    size_t pos = 0;
    if (size > 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3')
        pos = 10 + (((size_t)d[6] << 21) | ((size_t)d[7] << 14) | ((size_t)d[8] << 7) | (size_t)d[9]);
    int freeBytes = 0;
    FrameInfo f;
    while (readHeader(d, size, pos, f)) {
        if (f.bitrateIndex == 0) {
            if (!freeBytes)
                for (size_t q = pos + 4 + (size_t)f.sideBytes; q + 4 <= size; ++q)
                    if (d[q] == d[pos] && d[q + 1] == d[pos + 1] && (d[q + 2] & 0xFC) == (d[pos + 2] & 0xFC) && (d[q + 3] & 0xC0) == (d[pos + 3] & 0xC0)) {
                        freeBytes = (int)(q - pos) - (f.padding ? 1 : 0);
                        break;
                    }
            if (!freeBytes) break;
            f.bytes = freeBytes + (f.padding ? 1 : 0);
        }
        if (pos + (size_t)f.bytes > size) break;
        readSideInfo(d, f);
        out.push_back(f);
        pos += (size_t)f.bytes;
    }
    return out;
}
inline std::vector<FrameInfo> readFrames(const std::vector<unsigned char>& b) { return readFrames(b.data(), b.size()); }

// True if the frame is an Info or Xing frame (no audio).
inline bool isInfoFrame(const unsigned char* d, const FrameInfo& f) {
    const unsigned char* t = d + f.pos + 4 + f.sideBytes;
    return std::memcmp(t, "Info", 4) == 0 || std::memcmp(t, "Xing", 4) == 0;
}

} // namespace mp3t
