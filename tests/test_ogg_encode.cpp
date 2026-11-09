// Ogg Vorbis encoding: what encodeOgg writes, read back by our own decoder. (That other decoders read
// the same streams the same way is checked with ffmpeg and libsndfile during development, not here:
// see staging/ogg/NOTES.md.)
#include "Util.h"
#include "pulp/Ogg.h"
#include <filesystem>
using namespace pulp;
using namespace tu;

namespace {

struct Rand {
    uint32 s;
    explicit Rand(uint32 seed) : s(seed) {}
    uint32 next() { s = s * 1664525u + 1013904223u; return s >> 8; }
    float uni() { return (float)next() / 8388608.0f - 1.0f; }        // -1 to just under 1
};

std::shared_ptr<SampleData> blank(int channels, size_t frames, double sr = 44100.0) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = channels;
    for (int c = 0; c < channels; ++c) s->ch[c].assign(frames, 0.0f);
    return s;
}

enum class Sig { Sine, Sweep, Noise, Music, Silence };
const char* sigName(Sig k) {
    static const char* n[] = {"sine", "sweep", "noise", "music-like", "silence"};
    return n[(int)k];
}
std::shared_ptr<SampleData> signal(Sig kind, int channels, size_t frames, double sr = 44100.0, uint32 seed = 1) {
    auto s = blank(channels, frames, sr);
    Rand rng(seed);
    const double secs = (double)frames / sr;
    for (int c = 0; c < channels; ++c) {
        for (size_t i = 0; i < frames; ++i) {
            const double t = (double)i / sr;
            float v = 0.0f;
            switch (kind) {
            case Sig::Sine: v = 0.5f * (float)std::sin(kTwoPi * (c ? 554.37 : 440.0) * t); break;
            case Sig::Sweep: {      // 30 Hz up to 0.4 of the sample rate (the other way in the second channel)
                const double u = c ? secs - t : t, k = std::log(0.4 * sr / 30.0) / secs;
                v = 0.5f * (float)std::sin(kTwoPi * 30.0 * (std::exp(k * u) - 1.0) / k);
                break;
            }
            case Sig::Noise: v = 0.3f * rng.uni(); break;
            case Sig::Music: {      // a chord of decaying notes with overtones, struck twice a second, a noise burst on each stroke, a little hiss
                const double beat = std::fmod(t, 0.5), env = std::exp(-3.0 * beat);
                double x = 0.0;
                for (int h = 1; h <= 6; ++h) x += (0.2 / h) * std::sin(kTwoPi * (c ? 164.81 : 110.0) * h * t) + (0.12 / h) * std::sin(kTwoPi * 277.18 * h * t + c);
                v = (float)(env * x) + (beat < 0.03 ? 0.25f * (float)std::exp(-beat * 150.0) * rng.uni() : 0.0f) + 0.002f * rng.uni();
                break;
            }
            case Sig::Silence: break;
            }
            s->ch[c][i] = v;
        }
    }
    return s;
}

double snrDb(const std::vector<float>& a, const std::vector<float>& b) {
    double sig = 0.0, err = 0.0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) { sig += (double)a[i] * a[i]; err += ((double)a[i] - b[i]) * ((double)a[i] - b[i]); }
    return 10.0 * std::log10(std::max(sig, 1.0e-30) / std::max(err, 1.0e-30));
}

// The tests' own reading of the Ogg layer (see test_ogg_decode.cpp).
uint32 pageCrcOf(const unsigned char* p, size_t n) {
    uint32 c = 0;
    for (size_t i = 0; i < n; ++i) {
        c ^= (uint32)(i >= 22 && i < 26 ? 0 : p[i]) << 24;
        for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? ((c << 1) ^ 0x04c11db7u) : (c << 1);
    }
    return c;
}
inline uint32 le32At(const unsigned char* p) { return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24); }

// "" if the bytes are a well-formed Ogg stream holding `frames` frames, else what is wrong.
std::string checkStream(const std::vector<unsigned char>& f, int64 frames, int channels, double rate) {
    char b[200];
    size_t pos = 0;
    int page = 0;
    uint32 serial = 0;
    int64 lastGranule = 0;
    bool ended = false;
    size_t packetBytes = 0, biggestPage = 0;
    int packets = 0;
    while (pos < f.size()) {
        const unsigned char* p = f.data() + pos;
        if (ended) return "data after the last page";
        if (f.size() - pos < 27 || std::memcmp(p, "OggS", 4) != 0 || p[4] != 0) { snprintf(b, sizeof b, "no page at byte %zu", pos); return b; }
        const size_t segments = p[26];
        size_t body = 0;
        for (size_t i = 0; i < segments; ++i) body += p[27 + i];
        const size_t bytes = 27 + segments + body;
        if (f.size() - pos < bytes) return "the last page is cut short";
        if (pageCrcOf(p, bytes) != le32At(p + 22)) { snprintf(b, sizeof b, "page %d has a bad checksum", page); return b; }
        if (page == 0) serial = le32At(p + 14);
        if (le32At(p + 14) != serial || le32At(p + 18) != (uint32)page) { snprintf(b, sizeof b, "page %d has the wrong serial or sequence number", page); return b; }
        if (((p[5] & 2) != 0) != (page == 0)) { snprintf(b, sizeof b, "page %d: wrong start-of-stream flag", page); return b; }
        ended = (p[5] & 4) != 0;
        const int64 granule = (int64)((uint64)le32At(p + 6) | ((uint64)le32At(p + 10) << 32));
        bool endsPacket = false;
        for (size_t i = 0; i < segments; ++i) {
            packetBytes += p[27 + i];
            if (p[27 + i] < 255) { ++packets; packetBytes = 0; endsPacket = true; }
        }
        if (endsPacket ? granule < lastGranule : granule != -1) { snprintf(b, sizeof b, "page %d: position %lld after %lld", page, (long long)granule, (long long)lastGranule); return b; }
        if (endsPacket) lastGranule = granule;
        if (page == 0) {
            // the identification header alone on the first page
            if (segments != 1 || body != 30 || std::memcmp(p + 28, "\x01vorbis", 7) != 0) return "the first page is not a Vorbis identification header";
            if (p[28 + 11] != channels || le32At(p + 28 + 12) != (uint32)std::lround(rate)) return "the identification header has the wrong rate or channels";
        }
        if (page == 1 && std::memcmp(p + 27 + segments, "\x03vorbis", 7) != 0) return "the second page does not start with the comment header";
        biggestPage = std::max(biggestPage, bytes);
        pos += bytes;
        ++page;
    }
    if (!ended) return "no end-of-stream page";
    if (lastGranule != frames) { snprintf(b, sizeof b, "the last position is %lld, expected %lld", (long long)lastGranule, (long long)frames); return b; }
    if (packets < 5) return "fewer than two audio packets";
    if (biggestPage > 65307) return "a page is too big";
    return "";
}

// Encodes, checks the stream's form, decodes, checks the shape. Null (with the reason in why) if any of that fails.
std::shared_ptr<SampleData> roundTrip(const SampleData& src, float quality, std::string& why, size_t* bytes = nullptr) {
    const auto f = encodeOgg(src, quality);
    if (bytes) *bytes = f.size();
    const int channels = src.channels > 1 ? 2 : 1;
    why = checkStream(f, src.frames(), channels, src.sampleRate);
    if (!why.empty()) return nullptr;
    std::string error;
    auto got = readOggMemory(f.data(), f.size(), &error);
    if (!got) { why = "did not decode: " + error; return nullptr; }
    char b[160];
    if (got->channels != channels || got->frames() != src.frames() || got->sampleRate != std::round(src.sampleRate) || (channels == 2 && got->ch[1].size() != got->ch[0].size())) {
        snprintf(b, sizeof b, "%d channels, %lld frames, %g Hz; expected %d, %lld, %g", got->channels, (long long)got->frames(), got->sampleRate, channels, (long long)src.frames(), src.sampleRate);
        why = b;
        return nullptr;
    }
    return got;
}

} // namespace

TEST(ogg_round_trip_keeps_the_length_at_every_size) {
    // Very short inputs and lengths around the block sizes, at every block-size pair the encoder uses.
    for (double sr : {8000.0, 22050.0, 44100.0, 96000.0})
        for (int channels : {1, 2})
            for (size_t frames : {(size_t)0, (size_t)1, (size_t)2, (size_t)3, (size_t)31, (size_t)127, (size_t)128, (size_t)129, (size_t)255, (size_t)511, (size_t)1023, (size_t)1024, (size_t)1025, (size_t)2049, (size_t)4096, (size_t)10000}) {
                const auto src = signal(Sig::Music, channels, frames, sr, 7);
                std::string why;
                const auto got = roundTrip(*src, 0.5f, why);
                CHECK_MSG(got != nullptr, "%zu frames, %d channels, %g Hz: %s", frames, channels, sr, why.c_str());
                // a short input still sounds like itself: its level comes back within 3 dB
                if (got && frames >= 1024 && frames < 100000) {
                    const double in = rmsOf(src->ch[0]), out = rmsOf(got->ch[0]);
                    CHECK_MSG(out > in * 0.7 && out < in * 1.41, "%zu frames, %d channels, %g Hz: level %g became %g", frames, channels, sr, in, out);
                }
            }
}

TEST(ogg_round_trip_quality_and_size) {
    // Signal-to-noise ratios over the whole signal, in dB, that each kind of signal must reach at
    // qualities 0.2, 0.6 and 1.0. The encoder is perceptual: it spends accuracy on tones and low
    // frequencies and lets noise be replaced by noise of the same level, so noise has little SNR by
    // design and is checked by its level instead (every signal's level must come back within 1 dB;
    // measured within 0.25 dB); and a sweep this fast (the whole range in three quarters of a second)
    // is never a steady tone in any block. The measured figures are 2 to 4 dB above these: sine 26.3
    // to 28.2 / 37.7 to 40.6 / 59.6 to 60.5, sweep 6.0 / 15.1 / 32.0, music-like 19.0 / 27.2 / 44.8,
    // noise 1.1 / 7.3 / 23.9.
    struct Want { Sig kind; double snr[3]; };
    const Want wants[] = {{Sig::Sine, {24.0, 36.0, 56.0}}, {Sig::Sweep, {4.0, 13.0, 29.0}}, {Sig::Music, {16.0, 24.0, 41.0}}, {Sig::Noise, {-1.0, 5.0, 21.0}}};
    const float qualities[3] = {0.2f, 0.6f, 1.0f};
    const size_t frames = 33075;        // three quarters of a second
    for (const Want& want : wants)
        for (int channels : {1, 2}) {
            const auto src = signal(want.kind, channels, frames, 44100.0, 3);
            size_t sizes[3] = {};
            for (int q = 0; q < 3; ++q) {
                std::string why;
                const auto got = roundTrip(*src, qualities[q], why, &sizes[q]);
                CHECK_MSG(got != nullptr, "%s, %d channels, quality %.1f: %s", sigName(want.kind), channels, qualities[q], why.c_str());
                if (!got) continue;
                for (int c = 0; c < channels; ++c) {
                    const double snr = snrDb(src->ch[c], got->ch[c]);
                    CHECK_MSG(snr >= want.snr[q], "%s, %d channels, quality %.1f: channel %d SNR %.1f dB, expected %.1f", sigName(want.kind), channels, qualities[q], c, snr, want.snr[q]);
                    const double in = rmsOf(src->ch[c]), out = rmsOf(got->ch[c]);
                    CHECK_MSG(out > in * 0.89 && out < in * 1.12, "%s, %d channels, quality %.1f: level %g became %g", sigName(want.kind), channels, qualities[q], in, out);
                }
            }
            CHECK_MSG(sizes[0] < sizes[1] && sizes[1] < sizes[2], "%s, %d channels: %zu, %zu, %zu bytes at qualities 0.2, 0.6, 1.0", sigName(want.kind), channels, sizes[0], sizes[1], sizes[2]);
            // and every one of them is smaller than 16-bit PCM
            CHECK_MSG(sizes[2] < frames * 2 * (size_t)channels, "%s, %d channels: %zu bytes at quality 1", sigName(want.kind), channels, sizes[2]);
        }
}

TEST(ogg_size_falls_as_quality_falls) {
    const auto src = signal(Sig::Music, 2, 44100, 44100.0, 5);
    size_t before = 0;
    double snrBefore = -100.0;
    for (int step = 0; step <= 10; ++step) {
        std::string why;
        size_t bytes = 0;
        const auto got = roundTrip(*src, (float)step / 10.0f, why, &bytes);
        CHECK_MSG(got != nullptr, "quality %.1f: %s", step / 10.0, why.c_str());
        if (!got) continue;
        const double snr = snrDb(src->ch[0], got->ch[0]);
        CHECK_MSG(bytes > before, "quality %.1f: %zu bytes after %zu", step / 10.0, bytes, before);
        CHECK_MSG(snr > snrBefore - 0.5, "quality %.1f: SNR %.1f dB after %.1f", step / 10.0, snr, snrBefore);
        before = bytes;
        snrBefore = snr;
    }
    // out-of-range qualities are the ends of the scale, and the default is 0.6
    CHECK(encodeOgg(*src, -3.0f) == encodeOgg(*src, 0.0f));
    CHECK(encodeOgg(*src, 7.0f) == encodeOgg(*src, 1.0f));
    CHECK(encodeOgg(*src) == encodeOgg(*src, 0.6f));
    // the same input gives the same bytes every time
    CHECK(encodeOgg(*src, 0.6f) == encodeOgg(*src, 0.6f));
}

TEST(ogg_silence_is_tiny_and_stays_silent) {
    for (int channels : {1, 2}) {
        const auto src = blank(channels, 100000);
        std::string why;
        size_t bytes = 0;
        const auto got = roundTrip(*src, 0.6f, why, &bytes);
        CHECK_MSG(got != nullptr, "%s", why.c_str());
        if (!got) continue;
        CHECK_MSG(got->peak() == 0.0f, "peak %g", got->peak());
        CHECK_MSG(bytes < 2500, "%zu bytes", bytes);
        // silence around a sound stays silent away from it
        auto mixed = blank(channels, 60000);
        const auto tone = signal(Sig::Sine, channels, 10000);
        for (int c = 0; c < channels; ++c) std::copy(tone->ch[c].begin(), tone->ch[c].end(), mixed->ch[c].begin() + 25000);
        const auto back = roundTrip(*mixed, 0.6f, why);
        CHECK_MSG(back != nullptr, "%s", why.c_str());
        if (!back) continue;
        CHECK_MSG(peakOf(back->ch[0], 0, 20000) < 1.0e-4f && peakOf(back->ch[0], 40000, 60000) < 1.0e-4f, "peaks %g and %g in the silence", peakOf(back->ch[0], 0, 20000), peakOf(back->ch[0], 40000, 60000));
    }
}

TEST(ogg_stereo_keeps_the_channels_apart) {
    const size_t frames = 30000;
    const auto tone = signal(Sig::Sine, 1, frames);
    const auto other = signal(Sig::Sweep, 1, frames);
    struct Case { const char* what; float left, right, otherRight; double snr; };
    // left = tone * left; right = tone * right + sweep * otherRight. Less is asked of a channel with the sweep in it.
    const Case cases[] = {{"the same in both", 1, 1, 0, 36.0}, {"opposite phase", 1, -1, 0, 36.0}, {"left only", 1, 0, 0, 36.0}, {"right only", 0, 1, 0, 36.0},
                          {"different signals", 1, 0, 1, 12.0}, {"nearly the same", 1, 0.9f, 0.05f, 25.0}};
    for (const Case& k : cases) {
        auto src = blank(2, frames);
        for (size_t i = 0; i < frames; ++i) {
            src->ch[0][i] = tone->ch[0][i] * k.left;
            src->ch[1][i] = tone->ch[0][i] * k.right + other->ch[0][i] * k.otherRight;
        }
        std::string why;
        const auto got = roundTrip(*src, 0.6f, why);
        CHECK_MSG(got != nullptr, "%s: %s", k.what, why.c_str());
        if (!got) continue;
        for (int c = 0; c < 2; ++c) {
            if (peakOf(src->ch[c]) == 0.0f) CHECK_MSG(peakOf(got->ch[c]) < 1.0e-4f, "%s: the silent channel has %g in it", k.what, peakOf(got->ch[c]));
            else CHECK_MSG(snrDb(src->ch[c], got->ch[c]) > k.snr, "%s: channel %d SNR %.1f dB, expected %.0f", k.what, c, snrDb(src->ch[c], got->ch[c]), k.snr);
        }
    }
    // the second channel missing or short counts as silence there
    auto lopsided = signal(Sig::Sine, 2, 20000);
    lopsided->ch[1].resize(5000);
    std::string why;
    const auto got = roundTrip(*lopsided, 0.6f, why);
    CHECK_MSG(got != nullptr, "%s", why.c_str());
    CHECK(got && peakOf(got->ch[1], 9000, 20000) < 1.0e-3f && snrDb(lopsided->ch[0], got->ch[0]) > 30.0);
}

TEST(ogg_attacks_do_not_ring_before_they_start) {
    // Silence, then a burst of noise: what the decoder gives before the burst must stay far below it.
    // (This is what the short blocks are for; with long blocks alone the noise before each burst is
    // only some 20 dB down.)
    for (int channels : {1, 2}) {
        auto src = blank(channels, 60000);
        Rand rng(11);
        const size_t hits[3] = {10000, 30011, 50777};
        for (size_t h : hits)
            for (size_t i = 0; i < 3000; ++i)
                for (int c = 0; c < channels; ++c) src->ch[c][h + i] = 0.8f * rng.uni() * (float)std::exp(-(double)i / 300.0);
        for (float quality : {0.3f, 0.6f, 0.9f}) {
            std::string why;
            const auto got = roundTrip(*src, quality, why);
            CHECK_MSG(got != nullptr, "%s", why.c_str());
            if (!got) continue;
            for (size_t h : hits) {
                // from 1500 samples to 2 ms before the burst, and the 2 ms right before it
                const double far = rmsOf(got->ch[0], h - 1500, h - 88), near = peakOf(got->ch[0], h - 88, h);
                const double limit = quality < 0.5f ? 0.012 : 0.004;
                CHECK_MSG(far < limit, "quality %.1f, %d channels, burst at %zu: %.4f RMS before it", quality, channels, h, far);
                CHECK_MSG(near < 0.4, "quality %.1f, %d channels, burst at %zu: peak %.3f just before it", quality, channels, h, near);
            }
        }
    }
}

TEST(ogg_encoder_copes_with_awkward_input) {
    std::string why;
    // louder than full scale: floating point files can be
    auto loud = signal(Sig::Sine, 1, 20000);
    for (auto& v : loud->ch[0]) v *= 6.0f;
    auto got = roundTrip(*loud, 0.6f, why);
    CHECK_MSG(got && snrDb(loud->ch[0], got->ch[0]) > 30.0, "loud: %s", why.c_str());
    // very quiet: the result is as good as for the same thing at full level (the encoder works relative to the file's peak)
    auto quiet = signal(Sig::Music, 1, 20000);
    const auto normal = roundTrip(*quiet, 0.6f, why);
    for (auto& v : quiet->ch[0]) v *= 1.0e-3f;
    got = roundTrip(*quiet, 0.6f, why);
    CHECK_MSG(got && normal && snrDb(quiet->ch[0], got->ch[0]) > snrDb(signal(Sig::Music, 1, 20000)->ch[0], normal->ch[0]) - 1.5, "quiet: %s", why.c_str());
    // things that are not numbers count as silence, and the output has none
    auto broken = signal(Sig::Sine, 2, 20000);
    broken->ch[0][5000] = std::numeric_limits<float>::quiet_NaN();
    broken->ch[1][7000] = std::numeric_limits<float>::infinity();
    broken->ch[1][7001] = -std::numeric_limits<float>::infinity();
    got = roundTrip(*broken, 0.6f, why);
    CHECK_MSG(got != nullptr, "not numbers: %s", why.c_str());
    if (got) {
        bool finite = true;
        for (int c = 0; c < 2; ++c) for (float v : got->ch[c]) finite = finite && std::isfinite(v);
        CHECK(finite);
    }
    // a constant, a single click, a full-scale square wave
    auto dc = blank(1, 20000);
    std::fill(dc->ch[0].begin(), dc->ch[0].end(), 0.25f);
    got = roundTrip(*dc, 0.6f, why);
    CHECK_MSG(got && std::fabs(got->ch[0][10000] - 0.25f) < 0.01f, "constant: %s", why.c_str());
    auto click = blank(2, 20000);
    click->ch[0][9000] = 1.0f;
    click->ch[1][9001] = -1.0f;
    got = roundTrip(*click, 0.8f, why);
    CHECK_MSG(got && got->ch[0][9000] > 0.7f && got->ch[1][9001] < -0.7f && peakOf(got->ch[0], 0, 8000) < 0.01f, "click: %s", why.c_str());
    auto square = blank(1, 20000);
    for (size_t i = 0; i < 20000; ++i) square->ch[0][i] = ((i / 50) & 1) ? 1.0f : -1.0f;
    got = roundTrip(*square, 0.6f, why);
    CHECK_MSG(got && snrDb(square->ch[0], got->ch[0]) > 18.0, "square: %s", why.c_str());
    // odd sample rates, including none at all (taken as 44100)
    for (double sr : {1000.0, 11025.0, 37800.0, 192000.0, 384000.0}) {
        const auto src = signal(Sig::Music, 2, 6000, sr);
        got = roundTrip(*src, 0.6f, why);
        CHECK_MSG(got != nullptr, "%g Hz: %s", sr, why.c_str());
    }
    auto norate = signal(Sig::Sine, 1, 3000);
    norate->sampleRate = 0.0;
    const auto f = encodeOgg(*norate);
    const auto back = readOggMemory(f.data(), f.size());
    CHECK(back && back->sampleRate == 44100.0 && back->frames() == 3000);
    // more than two channels are not something SampleData holds; a channel count above two means two
    auto wide = signal(Sig::Sine, 2, 3000);
    wide->channels = 5;
    const auto f2 = encodeOgg(*wide);
    const auto back2 = readOggMemory(f2.data(), f2.size());
    CHECK(back2 && back2->channels == 2 && back2->frames() == 3000);
}

TEST(ogg_long_file_round_trip) {
    // Long enough for hundreds of pages; the positions on them must stay exact.
    const auto src = signal(Sig::Music, 2, 44100 * 12, 44100.0, 9);
    std::string why;
    size_t bytes = 0;
    const auto got = roundTrip(*src, 0.4f, why, &bytes);
    CHECK_MSG(got != nullptr, "%s", why.c_str());
    if (!got) return;
    CHECK_MSG(snrDb(src->ch[0], got->ch[0]) > 15.0 && snrDb(src->ch[1], got->ch[1]) > 15.0, "SNR %.1f and %.1f dB", snrDb(src->ch[0], got->ch[0]), snrDb(src->ch[1], got->ch[1]));
    // the two ends line up with the source to the sample: correlate a stretch at each end
    for (size_t from : {(size_t)0, src->ch[0].size() - 20000}) {
        double best = -1.0;
        int bestLag = 99;
        for (int lag = -3; lag <= 3; ++lag) {
            double sum = 0.0;
            for (size_t i = 100; i < 19900; ++i) sum += (double)src->ch[0][from + i] * got->ch[0][(size_t)((int64)(from + i) + lag)];
            if (sum > best) { best = sum; bestLag = lag; }
        }
        CHECK_MSG(bestLag == 0, "the audio at %zu is shifted by %d samples", from, bestLag);
    }
    CHECK_MSG(bytes > 100000 && bytes < 44100 * 12 * 4 / 6, "%zu bytes", bytes);
}

TEST(ogg_file_round_trip_with_a_non_ascii_path) {
    const std::string dir = t::tmpDir() + "/ogg";
    std::filesystem::create_directories(std::filesystem::path(std::u8string(dir.begin(), dir.end())));
    const std::string path = dir + "/caf\xC3\xA9 \xE2\x99\xAA \xE9\x9F\xB3.ogg";        // "café ♪ 音.ogg" in UTF-8
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
    const auto src = signal(Sig::Music, 2, 12345, 48000.0);
    std::string error;
    CHECK_MSG(writeOggFile(path, *src, 0.7f, &error), "%s", error.c_str());
    CHECK(std::filesystem::exists(std::filesystem::path(std::u8string(path.begin(), path.end()))));
    const auto got = readOggFile(path, &error);
    CHECK_MSG(got != nullptr, "%s", error.c_str());
    CHECK(got && got->path == path && got->frames() == 12345 && got->channels == 2 && got->sampleRate == 48000.0);
    CHECK(got && snrDb(src->ch[0], got->ch[0]) > 20.0);
    // the file on disk is what encodeOgg returns, and reading it is the same as reading those bytes
    std::vector<unsigned char> onDisk;
    CHECK(readFileBytes(path, onDisk) && onDisk == encodeOgg(*src, 0.7f));
    const auto direct = readOggMemory(onDisk.data(), onDisk.size());
    CHECK(got && direct && identical(*got, *direct));
    // the default quality through a file too
    CHECK(writeOggFile(path, *src));
    CHECK(readFileBytes(path, onDisk) && onDisk == encodeOgg(*src, 0.6f));
    // a path that cannot be written reports it
    error.clear();
    CHECK(!writeOggFile(path + "/x.ogg", *src, 0.6f, &error) && error.find("cannot write") == 0);
    CHECK(!writeOggFile(path + "/x.ogg", *src));
}
