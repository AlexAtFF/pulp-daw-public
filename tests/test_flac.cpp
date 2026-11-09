// FLAC: the encoder and decoder against each other, against damage, and the decoder against files
// made by other encoders (embedded at the end of this file).
#include "Util.h"
#include "pulp/Flac.h"
#include <filesystem>
using namespace pulp;
using namespace tu;

namespace {

// A small deterministic generator, so every run tests the same signals.
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

enum class Sig { Noise, QuietNoise, Sine, Silence, Constant, Square, Music };
const char* sigName(Sig k) {
    static const char* n[] = {"noise", "quiet noise", "sine", "silence", "constant", "square", "music-like"};
    return n[(int)k];
}
std::shared_ptr<SampleData> signal(Sig kind, int channels, size_t frames, double sr = 44100.0, uint32 seed = 1) {
    auto s = blank(channels, frames, sr);
    Rand rng(seed);
    for (int c = 0; c < channels; ++c) {
        for (size_t i = 0; i < frames; ++i) {
            const double t = (double)i / sr;
            float v = 0.0f;
            switch (kind) {
            case Sig::Noise: v = rng.uni(); break;
            case Sig::QuietNoise: v = 0.001f * rng.uni(); break;
            case Sig::Sine: v = 0.8f * (float)std::sin(kTwoPi * (c ? 554.37 : 440.0) * t); break;
            case Sig::Silence: break;
            case Sig::Constant: v = c ? -0.25f : 0.3f; break;
            case Sig::Square: v = ((i / 50 + (size_t)c) & 1) ? 1.0f : -1.0f; break;        // full scale: +1 has to clamp
            case Sig::Music: v = 0.5f * (float)std::sin(kTwoPi * 220.0 * t) + 0.2f * (float)std::sin(kTwoPi * (c ? 3301.0 : 3300.0) * t + 1.0) + 0.01f * rng.uni(); break;
            }
            s->ch[c][i] = v;
        }
    }
    return s;
}

// The integer the encoder stores for a float: the same rounding and clamping as the WAV writer.
int quantise(float v, int bits) {
    const double scale = bits == 16 ? 32768.0 : 8388608.0;
    const long long q = std::llround((double)v * scale);
    return (int)clampT<long long>(q, -(long long)scale, (long long)scale - 1);
}

// "" if the decoded audio holds exactly the integers the source rounds to, else what differs.
std::string compare(const SampleData& src, const SampleData* got, int bits, const std::string& error) {
    char b[200];
    if (!got) return "did not decode: " + error;
    if (got->channels != src.channels) { snprintf(b, sizeof b, "%d channels, expected %d", got->channels, src.channels); return b; }
    if (got->frames() != src.frames()) { snprintf(b, sizeof b, "%lld frames, expected %lld", (long long)got->frames(), (long long)src.frames()); return b; }
    if (got->sampleRate != std::round(src.sampleRate)) { snprintf(b, sizeof b, "rate %g, expected %g", got->sampleRate, src.sampleRate); return b; }
    const double scale = bits == 16 ? 32768.0 : 8388608.0;
    for (int c = 0; c < src.channels; ++c)
        for (size_t i = 0; i < src.ch[c].size(); ++i) {
            const double back = (double)got->ch[c][i] * scale;
            if (back != (double)quantise(src.ch[c][i], bits)) {
                snprintf(b, sizeof b, "channel %d sample %zu is %.3f, expected %d", c, i, back, quantise(src.ch[c][i], bits));
                return b;
            }
        }
    return "";
}
std::string roundTrip(const SampleData& src, int bits, size_t* encodedBytes = nullptr) {
    const auto bytes = encodeFlac(src, bits);
    if (encodedBytes) *encodedBytes = bytes.size();
    std::string error;
    const auto got = readFlacMemory(bytes.data(), bytes.size(), &error);
    return compare(src, got.get(), bits, error);
}

// Where things are in a file from encodeFlac: "fLaC", a 4-byte block header, 34 bytes of STREAMINFO, then frames.
const size_t kInfoAt = 8, kFirstFrame = 42;
// How the channels of the first frame are stored: 0 or 1 for plain mono or stereo, 8 left/side, 9 side/right, 10 mid/side.
int firstFrameChannelCode(const std::vector<unsigned char>& f) { return f.size() > kFirstFrame + 3 ? f[kFirstFrame + 3] >> 4 : -1; }
// The subframe type of the first channel of the first frame, for a first frame with a short header (frame 0 at 4096 samples and a standard rate).
int firstSubframeType(const std::vector<unsigned char>& f) { return f.size() > kFirstFrame + 6 ? (f[kFirstFrame + 6] >> 1) & 63 : -1; }

// Where each frame of a file from encodeFlac starts, found by the frame numbers in the headers (so for files of under 128 frames).
std::vector<size_t> frameStarts(const std::vector<unsigned char>& f) {
    std::vector<size_t> at;
    for (size_t i = kFirstFrame; i + 5 < f.size(); ++i)
        if (f[i] == 0xFF && f[i + 1] == 0xF8 && (f[i + 2] & 15) == (f[kFirstFrame + 2] & 15) && (f[i + 3] & 15) == (f[kFirstFrame + 3] & 15) && f[i + 4] == at.size()) at.push_back(i);
    return at;
}

// FNV-1a over the samples as 32-bit little-endian integers, channels interleaved. The reference streams
// below carry the same hash of what their own encoder was given.
uint64 hashSamples(const SampleData& s, int bits) {
    const double scale = std::ldexp(1.0, bits - 1);
    uint64 h = 1469598103934665603ull;
    for (size_t i = 0; i < s.ch[0].size(); ++i)
        for (int c = 0; c < s.channels; ++c) {
            const uint32 v = (uint32)(int)std::llround((double)s.ch[c][i] * scale);
            for (int k = 0; k < 4; ++k) { h ^= (v >> (8 * k)) & 255u; h *= 1099511628211ull; }
        }
    return h;
}

} // namespace

TEST(flac_round_trip_is_exact_for_every_kind_of_signal) {
    for (int bits : {16, 24})
        for (int channels : {1, 2})
            for (Sig kind : {Sig::Noise, Sig::QuietNoise, Sig::Sine, Sig::Silence, Sig::Constant, Sig::Square, Sig::Music}) {
                const auto s = signal(kind, channels, 10000);
                const std::string why = roundTrip(*s, bits);
                CHECK_MSG(why.empty(), "%s, %d bits, %d channels: %s", sigName(kind), bits, channels, why.c_str());
            }
}

TEST(flac_round_trip_is_exact_at_every_length_around_a_block) {
    for (size_t frames : {(size_t)0, (size_t)1, (size_t)2, (size_t)3, (size_t)5, (size_t)255, (size_t)256, (size_t)257, (size_t)4095, (size_t)4096, (size_t)4097, (size_t)8192, (size_t)100000})
        for (int bits : {16, 24})
            for (int channels : {1, 2}) {
                const auto s = signal(Sig::Music, channels, frames, 44100.0, 7);
                const std::string why = roundTrip(*s, bits);
                CHECK_MSG(why.empty(), "%zu frames, %d bits, %d channels: %s", frames, bits, channels, why.c_str());
            }
    // odd lengths of noise too, where the last short block is stored unpredicted
    for (size_t frames : {(size_t)1, (size_t)4, (size_t)17, (size_t)4097}) {
        const std::string why = roundTrip(*signal(Sig::Noise, 2, frames), 24);
        CHECK_MSG(why.empty(), "%zu frames of noise: %s", frames, why.c_str());
    }
}

TEST(flac_round_trip_keeps_the_sample_rate) {
    // the usual rates, and ones a frame header has to spell out in kHz, Hz or tens of Hz, or cannot state at all
    for (double sr : {44100.0, 48000.0, 96000.0, 8000.0, 11025.0, 22050.0, 32000.0, 88200.0, 176400.0, 192000.0, 12345.0, 50000.0, 250000.0, 384000.0, 123457.0, 655350.0, 1.0})
        for (int bits : {16, 24}) {
            const auto s = signal(Sig::Music, 2, 5000, sr);
            const std::string why = roundTrip(*s, bits);
            CHECK_MSG(why.empty(), "%g Hz, %d bits: %s", sr, bits, why.c_str());
        }
    // a rate FLAC cannot hold is brought into range rather than written wrong
    auto s = signal(Sig::Sine, 1, 100, 1.0e6);
    auto got = readFlacMemory(encodeFlac(*s, 16).data(), encodeFlac(*s, 16).size());
    CHECK(got && got->sampleRate == 655350.0);
}

TEST(flac_stream_info_describes_the_audio) {
    const auto s = signal(Sig::Music, 2, 10000, 48000.0);
    const auto f = encodeFlac(*s, 24);
    CHECK(f.size() > kFirstFrame && std::memcmp(f.data(), "fLaC", 4) == 0);
    CHECK(f[4] == 0x80 && f[5] == 0 && f[6] == 0 && f[7] == 34);         // the last metadata block, STREAMINFO, 34 bytes
    const unsigned char* p = f.data() + kInfoAt;
    CHECK(((p[0] << 8) | p[1]) == 4096 && ((p[2] << 8) | p[3]) == 4096);
    const unsigned minFrame = (p[4] << 16) | (p[5] << 8) | p[6], maxFrame = (p[7] << 16) | (p[8] << 8) | p[9];
    CHECK(minFrame > 0 && minFrame <= maxFrame && (size_t)maxFrame < f.size() - kFirstFrame);
    CHECK((((unsigned)p[10] << 12) | (p[11] << 4) | (p[12] >> 4)) == 48000u);
    CHECK(((p[12] >> 1) & 7) == 1);                                       // channels - 1
    CHECK(((((unsigned)p[12] & 1) << 4) | (p[13] >> 4)) == 23u);          // bits - 1
    CHECK((((uint64)(p[13] & 15) << 32) | ((uint64)p[14] << 24) | (p[15] << 16) | (p[16] << 8) | p[17]) == 10000u);
    // a file of one frame: both frame sizes are that frame
    const auto one = encodeFlac(*signal(Sig::Music, 1, 1000), 16);
    const unsigned char* q = one.data() + kInfoAt;
    CHECK((size_t)((q[4] << 16) | (q[5] << 8) | q[6]) == one.size() - kFirstFrame);
    CHECK((size_t)((q[7] << 16) | (q[8] << 8) | q[9]) == one.size() - kFirstFrame);
    // anything but 16 means 24, and only the first two channels' worth is written for a mono sample
    CHECK(encodeFlac(*s, 0) == f && encodeFlac(*s, 32) == f);
}

TEST(flac_md5_of_the_audio_matches_reference_values) {
    // Expected values are Python's hashlib.md5 of the same samples as little-endian PCM.
    auto md5 = [](const std::vector<unsigned char>& f) {
        char b[33];
        for (int i = 0; i < 16; ++i) snprintf(b + i * 2, 3, "%02x", f[kInfoAt + 18 + (size_t)i]);
        return std::string(b, 32);
    };
    CHECK(md5(encodeFlac(*blank(1, 0), 16)) == "d41d8cd98f00b204e9800998ecf8427e");      // no audio at all
    // 16-bit mono: sample i is (i * 37) mod 65536 - 32768
    auto a = blank(1, 1000);
    for (int i = 0; i < 1000; ++i) a->ch[0][(size_t)i] = (float)((i * 37) % 65536 - 32768) / 32768.0f;
    CHECK_MSG(md5(encodeFlac(*a, 16)) == "3f990f19edef63bee1fcd0940aee687f", "got %s", md5(encodeFlac(*a, 16)).c_str());
    // 24-bit stereo, 5000 frames (more than a block): left is (i * 7919) mod 2^24 - 2^23, right is (i * 104729 + 12345) mod 2^24 - 2^23
    auto b = blank(2, 5000);
    for (int i = 0; i < 5000; ++i) {
        b->ch[0][(size_t)i] = (float)((int)(((long long)i * 7919) % 16777216) - 8388608) / 8388608.0f;
        b->ch[1][(size_t)i] = (float)((int)(((long long)i * 104729 + 12345) % 16777216) - 8388608) / 8388608.0f;
    }
    CHECK_MSG(md5(encodeFlac(*b, 24)) == "43fb3208d925de1b14fafbf2256dd1a5", "got %s", md5(encodeFlac(*b, 24)).c_str());
    // lengths either side of MD5's own 64-byte blocks and its padding rule (55, 56, 63, 64 and 65 bytes of 16-bit mono are not whole samples, so 54 to 66)
    static const char* expect[] = {"9d5b84a7367209938f470210d83d3d52", "018177bb66687bc32a088c515c528ce4", "f72efeeb8b6ff1fda8ff71addc86e202", "55b9398adbc4893649b8699a0ab56536",
                                   "f3a2a998db909a9bdef8ef19000907f4", "f1a57e6a9cf68170cba1533df4ea8f4d", "d7e76235cda8338e68927a17a26254b4"};
    for (int n = 27; n <= 33; ++n) {
        auto c = blank(1, (size_t)n);
        for (int i = 0; i < n; ++i) c->ch[0][(size_t)i] = (float)(i * 1000 - 9000) / 32768.0f;
        CHECK_MSG(md5(encodeFlac(*c, 16)) == expect[n - 27], "%d samples: got %s", n, md5(encodeFlac(*c, 16)).c_str());
    }
}

TEST(flac_stereo_is_stored_the_cheapest_way_for_each_block) {
    const size_t n = 4096;      // one block, so the first frame is the whole file
    Rand rng(99);
    // strongly correlated: the right channel is the left plus a whisper
    auto close = signal(Sig::Music, 2, n);
    for (size_t i = 0; i < n; ++i) close->ch[1][i] = close->ch[0][i] + 0.0005f * rng.uni();
    const auto closeBytes = encodeFlac(*close, 16);
    CHECK_MSG(firstFrameChannelCode(closeBytes) >= 8, "channel code %d", firstFrameChannelCode(closeBytes));
    auto left = blank(1, n), right = blank(1, n);
    left->ch[0] = close->ch[0];
    right->ch[0] = close->ch[1];
    const size_t apart = encodeFlac(*left, 16).size() + encodeFlac(*right, 16).size();
    CHECK_MSG(closeBytes.size() < apart * 8 / 10, "%zu bytes together, %zu apart", closeBytes.size(), apart);
    CHECK(roundTrip(*close, 16).empty() && roundTrip(*close, 24).empty());

    // a shared part plus separate noise in each channel: the average is quieter than either, so mid/side wins
    auto shared = blank(2, n);
    for (size_t i = 0; i < n; ++i) {
        const float common = 0.2f * rng.uni();
        shared->ch[0][i] = common + 0.2f * rng.uni();
        shared->ch[1][i] = common + 0.2f * rng.uni();
    }
    for (int bits : {16, 24}) {
        CHECK_MSG(firstFrameChannelCode(encodeFlac(*shared, bits)) == 10, "%d bits: channel code %d", bits, firstFrameChannelCode(encodeFlac(*shared, bits)));
        CHECK(roundTrip(*shared, bits).empty());
    }

    // a clean tone on one side and the same tone with noise on the other: the noise alone is the cheap thing to store
    auto tone = signal(Sig::Sine, 2, n);
    tone->ch[1] = tone->ch[0];
    auto leftSide = std::make_shared<SampleData>(*tone), sideRight = std::make_shared<SampleData>(*tone);
    for (size_t i = 0; i < n; ++i) {
        const float hiss = 0.01f * rng.uni();
        leftSide->ch[1][i] += hiss;
        sideRight->ch[0][i] += hiss;
    }
    for (int bits : {16, 24}) {
        CHECK_MSG(firstFrameChannelCode(encodeFlac(*leftSide, bits)) == 8, "%d bits: channel code %d", bits, firstFrameChannelCode(encodeFlac(*leftSide, bits)));
        CHECK_MSG(firstFrameChannelCode(encodeFlac(*sideRight, bits)) == 9, "%d bits: channel code %d", bits, firstFrameChannelCode(encodeFlac(*sideRight, bits)));
        CHECK(roundTrip(*leftSide, bits).empty() && roundTrip(*sideRight, bits).empty());
    }

    // unrelated channels stay as they are
    CHECK(firstFrameChannelCode(encodeFlac(*signal(Sig::Noise, 2, n), 16)) == 1);
    CHECK(firstFrameChannelCode(encodeFlac(*signal(Sig::Noise, 1, n), 16)) == 0);
}

TEST(flac_encoder_picks_the_subframe_type_that_fits_the_signal) {
    const size_t n = 4096;
    CHECK(firstSubframeType(encodeFlac(*signal(Sig::Silence, 1, n), 16)) == 0);       // constant
    CHECK(firstSubframeType(encodeFlac(*signal(Sig::Constant, 1, n), 24)) == 0);
    CHECK(firstSubframeType(encodeFlac(*signal(Sig::Noise, 1, n), 16)) == 1);         // verbatim: full-scale noise cannot be predicted
    const int sine = firstSubframeType(encodeFlac(*signal(Sig::Sine, 1, n), 16));
    CHECK_MSG(sine >= 10 && sine <= 12, "subframe type %d", sine);                    // a fixed predictor of order 2 to 4
    const int quiet = firstSubframeType(encodeFlac(*signal(Sig::QuietNoise, 1, n), 16));
    CHECK_MSG(quiet == 8, "subframe type %d", quiet);                                 // order 0: noise is only made worse by predicting
    // 16-bit audio in a 24-bit file: the eight empty low bits are not stored
    auto coarse = signal(Sig::Music, 1, n);
    for (float& v : coarse->ch[0]) v = (float)quantise(v, 16) / 32768.0f;
    const auto as24 = encodeFlac(*coarse, 24), as16 = encodeFlac(*coarse, 16);
    CHECK((as24[kFirstFrame + 6] & 1) == 1);                                          // the wasted-bits flag
    CHECK_MSG(as24.size() <= as16.size() + 2, "%zu bytes as 24-bit, %zu as 16-bit", as24.size(), as16.size());
    CHECK(roundTrip(*coarse, 24).empty());
    // residuals too wide for 4-bit Rice parameters: loud noise in 24 bits, kept just under full scale so it is still predicted
    auto loud = signal(Sig::Noise, 2, n);
    for (int c = 0; c < 2; ++c) for (float& v : loud->ch[c]) v *= 0.4f;
    CHECK(roundTrip(*loud, 24).empty() && roundTrip(*loud, 16).empty());
}

TEST(flac_compresses_tones_and_silence_and_does_not_inflate_noise) {
    const size_t n = 44100;
    for (int bits : {16, 24})
        for (int channels : {1, 2}) {
            const size_t pcm = n * (size_t)channels * (size_t)(bits / 8);
            size_t sine = 0, silence = 0, noise = 0, music = 0;
            CHECK(roundTrip(*signal(Sig::Sine, channels, n), bits, &sine).empty());
            CHECK(roundTrip(*signal(Sig::Silence, channels, n), bits, &silence).empty());
            CHECK(roundTrip(*signal(Sig::Noise, channels, n), bits, &noise).empty());
            CHECK(roundTrip(*signal(Sig::Music, channels, n), bits, &music).empty());
            CHECK_MSG(sine * 2 < pcm, "sine: %zu of %zu bytes (%d bits, %d ch)", sine, pcm, bits, channels);
            CHECK_MSG(silence * 100 < pcm, "silence: %zu of %zu bytes (%d bits, %d ch)", silence, pcm, bits, channels);
            CHECK_MSG(noise < pcm + pcm / 200 + 64, "noise: %zu of %zu bytes (%d bits, %d ch)", noise, pcm, bits, channels);
            CHECK_MSG(music * 100 < pcm * 90, "music-like: %zu of %zu bytes (%d bits, %d ch)", music, pcm, bits, channels);
        }
}

TEST(flac_decoder_copes_with_what_stream_info_says_about_length) {
    const auto src = signal(Sig::Music, 2, 10000);
    const auto good = encodeFlac(*src, 16);
    auto setTotal = [](std::vector<unsigned char>& f, uint64 total) {       // 36 bits: the low half of byte 13, then bytes 14 to 17
        unsigned char* p = f.data() + kInfoAt;
        p[13] = (unsigned char)((p[13] & 0xF0) | ((total >> 32) & 15));
        for (int i = 0; i < 4; ++i) p[14 + i] = (unsigned char)(total >> (24 - 8 * i));
    };
    std::string error;
    // an encoder that could not go back to fill in the total leaves it at zero: everything in the file is decoded
    auto f = good;
    setTotal(f, 0);
    CHECK(compare(*src, readFlacMemory(f.data(), f.size(), &error).get(), 16, error).empty());
    // ... also with a tag stuck on the end, which is not a frame
    f.insert(f.end(), {'T', 'A', 'G', 0, 1, 2, 3, 0xFF, 0xF8, 0xFF, 0xFF});
    CHECK(compare(*src, readFlacMemory(f.data(), f.size(), &error).get(), 16, error).empty());
    // a total that stops short of the last frames is respected
    f = good;
    setTotal(f, 5000);
    auto got = readFlacMemory(f.data(), f.size(), &error);
    CHECK(got && got->frames() == 5000 && got->ch[1].size() == 5000);
    CHECK(got && std::equal(got->ch[0].begin(), got->ch[0].end(), readFlacMemory(good.data(), good.size())->ch[0].begin()));
    // a total that promises more than the file holds gives what there is
    f = good;
    setTotal(f, 60000000000ull);
    CHECK(compare(*src, readFlacMemory(f.data(), f.size(), &error).get(), 16, error).empty());
    // no audio at all is a valid, empty sample
    const auto none = encodeFlac(*blank(2, 0, 48000.0), 24);
    CHECK(none.size() == kFirstFrame);
    got = readFlacMemory(none.data(), none.size(), &error);
    CHECK(got && got->frames() == 0 && got->channels == 2 && got->sampleRate == 48000.0);
    // STREAMINFO has to be there, and first
    f = good;
    f[4] = 0x80 | 4;        // the block now claims to be a comment block
    CHECK(!readFlacMemory(f.data(), f.size(), &error) && error == "FLAC file has no stream info");
    // a rate of zero or a depth below 4 bits cannot be audio
    f = good;
    f[kInfoAt + 10] = f[kInfoAt + 11] = 0;
    f[kInfoAt + 12] &= 0x0F;
    CHECK(!readFlacMemory(f.data(), f.size(), &error) && error == "bad FLAC stream info");
}

TEST(flac_rejects_what_is_not_flac) {
    std::string error;
    CHECK(!readFlacMemory(nullptr, 0, &error) && !error.empty());
    error.clear();
    CHECK(!readFlacMemory("", 0, &error) && !error.empty());
    const auto good = encodeFlac(*signal(Sig::Music, 2, 3000), 16);
    auto bad = good;
    bad[0] = 'F';       // "FLaC"
    error.clear();
    CHECK(!readFlacMemory(bad.data(), bad.size(), &error) && error == "not a FLAC file");
    const auto wav = encodeWav(*signal(Sig::Sine, 1, 500), WavFormat::Pcm16);
    CHECK(!readFlacMemory(wav.data(), wav.size(), &error));
    for (size_t n = 1; n < kFirstFrame; ++n) CHECK(!readFlacMemory(good.data(), n, &error));     // the header alone, cut anywhere
    // the header and no frames, when frames were promised
    error.clear();
    CHECK(!readFlacMemory(good.data(), kFirstFrame, &error) && !error.empty());
    // a missing file
    error.clear();
    CHECK(!readFlacFile(t::tmpDir() + "/flac/no such file.flac", &error) && error.find("cannot open") == 0);
    // calls without an error string must work as well
    CHECK(!readFlacMemory(bad.data(), bad.size()));
    CHECK(!readFlacMemory(good.data(), 10));
}

TEST(flac_damaged_frames_are_caught_by_their_checksums) {
    const auto src = signal(Sig::Music, 1, 10000);      // three frames: 4096, 4096 and 1808 samples
    for (int bits : {16, 24}) {
        const double scale = bits == 16 ? 32768.0 : 8388608.0;
        const auto good = encodeFlac(*src, bits);
        const auto starts = frameStarts(good);
        CHECK_MSG(starts.size() == 3, "%zu frames found", starts.size());
        if (starts.size() != 3) continue;
        int wrong = 0, undetected = 0, tested = 0;
        std::string firstError;
        for (size_t at = kFirstFrame; at < good.size(); at += 19) {
            for (unsigned char flip : {(unsigned char)0x04, (unsigned char)0xFF}) {
                auto bad = good;
                bad[at] ^= flip;
                std::string error;
                const auto got = readFlacMemory(bad.data(), bad.size(), &error);
                ++tested;
                if (!got) { if (firstError.empty()) firstError = error; continue; }
                // Damage in the last frame can look like a file that ends early; then the frames before it are
                // returned. Nothing else may get through, and what is returned must be right.
                if (at < starts[2] || got->frames() != 8192) ++undetected;
                for (size_t i = 0; i < got->ch[0].size() && i < src->ch[0].size(); ++i)
                    if ((double)got->ch[0][i] * scale != (double)quantise(src->ch[0][i], bits)) { ++wrong; break; }
            }
        }
        CHECK_MSG(undetected == 0 && wrong == 0, "%d bits: %d of %d damaged files decoded, %d with wrong audio", bits, undetected, tested, wrong);
        CHECK_MSG(firstError.find("FLAC") != std::string::npos, "error was '%s'", firstError.c_str());
        // one flipped bit in the audio of a frame names the checksum
        for (size_t frame = 0; frame < 3; ++frame) {
            auto bad = good;
            bad[starts[frame] + 40] ^= 0x10;
            std::string error;
            CHECK(!readFlacMemory(bad.data(), bad.size(), &error));
            CHECK_MSG(error == "FLAC frame CRC mismatch", "frame %zu: error was '%s'", frame, error.c_str());
            // the checksum itself counts too
            bad = good;
            bad[(frame < 2 ? starts[frame + 1] : good.size()) - 1] ^= 0x01;
            CHECK(!readFlacMemory(bad.data(), bad.size(), &error));
            CHECK_MSG(error == "FLAC frame CRC mismatch", "frame %zu: error was '%s'", frame, error.c_str());
        }
        // and one in a frame header names that one's
        auto bad = good;
        bad[starts[1] + 4] ^= 0x02;       // the frame number
        std::string error;
        CHECK(!readFlacMemory(bad.data(), bad.size(), &error));
        CHECK_MSG(error == "FLAC frame header CRC mismatch", "error was '%s'", error.c_str());
        // a frame that no longer starts with the sync code
        bad = good;
        bad[starts[1]] = 0x7F;
        CHECK(!readFlacMemory(bad.data(), bad.size(), &error));
        CHECK_MSG(error == "FLAC frame sync lost", "error was '%s'", error.c_str());
    }
}

TEST(flac_truncated_file_gives_the_whole_frames_before_the_cut) {
    for (int channels : {1, 2}) {
        const auto src = signal(Sig::Music, channels, 10000);      // frames of 4096, 4096 and 1808 samples
        const auto good = encodeFlac(*src, 16);
        auto starts = frameStarts(good);
        CHECK_MSG(starts.size() == 3, "%zu frames found", starts.size());
        if (starts.size() != 3) continue;
        starts.push_back(good.size());
        int wrong = 0, wrongLength = 0, tested = 0;
        for (size_t n = kFirstFrame + 1; n < good.size();) {
            // a cut before the first frame is whole fails; after that it gives every frame that is whole
            const int64 expect = n < starts[1] ? -1 : n < starts[2] ? 4096 : 8192;
            std::string error;
            const auto got = readFlacMemory(good.data(), n, &error);
            ++tested;
            if (!got) { if (expect >= 0 || error != "FLAC file is cut short") ++wrongLength; }
            else {
                if (got->frames() != expect || got->channels != channels) ++wrongLength;
                for (int c = 0; c < got->channels; ++c)
                    for (size_t i = 0; i < got->ch[c].size(); ++i)
                        if ((double)got->ch[c][i] * 32768.0 != (double)quantise(src->ch[c][i], 16)) { ++wrong; break; }
            }
            // every length close to a frame edge, a sampling of the rest
            bool nearEdge = false;
            for (size_t edge : starts) nearEdge = nearEdge || (n + 24 >= edge && n <= edge + 24);
            n += nearEdge ? 1 : 29;
        }
        CHECK_MSG(wrong == 0 && wrongLength == 0 && tested > 200, "%d channels: %d cuts, %d with wrong audio, %d with the wrong length", channels, tested, wrong, wrongLength);
        // whole file plus junk on the end (a tag, say) still decodes in full
        auto tagged = good;
        tagged.insert(tagged.end(), {'T', 'A', 'G', 'x', 'y', 'z', 0, 0, 0xFF, 0xF8, 1, 2, 3});
        std::string error;
        const auto got = readFlacMemory(tagged.data(), tagged.size(), &error);
        CHECK(compare(*src, got.get(), 16, error).empty());
    }
}

TEST(flac_decoder_survives_random_damage_without_reading_out_of_bounds) {
    // Not a check of what comes back, only that nothing crashes or overruns (the Debug run has the checked heap on).
    Rand rng(4242);
    int decoded = 0, refused = 0;
    for (int bits : {16, 24}) {
        const auto good = encodeFlac(*signal(Sig::Music, 2, 6000, 44100.0, 3), bits);
        for (int round = 0; round < 400; ++round) {
            // each damaged copy lives in a buffer of exactly its size, so an overrun lands on the heap's guard bytes
            const size_t n = round % 4 == 0 ? rng.next() % good.size() : good.size();
            std::vector<unsigned char> bad(good.begin(), good.begin() + (std::ptrdiff_t)n);
            const int hits = 1 + (int)(rng.next() % 6);
            for (int k = 0; k < hits && n > 4; ++k) {
                // every other round leaves the 42 header bytes alone so the damage lands in the frames
                const size_t at = (round & 1) && n > kFirstFrame ? kFirstFrame + rng.next() % (n - kFirstFrame) : 4 + rng.next() % (n - 4);
                bad[at] = (unsigned char)rng.next();
            }
            const auto got = readFlacMemory(bad.data(), bad.size());
            if (got) { ++decoded; CHECK(got->channels >= 1 && got->channels <= 2 && (got->channels == 1 || got->ch[1].size() == got->ch[0].size())); }
            else ++refused;
        }
    }
    // pure noise behind a real header
    for (int round = 0; round < 200; ++round) {
        auto junk = encodeFlac(*signal(Sig::Music, 2, 100), 16);
        junk.resize(kFirstFrame);
        const size_t n = 1 + rng.next() % 3000;
        junk.push_back(0xFF);
        junk.push_back(0xF8);
        for (size_t i = 0; i < n; ++i) junk.push_back((unsigned char)rng.next());
        const auto got = readFlacMemory(junk.data(), junk.size());
        if (got) ++decoded; else ++refused;
    }
    CHECK_MSG(refused > 500, "%d refused, %d decoded", refused, decoded);
}

TEST(flac_file_round_trip_with_a_non_ascii_path) {
    const std::string dir = t::tmpDir() + "/flac";
    const std::string path = dir + "/caf\xC3\xA9 \xE2\x99\xAA \xE9\x9F\xB3.flac";        // "café ♪ 音.flac" in UTF-8
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
    const auto src = signal(Sig::Music, 2, 12345, 48000.0);
    std::string error;
    CHECK_MSG(writeFlacFile(path, *src, 24, &error), "%s", error.c_str());
    CHECK(std::filesystem::exists(std::filesystem::path(std::u8string(path.begin(), path.end()))));
    const auto got = readFlacFile(path, &error);
    const std::string why = compare(*src, got.get(), 24, error);
    CHECK_MSG(why.empty(), "%s", why.c_str());
    CHECK(got && got->path == path);
    // the file on disk is what encodeFlac returns
    std::vector<unsigned char> onDisk;
    CHECK(readFileBytes(path, onDisk) && onDisk == encodeFlac(*src, 24));
    // the default is 24 bits, and 16 works through a file too
    CHECK(writeFlacFile(path, *src));
    CHECK(readFileBytes(path, onDisk) && onDisk == encodeFlac(*src, 24));
    CHECK(writeFlacFile(path, *src, 16));
    CHECK(compare(*src, readFlacFile(path).get(), 16, "").empty());
    // a path that cannot be written reports it
    error.clear();
    CHECK(!writeFlacFile(dir + "/caf\xC3\xA9 \xE2\x99\xAA \xE9\x9F\xB3.flac/x.flac", *src, 16, &error) && error.find("cannot write") == 0);
}

// ---------------------------------------------------------------------------------------------
// Streams from other encoders, so the decoder is shown to read more than our own encoder's output.
// The bytes at the end of this file were made once, on the development PC, by ffmpeg 8.1, by libFLAC
// (through libsndfile 1.2.2, Python's soundfile), and by a bit-level writer written for the purpose,
// for what neither of those produces: variable block size, escaped partitions, 12, 20 and 32-bit
// audio, every form of frame header. Each stream was then decoded by ffmpeg (and by libsndfile where
// it can open the file) to confirm it holds exactly the audio its encoder was given; the hash and
// spot values below are of that audio. Nothing here needs those programs when the tests run.
struct FlacRef {
    const char* what;
    const unsigned char* data;
    size_t size;
    size_t audioAt;              // where the first frame starts
    int rate, channels, fileChannels, bits;      // channels as decoded: at most two of the file's
    long long frames;
    unsigned long long hash;     // hashSamples() of the first two channels of the audio the encoder was given
    struct { int frame, channel; long long value; } spot[3];
    bool md5;                    // mono or stereo at 16 or 24 bits: our encoder can store the same audio, so the MD5s must agree
};
extern const FlacRef kFlacRefs[];
extern const size_t kFlacRefCount;

TEST(flac_decodes_streams_made_by_other_encoders) {
    CHECK(kFlacRefCount >= 25);
    for (size_t r = 0; r < kFlacRefCount; ++r) {
        const FlacRef& ref = kFlacRefs[r];
        std::string error;
        const auto s = readFlacMemory(ref.data, ref.size, &error);
        CHECK_MSG(s != nullptr, "%s: %s", ref.what, error.c_str());
        if (!s) continue;
        const bool shape = s->sampleRate == ref.rate && s->channels == ref.channels && s->frames() == ref.frames && (s->channels == 1 || s->ch[1].size() == s->ch[0].size());
        CHECK_MSG(shape, "%s: %g Hz, %d channels, %lld frames", ref.what, s->sampleRate, s->channels, (long long)s->frames());
        if (!shape) continue;
        CHECK_MSG(hashSamples(*s, ref.bits) == ref.hash, "%s: the audio is not what the encoder was given", ref.what);
        const double scale = std::ldexp(1.0, ref.bits - 1);
        for (const auto& spot : ref.spot) {
            const long long got = std::llround((double)s->ch[spot.channel][(size_t)spot.frame] * scale);
            CHECK_MSG(got == spot.value, "%s: frame %d channel %d is %lld, expected %lld", ref.what, spot.frame, spot.channel, got, spot.value);
        }
        CHECK_MSG(s->peak() <= 1.0f, "%s: peak %g", ref.what, s->peak());       // scaled by the stream's own depth
    }
}

TEST(flac_audio_from_other_encoders_gets_the_same_md5_from_ours) {
    // An encoder writes the MD5 of the audio it was given into STREAMINFO. Decoding a reference stream and
    // encoding the result must arrive at the same MD5: our MD5 and decoder checked against ffmpeg's and
    // libFLAC's, with no trust placed in our own round trip.
    int compared = 0;
    for (size_t r = 0; r < kFlacRefCount; ++r) {
        const FlacRef& ref = kFlacRefs[r];
        if (!ref.md5) continue;
        const auto s = readFlacMemory(ref.data, ref.size);
        CHECK_MSG(s != nullptr, "%s", ref.what);
        if (!s) continue;
        const auto ours = encodeFlac(*s, ref.bits);
        static const unsigned char magic[4] = {'f', 'L', 'a', 'C'};
        const unsigned char* theirs = std::search(ref.data, ref.data + ref.size, magic, magic + 4);       // an ID3 tag may come first
        CHECK(theirs + 42 <= ref.data + ref.size);
        CHECK_MSG(std::memcmp(ours.data() + kInfoAt + 18, theirs + kInfoAt + 18, 16) == 0, "%s", ref.what);
        const auto back = readFlacMemory(ours.data(), ours.size());
        CHECK_MSG(back && identical(*back, *s) && back->sampleRate == s->sampleRate && back->channels == s->channels, "%s: did not survive our encoder", ref.what);
        ++compared;
    }
    CHECK_MSG(compared >= 15, "%d streams compared", compared);
}

TEST(flac_damaged_streams_from_other_encoders_fail_cleanly) {
    // Flips and cuts across streams with LPC, escaped partitions, many channels and odd depths. A flipped bit
    // inside a frame must be refused (or, in the last frame, may read as a file that ends early); a cut file
    // gives fewer frames or nothing. Neither may crash.
    int flips = 0, passedFlips = 0, cuts = 0, passedCuts = 0;
    for (size_t r = 0; r < kFlacRefCount; ++r) {
        const FlacRef& ref = kFlacRefs[r];
        const size_t step = std::max<size_t>(1, (ref.size - ref.audioAt) / 50);
        for (size_t at = ref.audioAt; at < ref.size; at += step) {
            std::vector<unsigned char> bad(ref.data, ref.data + ref.size);
            bad[at] ^= (unsigned char)(1u << (at % 8));
            auto got = readFlacMemory(bad.data(), bad.size());
            ++flips;
            if (got && got->frames() >= ref.frames) ++passedFlips;
            const std::vector<unsigned char> cut(ref.data, ref.data + at);       // its own buffer, so reading past the cut would be caught
            std::string error;
            got = readFlacMemory(cut.data(), cut.size(), &error);
            ++cuts;
            if (got ? got->frames() >= ref.frames : error.empty()) ++passedCuts;
        }
    }
    CHECK_MSG(passedFlips == 0 && passedCuts == 0, "%d of %d flipped files and %d of %d cut files were taken as whole", passedFlips, flips, passedCuts, cuts);
}

// ---- reference streams: begin (generated; see the note above the tests that use them)
// ffmpeg -compression_level 0 (fixed predictors), 16-bit stereo at 44.1 kHz, blocks of 576. 2392 bytes.
static const unsigned char kFlac_ffmpeg_level0_16bit_stereo[] = {
    102,76,97,67,0,0,0,34,2,64,2,64,0,1,229,0,2,111,10,196,66,240,0,0,8,157,25,230,106,205,243,234,183,224,6,70,70,152,158,43,97,60,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,
    49,48,48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,41,168,0,67,22,0,196,1,155,2,
    113,8,189,177,219,213,16,113,58,145,196,20,104,230,82,114,118,148,197,24,164,169,102,222,39,168,187,61,103,71,110,169,228,179,125,118,237,201,84,74,138,39,209,166,213,167,51,230,
    88,65,68,116,40,131,140,57,63,147,204,113,11,53,146,235,24,163,28,221,148,206,72,52,145,24,70,39,9,177,24,131,8,105,25,129,0,194,193,128,131,98,52,201,152,8,12,17,165,149,21,138,
    138,74,35,1,3,2,12,45,83,38,83,200,215,24,141,102,8,49,12,171,137,3,2,166,42,42,133,149,161,12,41,148,203,97,28,196,25,76,196,129,193,8,100,225,128,128,194,50,107,43,83,2,24,65,
    128,134,73,133,226,49,83,43,70,45,150,106,153,27,19,200,225,3,11,70,19,132,49,19,192,131,4,48,33,130,56,38,150,16,98,33,132,48,131,11,217,115,91,83,39,216,131,9,225,15,9,6,34,48,
    67,23,20,184,32,200,151,93,219,201,44,169,254,178,220,172,84,223,144,168,143,8,228,84,233,52,221,152,81,43,58,102,20,35,196,228,121,166,241,68,44,164,42,116,118,183,85,24,83,43,56,
    135,38,236,173,72,82,83,14,19,246,186,111,183,101,164,120,138,197,33,72,249,31,88,133,20,255,60,255,159,2,24,173,17,129,109,9,150,17,138,98,158,4,56,76,182,16,98,203,114,70,16,204,
    4,54,154,77,194,54,71,50,32,112,32,120,131,8,52,67,32,202,152,131,68,195,16,64,97,12,17,248,135,18,53,147,35,34,131,4,67,73,49,81,130,67,17,24,180,32,96,19,177,49,4,241,16,224,76,
    39,96,160,192,153,120,140,74,48,95,192,135,137,58,33,132,48,65,165,226,99,19,68,125,44,200,96,134,140,67,84,97,24,96,64,193,14,145,166,71,82,198,68,108,19,105,193,12,64,193,114,48,
    131,8,135,32,134,76,164,197,33,136,16,112,173,66,45,112,154,8,24,8,104,70,17,160,182,151,4,200,193,6,19,76,16,100,136,98,179,33,63,41,166,8,25,8,24,70,192,64,209,161,12,32,24,22,
    49,102,35,39,171,101,195,33,31,50,70,197,32,210,46,102,2,26,105,24,39,100,97,26,33,128,129,137,26,18,122,75,34,47,19,246,164,135,33,139,179,62,49,205,107,122,134,28,137,21,179,147,
    178,162,181,168,197,212,139,198,21,42,46,32,226,20,222,253,83,161,76,82,28,97,196,235,178,165,186,242,62,86,203,211,121,107,42,124,197,166,57,172,252,223,178,10,196,120,110,169,98,
    77,107,0,110,12,255,248,41,168,1,68,22,1,147,1,159,1,169,8,76,76,166,76,76,77,38,74,196,101,68,233,35,66,12,32,194,50,24,182,33,201,12,180,229,33,132,226,140,40,96,16,50,43,192,64,
    97,75,193,13,74,213,213,49,26,244,142,87,168,193,56,96,77,132,214,148,152,153,98,52,70,22,208,175,19,98,121,61,32,194,204,67,19,67,4,122,78,47,68,49,73,148,192,129,132,96,65,162,
    214,77,130,118,194,50,118,78,163,4,24,157,48,155,20,241,16,210,90,16,98,89,116,86,170,151,84,134,19,152,70,96,143,146,56,64,193,102,19,21,152,8,100,101,19,73,146,216,16,48,182,36,
    112,134,38,152,131,2,6,35,8,113,54,36,12,45,25,100,101,145,146,153,38,89,70,8,97,56,154,35,68,98,49,26,80,96,32,213,164,100,229,144,196,143,4,252,93,42,196,196,105,47,8,101,151,35,
    19,60,220,154,186,190,74,182,243,154,173,142,244,45,17,23,145,26,171,136,158,153,174,79,85,199,92,245,197,69,213,76,77,78,127,111,127,76,212,197,110,103,228,77,69,106,147,213,49,
    40,0,0,0,0,0,0,128,0,9,0,0,0,0,74,111,110,238,229,11,100,74,17,166,70,136,244,83,22,168,186,166,161,177,127,123,13,83,103,42,228,83,255,89,255,169,8,166,113,47,220,145,73,45,164,
    177,149,156,156,197,101,49,76,173,233,10,137,80,163,45,169,72,88,98,209,11,144,230,197,100,28,33,82,231,50,214,121,180,120,71,69,34,184,194,152,172,199,69,50,185,36,93,162,179,146,
    171,94,75,226,132,21,17,80,131,146,84,66,146,169,16,159,101,17,98,43,78,103,107,148,158,17,243,41,133,8,114,71,8,125,70,59,20,137,126,74,130,141,142,101,137,172,86,206,136,80,131,
    163,20,158,33,80,135,68,57,190,251,43,57,14,49,210,214,93,34,145,107,116,42,17,98,180,84,186,84,183,88,158,49,73,234,77,88,150,106,66,183,33,90,38,40,133,76,133,55,155,114,60,55,
    82,79,183,34,164,177,62,155,228,114,89,211,152,169,36,145,82,39,113,48,226,98,162,58,116,177,68,46,136,84,149,43,146,92,113,21,35,180,179,21,50,57,142,210,85,24,84,73,246,115,57,
    10,98,152,225,56,167,187,250,241,42,154,156,216,68,239,78,111,76,246,229,76,69,77,162,38,217,241,137,35,110,170,55,59,106,34,250,182,42,38,185,164,53,137,142,148,144,156,218,71,
    196,228,231,210,0,0,0,0,0,0,0,0,0,0,0,0,11,0,0,0,0,0,0,0,0,0,0,0,0,35,170,149,181,136,156,155,33,57,219,93,121,240,175,136,245,38,114,61,42,165,221,231,213,62,57,96,216,179,255,
    248,41,168,2,77,22,10,245,10,103,9,203,8,137,140,140,50,140,74,104,210,76,249,234,156,193,227,190,85,251,233,184,148,155,51,108,218,51,178,238,236,28,64,82,21,106,174,174,218,40,
    69,50,66,9,204,78,37,109,211,58,44,120,165,32,228,45,138,71,17,115,18,154,9,68,212,34,147,164,203,236,82,227,222,116,33,72,44,157,188,68,227,56,69,38,104,221,17,204,125,106,255,11,
    139,16,82,57,151,36,36,100,100,108,205,217,190,79,111,171,139,20,164,45,149,91,162,92,72,208,220,201,201,88,166,92,86,28,197,42,30,215,133,35,252,157,52,36,106,109,70,138,24,113,
    157,188,139,201,133,101,118,125,126,213,187,98,84,102,166,208,159,37,187,216,180,168,171,11,161,209,133,16,164,69,35,118,37,140,99,145,159,111,59,183,119,72,58,32,169,110,98,154,
    36,108,110,134,112,157,36,219,239,47,183,29,151,167,71,37,136,206,55,100,144,69,204,166,20,132,126,199,197,32,168,116,46,86,57,23,155,236,144,159,8,172,146,109,110,139,60,255,161,
    121,30,41,150,102,185,184,202,101,27,166,164,243,71,33,88,236,249,85,186,163,153,210,100,237,26,113,45,39,166,46,142,66,161,244,121,234,167,186,234,69,37,68,72,212,80,2,88,1,38,8,
    157,2,155,251,103,221,219,42,57,28,206,203,154,183,205,91,43,8,168,211,17,70,40,32,225,41,20,103,70,72,214,81,21,174,188,71,210,91,175,103,144,164,127,80,250,47,184,86,135,99,144,
    114,123,139,28,66,167,130,137,34,181,177,175,104,202,136,166,173,169,49,124,105,185,49,62,217,41,156,202,217,22,145,10,101,166,62,106,81,14,147,221,254,247,117,151,28,65,218,218,
    186,94,172,172,172,183,116,230,56,207,198,28,107,188,8,41,169,142,50,182,173,73,89,196,206,49,68,118,99,136,185,36,210,43,69,98,146,21,44,145,228,226,32,230,32,225,132,204,141,178,
    142,163,38,154,48,153,129,3,8,96,190,8,197,48,162,101,22,128,158,23,33,48,16,48,89,76,37,171,22,188,146,25,97,128,142,76,216,166,96,128,101,112,98,102,78,30,147,135,35,34,27,66,1,
    129,70,146,114,16,98,150,132,72,100,17,130,101,149,8,145,130,112,149,137,146,38,90,96,16,59,4,24,134,8,238,8,57,110,77,109,65,144,64,238,242,218,114,52,97,49,6,4,12,76,136,98,81,
    160,156,68,203,41,35,2,25,5,177,48,136,49,100,72,203,136,65,151,8,204,68,113,24,24,16,255,248,121,168,3,1,220,44,22,7,235,7,138,7,44,0,137,122,76,177,57,198,42,163,165,170,66,140,
    92,154,17,73,179,41,34,73,204,114,62,65,116,191,253,209,209,97,60,201,204,80,193,65,22,36,186,179,242,61,207,186,236,86,84,113,135,12,41,146,162,106,76,149,41,21,151,39,114,45,232,
    84,191,136,20,79,83,40,159,34,155,84,237,175,33,72,234,103,183,231,222,92,228,115,21,8,173,53,100,233,113,133,109,142,136,191,121,242,232,169,186,114,93,160,142,153,18,179,146,84,
    118,174,203,159,91,244,41,157,46,202,111,8,173,54,155,183,66,153,98,162,177,106,44,222,167,200,173,115,77,205,209,138,68,84,236,179,46,151,246,254,51,166,40,71,74,137,90,209,47,33,
    68,118,190,68,62,150,171,21,157,157,55,145,188,110,79,26,74,153,201,122,119,24,82,21,47,136,186,167,76,164,66,141,230,107,162,41,42,145,80,166,57,133,16,188,65,204,188,151,183,77,
    201,119,36,145,10,24,115,62,67,147,249,33,91,200,169,133,0,23,64,12,0,4,17,167,49,12,179,91,76,193,14,48,155,156,16,12,22,232,159,44,131,2,24,16,229,96,134,46,4,12,40,70,38,128,
    129,130,106,37,176,80,194,40,209,33,136,192,141,169,24,89,228,48,160,98,65,132,98,13,53,236,160,96,67,80,101,41,152,178,123,18,24,165,204,76,33,132,214,8,196,212,130,6,2,216,152,
    90,70,18,120,84,193,60,181,87,64,129,137,228,141,136,64,97,24,131,132,113,52,54,1,1,225,49,130,54,8,48,142,67,21,100,226,241,13,18,136,24,66,101,152,76,69,178,136,196,70,146,104,
    91,8,204,4,56,88,96,76,48,8,56,160,201,100,51,0,129,176,134,16,26,4,61,136,201,14,232,131,9,235,104,91,46,211,4,24,162,50,165,162,140,133,24,8,60,20,49,22,194,182,8,97,76,180,48,
    76,158,74,12,19,106,96,128,192,32,102,17,132,108,32,203,216,152,135,136,214,194,58,213,16,209,52,200,158,68,24,17,201,81,50,165,162,136,48,181,36,142,2,187,106};
// ffmpeg -compression_level 5 (LPC), 16-bit stereo at 48 kHz, blocks of 1152. 2393 bytes.
static const unsigned char kFlac_ffmpeg_level5_16bit_stereo[] = {
    102,76,97,67,0,0,0,34,4,128,4,128,0,0,160,0,4,74,11,184,2,240,0,0,9,96,218,121,49,65,43,102,183,88,20,252,182,175,204,249,53,185,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,
    49,48,48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,58,168,0,92,78,0,196,1,137,2,79,
    3,17,3,209,4,140,5,66,5,244,230,184,227,238,199,215,159,190,95,101,240,169,162,167,253,222,128,18,16,64,161,32,21,0,132,10,64,33,132,2,66,1,128,72,64,129,8,72,64,32,100,36,39,0,
    138,82,160,133,24,198,8,148,72,148,97,28,135,16,192,132,148,129,33,32,21,9,9,129,82,19,132,25,105,136,133,162,132,67,72,192,161,148,9,9,2,129,32,96,80,8,64,136,66,67,9,144,161,100,
    162,70,144,74,44,18,140,18,8,65,148,67,98,28,52,204,16,36,35,11,16,45,168,133,65,137,5,65,18,10,137,5,36,19,97,79,40,4,34,64,194,72,73,12,9,129,146,31,3,142,21,161,16,208,180,48,
    34,7,128,96,100,40,4,8,19,32,16,129,132,129,33,12,40,6,134,18,147,34,98,20,105,49,67,76,229,12,40,6,73,32,112,12,40,16,36,153,36,50,80,34,1,16,145,41,154,124,228,206,6,16,224,18,
    18,128,64,48,144,32,76,2,18,1,64,129,72,25,12,134,104,18,161,49,36,228,168,17,3,60,33,49,2,28,14,66,67,32,68,48,50,148,40,134,237,169,20,160,156,4,252,16,145,36,236,2,153,194,20,
    52,201,146,229,37,101,40,203,210,136,20,34,146,27,2,22,24,80,33,135,10,16,145,36,11,34,25,145,36,131,145,66,176,66,98,146,36,230,147,144,161,16,132,73,134,26,73,144,66,36,149,172,
    199,59,40,20,104,29,32,100,64,32,88,17,0,176,32,97,18,1,134,66,75,8,80,40,68,12,145,33,97,54,1,78,24,80,132,195,12,8,112,41,2,66,33,32,7,133,3,14,82,112,138,6,68,66,75,40,128,101,
    8,129,73,33,73,12,56,82,5,10,67,2,133,3,16,52,33,19,146,36,226,4,73,204,176,57,33,144,208,40,6,25,32,104,64,62,1,66,97,66,132,57,76,148,184,17,40,114,134,100,184,25,57,2,204,52,
    192,240,165,8,129,68,12,80,136,17,18,234,24,188,150,65,0,60,66,30,114,4,178,72,68,52,36,137,2,41,66,68,161,34,132,64,49,194,17,40,24,132,59,3,129,66,105,8,22,97,133,59,252,245,254,
    123,255,62,127,160,127,208,159,232,159,244,119,250,75,156,153,56,163,137,122,192,42,224,237,62,1,119,33,233,158,0,6,25,32,104,33,66,4,8,224,65,129,169,73,8,66,13,64,160,82,49,56,4,
    57,226,24,164,38,105,80,133,10,66,155,33,145,129,8,57,132,49,66,83,32,89,64,164,2,18,212,33,9,16,144,160,90,4,36,33,82,121,2,132,134,24,16,66,33,32,24,17,12,78,64,128,100,217,2,
    132,64,216,67,67,48,36,18,128,76,10,33,36,8,148,18,4,132,242,19,9,34,192,144,66,5,204,48,9,68,204,9,32,108,133,0,166,26,25,0,225,8,144,134,136,25,132,134,146,16,156,161,32,19,133,
    97,96,72,24,24,33,16,144,200,81,36,2,134,9,153,198,73,2,106,145,210,129,62,72,81,5,48,129,84,225,74,49,228,36,109,40,110,17,50,11,128,16,41,6,2,153,132,2,20,1,76,194,194,9,135,64,
    164,77,37,2,129,32,194,9,36,32,195,164,161,32,65,196,12,150,136,17,132,10,146,192,164,132,66,129,81,225,36,199,12,8,240,145,36,97,33,44,13,9,40,146,4,202,4,41,112,44,14,66,132,148,
    0,151,8,72,88,73,8,129,48,41,33,64,35,2,16,224,80,132,9,82,72,0,112,50,132,42,1,0,42,1,9,140,2,64,36,42,146,16,9,38,24,83,2,128,4,155,9,24,16,144,176,172,2,6,90,1,73,33,73,195,144,
    146,19,98,66,1,42,129,0,202,161,33,130,7,40,66,5,36,138,7,24,25,36,134,41,0,136,30,22,120,128,76,100,18,24,64,176,70,4,3,17,41,48,36,20,16,44,194,74,46,26,5,16,213,230,32,204,36,
    54,32,129,52,140,33,6,8,99,20,32,17,37,65,144,133,196,97,135,144,22,4,50,141,66,66,168,84,73,8,4,128,161,56,132,64,36,42,148,137,9,10,6,33,3,49,72,66,22,33,66,114,72,18,36,144,210,
    75,48,133,14,25,148,3,42,20,132,3,80,228,33,146,18,144,48,208,152,72,83,2,73,2,36,60,32,16,185,66,16,8,72,105,48,48,9,134,97,10,5,144,48,148,33,32,96,22,19,144,129,164,129,8,80,41,
    66,64,144,132,16,34,128,70,248,255,248,58,168,1,91,78,253,241,253,244,253,248,253,254,254,7,254,14,254,23,254,33,230,34,48,234,179,219,7,214,160,32,48,149,33,1,126,24,10,4,132,102,
    102,92,8,205,129,28,133,99,134,73,33,142,41,33,134,104,64,2,160,200,2,176,2,176,188,144,50,93,20,181,65,36,16,77,20,83,77,21,80,212,34,8,162,16,162,148,17,201,221,163,137,153,100,
    16,98,8,193,10,66,66,18,9,18,4,41,41,2,48,187,45,105,112,149,9,4,17,144,66,8,88,130,16,38,66,11,66,49,41,214,32,231,68,35,49,77,69,228,44,177,40,193,36,122,79,231,163,56,77,15,105,
    237,83,21,72,47,17,36,200,35,18,167,33,144,213,140,150,229,17,148,37,20,68,137,68,144,155,4,52,250,227,201,186,120,166,17,168,88,35,2,224,82,68,36,200,165,203,100,83,101,98,210,
    208,164,130,18,33,65,9,2,137,8,20,69,34,98,201,38,168,137,151,148,34,146,18,192,139,80,42,9,113,44,197,115,216,80,232,168,100,16,101,2,96,18,80,10,16,206,4,32,36,35,122,90,17,38,
    194,202,194,200,68,48,162,76,11,152,90,56,143,136,206,18,60,212,237,70,82,178,76,168,140,146,54,75,26,98,147,54,186,124,77,146,83,16,67,64,184,148,36,209,98,19,153,43,70,43,22,180,
    194,74,76,18,69,133,133,132,196,146,132,201,98,25,43,165,163,4,242,144,76,137,17,34,25,33,10,38,128,154,90,43,19,82,45,36,66,145,9,16,146,20,17,5,146,10,68,176,140,41,197,98,152,
    140,38,146,132,181,2,208,180,44,164,17,136,166,79,23,105,203,178,19,16,200,90,201,82,45,33,66,50,43,47,137,200,202,18,57,52,17,145,24,38,76,68,168,145,113,41,166,136,236,87,53,121,
    91,161,102,73,82,98,98,76,136,98,77,0,135,4,136,24,232,17,195,52,3,64,224,78,4,42,33,42,145,87,41,76,162,41,194,194,210,97,10,17,136,2,67,9,34,64,214,134,88,8,18,124,50,28,128,69,
    19,36,41,34,136,180,177,113,56,178,49,84,73,12,16,150,136,180,69,164,68,201,41,130,97,13,37,200,196,140,136,4,133,9,132,164,144,179,16,3,68,2,2,5,162,5,116,124,45,28,36,98,209,89,
    74,41,74,136,96,152,142,9,139,23,21,147,23,33,25,69,168,177,76,19,41,36,140,73,223,254,207,255,111,255,199,255,227,255,246,255,252,127,255,63,255,188,231,46,5,159,134,72,123,58,
    226,199,216,147,185,48,145,145,70,208,38,74,109,2,20,105,116,132,168,139,89,32,150,137,34,142,8,40,138,0,0,0,10,74,64,192,0,72,0,184,109,24,142,9,33,162,104,32,130,72,160,146,25,
    164,132,89,33,33,123,200,167,200,180,205,201,50,255,70,204,0,161,24,49,28,152,32,146,172,11,144,68,36,59,18,75,20,137,168,130,148,139,36,82,76,130,201,54,22,128,201,35,48,41,16,32,
    210,101,32,145,36,34,17,138,1,1,10,40,76,196,160,73,4,50,80,161,129,208,48,192,145,216,132,147,17,34,146,8,136,202,32,180,82,98,148,145,33,204,90,8,108,35,74,83,167,68,141,245,163,
    217,22,41,183,21,169,209,13,98,81,89,144,68,50,72,133,200,81,33,107,9,16,137,147,22,8,70,44,33,9,79,2,208,150,32,37,8,20,144,66,4,36,48,72,105,200,20,202,32,6,168,194,37,52,97,68,
    184,76,89,40,84,193,2,213,37,2,132,88,132,90,68,41,37,0,154,18,36,40,76,180,80,34,78,20,19,33,35,17,101,144,75,122,82,82,70,132,100,100,130,96,161,177,12,2,58,8,4,130,1,68,32,22,
    194,229,3,141,100,69,142,8,142,211,19,203,66,77,97,228,37,190,18,126,201,30,182,251,205,149,70,129,197,144,111,17,194,222,182,69,190,47,60,74,216,142,76,45,68,112,176,48,33,24,76,
    48,228,146,16,228,36,14,132,204,0,198,76,176,146,43,97,64,72,100,137,208,34,104,188,72,152,80,146,130,72,209,65,100,178,65,20,152,33,16,142,65,8,148,40,82,4,75,36,20,68,133,150,8,
    178,8,94,17,136,22,73,33,16,163,66,129,89,196,22,38,156,23,100,136,65,196,188,173,0,24,152,48,33,32,148,134,161,11,166,98,20,32,70,68,158,145,54,66,215,184,74,124,18,121,162,97,78,
    208,204,201,137,122,67,178,141,152,146,218,158,224,187,216,240,77,25,28,22,183,197,2,244,52,69,80,85,52,132,80,17,7,48,68,18,50,196,133,73,17,101,5,148,70,33,20,60,162,4,182,81,68,
    76,76,164,162,72,153,76,37,38,40,68,72,248,73,22,88,162,162,104,81,106,8,208,165,129,64,148,164,32,112,168,19,2,130,24,16,134,107,64,144,128,200,130,255,248,106,168,2,95,223,78,
    255,80,255,102,255,126,255,146,255,166,255,183,255,199,255,211,230,42,101,169,92,105,51,168,63,192,1,144,125,112,129,167,2,57,144,104,115,50,1,130,6,33,173,147,81,112,70,22,88,169,
    101,17,149,68,56,65,99,42,90,150,34,115,113,41,154,8,144,53,33,184,55,49,17,153,25,17,33,186,119,255,207,255,243,255,252,0,1,128,1,192,1,96,1,0,0,183,53,49,112,111,239,69,32,127,
    124,30,121,193,254,118,20,148,17,232,144,185,19,16,153,121,4,131,18,176,156,163,18,144,212,76,38,81,34,38,208,136,40,114,10,36,66,227,97,128,132,72,8,5,34,48,98,82};
// ffmpeg -compression_level 12 -lpc_type cholesky, orders up to 32, 24-bit stereo at 32 kHz. 2458 bytes.
static const unsigned char kFlac_ffmpeg_level12_24bit_stereo[] = {
    102,76,97,67,0,0,0,34,3,32,3,32,0,4,116,0,4,182,7,208,3,112,0,0,6,64,225,225,34,206,136,189,140,157,121,251,195,168,143,193,220,137,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,
    46,49,48,48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,120,140,0,3,31,8,96,255,255,
    255,1,43,11,2,82,59,3,114,211,4,138,70,5,150,52,6,148,130,7,131,69,8,96,234,9,44,58,9,228,85,10,136,168,11,25,25,11,149,221,11,255,134,12,86,250,12,157,105,230,33,101,249,41,200,
    35,196,119,183,240,81,193,239,131,72,3,62,2,47,236,167,197,151,209,47,213,65,85,131,240,249,243,64,97,237,187,209,169,131,108,153,213,146,49,124,23,11,37,73,235,185,120,123,134,
    178,154,106,132,169,127,41,248,41,232,231,55,208,8,210,204,102,25,17,100,148,57,100,72,241,36,16,180,206,217,50,90,4,233,163,56,177,58,113,37,199,47,103,76,128,77,118,9,30,67,254,
    212,72,121,117,105,161,38,214,197,146,74,84,57,132,217,164,64,130,134,88,154,24,96,137,44,224,141,83,145,132,168,194,145,5,48,195,124,64,65,90,133,5,32,197,21,226,164,112,32,133,
    169,218,16,225,223,119,16,8,148,194,13,182,90,49,91,237,197,208,84,251,185,47,49,163,10,152,22,83,53,10,95,83,82,173,16,239,192,79,185,134,93,180,100,40,235,123,24,118,64,135,40,
    196,15,8,135,80,229,116,155,196,122,153,141,199,208,97,100,22,126,24,116,168,46,208,202,229,210,76,39,9,168,95,152,36,157,65,34,215,242,131,196,224,106,35,203,47,37,70,151,52,183,
    75,105,62,71,80,242,82,40,202,18,217,18,103,16,39,119,233,218,226,72,62,135,52,168,148,168,20,180,111,160,238,244,34,89,105,99,244,198,136,137,13,84,254,192,134,19,99,200,222,49,
    245,110,124,67,147,188,228,30,182,20,75,122,17,188,95,144,38,68,66,106,202,89,141,87,240,98,33,108,114,179,92,40,204,47,49,1,200,35,77,230,56,136,69,116,42,144,82,8,74,20,67,4,144,
    97,72,170,103,106,71,10,167,189,28,32,105,75,234,37,200,121,66,108,139,251,130,68,51,215,148,143,66,200,39,17,104,34,101,161,35,162,72,181,197,88,147,20,185,65,132,202,102,132,8,
    127,39,72,33,34,27,57,129,21,169,36,139,58,49,34,85,116,68,13,84,111,125,33,102,201,10,99,104,210,232,87,74,5,139,163,186,224,53,124,36,34,244,77,246,151,208,219,175,174,155,30,99,
    18,181,32,226,95,176,65,152,126,96,148,8,21,0,188,219,32,57,241,166,208,166,77,180,225,71,187,178,150,17,61,209,166,103,208,67,203,163,24,242,139,139,57,4,160,186,33,9,40,79,109,
    148,102,80,153,197,93,16,33,164,202,38,187,8,129,38,136,135,82,151,198,169,33,94,153,254,121,104,255,62,90,255,160,88,191,208,234,63,232,231,191,244,182,231,250,129,215,154,183,
    224,74,183,77,206,91,254,41,255,206,8,180,64,101,117,152,53,197,68,66,78,65,104,251,10,38,123,191,207,43,132,4,186,126,214,175,145,26,22,113,168,34,6,136,30,16,221,117,239,241,36,
    88,227,253,199,44,248,115,8,114,252,179,5,86,35,96,227,139,144,170,49,236,56,202,78,134,21,106,240,169,194,6,95,227,3,182,16,99,35,221,230,56,171,245,138,227,66,47,114,220,198,211,
    88,204,38,218,33,112,178,150,156,252,85,134,206,117,253,16,55,104,217,155,98,15,125,178,9,39,96,142,105,48,130,225,92,214,176,91,154,99,127,219,132,54,90,103,155,87,95,108,145,63,
    95,11,78,59,3,96,94,50,207,37,92,38,27,48,129,111,122,213,117,192,148,45,38,52,127,197,245,31,183,201,74,103,89,229,15,194,106,148,216,20,151,231,144,162,27,5,224,196,10,131,117,
    70,10,227,208,113,140,186,174,28,196,99,17,212,10,74,138,59,122,68,24,238,88,69,112,202,197,33,193,86,252,5,83,140,105,151,140,8,41,49,226,2,24,74,5,71,32,82,240,85,33,198,133,20,
    132,209,202,97,21,48,84,134,118,232,86,200,137,16,83,100,206,65,13,84,130,183,17,20,234,239,57,34,61,100,57,62,49,232,174,127,226,12,210,108,132,183,89,6,152,54,196,9,148,206,146,
    75,30,167,233,103,228,44,183,170,201,37,164,185,2,79,18,104,130,95,130,126,87,35,212,111,234,77,230,158,193,181,233,64,212,230,60,93,213,164,106,51,86,127,91,147,161,185,134,226,
    30,198,158,202,94,218,96,130,79,204,157,239,96,154,138,37,209,114,156,140,248,188,118,158,226,62,172,99,103,89,10,219,49,207,97,143,42,81,152,102,17,107,29,161,72,130,173,133,113,
    132,22,40,136,20,84,138,16,186,102,1,10,49,234,20,118,88,49,132,200,96,174,42,4,20,88,195,6,30,193,221,66,10,70,144,235,28,197,18,192,130,11,150,1,148,122,208,56,82,191,107,41,142,
    42,239,159,130,136,254,76,115,146,132,149,203,118,163,243,58,203,43,213,107,163,181,251,101,108,22,167,169,141,88,152,10,177,106,62,103,98,222,80,249,197,167,127,188,243,221,186,
    178,211,200,13,151,213,159,248,93,4,161,160,25,10,255,248,120,140,1,3,31,99,86,253,66,229,254,215,81,0,97,194,1,218,19,3,56,206,4,119,212,5,146,92,6,133,97,7,79,136,7,241,99,8,109,
    23,8,198,101,230,46,197,202,21,169,212,2,233,29,1,190,95,197,120,220,246,78,5,156,59,195,198,26,4,178,27,85,214,249,126,14,183,213,150,43,217,103,5,39,102,106,195,22,17,186,146,
    201,88,183,37,83,77,152,196,154,137,153,42,16,85,65,21,201,36,139,48,37,38,85,44,154,136,17,198,59,229,42,47,42,103,107,151,160,69,210,169,174,17,9,162,217,83,35,233,239,35,45,245,
    228,9,149,248,182,188,173,205,35,162,78,249,69,216,247,139,144,75,144,162,140,65,2,92,121,73,68,44,98,19,44,72,130,148,60,146,142,80,91,143,49,172,104,246,40,227,168,146,97,31,136,
    161,47,103,167,230,209,37,244,155,199,37,146,130,84,136,199,48,113,9,241,38,185,123,249,202,145,67,45,122,231,81,97,169,147,210,253,121,157,179,6,19,116,76,213,153,39,173,54,153,
    21,20,34,95,197,232,217,61,204,173,132,204,157,41,113,113,22,155,105,213,233,143,34,97,162,51,96,211,130,175,92,75,63,159,162,251,95,46,112,223,191,179,40,97,124,232,87,149,240,
    239,169,37,111,92,31,123,73,31,105,117,141,68,216,163,39,39,62,140,118,26,236,136,52,238,200,89,7,54,33,5,156,97,66,69,133,166,45,98,199,188,41,178,137,56,165,201,65,76,228,181,61,
    51,50,8,109,27,70,113,114,207,218,252,129,11,116,237,6,152,35,204,77,175,200,213,153,66,164,157,150,237,9,192,204,48,141,172,118,147,140,163,69,98,36,79,109,39,37,19,169,19,69,250,
    144,157,127,210,51,160,234,178,254,83,43,212,120,79,248,153,22,218,158,111,153,109,37,203,88,171,219,107,89,99,109,34,83,238,158,189,170,69,75,69,152,36,142,27,114,121,197,214,199,
    210,81,30,41,182,167,66,36,167,175,226,15,173,218,150,201,182,35,202,219,8,69,243,154,89,79,237,130,205,37,130,227,197,57,20,13,32,212,226,80,147,75,169,68,41,170,83,132,72,54,164,
    147,180,181,11,253,175,43,237,33,245,120,90,217,69,20,8,25,235,16,93,20,162,190,105,78,10,235,39,46,55,194,141,160,140,103,107,123,251,77,232,161,125,217,184,30,225,22,115,175,73,
    19,58,93,98,189,95,92,42,173,223,247,18,249,82,164,138,99,253,149,174,197,165,149,21,106,75,225,77,70,173,216,197,168,159,53,75,42,80,193,26,81,228,218,217,8,44,110,197,150,243,58,
    132,184,242,70,226,226,109,74,236,234,141,58,189,124,67,189,118,69,181,69,244,148,185,164,57,100,92,91,159,246,181,36,174,133,250,179,19,178,50,74,181,37,134,214,63,217,29,63,236,
    229,111,246,178,151,251,131,159,253,219,223,254,253,83,255,135,116,127,200,165,63,231,3,31,244,245,95,251,64,167,254,8,87,255,58,83,205,87,112,39,187,233,103,37,94,223,158,198,127,
    75,129,151,5,142,16,152,55,56,47,46,249,200,15,175,172,95,130,206,255,111,98,125,197,238,11,21,152,220,8,37,48,234,66,11,59,152,102,230,52,129,76,44,164,25,148,99,18,221,8,17,158,
    43,44,83,202,48,244,138,20,183,41,243,24,25,95,228,138,209,164,68,161,172,45,80,72,62,18,114,247,86,37,229,152,105,46,13,124,134,203,92,45,45,19,169,245,72,110,219,112,163,241,113,
    122,134,148,138,196,17,10,127,8,43,172,33,22,20,170,161,68,4,79,18,64,193,130,50,209,170,80,161,136,19,97,201,197,65,49,202,196,87,171,207,70,75,30,110,171,147,9,247,133,67,241,
    249,230,142,17,100,242,113,229,14,163,5,210,36,176,135,24,70,23,56,16,236,105,66,8,246,70,184,129,204,56,55,133,33,24,226,36,138,21,132,214,48,129,78,110,232,164,34,185,5,188,233,
    113,6,23,41,47,189,130,15,47,197,54,73,122,208,111,123,150,44,253,38,242,139,23,75,228,143,47,196,247,178,75,124,4,243,235,117,141,96,222,136,90,145,187,94,101,179,49,37,124,136,
    164,54,140,86,17,193,36,56,68,218,166,243,146,17,231,186,45,11,24,148,241,53,4,90,73,149,197,101,164,175,161,186,65,242,173,193,9,122,88,179,66,34,89,147,132,89,49,106,103,25,36,
    39,56,71,62,200,135,8,113,165,105,7,56,207,79,184,202,132,2,16,79,144,160,141,209,8,46,76,119,169,72,98,182,157,235,28,156,35,241,209,86,212,44,71,87,60,244,156,177,31,66,211,204,
    162,232,125,10,164,180,253,88,225,98,161,100,202,85,145,47,152,241,21,113,51,133,22,66,121,150,82,170,241,8,233,74,4,14,122,62,226,152,81,200,54,89,235,115,136,110,200,96,181,15,
    32,162,253,211,182,207,45,172,89,17,11,243,208,43,207,38,168,229,249,53,177,144,235,243,107,11,21,148,213,149,36,47,94,205,150,28,159,98,115,180,164,13,174,101,25,101,222,24,75,10,
    140,126,8,73,120,59,112,150,22,24,164,253,82,66,54,158,233,217,51,211,76,83,95,204,184,76,81,23,179,49,106,37,106,76,177,4,145,169,218,70,19,85,138,232,241,165,119,25,187,155,0,
    236,201};
// ffmpeg -compression_level 8, 24-bit mono at 96 kHz, one block of 4800. 3245 bytes.
static const unsigned char kFlac_ffmpeg_level8_24bit_mono_96k[] = {
    102,76,97,67,0,0,0,34,18,192,18,192,0,12,61,0,12,61,23,112,1,112,0,0,18,192,2,165,73,119,116,178,42,220,48,212,2,65,119,253,119,140,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,
    46,49,48,48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,123,12,0,18,191,180,86,255,
    255,255,0,100,1,0,199,226,1,43,139,1,142,223,1,241,199,2,84,41,2,181,232,3,22,238,3,119,31,3,214,105,4,52,173,229,153,84,191,152,107,207,74,193,50,45,252,195,87,250,156,136,75,243,
    220,17,75,243,82,3,17,165,89,137,20,72,96,176,65,39,70,20,179,173,200,229,115,59,20,196,115,40,203,21,100,96,165,134,60,16,241,157,206,65,16,41,33,73,51,148,165,41,88,174,101,20,
    209,143,35,180,173,82,72,89,5,136,22,40,72,129,226,134,185,178,152,74,248,74,134,148,61,6,218,90,189,46,218,92,151,164,173,101,224,91,129,44,27,130,109,246,127,4,184,93,30,161,229,
    2,69,4,169,243,242,153,185,185,186,216,229,245,95,86,170,50,105,147,12,170,68,114,33,8,241,132,136,89,215,202,210,150,33,108,150,93,98,146,205,115,78,52,81,42,180,18,66,251,106,63,
    38,246,56,179,15,98,114,86,162,72,39,43,243,105,42,218,218,169,70,171,217,229,60,171,71,43,72,148,50,74,164,97,205,14,44,49,98,180,84,57,156,238,41,10,42,156,167,99,40,86,153,74,
    82,32,169,85,16,146,172,135,138,52,65,39,18,173,202,111,126,39,227,87,132,241,168,30,224,151,13,131,208,19,130,104,106,131,112,215,200,219,121,41,54,196,216,188,22,191,45,111,75,
    222,151,164,157,46,94,221,47,143,146,100,245,13,80,181,27,68,198,191,11,133,221,175,239,91,13,248,38,159,130,200,9,48,26,16,6,134,1,34,129,227,0,145,128,73,65,120,46,11,91,223,99,
    108,146,90,247,188,151,164,125,4,224,37,128,213,2,80,23,231,191,67,84,27,173,19,38,147,35,120,108,22,150,192,181,31,43,151,210,229,113,127,20,181,108,161,61,201,56,147,52,141,119,
    73,222,66,56,139,28,177,24,196,51,14,162,160,206,81,20,117,40,228,132,121,138,195,44,164,83,177,72,199,120,194,71,22,16,73,215,136,211,55,59,180,171,164,100,34,220,209,11,41,165,
    52,205,206,233,33,46,165,104,135,133,26,49,225,69,133,44,69,149,140,98,20,198,96,197,133,73,140,193,18,17,227,160,69,10,177,8,50,142,227,32,198,113,218,42,8,226,36,138,102,114,169,
    154,98,76,120,162,206,78,100,205,74,88,184,166,160,149,22,162,88,47,62,215,109,195,105,118,245,4,136,4,138,27,90,252,53,77,248,108,125,173,134,184,182,26,65,100,55,177,170,213,125,
    116,74,165,73,59,200,149,73,17,140,174,35,197,44,233,51,206,133,81,16,196,65,75,10,60,56,145,9,16,241,196,153,84,138,236,174,234,168,84,171,81,85,89,72,246,107,189,22,207,118,247,
    90,43,186,21,102,106,145,212,68,58,152,206,114,144,66,14,128,137,8,240,237,28,231,29,193,16,12,104,28,144,100,4,20,97,198,28,32,162,135,40,41,66,140,16,192,204,4,64,115,12,24,129,
    78,16,227,8,57,132,20,83,28,226,144,102,25,133,81,218,42,20,168,43,72,179,18,85,226,167,58,243,165,94,138,153,202,188,220,175,234,222,188,138,156,73,11,68,228,212,100,171,213,40,
    222,66,72,214,95,87,101,86,35,221,61,82,198,152,88,225,227,9,35,102,84,153,205,143,203,107,9,173,244,227,254,9,180,183,210,148,147,175,179,253,235,107,214,90,79,128,214,1,39,3,112,
    46,13,105,111,72,181,164,78,155,35,100,149,151,65,100,2,92,61,65,46,63,224,216,94,236,146,166,225,110,37,68,179,102,220,74,171,114,213,170,75,166,72,36,97,227,139,51,97,203,35,200,
    105,11,41,170,228,179,233,30,164,215,53,205,228,245,250,26,134,161,236,52,161,36,19,87,243,143,65,100,26,163,78,11,24,45,205,203,222,106,191,22,81,174,216,171,32,146,150,99,93,120,
    207,51,221,108,69,156,177,199,131,6,131,5,134,121,22,100,149,46,146,41,212,132,83,181,153,221,153,12,72,131,196,22,48,180,86,212,108,225,165,30,80,88,224,88,224,149,11,81,234,27,
    133,224,254,121,58,210,116,157,37,105,19,130,86,182,147,98,228,249,27,130,240,77,147,231,209,169,75,82,148,181,48,108,53,48,191,90,4,184,252,76,166,159,155,247,248,185,252,93,109,
    207,199,196,183,151,207,65,174,46,190,27,9,185,39,141,96,182,13,113,183,235,182,218,219,5,223,182,203,80,246,4,144,19,205,245,254,173,52,122,11,197,236,61,4,176,181,53,11,99,213,
    112,133,186,250,85,80,87,145,78,130,160,235,57,10,65,216,229,29,131,188,50,69,80,85,134,36,28,240,84,10,193,138,59,134,32,129,24,32,142,29,206,113,5,113,76,50,194,161,70,80,164,
    136,194,57,72,117,138,88,231,142,89,218,101,187,59,25,44,236,70,58,148,138,34,76,181,58,184,138,86,25,36,115,56,201,17,37,41,216,103,16,167,49,197,72,172,17,132,29,97,20,34,136,43,
    142,145,208,100,8,177,144,101,40,67,65,2,65,131,192,129,33,5,142,120,226,206,212,84,144,88,225,225,6,162,61,134,138,30,97,42,109,211,105,48,212,9,83,110,215,43,130,120,153,146,84,
    36,129,40,74,102,254,238,26,199,160,146,137,32,88,225,36,60,65,226,11,24,120,194,198,53,209,5,36,164,149,14,215,34,8,179,18,49,163,158,49,225,69,135,22,118,81,26,119,145,5,44,99,
    197,122,179,169,154,204,210,146,237,236,174,73,23,44,188,132,90,53,90,173,238,247,37,154,203,69,71,34,85,72,165,66,25,46,177,132,135,22,20,241,214,34,74,195,20,199,49,200,16,167,
    48,199,32,82,8,58,130,32,102,12,83,132,72,41,96,141,8,144,143,5,18,10,210,12,129,20,236,16,145,80,174,119,89,16,142,172,133,105,13,84,242,122,38,226,82,141,79,63,173,179,118,152,
    93,252,55,13,66,239,254,61,50,184,122,15,32,216,219,182,168,190,244,168,188,216,77,125,211,80,220,180,242,241,238,60,130,107,11,48,107,159,95,243,169,162,121,255,6,243,91,70,209,
    180,47,133,195,210,215,164,216,23,103,200,251,53,34,224,46,79,123,77,89,175,88,155,11,192,187,31,129,114,45,38,216,155,23,3,212,19,70,219,210,223,177,42,23,141,151,241,117,191,77,
    81,167,9,33,232,190,105,13,102,194,150,232,170,130,9,57,101,44,137,68,242,45,93,229,110,100,163,22,32,179,30,115,216,148,93,70,108,69,242,114,99,242,245,15,70,241,167,30,226,200,
    92,87,229,226,212,182,60,135,148,120,195,204,36,133,171,45,137,103,107,52,139,101,103,65,154,165,65,16,204,67,44,113,33,13,21,100,88,136,228,97,73,17,100,105,203,51,72,242,45,147,
    209,93,170,252,136,152,187,164,241,164,19,75,166,211,101,62,153,38,116,157,92,151,5,201,63,162,84,61,71,238,154,160,178,131,200,23,135,243,246,75,157,79,195,112,158,44,129,103,13,
    48,245,124,167,34,230,212,73,199,148,158,152,235,235,239,41,168,168,203,71,179,209,53,21,40,75,18,194,76,122,154,97,236,220,250,159,171,198,171,238,150,167,144,94,99,106,240,147,5,
    136,22,40,104,163,72,94,102,171,84,181,101,171,196,18,117,243,169,24,114,68,104,205,59,148,230,16,131,184,166,16,103,28,87,6,104,118,4,80,66,133,48,193,12,16,98,3,172,29,224,168,8,
    64,229,10,225,69,40,226,152,82,130,52,115,16,50,68,33,142,97,158,24,88,48,176,65,35,180,232,98,169,212,232,101,50,29,38,44,235,116,169,29,140,165,34,153,46,234,41,97,4,142,72,199,
    135,30,17,44,163,60,102,136,241,90,43,198,88,173,28,208,196,153,133,115,185,152,33,97,134,133,36,82,206,132,16,177,94,235,35,202,186,115,202,105,133,136,60,194,202,44,162,216,186,
    107,13,66,246,254,215,4,241,124,109,37,178,217,53,4,217,56,77,190,61,240,180,253,23,63,4,168,73,131,92,90,23,20,154,74,190,181,27,86,165,187,114,179,188,167,156,151,36,138,164,100,
    145,40,179,150,82,196,111,34,168,167,136,105,86,203,101,162,163,50,17,21,125,90,226,68,26,99,216,148,109,78,109,109,68,214,215,162,86,65,37,22,83,115,43,213,174,220,66,92,151,69,
    119,102,50,221,103,91,41,10,86,40,164,140,178,169,14,116,144,68,140,72,173,28,145,201,25,101,35,25,71,52,33,161,71,133,18,20,36,40,177,197,136,90,179,202,123,181,15,41,177,159,9,
    81,106,45,134,160,154,92,190,255,215,171,193,165,7,138,2,194,1,100,15,166,217,252,55,143,151,167,203,64,73,64,147,129,98,3,202,11,40,122,132,227,117,180,76,38,9,164,239,221,166,23,
    77,132,194,230,214,129,36,11,113,51,171,149,202,225,252,94,203,229,105,210,229,242,93,158,129,182,93,11,212,180,157,55,73,105,112,26,193,246,46,130,120,75,252,146,108,110,13,192,
    210,129,110,31,130,100,153,38,214,180,218,87,134,176,254,123,5,152,94,108,207,124,161,61,154,175,102,187,204,91,171,160,164,145,5,36,205,42,21,212,69,149,166,105,84,174,147,150,
    114,69,37,208,207,49,38,52,169,103,144,211,146,138,151,60,196,171,89,180,165,185,166,60,227,78,121,77,68,100,162,122,212,147,139,8,22,20,105,211,144,69,145,60,200,226,146,117,145,
    68,88,194,193,5,134,60,40,144,226,65,6,133,70,25,212,116,157,4,98,29,200,195,60,196,88,141,116,148,145,130,67,132,138,254,200,71,169,103,26,113,108,216,220,212,184,242,133,144,91,
    15,32,185,78,23,48,77,47,109,112,187,255,250,38,117,52,254,37,70,247,225,40,219,231,228,52,130,72,95,106,191,167,37,22,175,41,231,60,203,86,152,151,74,161,8,204,197,71,42,177,25,
    14,178,22,56,177,15,59,81,85,214,237,32,177,198,140,26,24,52,32,183,76,162,166,170,50,42,146,99,202,75,22,71,170,81,172,180,83,61,208,142,68,58,74,130,36,203,49,10,227,57,70,114,
    136,80,137,24,131,168,98,5,64,101,24,43,2,168,81,12,24,129,12,20,129,142,80,238,16,163,136,57,3,176,83,24,50,194,164,66,4,104,114,68,41,216,35,76,194,59,25,133,91,172,235,226,45,8,
    172,167,121,9,33,103,52,162,69,26,117,217,68,145,22,171,233,33,228,90,181,215,209,217,40,239,16,88,194,69,18,16,120,67,202,169,41,46,179,139,21,236,182,69,16,210,175,17,21,72,213,
    68,100,206,36,163,202,52,133,204,124,90,235,90,162,230,9,148,201,106,30,80,123,13,135,189,1,110,9,80,217,95,189,45,209,40,11,96,254,47,73,213,250,236,254,18,161,49,165,160,222,18,
    112,104,129,172,37,77,180,63,182,147,149,240,166,185,42,244,111,74,39,181,21,25,42,73,132,149,189,12,74,171,85,185,89,53,21,148,178,137,101,197,213,108,92,71,226,106,190,105,104,
    122,55,103,151,51,27,124,73,6,136,30,40,104,225,226,133,142,22,32,182,122,189,90,226,68,18,115,93,150,196,138,44,202,170,118,89,25,68,121,86,84,51,78,210,44,230,136,156,100,59,84,
    164,149,165,60,99,213,218,197,144,215,52,161,34,132,144,74,154,130,92,93,95,209,52,187,76,30,192,179,3,72,31,137,37,48,53,130,228,152,63,130,72,15,40,38,141,146,100,189,242,112,
    109,55,91,240,122,134,152,30,112,213,9,81,120,78,38,237,184,184,107,5,152,36,130,98,90,162,216,121,193,34,135,185,179,68,184,73,65,35,129,39,30,64,241,1,37,11,112,148,55,117,119,
    169,212,250,224,186,47,143,191,110,233,41,163,240,187,76,159,194,92,18,64,244,63,232,190,92,166,111,246,147,53,252,183,61,205,100,122,170,169,22,87,186,216,139,20,208,135,142,73,
    138,163,52,116,149,132,33,70,73,220,198,48,136,24,144,81,224,231,142,145,29,204,116,12,88,172,87,25,231,98,185,24,84,99,169,152,169,34,169,153,36,84,59,72,242,150,114,202,211,26,
    41,174,167,36,239,117,157,228,91,21,74,162,60,84,163,8,84,157,103,65,8,66,12,146,144,166,97,210,83,48,132,16,164,40,168,59,56,169,58,204,177,75,17,167,121,85,76,166,120,131,67,6,
    134,9,16,149,79,144,242,158,196,161,49,87,184,152,189,165,227,80,250,255,227,204,11,48,184,159,221,195,76,9,24,22,113,121,107,32,242,143,41,168,91,26,227,71,15,24,246,93,35,81,92,
    147,47,40,133,185,22,233,49,98,18,115,69,121,137,57,35,26,35,217,216,200,100,16,179,181,148,169,84,186,104,133,152,149,85,118,156,104,195,68,18,115,221,238,89,75,86,185,104,172,
    214,36,195,194,134,133,26,48,240,129,32,129,163,47,169,218,100,160,141,51,168,170,81,90,20,208,164,132,44,57,97,82,65,146,17,161,94,17,35,177,197,32,167,48,134,16,85,12,240,233,43,
    132,113,74,82,5,120,168,43,157,35,30,16,211,41,218,98,76,75,50,145,10,88,163,197,61,155,74,106,182,102,174,113,106,126,70,205,205,46,35,101,46,61,141,228,231,199,230,206,226,250,
    118,212,120,130,208,182,38,122,238,178,125,132,145,246,133,228,68,39,146,133,168,210,27,205,67,107,233,180,154,109,62,155,26,248,74,255,86,159,38,143,193,104,31,70,217,176,38,134,
    160,61,193,168,9,130,219,162,248,37,128,179,128,177,64,214,13,64,74,133,239,151,45,92,147,45,131,202,11,48,121,130,202,26,81,228,19,73,112,145,7,148,74,55,189,9,232,138,140,72,129,
    35,15,20,222,236,73,19,164,60,80,176,161,161,5,144,211,132,132,26,196,148,105,75,233,137,196,162,63,46,107,114,231,115,111,26,227,197,4,128,188,213};
// ffmpeg -ch_mode indep, 16-bit stereo at 8 kHz. 686 bytes.
static const unsigned char kFlac_ffmpeg_ch_mode_indep[] = {
    102,76,97,67,0,0,0,34,2,64,2,64,0,2,62,0,2,62,1,244,2,240,0,0,1,144,142,90,198,150,45,233,101,174,60,47,46,55,117,15,195,116,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,49,
    48,48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,116,24,0,1,143,67,78,255,255,4,146,
    8,124,11,80,12,241,13,157,13,200,13,245,229,157,225,174,244,73,104,44,247,228,142,226,161,163,191,96,128,110,36,197,91,14,98,140,96,140,197,28,185,82,82,221,22,75,73,53,246,145,59,
    90,105,198,149,57,189,179,111,81,252,221,92,160,74,138,172,112,167,48,64,203,4,57,194,16,162,39,200,190,52,67,200,45,76,52,162,150,194,166,41,30,212,55,68,192,146,218,37,230,233,
    184,244,211,9,198,81,86,196,91,183,61,91,20,227,93,153,199,88,225,16,16,64,168,28,236,21,100,37,137,92,111,235,249,244,185,73,133,179,61,141,201,61,66,112,189,63,218,144,158,124,
    173,219,152,102,104,33,34,148,228,57,34,190,93,106,154,226,53,28,115,48,161,89,199,49,99,23,142,55,31,175,123,212,23,187,131,98,147,125,87,154,143,130,220,94,221,173,179,111,84,
    198,12,88,234,113,71,72,118,132,42,24,200,188,82,85,68,138,221,34,93,138,84,59,16,246,78,165,56,23,4,146,210,117,101,163,205,114,81,21,12,237,84,45,151,181,170,124,152,189,51,180,
    33,10,57,6,8,209,200,117,29,249,8,95,194,23,252,178,4,161,80,72,172,122,161,56,6,24,24,20,39,40,49,148,54,252,56,100,55,172,54,251,150,116,158,207,9,4,144,115,192,227,185,134,7,
    201,253,90,1,186,240,230,72,129,202,96,135,97,223,138,161,101,9,37,166,180,37,186,19,62,125,199,189,154,65,174,139,93,255,190,31,118,150,184,156,17,14,40,113,12,12,96,228,6,56,134,
    41,145,229,54,74,124,24,90,187,221,2,137,17,220,79,67,247,197,193,162,210,22,244,133,160,107,4,144,36,66,105,14,232,114,175,76,45,26,228,223,28,105,4,97,69,28,160,231,56,65,71,81,
    2,54,8,178,82,227,253,108,46,143,85,123,206,191,69,240,156,75,215,175,63,244,54,222,148,241,39,52,40,209,142,130,135,73,220,67,188,138,134,98,71,109,157,208,65,4,48,129,136,145,68,
    18,199,64,210,18,107,252,180,180,244,44,216,64,75,56,214,117,238,214,182,203,97,169,155,38,98,221,227,150,40,231,80,161,156,65,67,181,130,22,136,164,70,247,86,105,26,33,102,51,150,
    116,252,55,18,106,100,89,111,145,242,94,121,121,223,82,235,82,181,73,202,222,90,166,213,168,162,44,83,10,192,32,144,85,28,135,48,173,98,241,68,180,162,212,94,108,83,200,95,42,62,
    44,136,8};
// ffmpeg -ch_mode left_side, 16-bit stereo at 8 kHz. 626 bytes.
static const unsigned char kFlac_ffmpeg_ch_mode_left_side[] = {
    102,76,97,67,0,0,0,34,2,64,2,64,0,2,2,0,2,2,1,244,2,240,0,0,1,144,13,55,170,27,120,248,207,193,99,238,172,54,74,53,152,156,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,49,48,
    48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,116,136,0,1,143,21,78,255,255,4,146,8,
    126,11,81,12,242,13,158,13,200,13,246,229,157,177,178,196,49,20,118,119,117,111,32,97,197,127,53,128,124,211,144,241,140,83,138,98,6,39,4,46,24,145,100,18,23,248,30,105,67,215,133,
    206,221,94,104,130,234,248,189,230,173,69,236,110,177,46,42,134,58,195,7,44,112,236,96,81,37,24,45,142,122,53,26,141,164,136,140,195,22,118,202,234,18,210,120,91,201,19,3,125,112,
    191,172,89,30,206,238,101,249,139,203,135,61,25,108,243,48,167,48,81,144,41,198,43,156,56,247,16,47,41,169,194,216,109,67,98,217,105,51,94,201,45,172,105,254,129,229,222,139,215,
    27,74,113,101,20,132,134,81,8,117,43,163,69,26,116,202,58,78,120,119,140,67,24,84,21,89,102,46,233,173,110,11,219,46,146,157,180,148,123,173,19,241,41,223,132,166,253,124,217,231,
    60,87,40,168,56,132,20,101,48,69,122,152,150,53,12,91,190,25,4,77,12,143,35,22,196,205,207,141,81,235,122,201,227,114,205,56,183,77,149,188,82,90,49,105,172,148,19,34,139,209,25,
    156,59,8,64,83,194,152,168,32,65,170,207,66,126,16,244,61,143,32,210,178,114,59,83,191,206,255,232,191,245,167,251,175,254,103,255,135,255,238,128,14,57,174,222,125,14,234,138,238,
    129,206,248,145,8,248,78,99,193,21,172,150,216,201,202,68,132,190,17,53,74,134,42,49,70,59,84,141,78,77,18,69,201,13,255,140,135,132,116,103,222,145,169,77,145,133,196,194,61,136,
    132,234,205,70,16,224,144,163,4,38,80,72,96,189,160,151,156,73,12,193,19,67,66,117,33,137,109,9,154,17,104,17,177,10,1,6,66,40,33,128,182,16,72,120,40,68,208,70,176,74,216,147,134,
    8,104,26,35,100,156,100,163,69,35,21,44,145,129,12,133,34,49,2,40,168,4,81,18,42,9,138,73,101,197,151,35,138,142,208,241,27,70,71,216,177,15,17,34,39,26,93,136,144,135,48,71,166,
    132,51,147,100,34,93,216,200,250,162,56,133,242,136,83,161,5,218,33,74,204,233,215,86,239,181,101,52,55,81,169,152,166,34,26,214,109,137,146,161,33,40,14,177};
// ffmpeg -ch_mode right_side, 16-bit stereo at 8 kHz. 628 bytes.
static const unsigned char kFlac_ffmpeg_ch_mode_right_side[] = {
    102,76,97,67,0,0,0,34,2,64,2,64,0,2,4,0,2,4,1,244,2,240,0,0,1,144,95,20,165,249,194,83,40,205,232,222,141,71,123,161,77,135,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,49,48,
    48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,116,152,0,1,143,114,78,255,61,127,163,
    127,214,127,238,175,249,159,254,23,255,190,0,55,230,185,218,255,13,143,51,188,47,151,2,92,129,255,187,111,128,90,50,65,80,136,247,35,86,165,97,182,56,156,136,115,126,25,254,141,
    242,93,90,125,141,148,83,55,223,95,216,67,241,11,132,215,243,12,170,216,145,217,185,38,36,57,177,188,172,102,62,19,179,21,153,204,234,76,133,237,159,18,10,153,44,140,175,137,127,
    198,229,204,221,54,213,25,164,102,173,141,85,4,71,17,205,166,69,17,112,207,51,84,124,181,163,226,236,142,248,133,173,99,163,73,166,253,154,69,129,18,216,65,35,74,193,19,72,106,74,
    154,104,156,151,243,111,242,121,151,214,85,173,66,147,62,43,48,83,93,81,173,74,39,67,116,68,216,152,149,9,76,73,154,24,135,70,9,75,147,108,237,206,37,241,245,116,32,165,201,143,
    177,101,66,166,234,141,83,160,203,178,17,49,9,174,26,34,49,145,205,16,110,152,3,14,12,10,19,150,24,204,27,126,28,50,27,213,203,59,121,94,72,148,88,116,207,2,127,212,0,187,128,103,
    82,36,66,5,120,96,135,32,99,44,196,148,191,248,27,225,168,2,225,226,240,180,168,123,19,25,57,75,168,123,101,239,65,242,95,81,162,35,130,48,65,195,20,112,102,28,32,204,65,13,42,73,
    34,53,83,230,110,17,7,84,28,150,9,16,54,211,161,176,36,127,137,18,217,26,194,235,85,23,194,182,28,83,99,42,86,205,200,158,100,157,194,140,17,128,118,132,48,162,184,140,183,125,183,
    109,54,74,18,156,216,243,9,43,245,77,45,66,219,65,112,62,2,240,122,6,186,117,68,91,8,98,152,97,222,98,33,13,17,105,58,232,171,98,12,65,5,48,42,24,71,87,101,218,117,109,105,164,224,
    185,108,61,4,194,150,172,109,48,182,227,83,190,215,168,254,37,80,197,80,163,16,82,134,29,33,24,34,221,152,210,166,26,42,239,25,84,161,9,43,186,122,229,119,228,174,195,228,253,19,
    173,162,81,254,202,104,138,241,79,70,106,74,55,166,220,121,149,40,56,139,14,113,92,32,236,32,140,57,228,76,180,129,60,150,13,66,139,174,186,32,209,199,162,128,148,11};
// ffmpeg -ch_mode mid_side, 16-bit stereo at 8 kHz. 616 bytes.
static const unsigned char kFlac_ffmpeg_ch_mode_mid_side[] = {
    102,76,97,67,0,0,0,34,2,64,2,64,0,1,248,0,1,248,1,244,2,240,0,0,1,144,52,6,139,152,217,254,163,26,136,246,232,223,54,222,230,69,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,
    49,48,48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,116,168,0,1,143,219,78,0,196,5,
    76,9,36,11,219,13,89,13,219,13,216,13,217,229,158,118,171,72,81,68,37,135,250,142,137,98,55,191,9,128,117,238,87,73,71,41,7,41,10,132,78,184,152,111,159,226,91,39,174,150,187,219,
    169,168,66,213,233,91,15,198,231,146,239,123,8,242,144,28,240,229,57,194,20,114,136,230,76,35,213,113,211,25,158,232,49,166,103,110,160,152,178,232,79,172,243,224,147,106,253,36,
    54,171,145,152,204,132,53,25,178,139,99,74,214,98,138,80,174,56,196,48,199,73,210,115,220,123,23,109,229,251,4,177,53,244,141,204,36,130,83,178,246,193,186,245,190,254,82,172,237,
    41,158,43,153,198,118,57,106,197,60,236,243,137,25,108,81,24,96,143,41,8,172,97,42,194,83,169,91,83,62,90,55,215,153,241,100,124,54,169,47,190,75,249,55,176,141,42,148,98,16,83,25,
    199,24,241,136,132,156,88,134,247,84,161,36,74,177,213,217,219,151,232,107,88,62,215,227,102,214,157,203,111,40,177,19,80,237,140,185,137,203,138,201,70,144,68,29,194,48,198,64,
    134,67,29,168,244,54,114,211,215,51,197,156,148,44,97,121,73,223,231,159,244,111,250,203,253,217,255,49,255,194,255,247,64,6,252,215,114,157,158,249,161,235,121,7,30,19,52,149,111,
    64,208,10,206,36,165,54,156,156,71,80,151,83,77,49,44,113,169,14,153,142,34,228,116,51,238,155,104,244,154,61,73,89,92,73,105,156,65,68,108,84,67,92,237,136,138,209,179,92,162,33,
    53,40,208,158,184,214,91,147,138,50,147,207,246,236,228,47,18,223,172,230,252,143,70,114,103,49,42,104,141,72,101,212,73,176,135,195,89,160,139,178,227,104,171,170,34,225,99,65,
    118,247,72,237,210,166,116,77,85,140,237,27,74,70,51,78,34,26,78,9,90,164,166,149,39,201,23,100,183,29,49,10,45,106,64,255,70,59,225,10,194,146,37,107,201,9,172,100,100,132,148,70,
    70,105,153,144,211,110,105,154,248,166,254,244,67,214,43,198,148,171,72,86,47,79,22,137,14,155,50,56,202,53,54,196,37,108,54,236,67,106,132,198,160,251,217};
// ffmpeg, 11025 Hz (rate written in Hz) and blocks of 1000 (16-bit block size), 16-bit mono. 903 bytes.
static const unsigned char kFlac_ffmpeg_11025hz_blocks_of_1000[] = {
    102,76,97,67,0,0,0,34,3,232,3,232,0,0,153,0,2,126,2,177,16,240,0,0,4,79,88,99,175,118,91,150,52,176,22,199,60,2,117,104,31,85,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,49,
    48,48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,125,8,0,3,231,43,17,57,74,0,0,3,91,
    6,116,9,20,11,23,12,122,229,156,62,189,18,25,96,143,175,26,16,105,32,25,100,109,204,151,77,77,125,169,158,41,80,65,16,161,139,56,130,24,196,40,229,250,45,235,159,253,125,65,187,
    109,74,98,235,161,71,102,153,138,121,82,209,143,145,7,245,219,68,160,91,240,223,65,167,38,209,12,106,140,241,72,160,71,32,70,8,91,16,215,126,74,55,92,45,80,61,154,245,121,93,231,
    21,24,118,120,205,43,164,118,209,133,144,178,232,250,44,215,31,43,198,191,188,246,92,199,17,202,195,175,12,36,167,145,11,57,178,80,254,193,191,4,205,150,102,52,68,245,17,103,121,
    206,161,200,33,132,51,216,194,232,129,56,163,200,147,209,171,213,220,94,98,180,100,114,49,213,236,118,243,150,70,253,23,79,19,199,189,68,174,161,105,99,107,221,170,103,25,7,43,32,
    228,228,119,18,34,134,132,104,72,163,126,91,40,218,195,35,144,103,168,98,212,17,10,41,68,117,156,76,242,126,246,53,184,188,123,84,92,227,72,190,118,73,149,219,132,91,206,167,156,
    187,207,19,201,9,239,37,11,156,212,75,196,94,153,165,17,220,57,24,166,91,147,76,75,22,207,203,94,73,112,130,96,226,198,68,8,205,17,144,132,49,80,35,121,223,184,249,255,110,253,26,
    143,127,23,214,121,91,4,76,14,187,20,215,115,216,199,227,151,58,165,147,50,77,166,170,116,137,170,49,60,196,33,70,81,72,99,187,45,83,117,205,129,3,217,132,207,160,219,114,101,132,
    90,16,87,178,17,217,103,122,170,189,16,54,168,124,201,104,209,98,9,165,198,226,216,143,57,90,118,17,94,101,235,181,16,147,11,114,238,13,84,141,58,90,98,76,136,97,77,35,12,243,20,
    238,114,40,165,214,55,164,243,146,247,47,248,158,110,215,63,132,45,150,116,90,145,112,201,177,6,161,194,98,36,109,112,36,196,150,81,48,229,165,12,36,173,119,96,143,25,10,229,92,
    149,250,70,230,186,255,242,141,85,249,19,108,86,51,200,103,149,84,84,149,144,232,167,185,105,231,236,92,167,216,110,106,249,172,143,43,60,103,209,151,114,204,106,148,155,236,46,
    160,219,67,77,16,220,152,86,170,96,231,176,71,176,130,18,50,115,178,183,173,19,249,152,93,48,219,68,52,133,157,26,51,96,229,208,163,76,228,153,154,137,252,78,193,249,66,217,15,228,
    207,47,209,53,93,156,202,100,86,82,54,163,162,189,18,245,54,23,159,125,20,139,65,133,136,230,138,178,169,140,194,37,80,207,247,124,249,15,158,182,169,183,19,11,35,85,78,242,191,29,
    189,164,74,69,18,136,189,216,37,6,251,26,149,53,139,178,139,197,94,73,28,237,33,80,231,56,255,248,109,8,1,102,43,17,250,74,252,129,253,242,255,142,1,48,2,186,4,18,231,45,35,51,120,
    32,163,225,23,119,159,29,224,58,138,138,231,103,6,73,32,70,8,1,136,53,4,193,96,158,132,64,182,95,44,221,250,198,161,212,53,130,32,32,5,193,136,188,192,147,187,163,176,81,20,130,80,
    110,14,161,104,94,38,167,217,167,8,173,24,34,92,181,97,237,195,207,64,104,178,76,241,166,22,97,174,22,163,100,205,85,199,249,47,153,73,42,205,54,201,130,125,159,166,11,111,102,137,
    12,178,80,109,80,172,75,177,106,60,71,120,169,44,210,241,9,39,113,48,67,0,248,166};
// ffmpeg, 50 kHz (rate written in kHz) and blocks of 200 (8-bit block size), 24-bit stereo. 1605 bytes.
static const unsigned char kFlac_ffmpeg_50khz_blocks_of_200_24bit[] = {
    102,76,97,67,0,0,0,34,0,200,0,200,0,1,235,0,1,245,12,53,3,112,0,0,2,88,174,154,122,119,199,213,42,38,61,60,105,247,22,96,97,243,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,
    49,48,48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,108,140,0,199,50,207,72,255,255,
    253,0,191,231,1,127,5,2,60,154,2,247,245,230,44,233,156,58,136,35,109,160,83,212,18,182,18,242,2,229,178,235,218,86,152,163,249,46,155,178,238,60,143,9,214,112,253,63,9,198,237,
    123,14,125,155,224,200,74,128,178,48,109,195,170,66,201,73,88,47,166,116,228,158,17,10,17,81,59,19,209,90,21,113,102,22,65,90,21,145,72,19,33,39,63,167,136,226,153,82,236,77,73,3,
    156,222,173,106,26,20,148,107,188,247,113,255,82,90,131,98,44,45,197,185,119,174,64,36,6,67,100,66,69,47,164,134,220,219,189,104,108,173,110,167,84,103,227,236,184,45,55,7,200,213,
    150,109,208,126,138,49,93,52,167,28,249,8,240,154,10,224,186,11,240,211,12,184,217,13,72,206,13,128,191,12,104,176,138,160,152,8,137,244,55,102,180,179,149,50,36,255,53,237,66,192,
    165,162,40,34,121,160,236,188,95,11,216,117,61,247,97,226,245,61,135,49,193,110,154,110,127,134,224,233,194,192,197,54,47,49,9,38,133,180,195,27,147,194,127,4,164,80,5,68,93,198,
    20,95,134,168,99,198,128,97,70,92,88,69,212,74,132,240,248,157,243,72,96,73,241,26,115,218,5,121,34,72,117,94,151,202,164,20,79,243,203,31,249,235,231,252,250,61,204,81,49,122,112,
    70,66,100,13,143,252,124,77,145,249,14,216,94,4,103,93,168,187,18,245,138,163,204,18,40,85,195,34,111,136,153,21,50,54,100,112,240,224,184,233,224,200,152,248,204,75,75,12,12,140,
    76,11,14,76,12,202,143,72,132,132,198,101,103,38,37,37,135,4,2,1,83,11,142,10,13,155,24,31,48,34,48,232,77,113,82,165,74,18,48,219,100,19,50,46,190,77,159,20,235,38,120,222,206,71,
    118,183,66,149,255,111,72,93,185,110,101,125,171,251,216,127,171,243,33,235,107,65,109,254,190,215,213,239,235,170,11,74,43,58,169,74,68,230,36,166,34,50,50,178,243,21,231,45,72,
    200,88,146,165,122,181,62,238,81,206,65,218,39,215,237,89,101,105,113,130,17,30,88,161,97,35,129,70,0,129,224,65,208,255,248,108,140,1,199,50,164,68,14,25,106,14,43,158,14,54,99,
    230,40,176,188,162,35,241,53,89,188,63,154,249,31,67,251,104,42,139,169,90,153,234,26,161,170,146,165,106,118,165,170,62,156,41,186,145,168,222,131,168,30,209,137,22,147,254,108,
    203,169,31,119,216,213,29,35,233,235,240,54,218,45,54,111,63,178,214,246,58,180,77,51,85,163,186,185,31,89,240,159,7,253,210,37,51,214,69,116,217,197,187,221,85,247,72,134,75,82,
    141,149,116,189,101,243,50,233,166,77,34,111,117,27,60,221,102,251,54,57,177,205,26,102,19,16,151,124,174,165,47,37,233,25,95,85,224,91,141,157,216,13,84,212,29,24,254,239,135,222,
    250,28,225,42,46,78,183,250,253,15,119,87,19,81,29,89,7,29,123,114,163,85,147,165,248,189,27,225,222,9,240,40,85,216,255,58,16,164,234,107,170,90,202,174,139,7,179,91,67,181,171,
    104,183,107,131,183,59,132,182,107,103,181,59,45,177,154,250,173,26,169,160,61,97,157,9,40,232,138,169,233,49,47,155,114,141,242,235,240,219,38,163,252,144,50,12,83,144,252,58,68,
    73,224,118,19,224,115,219,54,61,115,83,17,116,63,67,221,251,223,126,150,85,235,59,119,164,46,85,229,234,110,167,162,35,49,65,139,116,95,168,159,251,171,15,254,70,7,255,91,239,152,
    179,194,176,88,208,52,11,46,5,201,75,176,249,82,37,138,18,34,120,137,194,103,203,152,60,120,168,219,2,130,228,5,5,9,15,190,46,96,84,249,193,3,231,143,9,190,16,38,44,208,89,226,131,
    110,12,182,120,152,171,164,14,153,104,145,114,137,207,177,216,185,121,84,83,36,135,232,232,159,233,47,151,68,104,25,225,174,58,216,222,253,233,221,189,14,125,181,222,42,211,214,
    107,49,142,109,81,167,114,165,41,29,164,168,221,242,75,105,138,53,181,158,222,199,182,174,96,57,177,171,186,127,78,207,121,210,255,238,179,179,224,151,43,113,222,27,110,98,201,175,
    210,118,130,143,215,91,228,104,167,69,2,12,166,249,52,137,42,113,73,20,75,174,105,75,192,225,118,255,248,108,156,2,199,50,126,68,0,113,58,128,58,123,128,30,31,252,197,7,151,209,68,
    62,38,65,229,24,24,41,38,4,178,166,181,85,25,154,248,55,86,68,205,63,94,116,58,106,60,122,162,164,251,77,89,69,95,87,79,101,99,95,99,117,89,119,119,51,27,155,123,233,29,92,174,210,
    87,93,33,145,200,45,101,119,82,106,233,109,213,212,130,67,105,37,153,197,172,130,194,71,87,37,165,179,191,162,182,178,165,172,169,168,160,172,160,165,227,81,227,188,135,201,138,
    190,94,67,188,51,101,214,38,82,211,73,162,158,240,122,33,2,198,253,179,98,130,22,101,238,76,156,150,147,25,185,17,58,58,40,49,47,58,126,120,108,86,51,83,19,67,210,99,210,147,147,
    51,114,17,83,211,114,227,18,131,146,51,50,215,71,68,37,226,182,54,116,72,64,108,144,137,115,166,12,19,88,109,153,84,8,162,116,149,181,250,36,126,91,150,126,80,145,254,66,47,126,48,
    125,254,27,158,115,22,215,76,176,70,89,178,236,45,210,104,31,209,197,26,81,89,19,115,206,104,75,41,25,105,217,20,47,151,125,138,201,176,92,132,41,114,79,1,250,126,73,97,40,42,101,
    44,175,151,50,182,94,138,233,109,41,165,92,153,147,50,44,254,188,174,115,118,192,178,10,96,232,140,40,106,188,149,122,36,139,102,153,36,137,170,96,153,229,249,154,75,150,108,247,
    175,218,86,94,79,33,52,101,149,138,109,221,183,240,155,148,18,254,104,13,169,236,16,97,25,20,81,81,22,209,122,25,129,137,27,81,139,27,129,152,25,194,76,88,194,139,80,174,10,40,140,
    136,209,214,58,166,92,196,20,210,114,255,61,45,211,34,183,169,41,74,28,146,227,186,206,173,59,237,238,74,153,37,41,118,104,146,229,155,229,246,179,89,239,237,199,76,119,42,154,5,
    113,188,39,6,96,86,37,20,42,69,32,183,24,163,12,104,205,81,173,54,71,8,206,156,35,42,103,204,33,115,43,133,48,144,17,55,209,188,111,151,66,59,214,74,52,110,199,169,241,164,107,123,
    26,245,187,37,28,143,153,18,142,107,203,201,127,50,37,196,162,150,18,14,246,128,92,78};
// ffmpeg, 384 kHz (rate written in tens of Hz), 16-bit mono. 918 bytes.
static const unsigned char kFlac_ffmpeg_384khz[] = {
    102,76,97,67,0,0,0,34,128,0,128,0,0,3,38,0,3,38,93,192,0,240,0,0,6,0,9,145,176,232,64,95,45,187,175,245,150,45,136,2,79,109,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,49,48,
    48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,126,8,0,5,255,150,0,9,78,0,0,1,197,3,
    128,5,40,6,182,8,33,9,100,10,125,230,37,189,224,235,207,119,228,184,101,112,242,31,77,255,29,4,85,18,165,237,191,212,184,164,229,142,213,90,70,240,109,72,34,114,9,18,4,166,217,140,
    200,70,48,220,102,66,8,147,16,223,180,220,164,138,86,150,148,138,142,158,71,78,248,164,45,251,242,116,148,137,8,216,212,78,100,84,34,98,153,73,110,141,21,19,145,28,166,236,61,176,
    234,244,181,253,21,143,82,58,153,230,153,158,39,68,214,52,37,19,8,153,136,25,148,66,53,32,207,145,18,43,54,91,141,25,217,196,114,21,45,139,150,123,148,203,19,137,232,76,39,54,50,
    51,51,113,49,170,27,26,153,13,77,2,72,196,71,73,191,66,238,125,109,21,58,23,238,92,221,75,17,229,88,168,172,237,73,185,54,17,166,67,76,79,116,36,153,230,98,30,53,172,186,229,235,
    61,47,176,93,11,70,65,139,18,120,128,134,18,224,132,88,146,32,38,8,129,8,72,8,4,16,76,1,8,72,40,89,10,232,193,40,49,74,153,17,177,63,32,116,71,15,5,200,66,160,65,96,65,9,136,129,
    34,16,193,8,164,66,96,76,33,49,33,113,50,13,97,200,49,131,144,120,207,24,77,159,163,235,7,35,151,48,190,0,139,17,4,72,16,130,24,20,75,9,67,17,66,28,133,208,134,146,240,100,67,62,
    102,65,248,154,133,96,137,20,32,144,36,22,128,88,33,34,2,10,192,16,8,160,64,36,34,8,74,21,108,16,210,29,4,198,132,108,132,13,24,89,140,159,14,208,129,192,152,136,161,8,176,132,145,
    37,32,180,194,236,73,17,132,24,133,134,132,120,109,25,134,134,205,141,7,136,108,134,136,204,41,12,96,84,77,37,4,20,129,8,65,80,4,65,22,64,137,37,64,182,145,49,105,8,112,181,198,17,
    154,32,113,64,200,146,200,144,40,136,16,88,128,94,64,40,162,8,36,36,33,0,148,16,17,98,65,24,139,105,233,204,99,17,3,12,19,32,241,25,166,64,100,50,97,136,204,43,200,133,194,4,81,5,
    66,129,60,76,67,4,49,107,75,66,49,129,28,220,50,15,12,196,3,32,218,104,116,140,43,196,19,17,224,44,133,225,16,130,160,128,65,80,4,32,136,34,16,38,10,196,8,52,90,136,79,96,47,148,
    33,178,33,188,70,108,145,1,132,88,129,113,4,2,98,64,137,36,128,153,4,197,18,4,147,2,10,208,72,228,77,176,211,56,50,12,232,115,65,161,150,64,113,26,6,140,68,12,150,177,5,17,65,66,8,
    42,193,2,60,145,50,19,242,18,244,162,132,216,77,140,70,59,96,96,144,49,84,76,80,68,81,32,68,148,8,176,64,152,19,1,16,18,16,34,2,36,33,4,32,131,17,32,129,161,49,15,65,33,140,17,67,
    139,160,202,99,137,198,33,144,86,192,69,9,17,8,133,144,90,96,145,100,96,152,19,176,38,45,162,64,216,188,24,50,108,31,3,33,178,198,96,67,158,77,44,50,37,208,188,32,161,18,5,4,8,176,
    144,22,136,185,73,130,244,132,216,133,54,74,235,12,17,251,89,35,72,66,45,4,32,80,84,8,16,67,32,129,48,40,34,64,80,80,11,4,16,134,128,86,200,16,25,6,72,110,144,109,37,155,36,206,49,
    25,187,70,130,14,9,19,11,16,37,133,139,17,11,56,8,201,28,160,65,148,170,142,67,64,200,219,14,13,65,131,9,25,165,136,209,72,193,64,180,224};
// ffmpeg, 5.1 (six channels, the first two are kept), 16-bit at 8 kHz. 1881 bytes.
static const unsigned char kFlac_ffmpeg_six_channels[] = {
    102,76,97,67,0,0,0,34,2,64,2,64,0,6,191,0,6,191,1,244,10,240,0,0,1,144,208,6,201,193,4,241,45,168,238,13,5,199,52,144,22,158,4,0,0,88,13,0,0,0,76,97,118,102,54,50,46,49,50,46,49,
    48,48,2,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,38,0,0,0,87,65,86,69,70,79,82,77,65,84,69,88,84,69,78,83,73,66,76,69,95,67,72,65,78,
    78,69,76,95,77,65,83,75,61,48,120,51,102,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,116,88,0,1,143,216,78,0,1,4,147,8,126,11,81,12,242,13,158,13,200,13,245,229,157,136,178,
    230,55,40,87,151,182,110,227,1,210,63,65,0,106,121,145,196,32,135,28,204,25,150,238,153,109,224,110,137,243,218,244,159,18,38,16,90,8,111,115,74,62,102,90,180,37,47,100,150,35,168,
    118,20,113,132,8,64,42,204,8,133,134,62,208,143,104,141,121,201,182,57,46,86,35,235,27,11,110,200,54,86,18,253,53,105,196,180,132,171,196,121,72,173,50,87,220,146,63,80,168,88,232,
    49,68,20,29,101,10,96,173,57,74,217,153,129,228,55,254,211,113,105,209,198,229,41,255,132,234,83,227,83,98,82,251,135,145,234,211,169,14,97,4,114,8,99,74,242,22,119,232,167,149,14,
    162,140,98,28,118,97,10,105,84,76,209,164,253,188,77,168,73,98,130,88,158,141,130,132,144,106,53,182,159,227,83,137,187,99,181,0,134,152,64,197,130,32,41,35,42,76,90,59,99,166,178,
    165,4,123,136,89,157,181,124,187,109,26,247,224,38,237,164,242,226,83,152,138,215,49,170,180,46,34,80,188,184,48,147,25,135,40,166,24,166,56,84,33,68,127,45,205,216,199,214,168,
    215,37,215,154,69,189,56,6,24,24,20,39,44,49,148,54,252,56,92,55,176,54,251,150,117,230,197,201,23,48,144,224,26,123,4,6,126,253,200,1,133,44,141,24,200,24,115,10,97,146,68,111,67,
    86,109,3,112,45,103,208,107,137,45,9,165,243,151,42,247,184,107,130,204,23,116,145,34,23,24,50,68,28,64,134,12,16,81,2,152,161,27,144,205,90,148,242,11,24,89,146,69,144,164,109,76,
    232,156,89,162,224,104,246,164,39,6,144,53,201,233,69,144,82,88,204,104,131,209,151,158,142,168,226,5,81,0,203,14,224,138,16,99,52,117,204,195,232,89,130,104,146,6,148,36,196,235,
    38,243,254,210,148,150,248,61,231,192,187,251,200,162,44,198,88,194,24,71,48,231,157,11,58,212,179,37,218,86,28,166,8,83,16,49,162,154,68,26,195,232,211,215,192,158,38,116,244,75,
    97,23,154,167,161,251,134,252,88,182,47,103,18,97,75,24,83,148,32,162,164,17,142,177,159,230,95,162,144,212,17,254,17,41,14,75,150,48,182,18,130,112,248,44,147,125,228,189,172,122,
    231,144,90,144,171,70,98,72,44,163,74,107,154,173,49,116,49,16,20,199,28,129,157,194,177,10,155,82,255,101,6,194,31,42,44,132,144,188,113,164,38,1,127,3,172,133,116,6,158,135,34,
    135,36,6,224,114,206,145,217,45,36,202,12,7,245,39,178,240,78,192,24,136,43,152,128,206,16,57,65,8,67,43,165,55,167,172,73,98,94,19,129,188,91,233,169,97,166,39,154,81,233,130,223,
    79,124,39,86,175,82,133,64,43,1,6,20,32,32,163,10,21,78,235,68,180,83,225,216,88,205,145,82,163,60,198,144,47,31,33,232,26,37,162,197,139,89,43,109,236,121,15,64,141,226,172,143,
    186,197,162,101,155,204,204,29,193,78,14,80,192,138,56,56,144,87,152,246,89,51,231,170,198,148,126,93,18,134,54,172,160,186,93,132,192,188,5,193,34,223,2,114,228,136,193,18,98,7,
    104,230,57,85,69,39,138,105,150,68,145,71,64,129,78,192,103,153,200,130,141,40,79,31,231,164,127,201,237,221,146,107,80,75,42,225,244,191,124,19,183,34,209,134,149,200,97,142,8,
    208,194,14,225,66,168,205,28,89,143,49,51,196,30,42,252,132,20,72,98,72,197,163,86,157,209,62,240,159,88,74,237,180,111,77,194,175,153,76,184,67,251,9,35,97,95,132,118,135,40,165,
    3,36,35,6,40,198,82,156,188,106,13,137,178,209,143,157,85,197,161,15,80,148,192,69,112,135,240,188,144,221,0,232,0,226,144,212,174,89,211,187,5,37,125,62,112,132,109,240,138,12,80,
    3,49,142,82,3,160,32,115,12,8,197,65,198,184,212,9,123,70,154,39,198,181,41,223,150,161,165,18,229,184,251,125,159,98,112,78,194,233,227,52,82,5,12,56,192,163,148,24,81,92,51,161,
    83,114,134,178,58,89,157,131,45,71,77,33,176,73,54,63,196,158,44,177,169,13,225,60,90,16,186,16,73,212,71,178,176,243,27,23,89,226,141,10,178,4,16,40,96,163,3,20,113,133,104,99,
    202,149,237,146,155,91,88,106,147,205,71,110,95,122,219,239,61,235,18,147,203,123,228,185,114,198,52,59,48,193,80,80,205,59,177,196,179,21,104,85,29,152,41,7,16,16,134,8,238,237,
    133,13,80,216,52,215,146,179,120,36,163,242,11,117,45,87,9,227,240,150,181,71,234,234,230,200,162,6,41,193,152,57,197,16,58,202,50,45,83,26,164,145,49,35,23,204,82,56,194,202,74,6,
    168,183,173,34,222,73,167,200,74,155,188,210,147,138,147,55,153,59,215,168,44,164,242,18,198,17,103,5,32,226,3,164,83,140,166,83,175,251,207,239,182,207,47,81,47,57,104,154,95,53,
    48,22,12,37,160,49,148,56,84,57,188,54,240,50,51,150,116,254,196,209,58,240,45,255,238,189,100,130,133,0,252,49,224,133,32,112,70,6,57,134,35,99,137,163,232,89,165,188,38,194,105,
    251,233,164,221,66,202,38,97,167,219,68,193,54,218,221,131,44,35,130,14,56,21,161,194,184,65,76,42,58,220,105,31,149,140,232,66,153,10,58,51,147,195,97,229,154,210,66,201,104,73,
    190,215,177,182,169,82,204,201,97,134,136,37,77,132,178,53,71,44,165,14,129,129,68,5,24,113,7,32,83,66,23,72,38,236,250,211,209,43,169,74,158,196,170,247,228,214,248,255,36,77,158,
    147,87,15,84,113,8,42,134,43,5,40,135,114,33,211,232,118,169,142,88,41,161,214,57,198,16,103,142,90,158,194,225,171,31,193,250,39,79,137,125,110,227,204,111,126,124,27,130,126,125,
    127,132,145,62,34,193,26,48,115,138,48,130,132,64,201,114,61,205,103,219,167,37,213,44,86,67,10,53,150,249,62,130,120,105,244,30,147,104,60,225,101,18,234,73,218,232,230,84,113,46,
    250,188,148,17,252,42,136,113,129,14,33,129,209,194,50,186,24,93,22,131,119,137,180,106,251,250,62,186,246,75,76,6,119,10,17,12,182,14,13,14,13,13,8,11,141,229,157,67,174,42,105,
    199,165,136,197,62,136,192,250,192,49,8,10,64,97,130,131,148,20,48,142,16,73,194,208,94,2,88,11,192,212,249,252,89,71,145,47,40,73,198,211,240,179,210,55,194,214,215,167,141,117,
    64,170,10,112,64,199,14,33,65,26,20,240,134,153,254,65,38,89,16,35,14,64,196,135,65,148,132,54,11,89,35,108,44,147,79,36,127,3,220,159,238,214,44,112,241,19,9,149,13,231,207,150,
    41,238,194,164,40,167,14,28,48,128,115,8,21,97,15,43,55,241,122,123,9,202,244,35,181,145,52,195,216,75,252,39,67,73,36,242,68,203,205,132,136,26,21,202,97,72,16,208,139,34,29,30,
    207,232,177,150,163,136,80,130,140,16,160,194,20,238,70,236,45,37,200,153,30,180,182,215,83,113,16,156,186,125,61,41,44,253,38,203,252,74,174,14,99,152,28,193,6,16,226,152,162,175,
    149,17,122,233,172,33,154,114,144,135,16,145,9,64,147,1,101,1,56,37,34,201,36,185,11,239,213,44,199,136,44,67,76,252,175,173,78,207,185,166,89,218,32,134,14,96,163,131,180,114,132,
    36,169,102,202,208,79,193,40,148,243,9,16,89,73,85,204,235,224,34,108};
// ffmpeg, blocks of 128 holding silence, full-scale noise, samples with empty low bits, a constant channel. 1146 bytes.
static const unsigned char kFlac_ffmpeg_silence_noise_wasted_bits[] = {
    102,76,97,67,0,0,0,34,0,128,0,128,0,0,15,0,2,11,5,98,34,240,0,0,2,128,64,244,250,54,117,21,201,69,243,76,126,28,129,229,68,217,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,49,
    48,48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,102,24,0,127,23,0,0,0,0,0,0,224,
    164,255,248,102,24,1,127,2,2,41,101,111,244,172,166,183,57,107,238,225,105,39,176,244,155,55,196,132,70,133,82,6,169,203,123,129,7,91,249,14,67,12,183,155,106,233,26,157,164,72,84,
    228,21,52,220,166,109,240,150,167,181,249,152,17,153,30,239,51,144,117,226,92,70,22,109,148,50,66,115,1,211,197,144,206,114,237,31,194,146,215,174,48,216,130,86,12,232,11,50,228,
    217,63,67,16,233,18,168,69,112,49,47,129,30,112,138,108,117,219,45,131,91,107,241,71,18,90,211,52,74,178,40,160,76,27,14,215,114,202,213,126,215,136,123,69,201,164,246,81,14,223,
    204,202,194,116,60,142,7,106,85,106,183,53,1,209,165,190,250,67,153,91,72,167,190,11,93,164,239,162,51,132,36,107,228,201,210,150,9,158,160,180,22,166,13,172,62,22,159,195,36,135,
    23,75,108,192,172,199,173,210,5,12,117,58,204,83,124,86,133,102,163,88,208,70,146,176,52,193,66,28,54,223,189,211,102,219,206,104,196,224,43,76,150,91,97,42,125,35,63,240,215,37,
    172,38,19,166,47,78,98,116,13,89,195,211,63,184,178,2,35,255,234,146,29,53,168,68,85,84,150,17,231,158,125,239,105,94,171,106,213,33,141,140,140,4,126,6,54,189,181,107,20,184,12,
    121,22,91,238,105,85,4,69,100,212,75,251,90,200,24,232,166,67,43,231,105,235,200,7,117,78,253,185,38,88,237,43,218,60,6,44,254,153,139,134,227,18,86,136,49,196,103,138,238,76,30,
    168,136,115,178,46,207,61,86,188,193,122,200,1,236,69,206,111,248,3,18,47,94,50,87,26,136,241,230,187,239,62,179,244,242,114,78,250,59,144,72,34,145,101,124,100,175,12,39,218,189,
    110,94,152,68,134,4,236,255,48,217,178,239,221,132,180,207,164,20,22,220,236,24,95,10,227,175,124,221,156,161,73,112,67,192,129,104,45,193,20,24,69,211,176,222,254,87,77,39,167,32,
    188,179,43,61,227,157,8,107,141,246,216,7,178,248,174,116,196,213,214,7,63,35,108,146,229,192,25,237,35,18,211,100,155,63,225,197,168,63,59,158,218,30,47,130,95,125,188,187,82,2,
    134,96,1,172,65,2,5,164,27,231,155,57,233,230,187,21,63,167,4,223,198,155,255,248,102,168,2,127,215,75,32,24,2,112,32,129,106,14,64,135,243,20,189,90,235,29,19,244,103,239,144,32,
    64,132,72,40,145,20,33,34,18,17,17,20,88,184,18,49,32,183,41,10,18,25,10,1,66,121,17,50,74,36,136,145,20,4,96,178,138,177,12,135,102,10,40,168,197,162,36,21,6,10,12,1,81,22,158,63,
    232,255,71,250,63,211,254,175,245,255,179,253,188,231,112,199,238,71,93,101,93,224,179,189,200,50,24,154,192,3,33,228,204,146,73,60,134,112,154,19,51,51,225,154,76,243,38,147,153,
    38,100,150,67,146,88,78,72,114,31,36,50,114,194,76,185,136,255,248,102,24,3,127,40,1,130,105,78,0,1,1,177,3,90,4,243,6,116,7,213,9,19,10,41,230,47,120,140,134,186,206,211,57,218,
    77,96,2,204,190,169,132,98,28,161,82,17,161,156,80,165,14,160,200,20,132,42,178,244,184,165,20,63,0,188,40,167,83,223,154,25,144,50,16,205,137,12,232,115,29,184,175,20,145,152,114,
    156,50,2,180,116,115,154,64,188,63,79,120,182,161,92,149,250,255,109,91,145,139,121,93,21,238,102,228,209,55,242,173,147,85,182,215,69,48,240,20,255,248,102,136,4,127,234,78,0,1,1,
    178,3,92,4,242,6,116,7,213,9,20,10,41,230,46,45,150,76,152,31,38,161,61,142,69,65,228,255,29,132,97,136,40,196,4,72,231,24,115,5,24,194,138,35,157,146,130,78,82,78,141,177,126,106,
    91,235,175,239,57,162,44,140,66,17,85,10,75,15,226,35,161,28,101,138,225,208,21,131,168,238,199,53,13,145,116,27,161,47,66,125,109,254,174,202,42,245,217,147,137,93,182,186,228,87,
    42,69,82,237,149,93,182,213,234,103,249,231,252,255,254,134,255,70,127,166,255,213,127,235,206,107,213,109,153,29,45,191,82,120,164,28,101,238,44,1,112,47,192,154,18,8,112,11,132,
    138,18,39,17,40,180,160,187,129,3,2,60,35,94,8,199,4,100,131,18,70,144,134,44,77,84,133,130,25,11,66,72,144,193,0,135,129,19,9,32,134,20,20,68,196,98,44,90,70,136,97,214};
// ffmpeg -min_partition_order 8 -exact_rice_parameters 1, loud 24-bit noise (Rice parameters above 14). 1423 bytes.
static const unsigned char kFlac_ffmpeg_partition_order_8_24bit[] = {
    102,76,97,67,0,0,0,34,2,0,2,0,0,5,31,0,5,31,10,196,65,112,0,0,2,0,164,255,114,209,10,214,26,170,191,46,234,65,35,83,35,206,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,49,48,
    48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,153,12,0,229,64,255,4,194,231,63,219,
    98,46,117,250,17,159,67,42,13,13,127,192,95,177,156,31,179,54,253,188,210,232,59,200,160,76,89,155,105,8,28,17,108,91,227,183,151,62,145,65,248,66,91,148,26,250,125,14,187,200,174,
    2,48,94,14,236,49,22,8,134,107,100,140,85,60,243,127,48,49,160,68,59,218,240,168,50,21,165,155,250,4,100,231,49,12,246,192,33,86,7,21,207,128,222,159,224,227,79,111,17,226,208,100,
    184,134,23,164,148,77,181,40,101,101,200,136,147,141,75,234,244,143,246,14,14,7,8,28,102,90,6,68,13,101,83,147,229,22,130,31,244,144,60,192,86,164,230,113,228,129,33,58,12,42,225,
    100,115,124,246,153,133,140,241,94,153,39,37,6,116,157,127,162,131,80,226,105,138,34,66,24,108,232,49,228,244,95,227,60,25,184,246,214,110,8,52,233,165,114,139,110,131,238,69,30,
    61,89,228,14,96,108,4,77,60,12,86,99,177,85,48,190,253,4,231,159,121,148,47,58,250,236,105,226,28,11,203,196,22,166,164,89,137,138,221,176,137,47,26,171,118,104,29,194,91,66,3,218,
    180,103,26,195,195,117,191,209,194,19,33,241,126,138,126,236,224,95,174,189,185,24,75,116,193,161,14,46,89,144,39,23,239,229,116,113,238,87,23,148,207,96,155,234,126,145,88,127,
    101,60,114,236,232,75,188,106,38,94,221,193,146,154,219,149,192,177,175,25,111,152,91,94,143,182,211,206,109,19,163,73,229,177,106,44,174,15,98,81,96,223,123,10,48,229,31,135,13,
    20,69,163,160,33,100,113,157,71,227,182,8,225,71,227,44,134,98,44,9,20,6,114,46,24,208,38,195,37,238,229,77,110,88,236,38,238,10,74,16,125,129,44,99,128,169,255,127,242,92,201,190,
    211,189,13,107,248,139,29,65,253,83,67,106,163,177,181,50,185,7,52,58,29,103,31,61,92,106,48,192,173,214,46,223,47,66,4,158,150,14,54,197,63,50,67,128,156,197,9,61,0,142,71,32,190,
    10,156,76,59,65,29,169,105,203,224,202,243,41,194,240,115,41,12,231,91,69,193,144,19,199,138,63,112,233,111,41,144,94,14,16,18,162,59,186,180,50,243,15,241,204,4,125,23,70,153,20,
    210,62,210,179,97,106,242,49,63,173,21,113,254,35,13,247,152,196,56,189,196,174,180,130,120,89,158,194,243,64,209,10,161,6,80,76,129,200,206,235,236,138,195,6,69,231,209,222,205,
    51,217,86,228,23,196,24,210,210,62,139,3,41,77,63,93,128,253,38,250,248,161,143,188,59,129,112,85,28,154,142,87,200,35,14,148,87,149,84,31,178,240,151,166,24,198,215,146,107,230,
    69,52,93,84,90,47,182,129,207,161,241,245,69,81,162,178,102,247,144,148,101,61,67,213,65,237,203,216,183,15,254,200,109,231,44,41,228,229,13,167,8,212,52,184,94,13,197,39,5,87,128,
    180,111,188,110,229,195,189,63,184,160,93,60,188,70,226,234,40,195,188,234,97,59,32,73,228,80,159,200,126,63,200,142,195,200,85,55,106,131,242,201,157,27,174,11,20,151,237,96,123,
    159,9,118,236,6,165,195,76,144,243,16,56,233,109,112,127,254,155,109,5,54,216,218,83,56,146,82,122,215,121,189,80,105,213,64,237,110,61,2,111,56,12,15,37,104,34,142,69,212,182,6,
    108,106,14,218,240,165,170,196,28,139,15,131,33,29,232,97,190,75,251,123,239,61,3,165,242,15,45,202,89,232,242,191,15,7,214,18,214,83,183,68,225,40,54,159,227,194,66,239,32,197,
    228,10,113,20,129,2,6,210,104,98,158,65,96,187,219,96,91,169,17,203,7,132,163,78,57,244,8,155,17,65,128,129,131,227,140,126,68,161,180,60,183,118,192,156,57,50,139,23,216,210,10,
    34,167,177,72,131,137,231,154,105,41,172,233,254,134,95,103,22,125,206,90,80,249,234,27,205,135,19,151,72,198,112,195,194,49,28,47,39,157,17,255,80,184,64,27,214,55,233,15,179,175,
    220,250,56,105,103,123,201,229,101,222,243,98,163,53,122,1,86,107,140,146,57,237,49,47,46,22,230,65,41,9,114,247,251,35,227,228,91,66,18,166,11,27,75,110,65,3,53,83,144,251,121,70,
    220,249,95,119,22,246,115,200,58,125,61,223,88,97,47,200,213,3,175,144,140,208,112,103,42,159,22,184,25,187,232,171,212,52,53,39,52,225,246,216,161,59,120,255,182,83,39,186,10,162,
    97,42,115,134,204,120,189,192,65,177,8,214,209,65,80,5,110,19,30,115,30,197,7,61,69,228,251,58,130,98,68,68,41,27,97,192,231,25,230,151,25,174,25,7,244,189,223,66,130,50,200,92,77,
    222,43,38,171,56,223,75,182,54,19,161,167,19,115,12,32,69,23,252,80,80,126,5,137,232,91,203,8,3,45,57,236,184,66,230,126,46,69,203,77,164,129,21,5,120,86,154,196,193,13,60,8,95,2,
    103,193,7,165,241,90,196,129,252,8,134,159,60,53,144,191,157,33,19,233,62,23,144,33,211,157,219,8,106,56,196,126,162,23,137,67,245,68,247,201,50,88,230,1,186,56,234,232,61,223,76,
    116,117,192,43,87,151,143,184,78,248,79,167,211,180,134,133,205,232,16,120,23,153,250,30,186,71,118,73,231,137,63,161,155,196,107,229,99,27,134,220,217,2,80,127,11,74,65,211,238,
    216,79,196,87,218,125,29,209,239,97,24,148,231,15,142,106,22,10,80,101,115,75,168,187,221,173,94,176,79,40,121,13,143,54,198,62,21,67,64,198,232,182,180,79,92,239,25,76,109,74,68,
    246,29,223,83,72,198,110,100,102,74,217,121,160,168,241,192,131,173,194,176,191,61,214,54,94,92,0,218,204};
// ffmpeg -compression_level 12, prediction order 32 with 15-bit coefficients, 16-bit mono. 1102 bytes.
static const unsigned char kFlac_ffmpeg_order_32_precision_15[] = {
    102,76,97,67,0,0,0,34,8,157,8,157,0,3,222,0,3,222,10,196,64,240,0,0,8,157,124,89,222,23,128,154,239,254,37,121,193,182,164,10,152,68,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,
    46,49,48,48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,121,8,0,8,156,20,126,0,0,0,
    218,1,179,2,136,3,90,4,42,4,245,5,183,6,118,7,43,7,216,8,123,9,22,9,167,10,45,10,170,11,28,11,133,11,226,12,53,12,127,12,194,12,247,13,39,13,79,13,110,13,138,13,158,13,174,13,186,
    13,196,13,203,231,60,54,63,170,43,27,224,23,81,158,95,220,235,250,121,249,175,251,149,244,92,22,112,92,241,42,65,131,130,117,4,162,15,111,248,104,0,159,147,63,145,94,123,190,93,
    126,65,253,28,0,52,13,240,51,144,151,0,82,62,31,128,19,12,149,3,132,36,80,164,132,36,9,25,167,32,64,129,135,40,100,144,132,132,132,166,76,36,146,1,168,76,97,10,16,129,224,89,66,
    132,193,9,66,133,3,147,131,34,100,82,74,21,134,44,20,135,68,198,191,200,147,209,73,38,56,146,154,20,33,5,194,230,5,66,194,84,10,96,73,169,64,145,0,134,68,144,194,33,83,33,136,121,
    200,66,6,36,105,33,136,66,98,192,156,35,194,40,113,12,202,76,34,146,98,132,40,25,19,35,2,72,16,54,17,50,82,64,132,32,67,22,66,97,2,6,80,39,3,8,80,144,8,97,72,64,133,132,33,36,132,
    201,8,64,144,40,88,76,153,12,33,9,2,16,104,36,137,0,133,32,72,147,137,132,129,156,83,44,8,114,70,6,132,137,18,18,132,50,136,25,176,40,72,84,44,128,74,83,96,96,97,132,205,160,74,72,
    64,38,155,33,66,74,72,144,156,74,18,17,50,37,112,49,8,145,66,156,49,29,53,100,184,18,10,11,41,140,133,170,12,35,9,10,136,99,11,132,40,71,2,194,37,228,32,80,184,66,145,9,132,48,146,
    204,33,14,4,136,82,39,32,16,50,137,132,12,136,121,12,178,192,36,202,146,68,48,132,32,87,198,66,1,200,82,68,145,66,66,19,19,129,129,2,65,14,80,192,128,78,66,28,129,146,97,36,2,100,
    10,4,161,40,16,194,16,160,82,16,3,144,36,37,64,146,72,4,36,2,24,164,66,16,134,100,8,96,155,132,32,81,200,25,86,192,51,65,33,40,77,52,216,68,44,34,90,97,50,139,54,68,128,76,183,16,
    176,33,176,36,73,72,114,146,20,35,3,11,32,18,9,75,15,33,83,164,73,36,35,144,73,160,116,201,5,129,216,197,9,109,66,164,37,21,138,76,12,144,106,25,16,208,58,201,144,33,137,44,36,36,
    37,144,146,152,96,66,4,112,129,49,194,16,129,8,28,134,144,144,132,9,10,18,68,64,146,24,16,133,50,72,22,73,57,19,8,25,50,48,128,75,13,32,78,12,128,114,5,10,153,161,9,18,72,72,133,
    14,66,20,39,146,1,40,22,18,28,8,76,38,66,104,67,2,20,136,72,64,136,72,100,136,78,18,18,72,18,54,36,200,64,129,138,228,44,112,133,42,24,34,22,6,214,68,137,64,196,88,101,4,166,201,
    140,53,53,34,20,145,33,76,219,12,41,140,10,4,244,148,161,12,36,152,152,74,134,28,97,114,4,196,37,18,148,12,137,204,13,168,33,148,129,40,66,12,82,17,33,76,18,129,161,52,151,9,75,3,
    129,66,133,8,104,75,72,24,64,132,112,146,88,64,144,129,48,66,100,36,33,3,8,78,144,144,48,146,66,4,180,129,2,64,158,16,210,16,146,4,167,2,70,16,36,66,89,32,73,16,136,20,34,64,164,
    10,77,161,72,76,232,100,34,76,129,114,4,49,56,24,192,40,18,70,5,56,67,8,68,194,17,48,40,16,172,50,4,37,16,160,100,50,102,32,72,132,72,23,32,88,132,208,97,154,88,72,49,134,196,34,
    17,133,160,210,88,108,76,65,41,57,10,40,122,26,36,72,86,4,40,242,18,107,73,36,164,48,166,18,202,18,114,66,77,36,38,27,50,100,9,40,80,165,2,145,8,68,41,24,73,0,178,48,200,200,18,52,
    33,24,29,67,146,102,82,194,133,2,82,24,66,201,9,82,20,8,72,76,9,72,66,73,33,50,74,20,33,8,1,144,132,133,144,36,132,33,153,8,64,129,148,132,10,100,3,13,10,16,133,9,133,76,40,74,84,
    41,36,195,138,72,156,132,146,24,193,146,67,164,44,38,163,132,2,65,9,131,36,132,52,49,48,200,134,132,38,50,76,132,186,16,50,201,9,248,240,52,129,9,168,192,146,107};
// ffmpeg -strict experimental, 32-bit stereo at 8 kHz. 1241 bytes.
static const unsigned char kFlac_ffmpeg_32bit_stereo[] = {
    102,76,97,67,0,0,0,34,0,200,0,200,0,2,28,0,2,77,1,244,3,240,0,0,1,144,23,220,165,69,231,213,71,89,11,61,130,184,15,63,65,121,4,0,0,46,13,0,0,0,76,97,118,102,54,50,46,49,50,46,49,
    48,48,1,0,0,0,21,0,0,0,101,110,99,111,100,101,114,61,76,97,118,102,54,50,46,49,50,46,49,48,48,129,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,248,100,142,0,199,206,78,0,0,0,12,0,1,
    43,127,0,2,44,89,0,2,229,184,0,3,80,119,0,3,124,55,0,3,135,54,0,3,147,5,229,162,2,141,66,181,135,141,56,54,174,239,33,215,127,10,32,136,231,115,23,133,217,127,202,22,208,154,27,34,
    168,211,43,200,197,75,37,78,252,79,212,37,238,156,40,102,116,186,133,12,197,139,91,142,216,155,158,217,129,39,173,91,219,203,244,246,173,30,175,196,166,123,183,71,251,125,61,47,17,
    157,157,119,217,116,109,135,175,175,44,83,217,242,63,77,193,172,106,137,163,184,35,2,36,254,2,16,250,19,7,235,237,229,182,74,102,8,219,73,167,133,73,94,27,81,87,70,152,20,33,188,
    241,108,21,237,12,11,202,131,218,241,46,60,133,35,46,102,116,87,77,104,114,95,115,2,86,80,126,218,94,77,216,161,183,92,191,41,86,216,204,45,73,247,145,92,211,229,146,20,10,185,212,
    130,90,144,164,126,124,170,185,155,210,208,219,24,97,14,38,196,80,189,48,203,145,194,122,8,65,152,76,30,175,190,81,243,251,183,61,21,61,71,145,17,123,121,25,32,170,14,221,37,161,
    77,170,158,14,143,0,71,115,69,86,218,81,229,104,106,82,113,215,51,162,39,109,249,15,101,83,63,255,243,130,255,255,250,13,111,255,253,85,191,255,254,229,15,255,255,150,173,255,255,
    224,145,255,255,251,134,115,22,111,88,196,14,200,7,115,251,200,64,191,176,232,24,108,207,19,98,251,179,95,109,122,98,184,160,106,71,84,107,238,21,138,235,83,204,78,135,176,129,31,
    19,2,56,136,198,252,183,98,174,150,199,148,130,74,38,21,84,238,139,189,206,130,137,114,210,125,229,250,54,18,140,174,243,249,219,246,33,197,194,47,169,100,8,62,42,42,217,61,39,171,
    180,60,19,196,138,177,10,206,165,163,8,188,148,99,68,32,121,17,11,33,49,109,146,224,208,149,151,240,152,189,148,51,63,160,51,77,72,83,61,12,169,202,109,45,66,204,31,247,213,217,
    249,251,199,141,205,231,214,224,99,88,226,199,245,252,178,160,234,87,81,81,130,65,177,237,13,32,154,62,10,33,137,5,33,49,161,104,252,180,138,31,166,102,25,63,68,92,43,49,120,174,
    18,30,194,55,247,34,51,22,242,71,113,210,69,244,193,52,78,162,214,136,58,255,248,100,158,1,199,121,78,0,0,37,80,0,0,18,162,128,0,8,191,96,0,3,208,128,0,1,126,0,0,0,124,252,0,0,23,
    7,255,255,245,211,230,46,42,163,246,86,195,129,201,54,173,200,227,10,62,7,160,118,15,26,189,132,101,171,62,212,68,251,155,213,208,157,181,234,129,161,251,78,124,238,232,37,240,176,
    217,9,121,95,109,229,52,77,225,193,166,118,81,145,24,137,166,55,121,29,15,161,37,106,216,54,223,140,214,145,202,220,111,169,55,230,96,240,189,111,45,150,111,34,197,5,167,12,243,
    136,124,241,25,190,114,194,22,185,142,231,76,9,149,51,45,72,87,186,173,153,174,234,233,40,103,86,187,201,166,72,95,228,32,185,224,155,154,255,84,41,45,43,69,245,105,53,34,33,18,38,
    198,146,185,22,125,112,184,157,102,3,79,233,71,179,124,223,181,71,195,43,230,87,176,125,251,187,186,26,108,152,184,137,150,141,76,76,137,243,208,219,206,24,110,227,62,225,223,97,
    183,19,136,141,40,157,100,103,84,183,35,169,89,12,162,123,93,147,17,44,170,181,84,43,123,31,17,176,143,232,93,72,105,39,127,254,145,101,255,254,65,92,255,254,5,130,255,253,231,128,
    127,253,231,216,255,253,254,89,255,254,30,61,255,254,58,151,242,206,160,83,212,75,175,152,36,149,215,110,224,16,192,54,80,75,234,72,2,176,55,224,187,140,32,77,201,25,21,94,243,182,
    35,187,124,159,50,188,215,51,73,46,17,222,178,193,17,24,208,109,129,14,6,164,46,129,0,22,115,34,42,198,68,249,137,75,252,232,186,82,127,54,144,133,198,26,43,145,89,174,179,65,254,
    205,133,145,137,229,199,150,143,87,145,242,154,199,105,195,40,252,212,220,72,116,186,72,221,236,163,178,55,117,38,186,43,179,129,25,9,49,244,179,26,114,42,167,44,121,143,193,197,
    31,115,94,9,0,246,1,59,10,80,37,35,248,87,74,98,167,145,43,94,146,130,92,152,247,89,41,107,118,54,59,200,56,39,44,52,162,194,16,115,30,100,80,84,67,94,214,114,119,243,45,148,235,
    113,11,76,114,224,150,194,121,10,148,255,77,93,204,173,242,193,68,229,174,200,208,223,99,247,108,14,97,107,127,16,133,194,173,253,151,115,96,163,156,155,131,164,81,143,98,213,119,
    251,125,75,47,97,27,12,240,90,195,168,53,225,48,25,16,140,142,104,34,129,18,14,232,215,157,6,117,84,231,172,255,221,248,225,124,54,44,216,123,180,106,200,123,26,134,111,117,220,67,
    148,64,80,224,102,10};
// libFLAC (through libsndfile) at its highest level, 16-bit stereo at 44.1 kHz. 2158 bytes.
static const unsigned char kFlac_libflac_level8_16bit_stereo[] = {
    102,76,97,67,0,0,0,34,16,0,16,0,0,8,24,0,8,24,10,196,66,240,0,0,8,157,31,1,101,178,154,185,180,203,10,129,87,75,184,37,129,127,132,0,0,40,32,0,0,0,114,101,102,101,114,101,110,99,
    101,32,108,105,98,70,76,65,67,32,49,46,52,46,51,32,50,48,50,51,48,54,50,51,0,0,0,0,255,248,121,168,0,8,156,235,80,0,196,1,154,2,113,3,69,4,20,4,221,5,161,6,95,7,21,181,56,64,26,
    111,254,154,250,120,95,142,80,159,115,208,3,88,140,93,221,174,181,242,238,20,152,165,210,34,107,65,58,164,159,171,77,226,205,149,15,36,228,97,49,105,44,132,200,172,130,148,66,90,9,
    18,98,144,187,20,201,116,188,175,69,74,137,73,37,44,148,136,151,138,38,144,177,147,68,245,102,142,105,27,212,122,221,84,113,92,147,80,175,37,22,92,168,163,42,38,172,194,73,14,82,
    72,187,144,21,74,32,177,34,36,40,145,68,22,144,136,147,66,92,82,106,21,25,75,82,76,178,75,196,89,50,37,34,68,79,72,149,171,74,211,82,134,81,71,164,203,46,82,88,162,225,9,200,21,
    100,66,9,150,72,136,77,36,138,225,68,186,66,101,34,92,68,178,65,113,5,100,144,177,19,32,154,22,68,156,130,159,75,212,164,218,98,71,146,68,58,8,159,138,81,41,162,196,114,74,105,98,
    122,210,253,47,100,170,164,212,73,52,176,136,158,130,146,37,49,18,38,132,188,169,9,126,66,140,145,57,23,38,74,92,68,84,197,32,156,145,113,40,202,19,203,113,72,253,18,244,213,77,11,
    206,17,116,133,25,11,69,202,132,73,177,18,242,184,137,51,32,137,235,72,35,68,209,44,162,74,17,41,8,184,65,86,66,83,5,41,101,145,86,17,146,177,92,164,35,38,72,189,65,86,133,82,88,
    150,47,20,187,37,201,59,73,102,40,173,53,66,180,39,137,37,194,34,78,36,41,56,38,69,73,80,146,156,138,145,146,4,246,32,164,120,136,147,18,226,73,36,147,18,76,68,203,210,150,209,122,
    244,169,93,104,87,52,149,106,33,61,69,18,153,44,164,212,146,106,36,236,85,49,40,197,23,164,90,197,36,145,56,68,149,145,50,162,146,104,34,218,201,72,95,165,138,35,74,81,82,36,166,
    68,147,208,36,217,36,165,24,177,105,57,144,150,104,90,94,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,3,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,48,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,1,0,0,0,0,0,0,2,106,68,20,148,66,181,221,241,198,111,38,102,234,105,226,162,176,137,19,68,82,21,18,47,196,68,240,133,66,194,36,32,146,40,33,37,36,181,57,13,181,177,178,99,181,
    252,215,70,36,218,119,239,126,110,218,235,82,194,42,64,132,180,8,16,185,8,66,72,83,18,34,136,90,66,36,136,130,33,90,8,66,180,89,56,79,100,222,166,71,69,214,66,212,80,150,37,37,17,
    78,21,81,68,146,48,136,45,1,66,17,68,32,145,40,66,35,72,74,216,157,42,70,86,165,104,80,211,9,237,102,79,26,55,103,173,188,93,69,69,22,132,73,74,34,83,137,23,218,21,132,100,42,34,
    33,50,42,18,194,56,146,207,68,230,228,238,214,100,92,210,83,209,56,141,54,36,222,148,104,38,73,10,72,66,22,66,20,130,18,41,33,5,80,132,104,66,92,132,44,73,44,68,37,148,69,57,8,70,
    112,81,173,78,90,76,173,42,68,145,161,98,92,138,184,173,82,44,43,73,2,80,145,18,197,8,46,17,133,229,197,52,240,147,244,93,82,140,89,91,139,157,168,216,144,231,149,24,86,165,22,68,
    149,34,69,212,36,189,34,148,147,17,73,36,82,40,144,154,18,86,101,69,195,65,31,58,145,29,49,33,123,17,75,212,81,58,194,137,232,68,168,160,145,34,144,34,77,4,37,194,21,164,21,161,74,
    133,112,10,61,9,9,138,89,42,208,91,210,187,43,24,149,170,168,149,69,68,165,68,181,37,106,45,34,45,69,20,90,37,21,161,56,170,225,127,98,189,194,201,197,49,70,42,78,85,184,75,59,9,
    67,82,19,76,89,69,148,162,9,121,9,46,9,177,9,56,138,76,36,33,132,86,82,5,204,136,170,201,161,113,26,189,17,29,139,22,174,73,18,60,132,171,82,139,201,20,148,209,9,72,168,34,248,65,
    19,41,160,87,35,33,81,56,72,202,41,27,252,241,254,123,255,62,255,160,173,72,32,64,127,93,198,224,20,243,201,242,244,95,140,249,211,152,177,50,166,121,137,173,146,19,38,176,146,198,
    100,236,101,196,216,73,17,180,198,205,137,34,100,53,76,201,89,37,98,84,77,26,77,113,55,219,163,71,217,151,76,215,203,19,212,145,98,87,204,247,73,53,46,172,250,228,89,210,120,158,
    162,94,211,46,107,18,164,134,223,182,66,108,173,13,118,98,38,149,13,174,100,39,50,88,34,70,212,145,9,165,99,109,73,19,102,149,137,175,100,185,58,38,173,102,185,36,157,63,205,83,
    202,185,44,154,62,200,254,68,179,171,90,172,228,179,214,179,87,132,111,44,240,223,73,209,26,84,90,104,168,145,182,150,86,38,178,50,72,148,137,241,165,201,174,205,243,102,116,217,
    91,106,136,168,151,89,228,109,86,17,209,90,116,175,18,219,178,221,39,168,168,243,182,91,46,182,162,203,242,127,180,151,212,200,183,37,219,166,71,66,95,27,68,137,108,70,102,222,67,
    116,132,201,123,53,34,72,132,173,173,100,77,57,170,102,200,145,217,137,149,38,77,54,136,155,237,36,132,180,141,150,86,86,187,146,155,189,18,165,233,106,55,173,182,154,107,42,51,95,
    17,43,34,198,100,177,9,83,106,78,137,17,19,166,105,161,38,36,164,205,38,105,50,102,146,18,17,63,66,53,185,147,182,179,19,201,154,89,99,89,182,103,146,166,72,120,141,247,149,44,228,
    246,217,162,206,72,244,149,97,30,237,238,186,73,37,210,75,100,73,209,43,198,214,237,36,221,19,34,239,155,163,125,42,37,149,53,111,121,35,116,68,44,37,214,234,137,175,209,181,179,
    154,233,251,93,188,110,187,34,233,180,86,143,9,111,102,242,95,137,164,122,154,105,107,123,154,172,54,79,53,111,141,246,201,38,34,21,154,230,240,37,185,25,89,26,228,168,149,161,35,
    92,137,118,36,70,201,163,68,72,78,149,17,27,214,72,68,177,112,145,28,146,84,218,38,93,179,104,149,89,46,77,147,212,37,155,125,80,211,228,82,101,134,219,201,181,205,186,104,79,146,
    73,181,155,19,117,100,39,74,137,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,130,0,0,0,0,0,0,0,0,0,0,0,0,0,96,0,0,0,0,0,0,0,0,0,0,12,68,177,19,19,43,66,75,161,37,205,44,223,
    108,213,236,101,73,214,54,254,230,171,99,162,127,151,125,122,78,145,235,101,213,25,98,42,52,92,200,187,118,141,230,107,38,109,118,213,38,210,161,61,136,159,100,253,152,180,155,114,
    178,75,37,141,245,77,219,66,161,55,202,74,145,17,211,54,79,19,68,232,132,151,18,212,140,218,89,177,57,36,177,18,182,178,101,152,150,253,17,14,147,91,8,89,37,187,242,126,209,84,215,
    95,26,254,137,20,77,254,37,237,182,157,13,46,184,145,170,68,67,120,37,153,137,123,74,132,153,165,105,89,26,205,181,141,82,178,145,29,51,69,100,179,36,153,148,151,116,137,20,73,57,
    18,230,154,19,107,145,172,49,22,98,66,38,144,201,227,37,98,79,141,219,230,109,37,66,103,100,69,200,214,237,59,34,164,148,150,202,209,238,50,87,219,88,218,23,102,150,205,38,156,153,
    100,156,218,186,36,183,79,155,42,145,83,127,18,222,150,254,146,238,191,73,247,233,86,167,69,88,203,46,46,173,111,249,62,168,187,146,47,39,216,159,59,38,147,76,73,26,110,221,155,42,
    36,198,242,53,108,154,198,210,35,110,212,138,153,164,132,150,52,136,212,149,18,220,210,236,201,173,36,36,217,18,228,19,212,37,100,77,169,36,198,76,166,200,146,33,167,100,218,226,
    100,74,213,21,24,214,88,132,93,181,144,71,147,66,69,73,36,219,60,52,120,36,182,230,179,146,199,65,36,95,153,244,68,86,158,51,164,177,55,169,85,153,101,215,88,74,189,22,100,242,163,
    236,159,183,237,101,93,17,114,62,69,154,108,242,19,127,178,101,196,145,89,26,216,75,37,98,126,72,157,25,55,72,109,44,107,144,75,225,38,136,218,230,84,37,233,18,166,240,149,25,11,
    179,105,58,26,43,77,39,98,109,85,51,84,210,38,184,201,238,154,145,44,217,167,109,181,237,154,84,149,19,196,73,62,73,103,66,69,113,60,69,34,44,214,106,214,235,37,154,39,179,73,173,
    213,45,106,139,19,118,150,66,91,187,68,178,237,167,217,173,160,89,65};
// libFLAC at its default level, 24-bit mono at 48 kHz. 2249 bytes.
static const unsigned char kFlac_libflac_level5_24bit_mono[] = {
    102,76,97,67,0,0,0,34,16,0,16,0,0,8,115,0,8,115,11,184,1,112,0,0,9,96,199,110,71,180,188,237,45,46,106,210,188,146,58,200,227,89,132,0,0,40,32,0,0,0,114,101,102,101,114,101,110,99,
    101,32,108,105,98,70,76,65,67,32,49,46,52,46,51,32,50,48,50,51,48,54,50,51,0,0,0,0,255,248,122,12,0,9,95,184,24,255,255,254,0,199,173,1,142,6,2,82,55,21,83,114,178,178,41,211,42,
    209,19,104,14,179,52,203,197,31,58,212,186,156,27,126,233,106,171,99,169,170,130,59,55,67,139,88,121,37,158,246,43,111,189,124,108,229,153,255,21,215,187,132,178,40,130,134,84,168,
    243,71,73,208,245,132,152,240,123,227,201,72,33,42,154,229,33,10,210,198,248,118,27,31,163,229,75,174,95,33,152,31,84,154,35,164,135,247,20,217,112,139,133,197,151,56,217,229,144,
    224,214,68,146,52,146,193,134,77,28,83,33,102,80,130,18,73,146,31,175,252,77,124,8,192,191,204,208,19,36,62,103,7,209,117,92,187,3,101,134,196,4,196,25,242,73,108,227,208,96,145,
    164,22,53,193,237,61,160,243,162,158,37,25,5,5,76,87,13,146,47,31,7,198,144,43,248,139,172,8,224,139,123,27,172,71,120,251,235,25,241,118,40,76,198,140,130,130,225,230,222,18,116,
    49,102,28,160,66,30,60,132,15,70,206,123,254,230,115,193,51,135,122,174,131,230,132,198,88,17,68,108,160,35,156,6,214,204,243,230,189,25,253,204,171,22,170,165,55,235,151,45,46,
    138,2,239,216,32,211,4,145,184,157,21,44,161,213,242,223,52,110,155,254,21,227,241,45,200,119,216,70,224,135,152,51,71,108,170,122,115,117,77,81,83,60,136,153,154,32,248,40,84,145,
    228,196,172,145,134,176,74,134,26,161,15,34,12,5,196,156,93,17,71,123,40,51,70,236,23,18,29,94,45,191,33,155,216,125,131,232,214,207,163,38,134,196,8,189,45,172,242,165,56,163,77,
    129,33,240,120,152,178,69,3,157,18,46,32,227,254,91,168,76,138,208,126,42,209,83,60,50,208,54,180,6,89,40,13,136,14,172,50,32,29,20,19,52,117,6,132,67,130,231,36,81,114,75,57,50,
    84,18,232,231,116,22,237,70,186,196,144,180,45,164,190,103,117,9,229,54,129,80,160,126,201,220,235,71,91,104,38,88,155,62,186,180,149,244,153,166,88,230,196,137,133,175,79,14,27,5,
    142,142,82,161,97,144,196,23,40,211,169,100,131,41,181,146,169,127,175,194,34,190,117,21,171,105,245,90,21,142,242,68,147,251,89,213,180,154,23,20,125,137,12,23,36,107,126,72,135,
    173,51,209,75,140,135,2,34,215,236,196,131,103,136,249,18,38,135,237,85,223,120,95,118,109,28,27,72,143,29,164,86,174,96,226,31,202,136,95,94,154,255,160,209,163,23,217,90,219,242,
    10,98,235,104,33,218,27,175,129,145,90,228,41,156,197,162,28,241,68,75,130,116,69,235,22,211,159,196,117,34,53,34,118,185,187,197,49,121,110,217,188,118,92,141,4,27,18,159,60,67,
    166,8,130,3,36,141,25,33,214,55,168,58,197,127,62,174,88,127,83,55,106,191,53,123,71,119,78,151,107,2,100,147,185,161,114,215,121,211,203,177,197,163,103,160,105,147,20,184,65,53,
    195,72,100,198,80,93,235,210,187,96,68,240,201,99,55,104,62,18,11,242,199,208,85,86,133,210,179,243,72,153,5,208,173,252,88,108,36,143,22,68,81,51,69,195,19,108,48,223,12,54,44,42,
    18,34,73,242,217,71,221,96,206,160,159,108,192,92,133,198,212,72,62,113,251,147,115,166,210,64,109,198,2,167,161,219,82,72,38,44,92,146,79,143,110,226,143,152,187,25,49,148,71,6,
    65,0,76,45,215,72,103,242,187,67,170,102,23,19,89,1,223,109,61,46,153,157,13,144,87,10,176,202,80,251,90,101,164,171,204,41,76,139,46,83,237,16,46,122,28,134,38,67,46,140,19,73,91,
    40,54,48,77,27,86,100,80,95,4,210,39,142,232,68,162,252,17,22,58,96,249,40,117,18,141,166,44,25,4,25,113,83,217,183,16,89,159,12,15,138,116,129,177,7,212,173,145,166,29,89,149,79,
    6,196,15,211,131,100,153,220,251,70,212,46,178,244,194,108,27,48,138,201,235,137,34,150,46,113,183,177,20,97,18,84,178,99,70,72,87,138,152,38,243,172,43,10,88,76,80,85,162,106,46,
    155,76,157,14,2,239,69,210,14,190,60,85,145,37,122,132,72,10,135,135,76,91,145,182,22,195,33,39,112,231,84,164,152,117,108,71,27,0,216,0,1,181,64,0,0,0,0,52,55,0,0,56,66,128,226,5,
    168,20,160,166,129,242,10,232,43,160,182,130,138,13,168,32,160,204,129,242,9,40,22,160,134,128,106,2,40,14,160,41,129,22,1,56,29,96,75,130,174,8,56,31,224,175,131,22,7,248,39,96,
    167,130,22,8,88,17,96,127,128,38,5,216,18,160,35,128,202,5,8,31,32,78,130,90,9,72,49,32,144,130,170,14,8,30,160,212,130,90,8,40,30,32,126,128,242,3,232,10,32,33,128,34,4,184,16,96,
    117,129,214,8,216,38,224,179,130,238,11,88,41,216,242,41,174,139,62,79,50,93,224,81,153,227,99,146,193,0,196,232,108,50,25,27,132,166,34,50,82,242,197,158,147,248,164,235,109,39,
    185,191,150,203,238,162,138,9,173,125,181,15,190,252,22,248,38,126,102,50,38,26,13,132,99,113,48,228,80,92,35,17,39,55,75,189,87,46,82,43,105,45,176,154,214,197,149,19,121,45,92,
    190,135,229,94,220,203,106,208,200,159,155,10,76,130,34,241,249,104,232,248,192,241,225,82,149,232,248,240,180,167,173,150,200,228,210,185,140,230,254,93,34,144,215,92,116,247,195,
    10,230,36,69,6,134,34,33,129,144,196,90,110,43,51,18,180,58,54,139,148,160,101,202,142,186,146,230,202,89,93,35,147,82,201,168,106,191,252,195,131,117,130,34,161,3,33,89,232,160,
    188,108,108,37,54,60,124,120,177,187,119,26,92,208,83,84,218,94,73,47,46,165,151,50,26,251,154,106,220,222,112,111,163,90,78,220,236,184,76,96,106,52,50,129,121,41,169,155,87,238,
    212,96,206,71,87,154,89,65,123,43,175,145,73,238,239,45,235,106,191,81,229,37,164,75,245,201,203,97,73,120,140,220,86,122,102,44,42,41,54,34,80,74,163,59,92,22,185,47,107,40,100,
    117,119,182,148,245,182,94,232,118,228,231,213,53,146,43,54,40,48,45,50,18,147,8,136,132,236,11,76,87,213,168,193,195,69,85,31,169,5,149,92,174,218,142,115,65,121,230,227,159,126,
    58,87,249,80,98,164,220,188,180,212,228,224,200,220,140,78,130,76,157,1,97,47,98,108,149,180,21,117,117,178,26,121,71,57,158,187,159,127,45,134,188,200,244,146,54,33,46,47,48,52,
    59,58,52,42,20,59,39,51,100,157,234,70,88,153,249,172,219,127,79,242,254,235,245,60,143,16,143,110,111,153,85,121,82,212,133,177,145,41,155,81,1,115,115,119,204,73,21,161,112,235,
    23,63,159,255,213,221,247,189,185,169,185,40,87,184,239,181,39,45,26,162,251,6,67,10,3,7,2,99,64,177,144,68,240,160,97,65,2,137,48,83,145,127,109,206,75,95,136,198,105,199,235,205,
    79,228,19,201,78,179,145,96,185,177,96,211,161,48,80,200,92,216,36,208,113,82,98,204,239,76,79,106,177,126,66,74,59,220,106,126,252,244,140,132,166,17,10,6,107,146,152,85,240,163,
    131,229,195,175,6,223,9,60,54,145,118,26,41,163,246,255,149,153,165,57,23,179,37,22,171,41,37,229,108,45,219,122,250,163,170,5,197,75,2,197,67,166,131,166,4,70,72,149,102,216,73,
    161,251,65,156,67,91,70,106,197,110,200,69,105,85,156,227,43,121,222,41,112,171,65,116,161,71,199,143,142,157,24,88,52,196,170,109,94,25,240,75,24,174,177,170,50,81,202,19,50,82,
    214,127,171,114,129,234,136,124,109,49,146,131,140,2,165,140,88,80,137,211,15,9,26,155,16,209,27,251,74,87,189,70,53,66,66,103,137,89,28,242,147,160,165,4,14,188,36,112,216,208,
    200,129,225,225,146,39,6,211,42,76,141,193,15,12,201,99,45,110,118,132,228,173,187,83,158,93,161,214,244,110,24,148,45,138,58,19,88,84,224,201,17,49,84,39,145,104,178,15,224,137,
    202,21,239,80,149,145,177,51,82,82,141,139,62,104,119,198,220,172,113,244,33,84,202,135,214,133,92,132,219,137,168,129,148,59,51,183,161,127,135,248,155,161,66,47,144,239,117,120,
    170,90,225,195,119,29,96,241,231,198,15,148,22,20,120,109,65,116,29,36,190,40,29,224,78,191,210,255,79,84,191,59,128,75,89,29,42,228,174,222,16,89,226,99,13,18,8,146,32,42,68,250,
    133,139,181,186,235,125,165,74,93,203,239,49,94,95,175,78,165,255,133,181,68,148,177,217,180,73,136,163,31,70,64,121,120,250,243,40,11,57,105,239,220,227,158,100,177,149,230,90,
    182,179,255,111,115,138,185,166,71,242,109,223,88,89,162,162,79,12,50,48,204,96,132,217,122,232,113,169,74,163,255,55,174,211,173,39,98,181,123,91,223,227,13,107,220,91,167,138,
    172,32,76,144,139,194,68,200,27,104,195,83,39,234,169,230,15,220,196,189,222,172,208,179,164,191,66,127,18,199,18,165,165,86,108,234,194,68,132,16,140,189,18,122,124,184,168,78,92,
    74,153,188,139,245,78,246,19,127,115,39,111,161,173,202,241,141,18,213,147,46,212,235,37,83,10,165,84,109,235,165,144,85,181,119,49,187,180,105,251,111,104,143,34,119,184,218,111,
    116,111,248,151,132,46,46,73,97,86,219,0,246,15};
// libFLAC at its fastest level, 8-bit stereo. 959 bytes.
static const unsigned char kFlac_libflac_level0_8bit_stereo[] = {
    102,76,97,67,0,0,0,34,4,128,4,128,0,3,105,0,3,105,5,98,34,112,0,0,4,79,200,109,36,154,135,231,139,217,160,158,207,69,186,112,140,112,132,0,0,40,32,0,0,0,114,101,102,101,114,101,
    110,99,101,32,108,105,98,70,76,65,67,32,49,46,52,46,51,32,50,48,50,51,48,54,50,51,0,0,0,0,255,248,118,18,0,4,78,19,18,255,0,65,4,16,34,8,132,42,21,164,196,202,82,146,40,165,219,70,
    205,225,231,141,55,148,164,133,33,84,164,249,52,203,139,72,144,148,90,47,198,206,12,198,102,54,55,141,254,122,120,220,56,54,216,246,219,181,10,18,68,32,138,34,34,148,83,138,197,
    148,72,68,132,139,45,86,124,199,111,58,202,44,69,17,8,178,37,148,82,139,17,8,68,34,16,18,8,34,16,132,69,107,180,126,185,175,150,190,243,63,27,12,241,153,199,61,121,74,34,101,113,
    123,163,84,244,84,90,69,202,243,205,179,27,30,51,205,241,250,100,250,247,238,113,196,209,68,68,9,2,66,8,132,17,20,20,92,82,200,164,146,16,146,66,86,81,255,51,158,244,197,36,148,33,
    72,74,145,38,42,90,136,80,80,66,132,17,37,33,127,185,236,121,205,159,78,206,111,56,199,152,102,120,246,254,101,137,98,197,98,245,53,219,146,178,165,20,73,52,233,230,121,236,111,
    236,201,210,22,151,19,47,45,113,69,132,132,65,32,66,32,66,32,144,144,88,178,93,73,22,133,162,69,19,138,247,156,123,103,113,149,18,74,45,11,23,147,90,153,52,89,80,88,145,98,236,143,
    140,219,30,56,216,223,29,183,123,56,222,204,126,199,102,181,138,40,66,196,73,74,138,229,217,82,68,74,36,138,234,123,127,29,189,57,18,200,138,44,90,146,138,146,44,66,68,65,8,33,17,
    16,130,80,138,178,126,93,202,101,197,222,77,187,54,60,225,251,122,184,169,37,37,93,234,117,57,105,22,162,73,218,60,225,227,108,217,191,127,207,91,173,217,237,206,94,42,34,68,34,8,
    136,162,20,75,46,145,114,40,72,176,180,80,156,157,172,220,214,105,74,208,162,72,145,80,154,89,43,40,133,32,160,137,16,144,180,73,145,209,172,221,29,57,168,213,142,113,158,54,113,
    141,63,52,225,73,9,1,128,33,2,8,66,4,68,73,74,153,118,76,162,149,11,212,231,142,51,198,230,126,162,136,146,66,69,149,89,25,28,154,146,208,164,81,86,155,195,198,195,99,25,140,199,
    54,57,186,102,158,108,109,188,115,115,74,72,129,65,16,132,133,11,44,148,232,185,66,88,161,41,33,119,77,156,199,26,246,44,164,68,34,68,34,137,66,137,73,66,65,8,132,64,130,16,130,20,
    34,37,68,105,199,198,156,215,211,39,204,252,120,205,140,219,223,241,101,9,69,146,107,81,206,78,236,184,148,148,213,158,204,112,198,54,102,199,182,179,35,39,45,54,89,219,79,69,161,
    18,8,132,33,2,33,4,72,132,168,86,89,50,74,66,36,148,146,164,249,239,252,238,80,148,130,36,66,81,36,93,20,168,88,72,160,136,80,68,137,53,63,126,204,243,29,182,209,233,159,249,177,
    182,54,199,177,250,152,149,36,45,11,146,183,83,94,178,101,9,139,74,104,255,51,207,54,219,169,147,18,75,137,37,118,45,82,18,68,34,16,130,4,33,8,34,68,20,38,90,38,148,202,38,69,20,
    181,145,250,55,51,141,223,164,148,81,18,73,101,106,114,244,76,76,75,36,75,23,81,183,7,25,140,217,179,152,235,243,141,61,141,195,135,157,250,105,36,20,68,68,72,147,23,38,83,148,162,
    197,66,197,201,83,159,182,254,233,200,88,145,98,137,34,42,132,197,66,196,36,66,32,136,64,161,9,18,66,107,94,179,235,78,38,86,154,55,27,140,246,121,247,50,181,34,38,75,42,122,249,
    244,84,201,106,19,150,253,248,108,222,54,205,181,239,171,190,179,231,222,90,133,33,17,8,80,132,72,138,34,210,203,168,74,136,177,18,162,229,199,180,219,35,77,68,180,40,44,68,148,41,
    73,168,90,68,160,162,17,10,65,69,197,30,135,216,230,247,123,87,185,182,225,195,141,153,206,50,122,146,32,13,136};
// libFLAC, 16-bit audio in a 24-bit file (eight wasted bits). 1208 bytes.
static const unsigned char kFlac_libflac_16bit_audio_in_24bit[] = {
    102,76,97,67,0,0,0,34,16,0,16,0,0,4,98,0,4,98,5,98,35,112,0,0,4,79,138,61,147,180,206,228,102,212,216,130,59,160,13,32,36,63,132,0,0,40,32,0,0,0,114,101,102,101,114,101,110,99,101,
    32,108,105,98,70,76,65,67,32,49,46,52,46,51,32,50,48,50,51,48,54,50,51,0,0,0,0,255,248,118,28,0,4,78,215,79,1,0,1,1,177,3,92,4,242,6,117,7,214,9,18,10,41,197,35,35,182,60,69,245,
    40,108,3,18,4,239,139,0,66,61,72,142,187,119,85,45,58,15,85,121,245,229,209,98,87,107,54,213,77,13,249,141,99,13,153,19,13,146,227,36,114,123,17,75,180,30,229,88,94,246,21,237,41,
    254,114,222,235,229,71,223,33,52,166,17,51,49,144,148,65,18,136,130,83,16,201,76,27,65,51,106,37,154,183,93,215,90,191,233,22,197,39,241,175,237,19,202,98,71,48,201,194,50,25,4,35,
    8,217,134,66,65,148,78,201,72,168,142,211,94,142,186,215,117,189,92,90,235,84,90,169,207,209,41,226,17,110,201,58,67,75,8,109,73,136,111,134,74,102,141,55,70,90,250,83,161,102,60,
    253,118,115,196,229,133,159,66,185,36,253,147,136,136,200,132,33,12,36,97,9,134,33,8,76,196,49,33,146,9,136,101,36,218,137,255,213,124,199,247,67,167,67,202,150,79,40,142,221,54,
    77,200,36,74,32,132,81,176,130,92,98,54,78,216,221,77,230,184,186,213,241,10,89,214,149,239,115,187,165,57,86,144,177,106,39,178,153,20,132,164,113,53,35,38,34,68,49,171,25,182,67,
    70,181,183,218,172,139,107,164,167,111,246,194,238,78,84,79,108,37,68,136,78,102,27,81,132,18,16,132,54,52,16,101,64,155,141,8,101,18,230,107,19,101,228,240,182,245,66,245,86,124,
    82,177,11,230,86,187,116,245,35,84,81,181,66,18,144,148,140,110,136,132,79,147,200,207,116,94,178,117,42,49,207,101,113,81,233,253,67,189,217,231,107,172,66,19,41,16,72,216,205,
    198,198,77,132,98,54,72,19,36,100,54,183,52,234,137,253,175,153,124,186,196,85,106,147,85,38,157,137,149,13,144,200,104,198,97,9,8,104,67,66,83,35,118,196,157,13,74,73,217,87,92,
    122,135,59,59,163,139,189,62,190,44,98,244,172,250,55,137,115,55,36,208,196,220,74,70,236,107,49,55,83,121,213,43,203,11,190,58,63,223,219,254,95,43,214,84,79,54,55,140,33,169,17,
    24,208,196,70,134,204,66,70,129,51,102,198,154,144,139,171,33,85,55,95,175,250,61,201,47,83,70,91,19,51,237,154,27,193,183,9,137,4,200,67,102,172,212,137,98,101,205,236,217,210,
    216,89,159,10,138,87,212,165,93,125,58,58,101,228,142,205,227,70,156,152,159,65,162,39,128,128,196,1,153,130,106,131,50,3,235,132,150,5,43,133,173,98,145,211,214,78,37,252,160,32,
    194,18,255,199,204,0,34,98,147,200,93,214,169,225,105,106,170,171,167,86,127,155,121,165,36,55,236,78,205,104,76,137,12,201,144,221,147,36,241,29,41,229,45,253,121,106,199,165,138,
    241,207,89,212,230,162,164,86,176,145,161,25,153,12,76,52,32,130,115,33,145,145,136,200,132,25,89,8,159,26,39,189,24,171,100,47,253,23,181,203,244,53,114,102,90,38,75,8,52,74,50,
    25,141,136,198,25,169,140,140,144,137,68,251,145,99,47,173,95,167,123,121,226,250,61,58,40,248,178,175,84,223,44,52,214,49,37,200,76,151,17,9,177,50,99,38,212,140,169,58,119,69,
    176,175,50,150,202,94,145,122,167,89,188,233,21,148,137,187,25,13,196,49,49,132,98,33,130,100,99,67,17,166,35,34,33,43,52,168,204,89,34,231,168,85,139,189,44,69,169,122,145,178,
    232,72,151,34,18,196,52,27,98,13,9,152,200,219,18,55,77,246,199,237,215,244,229,84,199,61,33,222,173,114,245,67,175,84,241,227,41,59,52,172,200,136,137,141,169,144,146,144,109,32,
    136,208,140,147,114,101,132,119,73,215,243,31,21,60,149,83,90,166,69,88,70,114,82,76,66,54,99,32,137,134,51,27,49,4,19,86,38,72,196,179,81,34,163,127,180,194,196,87,232,175,78,158,
    23,47,72,185,45,100,114,88,212,151,18,162,72,52,36,152,201,153,56,156,146,68,251,219,15,117,127,151,172,116,119,98,229,44,67,194,162,194,183,215,180,153,132,240,100,76,130,19,32,
    145,152,149,8,208,136,39,134,33,31,17,62,107,100,157,117,125,125,232,143,37,103,39,73,185,190,72,196,41,176,154,49,155,16,217,134,198,194,104,134,104,138,140,213,83,75,217,57,97,
    20,172,165,136,122,85,152,234,182,251,126,133,66,87,33,186,145,152,166,78,136,218,52,108,134,173,9,9,80,197,73,42,87,217,97,235,41,81,101,62,215,11,17,103,213,246,86,253,25,58,18,
    32,212,141,134,104,50,50,9,137,145,16,131,121,132,53,232,53,83,37,137,188,203,244,125,111,185,111,34,166,92,79,46,106,141,60,49,47,33,51,34,49,27,113,136,151,8,215,195,78,222,58,
    51,197,158,178,188,249,250,175,191,186,238,94,133,71,72,232,202,202,98,42,66,104,105,50,49,57,24,166,157};
// hand-made: variable block size, every subframe type and channel assignment, escaped partitions, an ID3v2 tag and extra metadata blocks. 3685 bytes.
static const unsigned char kFlac_hand_variable_blocks_everything[] = {
    73,68,51,3,0,0,0,0,0,22,84,73,84,50,0,0,0,12,0,0,0,104,97,110,100,45,109,97,100,101,0,0,102,76,97,67,0,0,0,34,0,16,4,0,0,0,17,0,3,67,10,196,66,240,0,0,11,161,195,44,190,205,48,64,
    135,232,174,123,78,78,120,170,35,86,1,0,0,10,0,0,0,0,0,0,0,0,0,0,2,0,0,8,116,101,115,116,1,2,3,4,3,0,0,18,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,132,0,0,12,4,0,0,0,104,97,110,100,0,
    0,0,0,255,249,25,24,0,251,18,0,0,9,13,26,16,48,96,225,35,4,140,60,243,207,44,194,73,41,228,53,141,90,88,148,37,9,82,80,148,41,10,91,20,198,41,163,99,20,166,49,136,98,24,142,71,100,
    102,198,255,251,94,242,73,52,209,38,158,44,89,226,196,158,36,241,39,154,91,222,66,156,85,44,191,70,33,8,97,4,24,65,2,4,16,32,134,48,134,54,54,201,38,95,30,171,245,115,191,218,219,
    169,234,202,66,144,196,56,134,56,167,16,162,8,99,28,174,69,34,38,83,233,90,201,36,209,34,69,0,9,192,15,0,18,113,139,25,76,200,119,233,157,59,156,188,152,134,17,132,104,170,146,164,
    210,152,155,16,97,25,151,217,144,200,214,100,215,107,84,165,9,139,64,158,20,90,22,42,147,9,212,163,132,58,100,220,157,178,100,228,203,139,75,69,147,4,24,84,70,44,155,23,228,24,141,
    12,155,208,209,198,33,224,223,102,255,249,41,136,195,128,180,22,1,21,1,7,0,247,76,68,56,139,53,101,103,68,82,20,147,242,99,136,187,178,144,81,185,67,7,12,56,199,34,180,90,111,211,
    46,48,230,81,5,53,82,116,139,144,86,35,152,230,115,60,33,81,66,14,148,194,145,5,24,228,170,72,172,228,88,223,69,16,83,103,69,24,180,67,145,104,139,209,70,60,101,196,60,16,66,153,
    98,71,100,57,156,65,198,43,114,21,36,66,140,56,66,144,225,135,196,133,19,207,152,162,21,186,90,150,205,41,7,52,150,69,55,153,196,136,173,133,18,116,115,58,39,149,37,173,119,164,
    118,138,146,88,157,138,148,135,37,186,205,20,197,201,217,105,28,199,37,66,140,81,110,48,166,124,69,180,67,152,230,133,24,113,189,226,14,37,114,94,216,228,138,103,98,147,44,68,20,
    32,163,61,101,35,136,41,51,136,84,176,135,24,40,130,123,209,14,16,249,138,216,166,170,214,114,117,35,178,28,32,236,142,35,201,171,46,215,146,216,207,180,83,126,208,172,207,25,81,
    112,136,56,203,50,136,56,142,69,137,100,71,101,16,187,98,141,252,49,219,181,81,30,145,118,86,113,133,73,167,78,157,157,156,146,169,58,56,137,138,209,10,33,72,236,199,131,10,48,184,
    150,57,161,76,168,166,20,98,178,164,188,151,241,135,16,164,82,28,147,183,172,169,217,205,74,39,71,16,5,82,42,88,201,163,71,78,158,62,32,36,40,40,40,44,44,50,46,48,52,58,52,50,58,
    54,56,56,54,56,54,52,56,50,50,52,48,56,46,50,42,48,40,42,38,32,36,34,116,233,211,38,75,150,40,80,145,7,159,105,182,150,81,52,146,71,61,56,227,159,58,254,133,58,85,43,213,173,102,
    237,155,87,47,223,145,145,144,146,145,147,147,146,151,149,150,149,148,148,149,148,148,148,149,149,146,148,145,145,145,191,126,245,219,119,46,89,177,90,173,106,148,104,125,247,92,
    243,182,219,101,134,89,38,162,171,44,211,142,188,249,34,37,74,23,44,92,193,147,38,205,28,60,108,249,227,199,79,156,17,62,124,233,227,135,143,31,60,116,217,163,70,12,23,42,80,153,
    34,132,159,113,214,90,101,100,211,73,20,52,219,125,248,239,207,232,82,163,90,181,107,86,110,92,191,37,33,37,35,39,41,45,47,51,55,57,53,51,57,55,57,61,57,61,59,59,59,61,16,158,159,
    156,157,154,156,154,153,154,152,151,151,151,149,150,146,148,144,189,126,229,203,86,43,214,171,71,255,253,231,189,181,249,255,124,2,0,24,80,194,133,13,24,68,137,50,133,76,23,52,104,
    217,243,231,145,33,70,153,42,116,169,82,168,80,169,74,133,10,86,43,80,173,74,117,10,83,168,74,153,50,100,168,81,158,64,128,241,211,70,204,25,44,84,169,34,67,135,11,22,44,72,112,
    128,193,255,3,247,231,207,93,251,117,233,211,143,14,60,183,240,221,179,110,157,122,244,233,205,167,86,173,89,244,103,209,163,94,157,26,181,108,213,187,111,13,252,56,112,231,203,
    183,143,30,61,123,249,255,248,48,128,195,136,18,36,104,209,164,201,149,42,96,201,147,38,207,31,68,141,26,68,201,147,39,82,165,74,197,171,22,175,96,189,122,229,171,87,44,92,177,98,
    245,139,85,172,88,169,82,148,201,211,36,70,132,249,227,166,204,153,48,84,161,34,35,69,11,16,20,40,64,0,127,124,245,215,199,110,59,246,237,219,159,62,108,184,240,97,193,126,221,219,
    86,235,86,169,86,149,74,84,167,208,163,74,133,25,243,232,80,161,70,132,250,20,105,211,169,86,181,122,214,44,90,185,106,213,204,56,0,31,135,255,249,137,152,204,128,42,78,255,241,
    127,249,63,252,191,254,143,255,55,255,171,255,213,255,238,181,57,158,77,1,159,238,0,207,247,0,103,251,128,74,157,38,81,60,76,133,25,73,246,51,146,36,211,136,164,155,147,105,77,246,
    77,183,35,57,47,34,93,201,89,90,57,34,140,41,162,164,142,218,231,74,69,189,150,162,235,45,233,47,202,147,167,82,42,121,172,186,228,116,164,254,52,233,43,73,182,164,228,228,41,162,
    86,219,154,19,146,38,19,236,196,184,73,73,137,72,213,144,157,152,221,12,164,205,145,163,68,140,74,163,68,180,144,158,18,236,215,55,201,219,178,155,161,70,93,99,12,127,239,255,241,
    127,242,127,243,12,7,132,136,60,197,30,237,40,69,115,101,18,33,23,97,151,12,242,158,164,159,25,120,131,180,197,84,202,68,43,42,113,87,58,155,97,28,23,106,83,33,168,194,57,34,157,
    41,172,53,163,92,102,153,171,78,105,210,136,77,202,244,217,24,73,6,94,98,43,41,15,80,242,136,248,199,59,49,178,106,106,176,138,208,84,107,189,112,135,169,72,143,48,166,89,116,32,
    115,49,170,131,105,9,226,197,34,153,2,74,163,105,15,96,152,156,44,90,63,40,210,140,147,30,163,245,45,198,104,199,217,51,42,136,137,230,130,78,121,51,29,133,104,100,196,63,70,8,189,
    243,83,121,134,75,146,130,41,17,73,205,185,103,50,145,14,132,19,210,52,152,91,15,208,151,25,39,30,65,78,112,33,14,212,160,91,90,255,249,105,168,208,128,15,49,2,255,68,255,73,255,
    79,255,83,255,88,255,93,255,94,255,98,255,98,255,99,255,100,255,101,255,100,255,99,255,96,255,94,18,255,251,0,40,100,114,122,251,33,128,81,196,255,249,153,24,208,144,157,0,254,191,
    16,75,234,217,54,29,115,89,213,53,29,51,74,209,52,28,247,58,206,51,92,211,48,203,242,236,199,49,204,115,60,211,56,207,52,13,19,70,211,53,29,87,89,215,182,45,151,106,219,182,253,
    203,115,221,55,109,215,115,220,182,205,163,100,215,53,125,55,68,206,179,44,147,24,195,112,27,198,227,181,236,122,238,172,167,105,42,18,119,154,165,249,86,75,144,99,152,194,41,137,
    33,232,106,23,133,161,72,90,20,134,33,136,114,30,136,162,136,182,49,141,99,201,6,69,147,37,25,82,90,151,166,25,150,105,155,102,249,202,114,157,39,121,226,120,157,231,153,230,123,
    159,103,233,246,129,160,168,90,42,142,164,169,138,122,167,65,161,50,101,236,178,148,178,102,69,223,119,109,183,101,87,21,53,37,254,244,245,120,26,204,190,23,30,163,90,178,27,110,
    231,225,24,78,149,133,201,138,103,155,71,17,216,120,159,7,232,130,17,2,44,70,8,225,30,36,4,128,146,18,66,68,73,137,33,36,36,132,160,148,18,226,92,76,137,209,62,40,133,40,169,21,
    194,204,91,11,177,130,49,198,96,211,27,3,116,113,14,113,212,59,199,136,245,30,227,224,124,15,129,236,61,71,144,239,29,67,152,111,141,129,164,49,198,0,183,21,98,136,75,8,209,4,123,
    29,38,249,168,101,24,5,169,90,82,19,164,185,32,70,145,36,41,4,64,144,36,17,6,68,17,100,129,38,76,148,4,41,138,242,212,190,50,13,3,100,223,58,15,3,220,64,8,97,24,36,132,192,158,20,
    98,148,84,10,161,88,43,133,112,176,22,2,192,87,138,177,88,42,5,48,167,20,162,136,81,10,1,62,39,196,232,159,20,34,128,81,138,113,84,43,5,144,183,23,99,8,100,140,209,172,54,199,32,
    233,30,35,220,32,2,16,34,194,72,37,66,104,39,2,132,40,130,140,40,130,128,40,2,112,37,130,72,35,2,8,124,143,17,202,54,70,88,189,21,226,128,72,136,3,192,222,51,75,162,172,151,33,158,
    206,86,171,13,104,167,76,162,80,89,12,222,159,99,187,225,243,186,189,223,39,179,227,247,160,40,74,46,145,166,41,234,154,174,173,43,203,18,206,253,89,70,59,138,97,87,237,229,115,91,
    86,101,127,90,84,148,189,21,63,206,19,52,179,38,200,49,196,93,18,67,208,172,29,5,192,240,36,1,1,64,80,20,9,4,193,240,148,55,16,197,49,164,119,35,74,18,236,205,57,79,180,85,39,80,
    85,117,173,125,99,218,22,189,195,115,221,151,149,237,125,223,216,30,11,130,96,184,62,15,132,96,184,54,9,130,96,88,22,5,127,223,248,6,1,128,224,88,54,15,133,225,216,134,45,142,99,
    249,54,91,153,231,26,6,143,167,106,250,230,199,180,238,59,182,249,192,112,188,63,23,198,241,252,119,29,197,241,28,55,7,190,238,251,142,209,176,107,58,134,143,157,230,96,253,196,
    255,249,121,136,216,144,1,43,120,21,63,230,255,40,64,157,57,101,158,89,78,159,255,78,202,82,203,148,211,249,164,63,41,75,150,68,244,189,41,127,77,41,167,79,200,133,203,254,9,75,
    244,165,47,151,148,233,150,105,202,82,148,211,75,47,253,41,6,82,148,211,249,74,82,148,210,148,191,75,233,75,255,255,41,136,0,185,105,85,214,97,36,83,65,101,18,81,13,116,207,94,50,
    211,189,254,239,190,170,212,173,98,205,171,183,100,100,100,100,164,164,166,166,102,166,39,162,145,8,180,66,45,19,139,198,98,145,168,244,35,29,132,161,8,244,39,10,66,112,164,41,10,
    195,48,220,47,10,67,80,188,47,12,67,48,204,53,13,194,176,212,41,11,194,80,164,43,12,194,48,140,41,26,132,225,24,236,110,45,24,136,196,98,19,211,147,243,115,114,146,210,114,50,23,
    108,213,181,87,250,159,127,230,252,237,158,41,225,138,171,48,185,22,201,147,34,72,209,163,38,204,154,60,116,72,76,248,168,128,176,160,168,168,176,184,208,152,184,160,200,168,208,
    192,184,176,152,136,184,176,152,129,241,51,130,34,98,135,14,26,62,108,185,131,36,201,190,241,34,36,136,17,109,230,25,117,86,28,89,165,81,81,36,114,203,76,242,195,20,144,197,44,17,
    215,12,240,79,20,241,81,12,83,81,101,22,77,198,159,116,139,165,8,18,124,153,130,166,75,27,50,34,32,32,34,36,42,52,46,60,58,54,58,60,17,30,9,132,66,161,80,176,80,46,22,12,6,67,65,
    192,240,116,60,8,7,193,0,232,108,50,8,6,131,224,128,5,131,255,249,105,24,220,188,99,85,24,254,240,254,228,254,220,254,214,11,203,214,73,183,39,15,176,121,54,55,4,16,221,245,243,2,
    39,132,96,158,99,242,55,100,238,47,6,238,23,163,244,39,130,123,229,0,246,111,109,52,110,7,209,11,190,70,2,110,109,60,255,151,228,247,134,30,128,4,208,201,130,131,254,39,248,80,101,
    104,31,213,200,55,1,63,87,240,129,144,178,34,69,8,209,29,36,4,140,147,146,130,94,74,137,153,49,38,4,220,155,147,130,114,79,9,249,64,40,165,40,165,149,18,166,85,202,225,98,44,229,
    168,184,23,82,240,96,76,57,139,50,38,88,206,25,227,64,105,13,65,170,53,38,172,215,26,179,82,105,13,33,161,51,6,88,200,24,114,250,94,11,89,103,43,37,44,162,147,178,86,72,8,145,5,
    127,30,199,149,213,114,92,54,217,169,104,25,182,49,137,95,23,165,205,108,90,22,21,125,91,85,85,133,65,81,83,148,165,52,231,108,255,249,169,24,222,160,114,126,255,111,255,113,255,
    116,255,117,255,121,255,123,255,129,255,132,255,139,255,142,255,149,255,155,255,161,255,168,255,177,255,186,255,193,255,201,255,211,255,220,255,230,255,239,255,247,0,1,0,11,0,20,0,
    29,0,37,0,44,0,50,0,59,0,65,214,57,155,147,48,25,191,184,128,201,253,208,6,63,239,0,49,255,120,1,159,252,0,13,255,224,0,127,255,0,3,255,248,0,32,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,10,5,137,177,185,148,221,36,79,59,119,35,213,221,228,124,165,217,245,159,100,178,19,177,57,161,25,25,4,204,66,97,143,57,8,206,83,50,157,76,70,49,12,194,160,228,49,132,21,
    194,8,80,132,81,4,150,73,102,20,73,197,28,72,130,15,44,211,138,56,163,15,121,198,146,105,100,156,79,44,215,28,210,201,32,130,137,41,196,22,210,9,33,228,52,182,188,182,184,167,180,
    65,2,8,32,198,24,204,49,152,140,36,52,34,118,146,34,253,222,8,225,141,182,28,7,230,49,248,124,29,227,184,104,140,76,182,142,207,59,123,99,204,224,110,25,152,227,12,80,199,14,99,16,
    106,61,51,40,201,237,28,207,28,105,153,108,37,127,100,201,105,95,155,165,181,228,189,235,18,178,218,73,105,60,218,73,107,36,223,63,203,37,37,146,178,77,73,47,53,38,164,245,150,210,
    73,53,166,89,36,188,213,137,37,167,188,148,159,229,173,239,95,189,122,223,182,204,166,202,90,186,194,41,19,102,198,201,136,196,68,35,35,49,17,13,8,86,50,153,136,67,177,156,229,33,
    202,99,156,228,56,167,40,133,20,163,41,140,49,72,40,132,32,133,41,136,102,59,177,216,140,206,71,66,17,38,200,166,90,145,152,142,67,59,8,130,144,134,41,68,97,74,82,156,238,51,48,
    135,115,10,231,33,142,98,148,70,16,162,144,130,148,198,20,162,10,81,201,165,16,73,4,150,73,5,150,73,133,18,89,175,48,194,73,44,162,12,36,130,136,41,227,97,5,41,133,33,156,84,57,24,
    174,85,43,17,74,182,79,117,85,32,170,68,155,118,182,223,43,226,150,29,221,206,43,142,231,10,126,75,210,91,90,89,45,73,175,73,169,105,107,39,77,210,125,169,111,175,218,122,249,90,
    255,125,182,191,255,75,237,62,189,90,253,118,214,234,253,101,199,229,41,203,221,84,255,171,255,235,255,21,115,172,87,56,77,237,234,239,215,43,111,165,105,107,214,213,189,109,91,94,
    244,189,230,223,55,214,74,116,149,235,245,190,149,219,127,245,105,223,246,202,117,165,202,82,243,169,93,85,74,176,191,170,21,20,142,213,147,19,26,16,134,41,12,81,92,98,138,41,133,
    56,162,152,65,69,20,65,5,40,227,20,33,56,146,10,121,15,36,166,150,226,73,32,150,152,65,79,49,228,22,65,101,56,230,154,50,12,113,198,48,67,4,48,130,24,83,136,65,72,114,148,164,43,
    29,148,234,81,65,34,25,179,17,160,141,144,130,98,49,8,67,96,200,33,136,35,26,157,78,101,59,8,196,115,49,8,70,33,28,174,135,103,35,21,202,22,248,48,248,53,248,58,8,63,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,224,15,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,0,101,106,255,249,105,24,224,174,160,0,75,2,0,149,0,0,124,197,111};
// hand-made: 12-bit mono; rate and block size written every way a frame header allows, partition orders 0 to 8. 1867 bytes.
static const unsigned char kFlac_hand_12bit_header_forms[] = {
    102,76,97,67,128,0,0,34,1,0,1,0,0,0,40,0,1,119,7,208,0,176,0,0,9,37,43,67,161,200,104,254,42,158,28,127,45,209,62,105,124,115,255,248,136,4,0,132,16,1,160,72,165,123,38,206,158,
    145,18,148,149,22,23,153,25,154,26,154,155,27,27,27,156,27,156,27,156,28,28,29,29,29,29,158,30,158,136,68,2,49,24,140,76,39,20,10,69,2,113,64,160,78,38,18,136,196,71,231,135,135,
    38,134,70,5,165,68,196,132,100,15,222,59,114,229,187,119,14,27,185,114,241,235,199,196,36,36,132,164,228,229,37,69,101,101,101,133,69,37,69,69,4,229,36,228,228,164,164,164,132,196,
    196,164,228,165,5,5,37,69,165,197,198,70,102,134,166,231,7,7,71,39,39,39,6,230,230,166,134,70,5,101,4,164,111,221,54,100,185,98,116,104,81,99,207,167,110,238,60,187,117,243,231,
    215,207,180,31,232,105,41,169,170,235,107,172,109,109,174,174,239,164,50,73,60,170,95,47,152,205,38,243,121,212,238,115,58,157,78,167,19,137,180,202,101,47,150,202,100,210,73,28,
    130,246,234,230,222,210,198,194,194,186,182,174,170,170,174,170,170,170,174,166,174,162,166,166,162,162,158,150,146,138,131,247,239,94,123,246,233,199,126,253,154,245,105,211,162,
    138,197,255,248,108,4,1,255,32,10,18,255,116,195,191,225,183,25,183,28,53,28,137,68,161,16,81,4,146,10,34,145,21,9,138,148,72,144,132,68,66,99,17,152,132,33,4,98,27,17,13,145,18,
    164,146,75,229,14,219,233,202,105,169,53,211,177,161,227,108,33,153,140,198,204,60,108,216,251,121,52,145,48,162,88,73,38,162,230,143,158,142,230,91,36,153,18,12,138,41,39,43,123,
    102,51,102,102,49,134,51,30,24,118,108,207,231,124,183,215,191,142,57,152,91,174,255,248,125,4,2,0,255,125,0,218,18,250,162,0,88,176,139,4,33,18,2,9,48,18,96,147,17,9,136,130,68,
    66,99,96,145,49,24,136,76,36,68,34,19,4,72,136,16,36,193,2,4,72,32,66,4,17,32,137,4,72,34,66,64,73,128,147,19,9,19,1,8,145,16,33,2,4,72,8,152,64,130,8,177,48,36,226,197,130,38,44,
    34,197,132,16,65,8,16,32,132,9,48,144,68,137,137,132,128,137,9,19,1,9,48,144,68,128,132,33,18,8,17,98,64,137,4,72,33,2,36,36,36,68,2,36,76,76,36,76,70,38,38,18,2,68,18,2,76,16,32,
    64,137,4,88,17,49,97,39,22,18,113,48,147,139,39,4,88,177,48,65,19,4,16,69,139,2,38,8,68,194,38,8,152,176,137,137,147,139,22,38,69,19,19,35,130,69,34,139,22,78,38,78,9,56,18,112,64,
    132,76,34,65,18,8,144,64,147,4,8,144,144,66,17,32,130,4,8,152,144,33,22,44,8,152,16,64,137,129,2,44,200,128,255,248,142,4,3,12,128,97,20,253,255,223,84,81,59,58,6,45,151,160,206,
    72,104,66,41,31,105,240,72,209,12,43,14,92,92,66,138,103,106,172,14,206,22,70,73,196,29,117,8,98,49,73,186,88,20,201,105,144,18,226,97,140,39,211,9,44,197,49,0,16,229,195,151,23,
    48,194,60,189,7,145,136,228,20,210,49,120,3,74,73,14,164,100,70,18,79,69,152,66,57,118,133,148,142,202,148,37,197,241,7,89,25,44,95,34,179,228,0,50,38,197,217,200,177,40,116,197,
    226,192,32,92,176,136,37,255,248,104,4,4,255,86,22,255,143,245,255,81,132,203,179,152,125,162,33,201,4,197,98,197,195,17,161,70,20,105,115,84,74,223,155,26,175,18,38,201,174,41,
    178,22,226,57,149,0,78,114,134,176,161,68,57,8,115,113,81,144,168,196,74,98,225,68,108,132,95,8,197,76,94,51,136,180,196,153,49,69,177,139,17,180,162,84,52,100,72,233,142,117,205,
    35,62,49,189,201,192,204,247,177,166,201,214,27,200,106,6,67,136,206,156,169,29,130,37,9,198,97,50,188,32,229,132,58,18,62,49,10,101,164,101,168,93,28,195,12,177,90,17,189,215,134,
    215,70,149,33,70,33,91,99,75,151,196,166,118,19,89,14,163,38,168,115,114,81,160,158,225,255,248,124,4,5,0,255,32,66,24,1,96,22,1,96,24,68,100,142,55,16,243,30,139,237,203,170,73,
    133,179,106,123,212,152,68,39,185,168,66,234,163,147,8,186,222,186,67,209,84,178,23,12,78,152,253,24,61,200,76,244,106,55,68,31,136,186,242,15,20,252,198,149,169,40,149,76,168,177,
    66,197,22,170,91,35,225,143,33,238,221,228,38,212,56,90,16,186,66,209,170,146,70,30,233,143,70,177,51,214,135,149,241,112,207,132,136,53,16,130,200,139,180,127,82,213,207,66,30,
    207,232,212,93,169,172,167,169,69,144,149,60,173,89,73,151,22,204,251,113,99,4,178,154,113,104,65,163,31,172,187,32,88,227,221,11,86,47,17,246,67,220,120,226,192,155,220,255,248,
    141,4,6,125,0,57,16,33,139,204,90,98,211,21,24,156,196,230,37,49,25,136,140,64,103,230,120,103,102,116,103,6,112,103,6,106,102,134,106,102,70,100,102,70,100,102,5,60,83,197,54,83,
    69,48,83,5,38,82,37,120,174,149,162,26,66,136,242,48,102,17,137,130,78,109,51,81,148,109,31,66,168,101,13,161,212,62,172,80,234,29,67,232,125,13,161,212,46,133,208,154,17,66,40,
    234,54,142,153,230,121,154,140,153,166,122,50,140,163,40,202,54,132,80,154,17,66,168,77,10,161,148,46,134,80,202,25,67,104,117,11,161,180,46,132,208,170,21,66,104,69,29,70,209,147,
    60,205,51,204,147,44,205,50,204,243,53,25,71,80,154,23,66,232,109,88,171,117,118,164,170,78,166,42,110,167,42,122,204,108,210,205,236,234,206,236,242,207,236,254,207,236,67,98,27,
    17,88,130,196,22,32,177,13,136,44,67,98,11,63,177,13,136,44,250,196,22,127,103,214,125,98,27,16,88,134,196,54,33,177,13,136,236,73,98,59,18,216,142,196,182,35,177,45,137,44,69,98,
    59,17,89,253,158,89,221,155,217,189,153,89,141,78,212,205,75,84,133,95,171,116,62,133,81,243,60,203,34,139,2,68,55,25,70,98,48,141,35,72,65,9,33,4,40,132,144,178,20,66,200,105,14,
    33,132,52,172,21,130,180,86,138,225,92,43,165,32,82,69,38,82,165,44,82,229,48,83,69,54,83,101,56,83,197,58,83,197,58,83,133,58,83,165,54,83,37,46,82,229,44,82,69,32,87,202,241,91,
    43,68,60,134,144,210,20,66,200,72,47,13,255,248,110,4,7,255,12,128,203,24,1,16,16,1,0,15,72,106,105,31,236,184,196,209,5,185,116,228,164,194,85,196,179,26,201,186,158,185,133,18,
    202,218,207,151,89,35,133,186,182,81,55,94,32,98,121,207,69,18,16,45,80,130,89,94,194,196,26,97,165,92,127,103,168,145,69,163,97,201,88,130,81,133,138,37,19,61,232,185,151,22,82,
    242,183,184,98,67,6,156,104,129,35,133,149,169,16,76,171,216,153,201,82,251,105,94,177,15,202,73,199,179,89,233,49,253,144,72,162,200,121,15,114,115,54,88,88,193,35,84,102,212,246,
    195,5,133,26,165,178,225,6,149,91,234,171,214,126,74,78,90,196,18,137,169,243,4,132,6,135,13,49,230,13,12,22,200,188,242,11,116,128,131,31,255,248,120,4,8,0,255,73,20,0,224,16,16,
    75,47,151,178,70,33,56,71,100,214,172,180,58,100,203,46,101,196,98,152,67,202,190,90,177,38,184,184,67,18,112,70,90,84,232,164,84,93,179,144,78,38,154,45,147,66,52,56,140,70,82,
    101,229,172,90,145,137,177,60,157,98,35,146,50,162,248,180,53,225,25,56,70,171,134,151,48,134,17,196,154,133,136,155,11,136,196,209,102,70,234,136,172,228,89,175,72,57,54,19,81,
    137,115,20,40,98,151,49,12,17,244,64,167,209,255,248,104,4,9,36,176,72,0,0,2,0,15,255,0,8,59,155,38,6,254,0,192,31,90,181,233,35,8,196,96,154,49,73,65,12,19,32,248,214};
// hand-made: 20-bit mono at 37 kHz with frame headers that leave the depth and rate to STREAMINFO. 335 bytes.
static const unsigned char kFlac_hand_header_defers_to_streaminfo[] = {
    102,76,97,67,128,0,0,34,0,192,0,192,0,0,37,0,1,0,9,8,129,48,0,0,0,222,43,6,7,219,74,225,147,244,254,203,181,18,168,186,55,208,255,248,16,0,0,40,70,255,255,240,1,160,0,51,160,4,209,
    181,57,158,77,1,159,238,1,27,110,199,115,232,133,33,201,2,96,154,41,10,50,168,176,43,75,114,204,186,46,75,194,248,190,49,12,67,16,201,50,12,211,68,209,53,13,147,112,225,55,206,99,
    168,237,59,207,51,212,250,61,207,211,240,254,16,7,209,252,124,159,39,169,234,118,157,71,33,186,109,154,6,105,134,94,22,165,121,76,81,18,100,153,26,64,191,222,239,103,131,169,214,
    233,114,185,28,78,87,27,161,206,232,117,59,94,79,87,179,237,246,65,16,4,81,8,70,17,196,113,32,71,146,4,153,30,74,18,4,105,34,71,145,132,89,22,68,17,68,57,20,66,17,36,81,12,68,145,
    132,121,26,75,146,68,217,54,80,20,165,57,90,89,22,101,209,118,95,152,134,49,140,99,25,102,89,148,98,152,230,41,120,95,22,197,137,80,82,147,100,153,20,65,61,158,14,54,235,49,140,
    189,87,170,19,233,68,66,25,15,141,202,231,180,58,205,126,223,129,192,229,115,122,125,127,23,139,211,239,250,64,82,171,255,248,96,0,1,29,172,20,255,59,223,243,60,1,38,158,87,251,41,
    170,202,235,245,61,223,184,175,172,210,76,22,44,72,80,64,28,31};
// hand-made: 20-bit stereo, all four channel assignments, Rice parameters above 14. 2082 bytes.
static const unsigned char kFlac_hand_20bit_stereo[] = {
    102,76,97,67,128,0,0,34,1,0,1,0,0,0,225,0,3,57,11,184,3,48,0,0,3,100,210,34,114,123,230,127,218,235,9,112,115,201,148,101,188,110,255,248,138,170,0,156,74,0,19,128,2,118,0,59,48,4,
    238,0,98,48,7,82,181,57,158,77,1,159,238,0,207,247,6,235,230,35,73,188,232,119,62,136,66,40,140,37,137,226,128,160,43,10,194,168,180,44,139,66,216,186,44,139,194,232,184,133,1,56,
    81,19,229,1,66,81,148,101,33,74,85,20,165,97,84,87,149,165,153,98,91,150,165,193,120,94,23,229,249,130,97,152,166,49,136,99,25,8,100,24,134,57,136,98,152,101,249,128,94,23,133,153,
    108,87,149,197,49,76,80,19,68,193,34,70,145,4,25,2,247,122,61,29,110,103,51,125,186,117,130,177,84,160,79,38,19,9,100,178,65,36,150,74,36,20,9,132,226,145,68,172,88,44,23,139,134,
    98,249,168,210,109,54,156,14,103,117,59,30,15,71,195,217,240,252,126,62,136,7,227,233,248,250,125,62,30,143,167,131,201,220,240,118,58,157,206,103,115,145,208,236,116,59,177,216,
    240,119,61,159,15,130,0,130,34,8,194,48,152,38,137,226,168,162,44,139,34,224,190,47,12,99,16,200,51,12,163,48,208,50,140,227,25,204,130,248,194,45,11,130,152,164,39,137,2,32,130,
    120,58,155,76,198,66,193,84,150,73,125,119,116,103,101,92,84,79,74,64,67,198,189,45,45,157,238,94,126,191,47,79,143,223,250,13,38,143,77,168,84,235,21,106,237,122,203,98,181,91,
    173,183,91,197,242,253,35,68,191,251,26,34,106,218,230,188,151,154,243,14,44,65,165,69,242,132,72,20,36,84,169,51,36,203,148,52,92,171,152,44,96,217,83,101,141,150,48,88,193,147,
    37,10,185,82,166,74,21,38,76,137,66,4,72,190,65,225,138,48,80,162,136,32,166,41,72,68,244,29,254,207,165,239,37,231,150,121,226,69,163,173,147,34,249,18,166,75,151,42,92,217,211,
    165,213,123,21,171,84,175,94,197,106,213,235,214,171,94,149,14,159,50,92,185,178,165,201,190,85,247,72,182,217,137,52,243,94,75,222,149,254,174,50,146,56,151,40,151,188,162,136,52,
    163,141,60,168,184,72,145,50,100,139,20,36,92,185,99,5,139,149,131,5,205,150,58,88,193,163,70,12,25,48,96,177,91,40,96,185,82,197,138,148,36,76,145,71,136,16,35,143,48,242,138,121,
    79,45,141,91,84,137,228,109,250,82,146,94,73,38,158,44,240,218,188,255,248,138,26,1,212,20,255,12,95,240,69,9,19,202,57,250,55,237,239,189,194,51,133,165,100,49,229,60,194,141,60,
    193,6,137,16,32,81,194,68,154,40,209,5,154,65,196,18,245,16,150,35,43,214,181,255,122,111,105,102,180,246,158,210,94,90,210,215,211,98,119,51,187,148,198,40,166,56,194,20,83,148,
    226,49,10,202,169,159,150,173,235,45,231,158,72,179,207,26,103,143,44,72,178,207,44,179,86,151,166,23,149,110,228,20,131,4,16,32,160,131,142,28,40,80,161,133,14,48,161,4,28,166,33,
    138,158,171,141,245,222,189,250,255,54,191,95,36,211,98,226,251,149,138,114,140,40,131,132,16,40,65,135,12,48,65,2,138,32,134,17,204,149,109,46,31,165,188,246,137,26,88,243,194,
    207,13,22,44,120,130,114,69,61,135,46,211,84,181,220,221,10,168,32,78,249,209,73,160,25,119,246,223,239,43,251,6,191,170,244,83,72,50,205,18,40,216,254,28,147,235,171,49,118,68,
    252,206,11,142,72,115,62,125,106,157,246,235,247,103,29,31,38,133,217,60,165,255,157,59,162,115,47,179,235,151,61,225,51,20,235,1,138,232,74,63,250,82,21,185,238,83,104,203,19,236,
    12,223,14,167,81,172,14,52,200,205,165,242,67,14,176,9,2,132,97,127,42,121,172,220,254,154,59,9,150,162,175,77,145,227,237,73,30,98,180,118,36,160,83,73,185,212,233,139,142,164,92,
    31,197,188,226,69,159,129,78,234,150,191,160,239,115,100,75,84,194,107,160,249,237,88,162,31,70,34,91,213,121,141,111,21,119,15,98,222,92,96,121,39,165,221,252,84,229,25,27,232,
    110,60,94,128,12,86,255,102,15,67,86,143,146,61,130,18,172,193,197,58,115,173,173,138,174,145,220,250,57,65,9,120,156,106,121,130,24,158,77,209,56,238,44,174,140,136,147,220,157,
    213,205,132,100,227,172,191,187,231,125,102,122,64,165,2,53,164,52,150,112,68,134,90,213,161,37,230,189,252,220,243,101,247,34,233,43,60,241,47,96,198,43,199,74,8,34,32,169,198,
    193,232,55,111,30,63,74,56,42,151,89,216,172,161,203,51,50,45,253,178,126,138,198,45,152,195,45,67,96,79,178,50,57,39,147,140,202,37,122,163,67,122,99,24,179,5,43,45,30,160,18,126,
    204,136,36,232,237,149,70,71,68,198,167,111,93,54,244,28,39,38,175,6,224,84,184,245,207,51,223,222,197,208,65,214,171,110,69,0,242,27,110,244,153,178,14,13,219,113,53,136,94,72,
    221,165,136,137,151,23,209,244,107,174,221,202,175,25,36,135,14,206,35,206,35,57,28,138,87,135,41,159,80,137,160,87,54,244,27,187,166,128,210,90,26,37,251,55,34,4,51,61,120,75,8,
    165,134,251,94,5,103,149,157,221,101,168,174,5,74,118,26,101,118,252,213,51,139,249,7,60,150,29,131,222,190,182,246,180,83,71,182,244,68,40,74,129,124,130,247,137,46,24,116,82,156,
    135,149,41,56,77,16,242,216,70,131,154,181,35,164,244,17,78,97,91,194,12,150,12,215,101,199,58,175,162,164,30,173,21,33,143,241,230,165,0,77,55,185,224,86,31,230,11,98,220,20,94,
    169,245,28,19,223,35,188,107,101,13,145,27,94,137,183,182,75,157,34,75,123,45,134,161,75,159,4,145,202,69,231,243,174,255,133,216,20,9,191,210,189,90,129,227,224,105,13,204,218,
    185,54,130,111,5,225,121,80,137,195,139,100,192,128,224,25,159,42,59,253,45,249,58,143,25,190,255,136,87,12,33,253,57,116,112,114,202,226,5,105,58,228,255,248,138,138,2,60,86,2,52,
    160,35,117,2,57,0,35,159,2,57,224,35,147,2,55,240,35,93,2,51,80,35,3,2,44,160,34,137,230,185,154,201,154,6,107,247,8,12,159,238,128,24,191,221,128,48,255,188,0,99,255,122,135,54,
    141,131,64,210,57,156,102,25,70,113,136,101,24,198,97,128,100,25,134,49,140,100,24,198,81,204,163,24,200,49,140,195,0,196,48,12,34,240,190,44,139,34,184,164,41,142,76,18,196,129,8,
    254,123,59,28,13,198,115,25,124,172,84,38,145,134,36,62,56,51,46,43,37,35,118,241,163,54,43,86,168,13,196,51,16,135,83,28,230,20,40,193,197,233,87,139,148,46,112,177,195,198,143,
    28,18,54,120,82,6,207,26,56,96,209,83,4,137,147,113,246,218,8,64,66,27,8,166,93,118,168,132,229,57,229,20,105,130,6,158,16,48,48,32,36,87,205,158,58,124,225,243,134,205,24,46,85,
    247,94,23,8,36,174,212,127,175,126,252,188,66,126,51,29,132,231,70,167,211,234,245,218,245,142,215,105,185,220,174,247,139,237,250,66,156,137,33,72,242,68,145,38,73,82,100,153,40,
    73,114,132,165,38,202,114,140,229,89,74,82,150,101,105,102,90,150,37,233,130,97,152,102,137,146,107,155,232,86,214,53,173,111,91,87,245,253,123,99,88,150,61,145,101,217,150,101,
    153,20,255,245,175,255,179,192,47,185,45,51,201,204,243,110,142,146,197,106,169,229,168,249,81,217,88,241,14,145,211,58,165,171,145,236,90,207,50,196,44,71,219,231,50,185,21,157,
    187,42,132,62,166,56,139,204,169,174,137,218,66,28,78,206,17,105,37,100,40,209,88,142,73,163,78,78,49,82,66,93,198,58,66,78,206,101,77,102,249,44,40,145,83,160,161,29,68,82,147,
    219,190,93,106,144,181,22,179,148,149,88,233,122,152,243,117,148,199,250,239,249,10,152,82,34,217,42,123,91,220,128,107,197,255,248,106,154,3,99,73,18,255,253,32,74,205,155,150,
    236,219,175,98,197,170,182,43,87,171,78,157,90,223,82,167,245,9,141,22,52,177,39,180,178,214,221,246,213,237,103,118,49,76,68,16,65,164,152,121,102,154,120,145,2,70,30,44,104,129,
    131,7,142,22,56,170,148,48,80,177,82,229,11,23,40,88,177,18,196,203,148,36,64,145,34,79,190,68,253,204,239,220,139,253,196,246,43,172,160,128,112,170,11,96,186,12,96,206,13,64,224,
    14,96,232,15,160,248,15,192,254,4,16,32,129,12,8,96,246,4,16,122,7,112,115,6,224,106,6,64,91,5,80,79,4,64,252,29,67,72,98,10,129,56,34,14,163,128,198,44,9,130,57,236,230,101,49,
    150,10,4,114,43,211,195,123,115,75,51,26,250,226,250,202,210,234,178,218,226,202,218,202,234,242,226,226,234,226,242,234,210,202,210,162,154,138,114,74,74,2,6,70,86,142,158,247,15,
    63,95,164,40,120};
// hand-made: 32-bit stereo whose side channel needs 33 bits. 1639 bytes.
static const unsigned char kFlac_hand_32bit_wide_side[] = {
    102,76,97,67,128,0,0,34,0,128,0,128,0,0,141,0,2,238,10,196,67,240,0,0,1,16,199,127,35,89,2,90,129,100,87,125,159,2,41,105,19,215,255,248,105,174,0,127,82,20,0,195,76,231,1,155,250,
    109,74,117,176,195,158,24,189,255,126,143,195,85,112,29,178,170,245,202,40,175,212,181,36,184,140,165,177,178,213,222,76,35,173,50,230,84,209,188,243,82,76,77,65,55,52,10,12,201,
    91,82,252,144,203,35,217,40,163,252,144,82,55,225,22,185,106,61,155,133,171,72,79,77,31,15,251,32,242,29,94,73,28,203,116,207,212,153,248,129,77,109,243,56,137,98,37,84,43,17,99,
    39,214,56,183,147,214,203,16,101,120,138,59,68,93,100,33,190,19,240,250,59,6,179,87,150,46,184,114,108,53,123,80,12,230,9,69,132,34,223,141,222,87,237,206,42,5,206,211,15,228,40,
    203,45,120,121,201,71,196,132,187,23,43,142,110,243,151,127,171,57,26,35,36,194,249,148,15,142,82,218,137,79,229,165,54,148,148,83,154,77,148,9,32,228,36,21,255,188,127,37,189,70,
    108,134,159,87,246,186,85,123,254,54,55,107,228,110,232,134,110,189,97,65,74,150,160,49,147,179,152,146,16,167,20,163,12,37,249,53,54,91,225,226,227,4,56,119,19,61,145,50,167,77,
    11,108,238,48,24,8,198,214,35,39,92,140,150,134,49,106,184,190,209,162,210,1,10,116,250,37,206,24,132,95,199,122,66,53,247,177,125,109,10,64,16,67,251,167,89,26,102,246,197,129,
    134,73,141,116,79,36,210,211,190,2,128,236,191,63,134,104,161,124,45,49,183,170,147,71,91,94,171,6,59,217,41,238,36,177,73,196,183,78,119,16,205,157,231,28,162,165,245,212,9,9,10,
    83,5,114,236,230,25,253,208,228,96,7,195,34,17,121,140,79,117,145,98,181,198,27,55,26,150,65,69,150,60,151,88,158,97,62,177,147,181,166,134,241,26,239,208,110,221,73,199,18,199,71,
    236,29,191,68,121,59,161,237,77,7,211,56,31,177,252,32,20,176,65,92,170,132,36,17,8,94,148,16,218,150,33,211,24,67,154,8,134,183,209,11,142,194,17,51,196,18,216,136,0,86,63,75,72,
    249,216,131,216,205,143,30,239,59,83,136,232,73,37,28,93,40,110,121,225,173,122,166,128,170,153,33,8,96,224,49,115,226,165,143,125,21,50,94,80,118,113,47,228,68,117,204,144,167,
    206,251,132,7,141,68,185,222,29,35,210,135,25,87,208,160,85,15,84,12,201,236,201,226,117,228,68,177,122,4,204,96,146,195,247,219,231,173,49,165,198,139,11,76,187,131,121,49,129,
    233,161,166,93,179,8,242,189,159,190,23,255,44,221,65,131,5,24,204,84,172,118,83,204,179,83,123,117,94,113,181,185,132,215,215,227,99,2,37,153,211,246,155,117,104,217,140,219,125,
    37,174,73,83,186,94,26,238,0,123,199,255,175,89,13,190,45,210,251,113,11,246,135,239,246,237,144,22,77,32,98,102,64,243,236,129,182,89,2,123,90,16,4,73,127,70,45,249,36,55,209,3,
    94,230,31,121,233,141,223,239,119,93,205,92,225,111,112,245,221,184,181,246,179,147,218,1,163,100,171,13,132,120,21,214,74,192,35,221,255,248,105,142,1,127,4,70,99,254,210,61,100,
    15,189,163,100,26,122,94,100,31,46,40,148,57,185,48,27,251,161,166,255,71,188,111,226,239,134,252,253,254,111,187,5,70,250,93,232,111,144,166,102,247,194,204,111,105,0,166,245,124,
    124,111,73,57,70,243,224,116,111,54,127,166,243,50,68,111,52,149,166,243,176,222,111,70,253,38,245,132,78,111,111,104,102,248,199,52,111,175,21,102,253,117,56,112,5,8,71,3,127,132,
    112,111,107,103,10,181,212,112,234,243,231,18,220,96,113,114,194,199,27,156,248,114,1,168,103,36,151,228,114,144,216,103,45,102,4,115,25,83,103,53,143,42,115,148,53,135,60,163,0,
    115,250,23,135,66,52,64,116,68,142,167,69,217,240,116,109,157,71,71,62,108,116,112,78,199,70,27,198,116,72,194,71,66,69,166,115,244,154,167,59,157,24,115,115,117,167,50,33,80,114,
    197,194,39,37,235,94,113,237,175,167,23,43,182,112,238,209,103,6,39,4,111,206,29,166,243,48,120,110,145,102,166,222,173,164,109,63,100,166,201,10,6,107,223,31,70,178,185,184,106,
    119,118,102,156,50,16,105,15,121,134,133,216,40,103,173,206,70,112,16,184,102,87,203,166,91,45,204,101,18,98,38,71,111,78,99,224,183,198,53,2,122,98,197,50,70,35,253,46,97,192,67,
    70,20,96,176,96,209,119,198,6,26,8,95,246,167,5,249,2,72,95,45,26,197,236,220,146,94,113,1,101,225,101,148,93,189,97,69,214,86,200,93,13,164,37,203,87,184,92,92,128,5,192,31,238,
    91,165,83,229,180,100,200,90,227,248,5,167,229,192,90,20,234,101,154,119,52,89,53,191,133,139,252,58,88,69,49,197,124,106,28,87,67,163,101,107,204,218,86,50,48,101,90,74,234,85,20,
    2,69,72,21,232,83,236,235,37,53,121,32,82,194,13,229,34,210,24,81,153,92,37,16,126,200,80,121,161,196,254,239,8,79,105,101,4,238,148,70,78,111,169,36,223,211,174,77,147,26,65,69,
    142,31,3,138,198,228,201,237,23,146,33,4,191,161,67,104,83,172,22,213,5,194,225,126,212,100,238,25,135,6,166,129,171,136,108,213,27,254,199,23,145,197,96,115,72,28,168,71,70,241,
    202,152,113,106,28,192,71,1,145,190,0,109,26,26,201,6,138,177,153,192,98,229,24,62,5,187,161,94,224,85,99,19,177,196,189,65,24,224,64,187,62,64,157,115,142,7,198,149,243,46,33,113,
    108,174,194,80,157,36,122,144,211,158,144,93,4,155,143,88,219,23,196,213,119,211,177,17,199,80,75,241,216,52,4,53,210,183,202,249,157,187,102,189,165,63,143,20,107,117,17,182,98,
    215,80,105,114,180,77,25,164,44,153,14,59,183,16,117,130,27,190,38,94,61,238,71,118,228,139,70,221,148,2,196,245,92,224,173,175,213,128,234,152,117,27,10,124,29,59,58,151,23,75,29,
    165,51,210,74,105,4,20,164,42,79,173,39,98,149,141,76,140,166,249,84,28,170,51,85,88,90,191,61,105,34,189,233,97,166,178,244,218,152,173,228,119,55,155,189,173,247,223,7,179,137,
    95,199,105,229,50,179,89,250,42,109,65,222,188,7,108,63,189,135,225,200,243,33,250,81,253,160,191,20,64,223,56,255,248,105,158,2,15,206,2,89,136,118,126,172,191,244,25,150,93,213,
    149,139,45,216,147,245,150,99,102,234,202,237,128,193,101,84,212,6,178,153,145,186,89,68,111,245,172,158,22,160,22,77,1,56,171,37,127,71,197,146,65,16,234,200,226,97,49,100,82,190,
    144,178,26,124,99,2,146,152,26,135,146,48,165,44,145,207,255,139,145,118,253,76,145,38,101,205,144,222,240,82,144,161,69,33,144,109,248,62,144,69,134,141,144,40,89,236,144,22,191,
    178,144,16,239,49,144,23,2,173,144,40,252,96,144,70,193,194,144,112,30,158,238,195};
// hand-made: 24-bit, order-32 prediction with 15-bit coefficients whose sums need 64-bit arithmetic. 1417 bytes.
static const unsigned char kFlac_hand_24bit_lpc_64bit_sums[] = {
    102,76,97,67,128,0,0,34,1,0,1,0,0,1,107,0,3,244,23,112,1,112,0,0,1,128,79,163,114,227,212,36,180,141,133,193,116,58,193,112,129,85,255,248,139,12,0,145,126,121,249,169,130,207,32,
    122,81,233,130,219,71,125,87,215,136,240,111,120,16,233,129,189,108,123,106,1,131,2,109,126,12,211,131,250,183,125,97,74,129,132,212,117,247,95,137,2,94,123,100,137,135,42,161,125,
    160,147,135,125,168,119,115,40,132,82,80,124,69,168,137,184,46,120,188,238,138,83,110,117,240,183,135,242,148,117,160,231,134,93,53,119,46,27,133,183,205,231,63,255,128,2,255,254,
    0,11,255,248,0,47,255,224,0,191,255,128,2,255,254,0,11,255,248,0,47,255,224,0,191,255,128,2,255,254,0,11,255,248,0,47,255,224,0,191,255,128,2,255,254,0,11,255,248,0,47,255,224,0,
    166,14,62,246,81,48,252,35,12,171,240,94,49,143,196,53,83,190,255,129,40,251,188,144,251,239,129,7,79,195,56,158,191,10,242,228,252,79,10,27,240,213,9,143,187,125,236,190,234,190,
    100,251,172,255,251,238,10,156,79,185,81,87,190,242,132,252,251,111,247,171,238,163,102,143,182,56,84,190,226,233,204,251,221,193,11,238,248,151,207,189,204,175,191,3,190,72,252,
    30,91,171,242,3,136,143,208,89,145,191,67,107,176,253,76,133,139,245,108,149,143,218,146,219,241,251,192,210,231,237,25,177,95,176,161,102,126,165,98,133,250,184,249,167,236,192,
    229,223,184,187,4,126,221,73,117,251,126,133,7,237,185,218,95,173,33,172,126,172,155,205,251,184,248,231,238,154,208,223,179,101,190,126,186,85,173,250,239,72,103,237,231,15,223,
    193,38,60,126,205,112,85,251,97,161,39,235,112,230,95,164,170,34,126,128,182,69,249,96,105,167,227,236,187,95,150,38,144,126,109,243,181,249,15,84,7,230,0,141,223,151,249,248,126,
    70,150,223,143,197,195,4,63,23,148,38,252,59,75,3,239,188,102,47,189,176,162,62,233,90,210,251,83,243,99,236,88,240,175,175,12,1,62,191,90,166,250,146,153,227,234,193,2,175,166,
    242,57,62,146,207,46,250,17,68,83,231,131,230,47,161,200,155,62,132,225,250,249,232,60,115,231,200,181,111,163,150,161,62,129,31,50,250,77,211,131,233,14,40,111,164,205,203,62,158,
    90,14,250,147,62,163,233,244,8,47,159,157,130,62,138,252,134,249,232,4,3,231,151,230,60,125,17,241,1,244,171,38,23,206,241,142,31,62,9,197,125,30,114,17,244,108,191,23,207,210,207,
    31,68,201,5,124,253,208,89,244,128,23,119,206,70,105,31,57,6,27,124,250,176,129,244,16,217,215,207,240,168,159,62,63,109,125,41,232,241,244,126,207,23,212,224,166,159,87,202,3,125,
    113,89,113,245,193,169,23,214,208,15,31,96,147,101,125,151,50,17,246,150,227,183,218,45,58,159,113,204,109,125,219,102,169,247,166,165,87,220,132,226,159,120,92,45,227,239,254,92,
    143,193,63,229,190,244,70,16,251,224,92,11,239,206,30,143,184,13,229,190,246,230,156,251,203,240,43,239,98,48,207,198,19,102,191,30,211,180,252,242,74,75,245,93,200,143,211,6,81,
    191,91,163,8,253,128,138,59,245,214,223,79,216,135,185,191,86,255,156,253,64,165,91,245,31,31,15,213,104,155,191,94,209,128,253,171,253,203,246,177,186,143,215,1,117,191,80,67,228,
    252,255,168,203,242,82,214,15,195,149,103,191,17,88,212,252,155,14,207,31,136,248,0,126,19,118,21,248,104,136,39,228,18,178,95,141,188,118,126,33,7,253,249,128,200,167,230,58,39,
    223,164,65,16,126,142,150,197,249,77,76,7,227,252,115,223,129,6,102,125,215,140,61,247,198,61,71,221,171,84,223,116,16,18,126,12,82,165,247,30,230,231,223,61,138,223,116,118,200,
    125,153,251,109,245,194,137,199,211,212,64,223,75,196,206,124,249,163,141,244,236,96,71,212,22,8,95,80,172,18,125,103,154,53,246,16,15,71,214,153,135,248,250,137,157,131,234,94,
    207,111,166,214,183,62,147,193,202,249,235,6,115,232,232,45,111,168,6,213,62,158,85,70,250,130,0,3,232,203,240,111,161,23,157,62,158,73,6,250,166,247,3,236,112,71,175,182,36,122,
    62,212,250,118,251,123,137,179,238,46,130,111,178,11,82,62,230,151,234,251,30,66,19,236,240,43,47,178,159,153,62,221,111,222,251,246,6,115,239,242,133,239,191,160,212,62,220,219,
    166,251,132,138,83,239,174,93,239,189,88,109,62,246,228,194,223,208,255,248,107,12,1,127,245,126,255,255,254,0,50,1,0,99,244,0,149,196,0,199,111,0,248,229,1,42,23,1,90,247,1,139,
    120,1,187,146,1,235,53,2,26,88,2,72,236,2,118,237,2,164,65,2,208,243,2,252,229,3,40,23,3,82,132,3,124,19,3,164,203,3,204,162,3,243,135,4,25,130,4,62,129,4,98,124,4,133,119,4,167,
    104,4,200,80,4,232,35,5,6,233,5,36,146,230,185,154,201,154,6,107,247,8,12,159,238,128,24,191,221,128,48,255,188,0,99,255,120,0,207,255,0,1,191,254,0,3,255,252,0,7,255,248,0,16,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,36,6,191,92,32,6,32,186,33,102,33,236,34,148,35,2,35,154,35,240,36,116,36,192,37,34,37,116,37,200,37,244,38,58,38,130,38,152,38,196,39,4,39,2,
    39,50,39,76,39,90,39,108,39,122,39,146,39,152,39,160,39,174,39,172,39,200,105,61,193,63,33,62,129,63,33,63,241,64,33,64,241,65,97,66,65,66,241,67,209,69,49,70,81,71,177,72,209,74,
    129,76,81,78,49,79,225,82,161,83,241,87,161,88,225,92,177,94,145,98,65,100,241,104,113,107,209,111,33,114,241,117,243,75,214,139,238,140,21,12,47,140,83,12,116,12,150,140,183,140,
    220,140,253,141,32,141,65,141,98,13,136,13,169,13,198,141,236,14,10,142,39,142,70,142,100,14,127,142,158,14,178,142,208,142,226,142,249,15,15,143,29,143,47,143,65,15,72,0,141,227};
// hand-made: eight channels (the first two are kept). 1804 bytes.
static const unsigned char kFlac_hand_eight_channels[] = {
    102,76,97,67,128,0,0,34,0,192,0,192,0,1,120,0,5,106,5,98,46,240,0,0,0,242,88,35,212,229,172,27,236,180,106,90,144,233,169,30,174,131,255,248,22,120,0,95,20,0,1,0,44,0,162,174,21,
    75,202,234,175,50,164,74,105,155,233,21,225,250,185,225,86,235,205,136,204,194,49,147,17,39,79,250,247,197,202,150,19,34,33,27,83,123,227,187,139,202,167,44,45,182,38,98,108,100,
    72,157,49,109,254,187,21,19,136,216,194,16,68,27,13,145,50,177,97,106,215,42,46,206,37,108,109,147,93,45,63,87,83,209,114,169,22,137,137,134,18,0,39,9,1,96,88,56,26,11,13,12,26,40,
    210,8,82,85,132,239,41,210,220,226,137,36,135,144,196,101,148,85,124,153,211,166,197,68,79,151,46,250,59,190,194,136,48,193,7,137,20,121,229,152,246,165,10,207,61,190,231,57,227,
    92,81,38,154,97,133,189,74,79,237,147,50,34,174,73,73,201,202,202,74,202,72,95,187,102,173,58,95,251,223,93,251,239,148,168,211,171,90,213,107,85,170,85,161,215,27,98,179,77,146,
    46,84,217,179,100,20,18,56,88,161,37,146,198,41,40,198,48,166,56,147,12,52,241,2,15,52,194,30,148,179,245,219,42,76,185,114,164,221,127,249,152,16,2,96,3,168,5,0,6,48,7,80,8,80,9,
    48,9,232,10,128,10,248,11,64,11,120,11,136,11,144,11,104,11,88,11,40,11,0,10,216,10,184,10,152,10,152,10,152,10,176,10,192,10,232,11,48,11,96,11,160,11,216,11,240,12,16,12,32,12,8,
    11,240,11,168,11,72,10,208,10,80,9,168,9,8,8,80,7,152,6,216,6,56,5,168,5,16,4,176,4,88,4,40,4,40,4,40,4,112,4,176,5,32,5,160,6,48,6,216,7,112,8,8,8,160,9,48,9,160,9,240,10,80,10,
    128,10,144,10,160,10,144,10,112,10,72,10,40,9,240,9,208,9,168,9,136,9,120,9,144,9,160,9,208,10,8,10,56,10,144,10,224,11,32,11,96,11,152,11,184,11,192,11,168,11,128,11,40,10,152,10,
    0,9,48,8,72,7,80,6,40,4,240,3,192,2,120,1,56,0,7,254,215,253,199,252,191,251,215,250,247,250,79,249,175,249,31,248,175,248,63,247,223,247,135,247,63,246,231,246,143,246,39,245,167,
    245,63,244,167,244,15,243,111,242,183,242,15,241,95,240,183,240,31,239,167,239,55,238,239,238,191,238,183,238,207,239,7,239,95,239,231,240,119,241,55,241,255,242,207,243,175,244,
    143,245,87,246,39,246,223,247,143,248,23,248,135,248,207,249,23,249,55,249,71,249,63,249,47,249,23,248,247,248,239,248,223,248,215,248,207,248,231,249,23,249,95,249,151,249,239,
    250,103,250,199,251,39,251,135,251,223,252,39,252,111,252,127,252,151,252,119,252,71,251,247,251,159,251,31,250,135,249,255,249,87,248,207,248,55,247,175,247,47,246,207,246,159,
    246,127,246,112,128,19,111,101,155,215,140,129,146,34,118,84,11,1,106,46,5,204,186,151,18,214,88,202,217,83,41,69,8,158,19,162,108,77,201,217,62,40,101,28,167,21,82,176,86,202,241,
    93,42,229,68,163,147,130,80,70,8,43,226,238,185,45,179,70,203,49,140,51,6,193,177,76,155,68,217,56,174,203,226,65,200,225,49,40,133,80,177,22,178,232,93,139,185,118,46,69,180,180,
    22,2,180,85,74,137,78,41,133,44,167,21,34,178,87,75,33,105,45,229,204,185,151,18,212,88,138,145,64,37,36,53,237,114,154,37,237,82,74,16,28,171,79,220,249,94,199,204,248,255,122,15,
    69,40,245,40,166,52,226,160,212,138,169,87,107,101,120,177,86,114,214,91,235,161,120,111,140,128,72,68,136,72,156,137,72,116,130,95,123,199,116,45,181,157,176,21,150,165,82,234,47,
    64,62,207,107,208,241,124,14,255,191,240,60,127,51,209,245,189,207,127,226,248,190,15,111,213,242,252,46,227,173,233,57,206,75,142,227,56,206,67,150,232,58,238,243,204,247,255,26,
    9,69,105,29,48,167,52,250,159,211,202,48,4,112,5,152,6,176,7,205,169,204,242,104,12,255,112,4,81,100,152,89,5,26,242,202,36,163,9,40,226,139,60,178,207,40,211,12,121,134,52,146,
    152,226,150,210,22,66,202,115,203,44,146,206,40,211,203,56,65,102,154,32,194,206,40,243,11,52,179,140,52,243,204,56,243,202,20,89,229,20,89,46,114,158,140,171,44,230,183,71,127,
    207,250,143,250,170,127,85,180,213,125,119,73,178,76,139,229,93,38,233,21,221,117,21,217,234,104,247,191,71,40,247,105,229,126,255,233,255,170,221,92,239,247,222,235,250,215,251,
    223,247,213,126,138,233,174,154,233,170,138,105,239,104,139,0,82,128,101,0,117,0,68,253,28,149,195,10,212,209,90,81,31,178,22,34,144,86,138,146,118,221,174,35,167,24,180,157,115,
    14,98,237,243,145,201,81,201,50,121,12,252,148,225,5,75,233,30,100,82,57,19,167,8,116,219,232,166,57,5,38,121,170,57,186,88,153,142,68,118,84,118,189,198,29,188,169,39,16,176,98,
    146,103,38,249,117,209,209,72,249,47,102,56,209,89,46,113,128,11,96,13,144,15,80,17,48,76,36,132,55,185,229,52,225,165,40,104,231,215,18,32,183,60,64,241,139,152,87,223,32,145,6,
    163,41,226,134,142,30,20,45,82,132,243,196,47,85,51,21,141,56,180,48,211,9,56,104,65,118,112,147,139,86,37,199,140,18,56,105,87,44,50,200,187,32,89,147,77,86,39,35,72,52,143,66,
    122,114,98,106,171,200,18,33,53,228,54,196,5,132,62,41,6,142,18,232,185,115,232,135,245,162,90,40,104,134,230,38,156,91,147,220,247,110,94,85,202,63,181,134,149,2,128,24,96,27,224,
    23,170,117,115,151,139,234,221,19,102,17,49,26,54,182,227,226,219,94,38,230,68,17,8,132,38,100,143,185,105,75,206,174,92,118,43,105,17,17,55,155,250,82,250,42,245,139,54,98,98,8,
    67,17,136,74,39,168,178,58,162,253,90,54,54,49,49,169,34,95,121,93,84,167,253,231,151,35,33,33,177,33,60,139,49,252,95,81,77,204,140,35,8,42,232,255,248,102,120,1,49,42,18,254,250,
    1,106,195,47,190,72,161,50,36,72,144,113,214,24,93,117,88,89,151,30,40,80,201,195,226,66,66,194,227,2,194,130,2,7,12,18,125,149,17,65,125,122,137,127,114,128,182,105,210,36,202,
    150,44,84,177,18,143,190,227,110,46,211,13,178,65,242,101,204,154,16,17,20,22,21,22,21,17,16,58,96,169,22,151,77,61,118,239,184,151,246,168,11,119,72,148,44,88,209,131,133,204,21,
    40,65,247,219,113,166,221,113,210,37,12,23,56,124,68,80,72,84,72,72,64,249,131,4,154,105,36,117,227,191,40,196,191,178,64,94,16,38,96,201,179,135,77,156,54,92,193,34,68,8,58,227,
    173,62,65,242,165,139,157,60,120,68,72,68,64,64,249,178,196,159,93,100,22,223,223,255,169,18,254,197,1,81,34,100,209,195,231,143,136,31,60,104,193,114,100,72,62,243,206,58,69,226,
    164,204,154,54,120,232,129,243,167,76,151,44,243,107,34,159,62,123,82,157,120,151,246,72,10,153,115,103,4,4,69,4,68,196,4,14,154,50,80,161,2,14,58,235,164,93,40,68,185,99,70,205,
    156,56,116,185,146,111,184,186,41,109,223,212,170,89,175,18,254,214,1,86,50,120,248,160,168,176,152,168,152,145,227,102,139,19,32,69,215,92,113,183,222,38,76,169,114,198,204,151,
    46,92,145,38,154,77,29,59,246,149,59,85,237,196,191,186,0,86,205,137,137,137,139,141,10,139,138,9,8,157,52,88,137,18,13,54,227,109,58,219,196,136,21,40,84,169,114,100,200,58,186,
    168,37,207,116,105,217,177,110,228,182,135};
// hand-made: three channels of 8-bit audio (the first two are kept). 858 bytes.
static const unsigned char kFlac_hand_three_channels_8bit[] = {
    102,76,97,67,128,0,0,34,2,64,2,64,0,0,59,0,2,245,3,232,4,112,0,0,2,84,15,42,106,246,231,98,180,4,158,109,174,46,87,25,42,177,255,248,37,34,0,141,20,255,6,16,150,91,178,253,179,181,
    213,58,84,43,91,28,206,157,148,130,210,45,24,35,201,228,232,134,11,152,178,119,17,230,8,49,138,201,165,68,211,39,38,87,66,201,130,52,78,162,215,200,142,145,190,249,149,29,16,172,
    233,157,186,74,136,169,138,77,108,214,196,236,228,221,43,57,23,57,29,138,214,242,57,4,96,156,98,145,163,8,250,35,8,194,210,226,50,166,32,197,9,138,201,148,182,84,105,202,56,76,169,
    136,65,139,148,45,76,151,133,226,177,119,145,150,213,76,132,24,162,25,52,125,17,85,172,71,53,232,153,217,28,139,37,145,157,148,156,75,38,145,168,212,211,39,44,70,45,132,52,67,19,
    209,136,108,83,11,208,141,16,201,6,8,105,90,208,134,68,249,52,74,139,162,122,69,77,162,147,236,172,233,172,154,145,205,227,113,200,209,6,17,181,19,148,152,65,137,145,174,50,35,196,
    140,33,211,8,194,61,114,140,70,74,76,180,157,161,132,56,155,17,24,152,186,132,24,70,95,200,156,86,43,42,81,136,98,89,25,50,48,79,17,137,233,99,42,211,132,98,139,147,68,4,9,12,82,
    59,144,32,8,149,4,49,105,106,43,69,132,234,209,161,6,22,76,168,84,88,33,146,162,203,85,20,132,101,169,178,24,143,136,98,105,107,81,137,170,48,140,33,130,193,105,9,144,154,44,152,
    180,162,36,164,90,35,72,104,184,77,33,75,5,164,70,34,112,160,140,22,20,41,20,70,19,180,182,85,21,68,97,28,196,118,163,11,76,44,133,228,35,38,169,116,35,9,138,76,70,86,106,250,41,
    148,92,84,200,140,83,41,4,96,38,18,192,140,66,106,9,164,196,88,148,69,52,77,132,24,154,22,152,38,65,52,22,178,36,97,100,139,5,73,34,202,152,155,73,162,189,37,246,67,47,75,139,66,
    100,137,66,24,77,106,98,228,84,38,74,166,181,108,35,68,165,136,154,69,197,138,194,196,81,32,140,37,40,70,17,165,96,153,21,9,147,8,48,182,180,169,100,74,37,22,177,26,45,22,145,22,
    176,134,91,70,71,16,98,150,197,47,97,13,46,144,188,40,161,73,34,229,139,148,161,72,76,76,70,17,229,217,98,98,43,4,98,64,160,8,64,64,132,73,20,141,125,148,85,82,241,231,143,62,162,
    18,68,68,36,215,47,210,197,138,78,67,204,198,24,195,49,155,30,115,247,227,140,223,52,176,145,8,66,9,20,137,170,244,161,20,82,93,143,55,180,98,197,16,40,66,32,161,36,161,18,32,66,
    16,128,136,65,36,162,61,183,158,190,209,205,153,177,177,227,242,177,97,22,137,115,141,29,50,86,93,155,108,193,176,108,54,113,228,210,218,71,107,184,136,16,144,17,2,4,65,36,43,85,
    21,9,40,44,83,71,239,55,100,68,73,4,33,65,105,40,89,22,33,9,16,146,41,183,141,134,198,111,205,28,123,108,97,182,28,63,40,178,10,73,81,29,185,117,139,74,104,252,198,51,135,189,34,
    200,137,34,203,45,18,16,34,8,64,129,4,33,4,132,154,203,170,146,148,167,99,134,230,211,132,144,136,145,42,239,150,38,130,137,104,252,198,102,96,217,153,155,104,220,220,216,112,227,
    226,162,8,144,72,132,178,215,171,44,80,149,87,60,219,255,66,72,72,166,12,255,248,101,34,1,19,93,16,1,8,182,235,44,166,154,104,163,189,230,50,183,152,36,104,11,195,203,211,223,231,
    235,239,239,243,239,243,252,8,12,28,40,56,64,80,104,75,208,2,68,109,162,100,153,12,204,204,70,215,48};
// hand-made: frame numbers coded in four and five bytes. 148 bytes.
static const unsigned char kFlac_hand_frame_numbers_4_and_5_bytes[] = {
    102,76,97,67,128,0,0,34,0,16,0,16,0,0,25,0,0,29,10,196,64,240,0,0,0,64,108,164,129,241,136,117,59,243,46,44,153,52,43,175,148,126,255,248,105,8,247,191,191,190,15,64,18,255,255,1,
    15,26,56,96,225,99,69,137,20,36,243,206,44,89,128,255,248,105,8,247,191,191,191,15,85,18,1,30,0,202,49,206,41,72,71,98,35,50,0,17,88,255,248,105,8,248,136,128,128,128,15,198,18,1,
    99,0,190,137,147,35,54,33,12,192,182,20,255,248,105,8,248,136,128,128,129,15,211,18,1,136,0,249,200,114,152,238,82,148,172,236,79,138};
// hand-made: frame numbers coded in five and six bytes. 153 bytes.
static const unsigned char kFlac_hand_frame_numbers_5_and_6_bytes[] = {
    102,76,97,67,128,0,0,34,0,16,0,16,0,0,26,0,0,30,10,196,64,240,0,0,0,64,255,11,226,118,175,205,186,228,86,16,67,54,99,140,164,214,255,248,105,8,251,191,191,191,190,15,48,18,0,1,1,
    14,22,56,104,209,162,198,9,20,36,227,142,52,219,191,255,248,105,8,251,191,191,191,191,15,37,18,1,30,0,202,32,229,16,134,33,29,147,149,0,82,36,255,248,105,8,252,132,128,128,128,128,
    15,154,18,1,99,0,165,141,33,50,33,176,132,32,204,101,255,248,105,8,252,132,128,128,128,129,15,143,18,1,134,0,216,166,49,140,82,144,228,34,149,32,139,142};
// hand-made: sample numbers coded in six and seven bytes. 157 bytes.
static const unsigned char kFlac_hand_sample_numbers_6_and_7_bytes[] = {
    102,76,97,67,128,0,0,34,0,16,0,16,0,0,27,0,0,32,10,196,64,240,0,0,0,64,0,117,25,50,201,168,26,20,118,3,211,97,87,66,172,44,255,249,105,8,253,191,191,191,191,160,15,169,18,0,0,1,15,
    22,56,104,241,130,69,139,18,32,64,131,139,0,80,97,255,249,105,8,253,191,191,191,191,176,15,254,18,1,29,0,206,113,142,114,20,202,236,174,140,101,115,255,249,105,8,254,130,128,128,
    128,128,128,15,98,18,1,97,0,162,106,136,153,25,24,132,48,221,68,255,249,105,8,254,130,128,128,128,128,144,15,53,18,1,136,0,248,132,41,202,83,20,132,33,93,32,42,139};
extern const FlacRef kFlacRefs[] = {
    {"ffmpeg -compression_level 0 (fixed predictors), 16-bit stereo at 44.1 kHz, blocks of 576", kFlac_ffmpeg_level0_16bit_stereo, sizeof kFlac_ffmpeg_level0_16bit_stereo, 112, 44100, 2, 2, 16, 2205, 0xd3815e336018f450ull, {{315, 0, -1814LL}, {1102, 1, -375LL}, {2204, 0, 1012LL}}, true},
    {"ffmpeg -compression_level 5 (LPC), 16-bit stereo at 48 kHz, blocks of 1152", kFlac_ffmpeg_level5_16bit_stereo, sizeof kFlac_ffmpeg_level5_16bit_stereo, 112, 48000, 2, 2, 16, 2400, 0x70375c2cb453bd9cull, {{342, 0, -1821LL}, {1200, 1, -389LL}, {2399, 0, 998LL}}, true},
    {"ffmpeg -compression_level 12 -lpc_type cholesky, orders up to 32, 24-bit stereo at 32 kHz", kFlac_ffmpeg_level12_24bit_stereo, sizeof kFlac_ffmpeg_level12_24bit_stereo, 112, 32000, 2, 2, 24, 1600, 0x93511f0e1ca4e572ull, {{228, 0, -466177LL}, {800, 1, -99844LL}, {1599, 0, 251521LL}}, true},
    {"ffmpeg -compression_level 8, 24-bit mono at 96 kHz, one block of 4800", kFlac_ffmpeg_level8_24bit_mono_96k, sizeof kFlac_ffmpeg_level8_24bit_mono_96k, 112, 96000, 1, 1, 24, 4800, 0x75476ddec952c3e0ull, {{685, 0, -674244LL}, {2400, 0, -658568LL}, {4799, 0, -242673LL}}, true},
    {"ffmpeg -ch_mode indep, 16-bit stereo at 8 kHz", kFlac_ffmpeg_ch_mode_indep, sizeof kFlac_ffmpeg_ch_mode_indep, 112, 8000, 2, 2, 16, 400, 0xa5918aaf87429217ull, {{57, 0, -2638LL}, {200, 1, -2865LL}, {399, 0, -1119LL}}, true},
    {"ffmpeg -ch_mode left_side, 16-bit stereo at 8 kHz", kFlac_ffmpeg_ch_mode_left_side, sizeof kFlac_ffmpeg_ch_mode_left_side, 112, 8000, 2, 2, 16, 400, 0x0007a9609599e14cull, {{57, 0, -2639LL}, {200, 1, -2863LL}, {399, 0, -1121LL}}, true},
    {"ffmpeg -ch_mode right_side, 16-bit stereo at 8 kHz", kFlac_ffmpeg_ch_mode_right_side, sizeof kFlac_ffmpeg_ch_mode_right_side, 112, 8000, 2, 2, 16, 400, 0x128ec9f5581f86a7ull, {{57, 0, -2637LL}, {200, 1, -2865LL}, {399, 0, -1120LL}}, true},
    {"ffmpeg -ch_mode mid_side, 16-bit stereo at 8 kHz", kFlac_ffmpeg_ch_mode_mid_side, sizeof kFlac_ffmpeg_ch_mode_mid_side, 112, 8000, 2, 2, 16, 400, 0x9ea728f541c476c2ull, {{57, 0, -2639LL}, {200, 1, -2863LL}, {399, 0, -1120LL}}, true},
    {"ffmpeg, 11025 Hz (rate written in Hz) and blocks of 1000 (16-bit block size), 16-bit mono", kFlac_ffmpeg_11025hz_blocks_of_1000, sizeof kFlac_ffmpeg_11025hz_blocks_of_1000, 112, 11025, 1, 1, 16, 1103, 0xe02105336b95d1ebull, {{157, 0, 722LL}, {551, 0, -968LL}, {1102, 0, -753LL}}, true},
    {"ffmpeg, 50 kHz (rate written in kHz) and blocks of 200 (8-bit block size), 24-bit stereo", kFlac_ffmpeg_50khz_blocks_of_200_24bit, sizeof kFlac_ffmpeg_50khz_blocks_of_200_24bit, 112, 50000, 2, 2, 24, 600, 0xf8c3caa4b8e24f6dull, {{85, 0, 1047137LL}, {300, 1, -1407323LL}, {599, 0, 789376LL}}, true},
    {"ffmpeg, 384 kHz (rate written in tens of Hz), 16-bit mono", kFlac_ffmpeg_384khz, sizeof kFlac_ffmpeg_384khz, 112, 384000, 1, 1, 16, 1536, 0xcb5980df9819e9adull, {{219, 0, 4216LL}, {768, 0, -1316LL}, {1535, 0, -1409LL}}, true},
    {"ffmpeg, 5.1 (six channels, the first two are kept), 16-bit at 8 kHz", kFlac_ffmpeg_six_channels, sizeof kFlac_ffmpeg_six_channels, 154, 8000, 2, 6, 16, 400, 0x3352a163e7d296f4ull, {{57, 0, -2638LL}, {200, 1, -2865LL}, {399, 0, -1120LL}}, false},
    {"ffmpeg, blocks of 128 holding silence, full-scale noise, samples with empty low bits, a constant channel", kFlac_ffmpeg_silence_noise_wasted_bits, sizeof kFlac_ffmpeg_silence_noise_wasted_bits, 112, 22050, 2, 2, 16, 640, 0x9775af5f95617f3cull, {{91, 0, 0LL}, {320, 1, 2912LL}, {639, 0, -4609LL}}, true},
    {"ffmpeg -min_partition_order 8 -exact_rice_parameters 1, loud 24-bit noise (Rice parameters above 14)", kFlac_ffmpeg_partition_order_8_24bit, sizeof kFlac_ffmpeg_partition_order_8_24bit, 112, 44100, 1, 1, 24, 512, 0xc35ef41ab5d041c9ull, {{73, 0, 1122669LL}, {256, 0, -1287108LL}, {511, 0, 1166386LL}}, true},
    {"ffmpeg -compression_level 12, prediction order 32 with 15-bit coefficients, 16-bit mono", kFlac_ffmpeg_order_32_precision_15, sizeof kFlac_ffmpeg_order_32_precision_15, 112, 44100, 1, 1, 16, 2205, 0x34c80bd82ba4f07aull, {{315, 0, -2727LL}, {1102, 0, -2846LL}, {2204, 0, -1242LL}}, true},
    {"ffmpeg -strict experimental, 32-bit stereo at 8 kHz", kFlac_ffmpeg_32bit_stereo, sizeof kFlac_ffmpeg_32bit_stereo, 112, 8000, 2, 2, 32, 400, 0xdbb623c2842af362ull, {{57, 0, -172871LL}, {200, 1, -187701LL}, {399, 0, -73361LL}}, false},
    {"libFLAC (through libsndfile) at its highest level, 16-bit stereo at 44.1 kHz", kFlac_libflac_level8_16bit_stereo, sizeof kFlac_libflac_level8_16bit_stereo, 86, 44100, 2, 2, 16, 2205, 0xaaf18d3d628df8d3ull, {{315, 0, -1814LL}, {1102, 1, -374LL}, {2204, 0, 1010LL}}, true},
    {"libFLAC at its default level, 24-bit mono at 48 kHz", kFlac_libflac_level5_24bit_mono, sizeof kFlac_libflac_level5_24bit_mono, 86, 48000, 1, 1, 24, 2400, 0x70b325d6b02d512bull, {{342, 0, -466173LL}, {1200, 0, -179483LL}, {2399, 0, 255636LL}}, true},
    {"libFLAC at its fastest level, 8-bit stereo", kFlac_libflac_level0_8bit_stereo, sizeof kFlac_libflac_level0_8bit_stereo, 86, 22050, 2, 2, 8, 1103, 0xb1f36f3a1ba75f9bull, {{157, 0, -27LL}, {551, 1, -28LL}, {1102, 0, -8LL}}, false},
    {"libFLAC, 16-bit audio in a 24-bit file (eight wasted bits)", kFlac_libflac_16bit_audio_in_24bit, sizeof kFlac_libflac_16bit_audio_in_24bit, 86, 22050, 2, 2, 24, 1103, 0x56010dee79d1b0c0ull, {{157, 0, -675584LL}, {551, 1, -717568LL}, {1102, 0, -248064LL}}, true},
    {"hand-made: variable block size, every subframe type and channel assignment, escaped partitions, an ID3v2 tag and extra metadata blocks", kFlac_hand_variable_blocks_everything, sizeof kFlac_hand_variable_blocks_everything, 138, 44100, 2, 2, 16, 2977, 0xffe9136115afaa86ull, {{425, 0, -15LL}, {1488, 1, -253LL}, {2976, 0, 149LL}}, true},
    {"hand-made: 12-bit mono; rate and block size written every way a frame header allows, partition orders 0 to 8", kFlac_hand_12bit_header_forms, sizeof kFlac_hand_12bit_header_forms, 42, 32000, 1, 1, 12, 2341, 0x9d7ea1fb46062e54ull, {{334, 0, 73LL}, {1170, 0, -18LL}, {2340, 0, -5LL}}, false},
    {"hand-made: 20-bit mono at 37 kHz with frame headers that leave the depth and rate to STREAMINFO", kFlac_hand_header_defers_to_streaminfo, sizeof kFlac_hand_header_defers_to_streaminfo, 42, 37000, 1, 1, 20, 222, 0x793f82b0e7efb40cull, {{31, 0, 5714LL}, {111, 0, 4372LL}, {221, 0, -9149LL}}, false},
    {"hand-made: 20-bit stereo, all four channel assignments, Rice parameters above 14", kFlac_hand_20bit_stereo, sizeof kFlac_hand_20bit_stereo, 42, 48000, 2, 2, 20, 868, 0x9118e63e75383dceull, {{124, 0, 3119LL}, {434, 1, 22597LL}, {867, 0, 2410LL}}, false},
    {"hand-made: 32-bit stereo whose side channel needs 33 bits", kFlac_hand_32bit_wide_side, sizeof kFlac_hand_32bit_wide_side, 42, 44100, 2, 2, 32, 272, 0x414e2aba8f910821ull, {{38, 0, 1735099776LL}, {136, 1, -1301737472LL}, {271, 0, 1116379904LL}}, false},
    {"hand-made: 24-bit, order-32 prediction with 15-bit coefficients whose sums need 64-bit arithmetic", kFlac_hand_24bit_lpc_64bit_sums, sizeof kFlac_hand_24bit_lpc_64bit_sums, 42, 96000, 1, 1, 24, 384, 0x67790a3d7726cf0full, {{54, 0, 7975936LL}, {192, 0, 7718376LL}, {383, 0, 566893LL}}, true},
    {"hand-made: eight channels (the first two are kept)", kFlac_hand_eight_channels, sizeof kFlac_hand_eight_channels, 42, 22050, 2, 8, 16, 242, 0xeefd63d74bfa7485ull, {{34, 0, 456LL}, {121, 1, -342LL}, {241, 0, 544LL}}, false},
    {"hand-made: three channels of 8-bit audio (the first two are kept)", kFlac_hand_three_channels_8bit, sizeof kFlac_hand_three_channels_8bit, 42, 16000, 2, 3, 8, 596, 0x5cd6e10c96055f58ull, {{85, 0, -19LL}, {298, 1, 2LL}, {595, 0, 21LL}}, false},
    {"hand-made: frame numbers coded in four and five bytes", kFlac_hand_frame_numbers_4_and_5_bytes, sizeof kFlac_hand_frame_numbers_4_and_5_bytes, 42, 44100, 1, 1, 16, 64, 0x94abc3ce75763a1aull, {{9, 0, 183LL}, {32, 0, 355LL}, {63, 0, 461LL}}, true},
    {"hand-made: frame numbers coded in five and six bytes", kFlac_hand_frame_numbers_5_and_6_bytes, sizeof kFlac_hand_frame_numbers_5_and_6_bytes, 42, 44100, 1, 1, 16, 64, 0x72b521a37d8f6990ull, {{9, 0, 183LL}, {32, 0, 355LL}, {63, 0, 459LL}}, true},
    {"hand-made: sample numbers coded in six and seven bytes", kFlac_hand_sample_numbers_6_and_7_bytes, sizeof kFlac_hand_sample_numbers_6_and_7_bytes, 42, 44100, 1, 1, 16, 64, 0xe6b7af41f38f913bull, {{9, 0, 183LL}, {32, 0, 353LL}, {63, 0, 459LL}}, true},
};
extern const size_t kFlacRefCount = sizeof kFlacRefs / sizeof kFlacRefs[0];
// ---- reference streams: end
