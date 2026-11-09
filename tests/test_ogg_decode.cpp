// Ogg Vorbis decoding: reference streams from other encoders (embedded in test_ogg_streams.cpp) against
// what libvorbis decodes them to, and damaged, cut and nonsense input.
#include "Util.h"
#include "pulp/Ogg.h"
#include <filesystem>
using namespace pulp;
using namespace tu;

// (the same declaration as in test_ogg_streams.cpp)
struct OggRef {
    const char* what;
    const unsigned char* data;
    size_t size;
    int rate, channels, fileChannels;       // channels as decoded: at most two of the file's
    long long frames;
    float peak;
    float print[2][8];
    struct { long long frame; int channel; float value; } spot[3];
};
extern const OggRef kOggRefs[];
extern const size_t kOggRefCount;

namespace {

// A small deterministic generator, so every run tests the same damage.
struct Rand {
    uint32 s;
    explicit Rand(uint32 seed) : s(seed) {}
    uint32 next() { s = s * 1664525u + 1013904223u; return s >> 8; }
};

// What the decoder must reproduce is stored as eight sums of each decoded channel, every sample added or
// subtracted according to a fixed pattern, divided by the square root of the length. A wrong sample
// moves all eight; rounding differences between decoders move them by parts in ten million.
double projection(const std::vector<float>& x, int j) {
    double sum = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        uint32 h = (uint32)((uint64)i * 2654435761ull + (uint64)j * 0x9E3779B9ull);
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        sum += (h & 1u) ? (double)x[i] : -(double)x[i];
    }
    return x.empty() ? 0.0 : sum / std::sqrt((double)x.size());
}

const OggRef* findRef(const char* name) {
    for (size_t r = 0; r < kOggRefCount; ++r) if (std::string(kOggRefs[r].what) == name) return &kOggRefs[r];
    return nullptr;
}
std::vector<unsigned char> bytesOf(const OggRef& ref) { return std::vector<unsigned char>(ref.data, ref.data + ref.size); }

// The tests' own reading of the Ogg layer, kept apart from the decoder's.
uint32 pageCrc(const unsigned char* p, size_t n) {
    uint32 c = 0;
    for (size_t i = 0; i < n; ++i) {
        c ^= (uint32)(i >= 22 && i < 26 ? 0 : p[i]) << 24;      // the checksum field counts as zeros
        for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? ((c << 1) ^ 0x04c11db7u) : (c << 1);
    }
    return c;
}
struct PageAt {
    size_t at, bytes, bodyAt;
    unsigned flags;
    int64 granule;
    uint32 serial, seq;
    bool endsPacket;        // some packet finishes on this page
};
std::vector<PageAt> pagesOf(const std::vector<unsigned char>& f) {
    std::vector<PageAt> pages;
    for (size_t pos = 0; pos + 27 <= f.size() && std::memcmp(f.data() + pos, "OggS", 4) == 0;) {
        const unsigned char* p = f.data() + pos;
        PageAt pg{};
        pg.at = pos;
        pg.flags = p[5];
        uint64 g = 0;
        for (int i = 7; i >= 0; --i) g = (g << 8) | p[6 + i];
        pg.granule = (int64)g;
        pg.serial = (uint32)p[14] | ((uint32)p[15] << 8) | ((uint32)p[16] << 16) | ((uint32)p[17] << 24);
        pg.seq = (uint32)p[18] | ((uint32)p[19] << 8) | ((uint32)p[20] << 16) | ((uint32)p[21] << 24);
        const size_t segments = p[26];
        if (pos + 27 + segments > f.size()) break;
        size_t body = 0;
        for (size_t i = 0; i < segments; ++i) { body += p[27 + i]; pg.endsPacket = pg.endsPacket || p[27 + i] < 255; }
        pg.bodyAt = pos + 27 + segments;
        pg.bytes = 27 + segments + body;
        if (pos + pg.bytes > f.size()) break;
        pages.push_back(pg);
        pos += pg.bytes;
    }
    return pages;
}
void fixCrc(std::vector<unsigned char>& f, const PageAt& pg) {
    const uint32 c = pageCrc(f.data() + pg.at, pg.bytes);
    for (int i = 0; i < 4; ++i) f[pg.at + 22 + (size_t)i] = (unsigned char)(c >> (8 * i));
}

// True if `part` is the first part.frames() frames of `whole`, sample for sample.
bool isPrefix(const SampleData& part, const SampleData& whole) {
    if (part.channels != whole.channels || part.frames() > whole.frames()) return false;
    for (int c = 0; c < part.channels; ++c) {
        if (part.ch[c].size() != part.ch[0].size()) return false;
        if (!std::equal(part.ch[c].begin(), part.ch[c].end(), whole.ch[c].begin())) return false;
    }
    return true;
}
bool allFinite(const SampleData& s) {
    for (int c = 0; c < s.channels; ++c)
        for (float v : s.ch[c]) if (!std::isfinite(v)) return false;
    return true;
}

// The streams the damage tests work on: several pages each, one of them with packets that run across pages.
const char* const kDamageStreams[] = {"pages_of_two_segments", "libvorbis_silence_inside", "hand_extreme_blocks", "hand_residue0"};

} // namespace

TEST(ogg_decodes_reference_streams_to_what_libvorbis_gives) {
    // Tolerance: every projection and every spot sample within 1e-5 of the stream's peak (-100 dB) of what
    // libvorbis decoded. (Measured when the streams were made: -120 dB or better.) Lengths must be exact.
    CHECK(kOggRefCount >= 20);
    for (size_t r = 0; r < kOggRefCount; ++r) {
        const OggRef& ref = kOggRefs[r];
        std::string error;
        const auto s = readOggMemory(ref.data, ref.size, &error);
        CHECK_MSG(s != nullptr, "%s: %s", ref.what, error.c_str());
        if (!s) continue;
        CHECK_MSG(error.empty(), "%s: error text '%s' with a result", ref.what, error.c_str());
        const bool shape = s->sampleRate == ref.rate && s->channels == ref.channels && s->frames() == ref.frames && (s->channels == 1 || s->ch[1].size() == s->ch[0].size());
        CHECK_MSG(shape, "%s: %g Hz, %d channels, %lld frames; expected %d Hz, %d, %lld", ref.what, s->sampleRate, s->channels, (long long)s->frames(), ref.rate, ref.channels, ref.frames);
        if (!shape) continue;
        const double tol = 1.0e-5 * (double)ref.peak;
        for (int c = 0; c < ref.channels; ++c)
            for (int j = 0; j < 8; ++j) {
                const double got = projection(s->ch[c], j);
                CHECK_MSG(std::fabs(got - (double)ref.print[c][j]) <= tol, "%s: channel %d projection %d is %.9g, expected %.9g (tolerance %.3g)", ref.what, c, j, got, (double)ref.print[c][j], tol);
            }
        for (const auto& spot : ref.spot) {
            if (ref.frames == 0) continue;
            const double got = s->ch[spot.channel][(size_t)spot.frame];
            CHECK_MSG(std::fabs(got - (double)spot.value) <= tol, "%s: frame %lld channel %d is %.9g, expected %.9g", ref.what, spot.frame, spot.channel, got, (double)spot.value);
        }
        CHECK_MSG(s->peak() <= ref.peak * 1.0001f + 1.0e-12f, "%s: peak %g, expected %g", ref.what, s->peak(), ref.peak);
    }
}

TEST(ogg_reference_streams_cover_what_they_claim) {
    // A check on the test data itself: the streams named for a feature really have it.
    auto channelsInFile = [](const OggRef& ref) { return (int)ref.data[28 + 11]; };       // the identification packet starts after a 28-byte page header
    for (size_t r = 0; r < kOggRefCount; ++r) {
        const OggRef& ref = kOggRefs[r];
        CHECK_MSG(ref.size > 60 && std::memcmp(ref.data, "OggS", 4) == 0, "%s", ref.what);
        const std::string name = ref.what;
        if (name != "multiplexed") CHECK_MSG(channelsInFile(ref) == ref.fileChannels, "%s: %d channels in the file", ref.what, channelsInFile(ref));
    }
    const OggRef* six = findRef("libvorbis_six_channels");
    CHECK(six && six->fileChannels == 6 && six->channels == 2);
    const OggRef* many = findRef("pages_of_two_segments");
    CHECK(many != nullptr);
    if (many) {
        const auto pages = pagesOf(bytesOf(*many));
        int continued = 0;
        for (const auto& pg : pages) continued += (pg.flags & 1) ? 1 : 0;
        CHECK_MSG(pages.size() > 20 && continued >= 5, "%zu pages, %d continuing a packet", pages.size(), continued);
    }
    const OggRef* big = findRef("hand_extreme_blocks");
    CHECK(big && big->data[28 + 28] == 0xD6);       // block sizes 2^6 and 2^13
    const OggRef* mux = findRef("multiplexed");
    CHECK(mux != nullptr);
    if (mux) {
        const auto pages = pagesOf(bytesOf(*mux));
        std::vector<uint32> serials;
        for (const auto& pg : pages) if (std::find(serials.begin(), serials.end(), pg.serial) == serials.end()) serials.push_back(pg.serial);
        CHECK_MSG(serials.size() == 3, "%zu streams", serials.size());
    }
}

TEST(ogg_file_reader_sets_the_path) {
    const OggRef* ref = findRef("libvorbis_q0_mono_22k");
    CHECK(ref != nullptr);
    if (!ref) return;
    const std::string dir = t::tmpDir() + "/ogg";
    std::filesystem::create_directories(std::filesystem::path(std::u8string(dir.begin(), dir.end())));
    const std::string path = dir + "/r\xC3\xA9" "f\xC3\xA9rence \xE2\x99\xAB \xE9\x9F\xB3.ogg";      // "référence ♫ 音.ogg" in UTF-8
    CHECK(writeFileBytes(path, ref->data, ref->size));
    std::string error;
    const auto s = readOggFile(path, &error);
    CHECK_MSG(s != nullptr, "%s", error.c_str());
    const auto direct = readOggMemory(ref->data, ref->size);
    CHECK(s && direct && s->path == path && direct->path.empty() && identical(*s, *direct) && s->frames() == ref->frames);
    error.clear();
    CHECK(!readOggFile(dir + "/no such file.ogg", &error) && error.find("cannot open") == 0);
    CHECK(!readOggFile(dir + "/no such file.ogg"));
}

TEST(ogg_rejects_what_is_not_ogg_vorbis) {
    std::string error;
    CHECK(!readOggMemory(nullptr, 0, &error) && error == "not an Ogg file");
    error.clear();
    CHECK(!readOggMemory("", 0, &error) && error == "not an Ogg file");
    CHECK(!readOggMemory(nullptr, 100, &error));        // a null pointer with a size is still nothing
    const auto wav = encodeWav(*sine(440.0, 0.1, 44100.0), WavFormat::Pcm16);
    error.clear();
    CHECK(!readOggMemory(wav.data(), wav.size(), &error) && error == "not an Ogg file");
    const char text[] = "OggS is how an Ogg page starts, but this is a sentence. OggS OggS OggS";
    error.clear();
    CHECK(!readOggMemory(text, sizeof text, &error) && error == "not an Ogg file");
    CHECK(!readOggMemory(text, sizeof text));           // and without anywhere to put the reason

    const OggRef* ref = findRef("libvorbis_q0_mono_22k");
    CHECK(ref != nullptr);
    if (!ref) return;
    const auto good = bytesOf(*ref);
    const auto pages = pagesOf(good);
    CHECK(pages.size() >= 3);
    if (pages.size() < 3) return;
    // an Ogg file of something else
    auto opus = good;
    opus.resize(pages[1].at);
    std::memcpy(opus.data() + pages[0].bodyAt, "OpusHead", 8);
    fixCrc(opus, pages[0]);
    error.clear();
    CHECK(!readOggMemory(opus.data(), opus.size(), &error) && error == "Ogg Opus files cannot be read");
    std::memcpy(opus.data() + pages[0].bodyAt, "\x7F" "FLAC\x01\x00\x00", 8);
    fixCrc(opus, pages[0]);
    error.clear();
    CHECK(!readOggMemory(opus.data(), opus.size(), &error) && error == "no Vorbis stream in this Ogg file");
    // a Vorbis stream of a version that does not exist, and one with no channels
    for (size_t at : {(size_t)7, (size_t)11}) {
        auto bad = good;
        bad[pages[0].bodyAt + at] = at == 7 ? 1 : 0;
        fixCrc(bad, pages[0]);
        error.clear();
        CHECK(!readOggMemory(bad.data(), bad.size(), &error) && error == "bad Vorbis identification header");
    }
    // the headers alone, and the headers cut anywhere
    error.clear();
    CHECK(!readOggMemory(good.data(), pages[2].at, &error) && error == "Ogg Vorbis file is cut short");
    error.clear();
    CHECK(!readOggMemory(good.data(), pages[1].at, &error) && error == "Ogg Vorbis headers are cut short");
    int accepted = 0;
    for (size_t n = 0; n < pages[2].at; n += 7) accepted += readOggMemory(good.data(), n) ? 1 : 0;
    CHECK(accepted == 0);
    // a tag or other junk in front is skipped
    std::vector<unsigned char> tagged(300, (unsigned char)'x');
    std::memcpy(tagged.data(), "ID3\x04", 4);
    std::memcpy(tagged.data() + 100, "OggS", 4);        // and a false start inside it
    tagged.insert(tagged.end(), good.begin(), good.end());
    const auto s = readOggMemory(tagged.data(), tagged.size(), &error);
    CHECK(s && s->frames() == ref->frames);
}

TEST(ogg_page_with_a_bad_checksum_ends_the_audio_there) {
    // Every page is tried in turn. Damage in the pages that hold the headers gives an error; damage
    // later gives exactly the audio that the pages before it complete, the same samples as an
    // undamaged decode. Three kinds of damage: a flipped bit in the page's data, in its checksum,
    // and in its position field.
    int tried = 0;
    for (const char* name : kDamageStreams) {
        const OggRef* ref = findRef(name);
        CHECK_MSG(ref != nullptr, "%s", name);
        if (!ref) continue;
        const auto good = bytesOf(*ref);
        const auto clean = readOggMemory(good.data(), good.size());
        const auto pages = pagesOf(good);
        CHECK_MSG(clean && pages.size() >= 4, "%s: %zu pages", name, pages.size());
        if (!clean || pages.size() < 4) continue;
        int wrong = 0;
        std::string firstWrong;
        for (size_t k = 0; k < pages.size(); ++k) {
            // what the undamaged pages before this one complete
            int64 expect = -1;
            int packetsDone = 0;
            for (size_t j = 0; j < k; ++j) {
                const unsigned char* lacing = good.data() + pages[j].at + 27;
                for (size_t i = 0; i < good[pages[j].at + 26]; ++i) packetsDone += lacing[i] < 255 ? 1 : 0;
                if (packetsDone > 3 && pages[j].endsPacket && pages[j].granule >= 0) expect = pages[j].granule;
            }
            if (expect == 0) expect = -1;       // headers and a first block that completes nothing: an error
            for (int kind = 0; kind < 3; ++kind) {
                auto bad = good;
                const size_t at = kind == 0 ? (pages[k].bytes > pages[k].bodyAt - pages[k].at ? pages[k].bodyAt + (pages[k].bytes - (pages[k].bodyAt - pages[k].at)) / 2 : pages[k].at + 26)
                                : kind == 1 ? pages[k].at + 23 : pages[k].at + 7;
                bad[at] ^= 0x10;
                std::string error;
                const auto got = readOggMemory(bad.data(), bad.size(), &error);
                ++tried;
                bool ok;
                if (expect < 0) ok = !got && !error.empty();
                else ok = got && got->frames() == std::min<int64>(expect, clean->frames()) && isPrefix(*got, *clean) && got->sampleRate == clean->sampleRate;
                if (!ok && ++wrong == 1) {
                    char b[200];
                    snprintf(b, sizeof b, "page %zu of %zu, damage %d: %s, expected %lld frames", k, pages.size(), kind, got ? std::to_string(got->frames()).c_str() : ("error '" + error + "'").c_str(), (long long)expect);
                    firstWrong = b;
                }
            }
        }
        CHECK_MSG(wrong == 0, "%s: %d wrong, first: %s", name, wrong, firstWrong.c_str());
    }
    CHECK_MSG(tried > 120, "%d damaged files tried", tried);
    // the error names what happened when nothing could be decoded
    const OggRef* ref = findRef("libvorbis_q0_mono_22k");
    if (ref) {
        auto bad = bytesOf(*ref);
        const auto pages = pagesOf(bad);
        bad[pages[1].bodyAt + 40] ^= 1;
        std::string error;
        CHECK(!readOggMemory(bad.data(), bad.size(), &error) && error == "Ogg Vorbis headers are damaged");
        bad = bytesOf(*ref);
        bad[pages[2].bodyAt + 40] ^= 1;
        CHECK(!readOggMemory(bad.data(), bad.size(), &error) && error == "Ogg Vorbis file is damaged before any audio");
    }
}

TEST(ogg_missing_or_repeated_pages_end_the_audio_there) {
    const OggRef* ref = findRef("pages_of_two_segments");
    CHECK(ref != nullptr);
    if (!ref) return;
    const auto good = bytesOf(*ref);
    const auto clean = readOggMemory(good.data(), good.size());
    const auto pages = pagesOf(good);
    CHECK(clean && pages.size() > 20);
    if (!clean || pages.size() <= 20) return;
    int wrong = 0;
    for (size_t k = 13; k + 1 < pages.size(); ++k) {
        int64 expect = 0;
        for (size_t j = 0; j < k; ++j) if (pages[j].endsPacket && pages[j].granule > 0) expect = pages[j].granule;
        // page k left out
        std::vector<unsigned char> cut(good.begin(), good.begin() + (std::ptrdiff_t)pages[k].at);
        cut.insert(cut.end(), good.begin() + (std::ptrdiff_t)pages[k + 1].at, good.end());
        auto got = readOggMemory(cut.data(), cut.size());
        if (!(got && got->frames() == expect && isPrefix(*got, *clean))) ++wrong;
        // page k twice
        std::vector<unsigned char> twice(good.begin(), good.begin() + (std::ptrdiff_t)pages[k + 1].at);
        twice.insert(twice.end(), good.begin() + (std::ptrdiff_t)pages[k].at, good.end());
        int64 through = expect;
        if (pages[k].endsPacket && pages[k].granule > 0) through = pages[k].granule;
        got = readOggMemory(twice.data(), twice.size());
        if (!(got && got->frames() == through && isPrefix(*got, *clean))) ++wrong;
    }
    CHECK_MSG(wrong == 0, "%d wrong", wrong);
}

TEST(ogg_truncated_file_gives_the_audio_before_the_cut) {
    int cuts = 0;
    for (const char* name : kDamageStreams) {
        const OggRef* ref = findRef(name);
        if (!ref) continue;
        const auto good = bytesOf(*ref);
        const auto clean = readOggMemory(good.data(), good.size());
        const auto pages = pagesOf(good);
        if (!clean) continue;
        int wrong = 0;
        int64 before = -1;
        size_t page = 0;
        const size_t step = std::max<size_t>(1, good.size() / 400);
        for (size_t n = 0; n < good.size(); n += (n + 40 > good.size() ? 1 : step)) {
            // each cut copy lives in a buffer of exactly its size, so reading past the cut would be caught by the checked heap
            const std::vector<unsigned char> cut(good.begin(), good.begin() + (std::ptrdiff_t)n);
            std::string error;
            const auto got = readOggMemory(cut.data(), cut.size(), &error);
            ++cuts;
            // what the whole pages inside the cut complete
            while (page < pages.size() && pages[page].at + pages[page].bytes <= n) ++page;
            int64 expect = -1;
            int packetsDone = 0;
            for (size_t j = 0; j < page; ++j) {
                const unsigned char* lacing = good.data() + pages[j].at + 27;
                for (size_t i = 0; i < good[pages[j].at + 26]; ++i) packetsDone += lacing[i] < 255 ? 1 : 0;
                if (packetsDone > 3 && pages[j].endsPacket && pages[j].granule > 0) expect = pages[j].granule;
            }
            if (expect < 0) { if (got || error.empty()) ++wrong; }
            else if (!(got && got->frames() == std::min<int64>(expect, clean->frames()) && isPrefix(*got, *clean))) ++wrong;
            if (got) { if (got->frames() < before) ++wrong; before = got->frames(); }
        }
        CHECK_MSG(wrong == 0, "%s: %d cuts gave the wrong audio", name, wrong);
        // the whole file with junk after its last page still decodes in full
        auto tagged = good;
        tagged.insert(tagged.end(), {'T', 'A', 'G', 0, 'O', 'g', 'g', 'S', 0, 1, 2});
        const auto got = readOggMemory(tagged.data(), tagged.size());
        CHECK_MSG(got && identical(*got, *clean), "%s with junk after it", name);
    }
    CHECK(cuts > 1000);
}

TEST(ogg_corrupted_packets_with_good_checksums_never_crash) {
    // Bits are flipped inside pages and the checksums put right again, so the damage reaches the Vorbis
    // decoder itself: in the audio packets, and in the setup header that describes how to read them.
    // Nothing is promised about what comes back, only that nothing crashes or reads out of bounds (the
    // Debug run has the checked heap on), that a result is well formed, and that it holds no NaN or infinity.
    Rand rng(20241);
    int decoded = 0, refused = 0, malformed = 0;
    for (size_t r = 0; r < kOggRefCount; ++r) {
        const OggRef& ref = kOggRefs[r];
        const auto good = bytesOf(ref);
        const auto pages = pagesOf(good);
        if (pages.size() < 3) continue;
        for (int round = 0; round < 24; ++round) {
            auto bad = good;
            // a third of the rounds hit the setup header's pages, the rest anything after the first page
            const bool inSetup = round % 3 == 0;
            const int hits = 1 + (int)(rng.next() % 4);
            for (int h = 0; h < hits; ++h) {
                const PageAt& pg = inSetup ? pages[1] : pages[1 + rng.next() % (pages.size() - 1)];
                const size_t bodyBytes = pg.bytes - (pg.bodyAt - pg.at);
                if (!bodyBytes) continue;
                // skip the first bytes of the setup so that most damage lands past its signature
                const size_t at = pg.bodyAt + (inSetup && bodyBytes > 200 ? 100 + rng.next() % (bodyBytes - 100) : rng.next() % bodyBytes);
                if (round % 2) bad[at] = (unsigned char)rng.next();
                else bad[at] ^= (unsigned char)(1u << (rng.next() % 8));
                fixCrc(bad, pg);
            }
            const auto got = readOggMemory(bad.data(), bad.size());
            if (!got) { ++refused; continue; }
            ++decoded;
            const bool formed = got->channels >= 1 && got->channels <= 2 && (got->channels == 1 || got->ch[1].size() == got->ch[0].size()) && got->sampleRate >= 1.0 && allFinite(*got);
            if (!formed) ++malformed;
        }
    }
    CHECK_MSG(malformed == 0 && decoded > 100 && refused > 20, "%d decoded, %d refused, %d malformed", decoded, refused, malformed);
}

TEST(ogg_random_bytes_and_cut_packets_never_crash) {
    Rand rng(777);
    int decoded = 0;
    // noise, with and without the look of a page at the front
    for (int round = 0; round < 300; ++round) {
        std::vector<unsigned char> junk(1 + rng.next() % 2000);
        for (auto& b : junk) b = (unsigned char)rng.next();
        if (round % 3 && junk.size() > 30) {
            std::memcpy(junk.data(), "OggS", 4);
            junk[4] = 0;
            junk[5] = 2;
            if (round % 3 == 2) { PageAt pg{}; pg.at = 0; pg.bytes = std::min<size_t>(junk.size(), 27u + junk[26]); fixCrc(junk, pg); }
        }
        std::string error;
        const auto got = readOggMemory(junk.data(), junk.size(), &error);
        if (got) ++decoded;
        else CHECK(!error.empty());
    }
    CHECK_MSG(decoded == 0, "%d buffers of noise decoded", decoded);
    // real headers followed by pages of noise that pass their checksums: every packet is nonsense
    const OggRef* ref = findRef("libvorbis_q3_stereo_44k");
    CHECK(ref != nullptr);
    if (!ref) return;
    const auto good = bytesOf(*ref);
    const auto pages = pagesOf(good);
    CHECK(pages.size() >= 3);
    if (pages.size() < 3) return;
    int formed = 0;
    for (int round = 0; round < 40; ++round) {
        std::vector<unsigned char> f(good.begin(), good.begin() + (std::ptrdiff_t)pages[2].at);
        uint32 seq = pages[1].seq + 1;
        const int count = 1 + (int)(rng.next() % 4);
        for (int k = 0; k < count; ++k) {
            const size_t start = f.size();
            const int segments = 1 + (int)(rng.next() % 40);
            f.insert(f.end(), {'O', 'g', 'g', 'S', 0, (unsigned char)(k + 1 == count ? 4 : 0)});
            const uint64 granule = round % 4 == 0 ? rng.next() : (uint64)(k + 1) * 1000u;
            for (int i = 0; i < 8; ++i) f.push_back((unsigned char)(granule >> (8 * i)));
            for (int i = 0; i < 4; ++i) f.push_back((unsigned char)(pages[0].serial >> (8 * i)));
            for (int i = 0; i < 4; ++i) f.push_back((unsigned char)(seq >> (8 * i)));
            f.insert(f.end(), {0, 0, 0, 0, (unsigned char)segments});
            size_t body = 0;
            for (int i = 0; i < segments; ++i) { const unsigned char l = (unsigned char)(rng.next() % 3 ? rng.next() % 256 : 255); f.push_back(l); body += l; }
            for (size_t i = 0; i < body; ++i) f.push_back((unsigned char)(rng.next() & (i % 97 == 0 ? 0xFE : 0xFF)));
            PageAt pg{};
            pg.at = start;
            pg.bytes = f.size() - start;
            fixCrc(f, pg);
            ++seq;
        }
        const auto got = readOggMemory(f.data(), f.size());
        if (got && got->channels == 2 && got->ch[0].size() == got->ch[1].size() && allFinite(*got)) ++formed;
        else CHECK_MSG(!got, "round %d: a malformed result", round);
    }
    CHECK_MSG(formed > 0, "%d", formed);
}
