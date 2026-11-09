// MP3 decoding against streams made by other encoders and decoded by a reference decoder: the streams
// and what ffmpeg makes of them are embedded in Mp3TestStreams.h, so nothing outside is needed.
#include "Mp3TestUtil.h"
#include "Mp3TestStreams.h"
using namespace pulp;
using namespace tu;
using namespace mp3t;

namespace {

constexpr int kSpots = 32;

// The two sums of a channel's samples with pseudo-random signs (the same as in dev/make_streams.py).
// Every sample enters both, so an error anywhere moves them.
void signSums(const std::vector<float>& x, double out[2]) {
    for (int which = 0; which < 2; ++which) {
        double sum = 0.0;
        for (size_t i = 0; i < x.size(); ++i) {
            uint32 h = ((uint32)i + 1u + (uint32)which * 0x9E3779B9u) * 2654435761u;
            h ^= h >> 15;
            h *= 2246822519u;
            h ^= h >> 13;
            sum += (h & 1u) ? (double)x[i] : -(double)x[i];
        }
        out[which] = sum;
    }
}

const Mp3RefStream* refNamed(const char* name) {
    for (const auto& r : kMp3RefStreams) if (std::strcmp(r.name, name) == 0) return &r;
    return nullptr;
}

std::shared_ptr<SampleData> decodeRef(const char* name) {
    const Mp3RefStream* r = refNamed(name);
    if (!r) return nullptr;
    return readMp3Memory(r->data, r->size);
}

} // namespace

// Every embedded stream must come out as ffmpeg decodes it: the same rate, channels and exact length,
// and every sample within the stream's tolerance: 2e-5, two thirds of one step of 16-bit audio, for
// the streams from encoders (the measured difference is under 2e-6); for the synthetic streams
// 2e-6 + 2e-5 * peak, because ffmpeg requantises large values in integers.
TEST(mp3_decodes_reference_streams_like_the_reference_decoder) {
    int streams = 0;
    for (const auto& r : kMp3RefStreams) {
        ++streams;
        std::string error;
        const auto s = readMp3Memory(r.data, r.size, &error);
        CHECK_MSG(s != nullptr, "%s: %s", r.name, error.c_str());
        if (!s) continue;
        CHECK_MSG(s->sampleRate == r.rate, "%s: rate %g, expected %d", r.name, s->sampleRate, r.rate);
        CHECK_MSG(s->channels == r.channels, "%s: %d channels, expected %d", r.name, s->channels, r.channels);
        CHECK_MSG(s->frames() == r.frames, "%s: %lld frames, expected %lld", r.name, (long long)s->frames(), r.frames);
        if (s->frames() != r.frames || s->channels != r.channels) continue;
        CHECK_MSG(s->path.empty(), "%s: a path was set", r.name);
        for (int c = 0; c < r.channels; ++c) {
            CHECK_MSG((long long)s->ch[c].size() == r.frames, "%s: channel %d has %zu samples", r.name, c, s->ch[c].size());
            double worst = 0.0;
            for (int k = 0; k < kSpots; ++k) {
                const long long at = ((long long)k * r.frames / kSpots + 17 * k) % r.frames;
                worst = std::max(worst, std::fabs((double)s->ch[c][(size_t)at] - (double)r.spots[c][k]));
            }
            CHECK_MSG(worst <= r.tolerance, "%s: channel %d is off by %.3g at a spot value (tolerance %.3g)", r.name, c, worst, (double)r.tolerance);
            // the sums take in every sample; errors of the tolerated size at random would move them by about tolerance * sqrt(n)
            double sums[2];
            signSums(s->ch[c], sums);
            const double allowed = 0.5 * r.tolerance * std::sqrt((double)r.frames);
            for (int w = 0; w < 2; ++w)
                CHECK_MSG(std::fabs(sums[w] - r.sums[c][w]) <= allowed, "%s: channel %d sum %d is %.9g, expected %.9g (allowed %.3g)", r.name, c, w, sums[w], r.sums[c][w], allowed);
            const double rms = rmsOf(s->ch[c]);
            CHECK_MSG(std::fabs(rms - r.rms[c]) <= 1.0e-4 * r.rms[c] + 1.0e-7, "%s: channel %d rms %.9g, expected %.9g", r.name, c, rms, r.rms[c]);
        }
        if (r.channels == 1) CHECK_MSG(s->ch[1].empty(), "%s: a mono stream filled the second channel", r.name);
    }
    CHECK(streams >= 20);
}

// The embedded set has to contain every variant of Layer III the decoder claims: this reads the
// streams' headers and side information (with the test's own reader) and counts.
TEST(mp3_reference_streams_cover_every_kind_of_layer_3_frame) {
    std::map<std::string, int> seen;
    std::map<int, int> tables;
    for (const auto& r : kMp3RefStreams) {
        const auto frames = readFrames(r.data, r.size);
        CHECK_MSG(frames.size() >= 4, "%s: only %zu frames found", r.name, frames.size());
        std::map<int, int> bitrates;
        for (const auto& f : frames) {
            if (isInfoFrame(r.data, f)) { ++seen["info frame"]; continue; }
            ++seen[f.version == 1 ? "MPEG-1" : (f.version == 2 ? "MPEG-2" : "MPEG-2.5")];
            ++seen["rate " + std::to_string(f.rate)];
            static const char* modes[4] = {"stereo", "joint stereo", "dual channel", "mono"};
            ++seen[modes[f.mode]];
            if (f.mode == 1) {
                static const char* ext[4] = {"joint: left/right", "joint: intensity", "joint: mid/side", "joint: intensity and mid/side"};
                ++seen[ext[f.modeExt]];
                ++seen[std::string(f.version == 1 ? "MPEG-1 " : "MPEG-2 ") + ext[f.modeExt]];
            }
            if (f.crc) ++seen["crc"];
            if (f.bitrateIndex == 0) ++seen["free format"];
            if (f.mainDataBegin > 0) ++seen["reservoir"];
            if (f.scfsi[0] || f.scfsi[1]) ++seen["scfsi"];
            ++bitrates[f.bitrateIndex];
            for (int g = 0; g < f.granules; ++g)
                for (int c = 0; c < f.channels; ++c) {
                    const GranuleInfo& G = f.gr[g][c];
                    static const char* blocks[4] = {"long block", "start block", "short block", "stop block"};
                    ++seen[blocks[G.blockType]];
                    if (G.mixed) { ++seen[G.blockType == 2 ? "mixed short block" : "mixed start or stop block"]; ++seen[f.version == 1 ? "MPEG-1 mixed" : "MPEG-2 mixed"]; }
                    if (G.preflag) ++seen["preflag"];
                    if (f.version != 1 && G.compress >= 500) ++seen["MPEG-2 preflag"];
                    if (f.version != 1 && G.compress >= 400 && G.compress < 500) ++seen["MPEG-2 second scalefactor packing"];
                    if (G.scale) ++seen["scalefac_scale"];
                    ++seen[G.count1Table ? "count1 table B" : "count1 table A"];
                    if (G.subblockGain[0] || G.subblockGain[1] || G.subblockGain[2]) ++seen["subblock gain"];
                    if (G.bigValues) for (int i = 0; i < (G.switching ? 2 : 3); ++i) ++tables[G.table[i]];
                }
        }
        if (bitrates.size() > 2) ++seen["variable bitrate"];
    }
    for (const char* want : {"info frame", "MPEG-1", "MPEG-2", "MPEG-2.5", "rate 44100", "rate 48000", "rate 32000", "rate 22050", "rate 24000", "rate 16000",
                             "rate 11025", "rate 12000", "rate 8000", "stereo", "joint stereo", "dual channel", "mono", "joint: left/right", "joint: intensity",
                             "joint: mid/side", "joint: intensity and mid/side", "MPEG-1 joint: intensity", "MPEG-2 joint: intensity", "MPEG-2 joint: mid/side",
                             "crc", "free format", "reservoir", "scfsi", "long block", "start block", "short block", "stop block", "mixed short block",
                             "mixed start or stop block", "MPEG-1 mixed", "MPEG-2 mixed", "preflag", "MPEG-2 preflag", "MPEG-2 second scalefactor packing",
                             "scalefac_scale", "count1 table A", "count1 table B", "subblock gain", "variable bitrate"})
        CHECK_MSG(seen[want] > 0, "no embedded stream has: %s", want);
    for (int t = 0; t < 32; ++t)
        if (t != 4 && t != 14) CHECK_MSG(tables[t] > 0, "no embedded stream uses Huffman table %d", t);
}

// With an Info frame the decoder gives exactly the samples that were encoded; without one, every
// sample of every frame except the decoder's own 529.
TEST(mp3_length_is_exact_after_removing_delay_and_padding) {
    // the LAME streams were made from clips of known length
    struct { const char* name; long long frames; } made[] = {
        {"lame_mpeg1_44100_joint_cbr64", 13230},        // 0.30 s at 44.1 kHz
        {"lame_mpeg1_48000_stereo_cbr96", 12000},       // 0.25 s at 48 kHz
        {"lame_mpeg1_32000_mono_cbr48", 8000},          // 0.25 s at 32 kHz
        {"lame_mpeg2_24000_mono_cbr24", 10800},         // 0.45 s resampled to 24 kHz
        {"lame_mpeg25_8000_stereo_cbr24", 3600},        // 0.45 s resampled to 8 kHz
    };
    for (const auto& m : made) {
        const auto s = decodeRef(m.name);
        CHECK_MSG(s && s->frames() == m.frames, "%s: %lld frames, expected %lld", m.name, s ? (long long)s->frames() : -1LL, m.frames);
    }
    // no Info frame: frames * 1152 - 529
    for (const char* name : {"lame_mpeg1_44100_mono_cbr64_no_info", "patched_mpeg1_48000_dual_channel", "patched_mpeg2_24000_free_format"}) {
        const Mp3RefStream* r = refNamed(name);
        CHECK(r != nullptr);
        if (!r) continue;
        const auto frames = readFrames(r->data, r->size);
        const auto s = readMp3Memory(r->data, r->size);
        CHECK_MSG(s && s->frames() == (long long)frames.size() * frames[0].samples - 529, "%s: %lld frames from %zu MPEG frames", name, s ? (long long)s->frames() : -1LL, frames.size());
    }
    // An Info stream without its Info frame is the same audio, with the encoder's delay in front and its padding behind.
    const Mp3RefStream* r = refNamed("lame_mpeg1_44100_joint_cbr64");
    CHECK(r != nullptr);
    if (!r) return;
    const auto frames = readFrames(r->data, r->size);
    CHECK(frames.size() > 2 && isInfoFrame(r->data, frames[0]));
    if (frames.size() < 3) return;
    const unsigned char* tag = r->data + frames[0].pos + 4 + frames[0].sideBytes + 120;        // the LAME extension
    const int delay = (tag[21] << 4) | (tag[22] >> 4), padding = ((tag[22] & 15) << 8) | tag[23];
    const long long audioFrames = (long long)frames.size() - 1;
    const auto whole = readMp3Memory(r->data, r->size);
    CHECK(whole != nullptr);
    if (!whole) return;
    CHECK_MSG(whole->frames() == audioFrames * 1152 - delay - padding, "%lld frames; %lld MPEG frames, delay %d, padding %d", (long long)whole->frames(), audioFrames, delay, padding);
    const size_t from = frames[1].pos;
    const auto bare = readMp3Memory(r->data + from, r->size - from);
    CHECK(bare != nullptr);
    if (!bare) return;
    CHECK(bare->frames() == audioFrames * 1152 - 529);
    double worst = 0.0;
    for (int c = 0; c < 2; ++c)
        for (size_t i = 0; i < whole->ch[c].size() && i + (size_t)delay < bare->ch[c].size(); ++i)
            worst = std::max(worst, (double)std::fabs(whole->ch[c][i] - bare->ch[c][i + (size_t)delay]));
    CHECK_MSG(worst == 0.0, "the audio moved or changed: %.3g", worst);
}

// Tags and other bytes around the audio are stepped over.
TEST(mp3_tags_and_junk_around_the_audio_are_skipped) {
    const Mp3RefStream* r = refNamed("lame_mpeg1_44100_mono_cbr64_no_info");
    CHECK(r != nullptr);
    if (!r) return;
    const std::vector<unsigned char> plain(r->data, r->data + r->size);
    const auto want = readMp3Memory(plain.data(), plain.size());
    CHECK(want != nullptr);
    if (!want) return;
    auto same = [&](const std::vector<unsigned char>& bytes, const char* what) {
        std::string error;
        const auto s = readMp3Memory(bytes.data(), bytes.size(), &error);
        CHECK_MSG(s != nullptr, "%s: %s", what, error.c_str());
        if (s) CHECK_MSG(identical(*s, *want), "%s: decoded differently (%lld frames, expected %lld)", what, (long long)s->frames(), (long long)want->frames());
        CHECK_MSG(looksLikeMp3(bytes.data(), bytes.size()), "%s: does not look like MP3", what);
    };
    // an ID3v2 tag whose contents look like frame headers
    auto id3 = [](size_t body, bool footer) {
        std::vector<unsigned char> t = {'I', 'D', '3', 4, 0, (unsigned char)(footer ? 0x10 : 0), (unsigned char)((body >> 21) & 127), (unsigned char)((body >> 14) & 127),
                                         (unsigned char)((body >> 7) & 127), (unsigned char)(body & 127)};
        for (size_t i = 0; i < body; ++i) t.push_back((unsigned char)((i % 4 == 0) ? 0xFF : (i % 4 == 1 ? 0xFB : (i % 4 == 2 ? 0x90 : 0x64))));
        if (footer) t.insert(t.end(), {'3', 'D', 'I', 4, 0, 0x10, 0, 0, 0, 0});
        return t;
    };
    for (bool footer : {false, true})
        for (size_t body : {(size_t)0, (size_t)1, (size_t)300, (size_t)5000}) {
            auto b = id3(body, footer);
            b.insert(b.end(), plain.begin(), plain.end());
            same(b, footer ? "ID3v2 tag with footer" : "ID3v2 tag");
        }
    {   // two tags in a row, and an ID3v1 tag behind
        auto b = id3(40, false);
        const auto second = id3(200, false);
        b.insert(b.end(), second.begin(), second.end());
        b.insert(b.end(), plain.begin(), plain.end());
        std::vector<unsigned char> v1(128, ' ');
        v1[0] = 'T'; v1[1] = 'A'; v1[2] = 'G';
        v1[60] = 0xFF; v1[61] = 0xFB; v1[62] = 0x50; v1[63] = 0xC4;        // a frame header inside the tag
        b.insert(b.end(), v1.begin(), v1.end());
        same(b, "two ID3v2 tags and an ID3v1 tag");
    }
    {   // zeros before the first frame, and after the last
        std::vector<unsigned char> b(700, 0);
        b.insert(b.end(), plain.begin(), plain.end());
        b.insert(b.end(), 300, 0);
        same(b, "zeros before and after");
    }
    {   // bytes between two frames that are not a frame
        const auto frames = readFrames(plain);
        CHECK(frames.size() > 5);
        if (frames.size() > 5) {
            std::vector<unsigned char> b(plain.begin(), plain.begin() + (std::ptrdiff_t)frames[4].pos);
            const char* junk = "APETAGEX and some other bytes that do not belong here";
            b.insert(b.end(), junk, junk + std::strlen(junk));
            b.insert(b.end(), plain.begin() + (std::ptrdiff_t)frames[4].pos, plain.end());
            same(b, "junk between frames");
        }
    }
    // an ID3v2 tag that claims more than the file holds: nothing to decode, and no crash
    {
        auto b = id3(100000, false);
        b.resize(60);
        std::string error;
        CHECK(readMp3Memory(b.data(), b.size(), &error) == nullptr);
        CHECK(!error.empty());
    }
}

TEST(mp3_decoder_gives_the_same_result_every_time_and_from_a_file) {
    const Mp3RefStream* r = refNamed("lame_mpeg1_44100_joint_vbr");
    CHECK(r != nullptr);
    if (!r) return;
    const auto a = readMp3Memory(r->data, r->size), b = readMp3Memory(r->data, r->size);
    CHECK(a && b && identical(*a, *b));
    const std::string path = t::tmpDir() + "/mp3_reference.mp3";
    CHECK(writeFileBytes(path, r->data, r->size));
    std::string error;
    const auto c = readMp3File(path, &error);
    CHECK_MSG(c != nullptr, "%s", error.c_str());
    if (a && c) {
        CHECK(identical(*a, *c));
        CHECK(c->path == path);
    }
    CHECK(readMp3File(t::tmpDir() + "/no_such_file.mp3", &error) == nullptr);
    CHECK(error.find("cannot open") != std::string::npos);
}

TEST(mp3_is_recognised_by_its_contents) {
    for (const auto& r : kMp3RefStreams) CHECK_MSG(looksLikeMp3(r.data, r.size), "%s does not look like MP3", r.name);
    const auto wav = encodeWav(*signal(Sig::Noise, 2, 20000), WavFormat::Pcm16);
    CHECK(!looksLikeMp3(wav.data(), wav.size()));
    const auto wavFloat = encodeWav(*signal(Sig::Music, 1, 20000), WavFormat::Float32);
    CHECK(!looksLikeMp3(wavFloat.data(), wavFloat.size()));
    CHECK(!looksLikeMp3(nullptr, 0));
    CHECK(!looksLikeMp3("", 0));
    CHECK(!looksLikeMp3("ID", 2));
    const unsigned char flac[] = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(!looksLikeMp3(flac, sizeof flac));
    const unsigned char aiff[] = {'F', 'O', 'R', 'M', 0, 0, 0, 30, 'A', 'I', 'F', 'F', 'C', 'O', 'M', 'M'};
    CHECK(!looksLikeMp3(aiff, sizeof aiff));
    // an ID3v2 tag says MP3, unless a FLAC stream follows it
    std::vector<unsigned char> tagged = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 4, 1, 2, 3, 4};
    CHECK(looksLikeMp3(tagged.data(), tagged.size()));
    tagged.insert(tagged.end(), flac, flac + sizeof flac);
    CHECK(!looksLikeMp3(tagged.data(), tagged.size()));
    // random bytes and text
    Rand rng(99);
    int hits = 0;
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<unsigned char> b((size_t)(100 + rng.next() % 6000));
        for (auto& v : b) v = (unsigned char)rng.next();
        if (looksLikeMp3(b.data(), b.size())) ++hits;
    }
    CHECK_MSG(hits == 0, "%d of 200 buffers of random bytes looked like MP3", hits);
    const std::string text(5000, 'a');
    CHECK(!looksLikeMp3(text.data(), text.size()));
    std::vector<unsigned char> ff(5000, 0xFF);
    CHECK(!looksLikeMp3(ff.data(), ff.size()));
    // one frame header alone is not enough
    const unsigned char one[] = {0xFF, 0xFB, 0x90, 0x64, 0, 0, 0, 0, 0, 0, 0, 0};
    std::vector<unsigned char> lone(3000, 0x55);
    std::memcpy(lone.data() + 100, one, sizeof one);
    CHECK(!looksLikeMp3(lone.data(), lone.size()));
}
