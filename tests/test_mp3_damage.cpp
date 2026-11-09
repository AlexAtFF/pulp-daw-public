// MP3 input that is not what it should be: not MP3 at all, cut short, or damaged. The decoder must
// never crash or read outside the data (the input is fenced in by barred pages, and the debug build
// runs on the checked heap), must refuse
// what is not MP3, and must give back the audio that is still there.
#include "Mp3TestUtil.h"
#include "Mp3TestStreams.h"
#include <chrono>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
using namespace pulp;
using namespace tu;
using namespace mp3t;

namespace {

#ifdef _DEBUG
constexpr int kTrials = 60;             // the debug build is many times slower: fewer random trials, same checks
constexpr int kCutStep = 23;
#else
constexpr int kTrials = 400;
constexpr int kCutStep = 3;
#endif

std::vector<unsigned char> encoded(Sig kind, int channels, size_t frames, int kbps, uint32 seed = 1) {
    return encodeMp3(*signal(kind, channels, frames, 44100.0, seed), kbps);
}

// Decodes a copy of the bytes that lies between two pages of memory that may not be touched: the copy
// ends exactly where the page behind it starts, and a page in front is barred too. A decoder that
// reads even one byte outside its input stops the test run on the spot.
struct Fenced {
    unsigned char* base = nullptr;
    unsigned char* data = nullptr;
    Fenced(const unsigned char* from, size_t size) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        const size_t page = info.dwPageSize, body = (size + page - 1) / page * page;
        base = (unsigned char*)VirtualAlloc(nullptr, body + 2 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!base) return;
        data = base + page + body - size;
        if (size) std::memcpy(data, from, size);
        DWORD old;
        VirtualProtect(base, page, PAGE_NOACCESS, &old);
        VirtualProtect(base + page + body, page, PAGE_NOACCESS, &old);
    }
    ~Fenced() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
};
// True if reading the byte at p is stopped by the system.
bool faults(const unsigned char* p) {
    __try {
        volatile unsigned char v = *p;
        (void)v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return false;
}
std::shared_ptr<SampleData> decodeExact(const unsigned char* data, size_t size, std::string* error = nullptr) {
    Fenced f(data, size);
    CHECK(f.base != nullptr);
    if (!f.base) return nullptr;
    (void)looksLikeMp3(f.data, size);
    return readMp3Memory(f.data, size, error);
}
std::shared_ptr<SampleData> decodeExact(const std::vector<unsigned char>& b, std::string* error = nullptr) { return decodeExact(b.data(), b.size(), error); }

// What a decode of damaged data may be: nothing (with a reason), or audio of finite samples.
bool sane(const std::shared_ptr<SampleData>& s, const std::string& error, long long maxFrames) {
    if (!s) return !error.empty();
    if (s->channels < 1 || s->channels > 2 || s->sampleRate < 8000.0 || s->sampleRate > 48000.0) return false;
    if (s->frames() > maxFrames) return false;
    if (s->channels == 2 && s->ch[1].size() != s->ch[0].size()) return false;
    return allFinite(*s);
}

} // namespace

// The fence the other tests in this file rely on does what it says.
TEST(mp3_test_fence_stops_reads_outside_the_input) {
    const unsigned char bytes[5] = {1, 2, 3, 4, 5};
    for (size_t size : {(size_t)0, (size_t)1, (size_t)5}) {
        Fenced f(bytes, size);
        CHECK(f.base != nullptr);
        if (!f.base) continue;
        CHECK(faults(f.data + size));           // the first byte behind the data
        CHECK(faults(f.base + 100));            // the page in front
        if (size) CHECK(!faults(f.data) && !faults(f.data + size - 1) && f.data[size - 1] == bytes[size - 1]);
    }
}

TEST(mp3_rejects_what_is_not_mp3) {
    std::string error;
    auto refuse = [&](const void* data, size_t size, const char* what) {
        error.clear();
        const auto s = readMp3Memory(data, size, &error);
        CHECK_MSG(s == nullptr, "%s was decoded to %lld frames", what, s ? (long long)s->frames() : 0LL);
        CHECK_MSG(error.find("not an MP3") != std::string::npos, "%s: error is \"%s\"", what, error.c_str());
        CHECK(readMp3Memory(data, size) == nullptr);            // and without anywhere to put the error
    };
    refuse(nullptr, 0, "nothing");
    refuse("", 0, "an empty buffer");
    refuse("\xFF", 1, "one byte");
    refuse("\xFF\xFB\x90", 3, "three bytes");
    refuse("\xFF\xFB\x90\x64", 4, "a header and nothing else");
    const auto wav = encodeWav(*signal(Sig::Music, 2, 30000), WavFormat::Pcm16);
    refuse(wav.data(), wav.size(), "a WAV file");
    const auto wav24 = encodeWav(*signal(Sig::Noise, 1, 30000), WavFormat::Pcm24);
    refuse(wav24.data(), wav24.size(), "a 24-bit WAV file of noise");
    const std::string text = "This is not audio. It is a sentence, repeated. ";
    std::string longText;
    for (int i = 0; i < 300; ++i) longText += text;
    refuse(longText.data(), longText.size(), "text");
    std::vector<unsigned char> zeros(20000, 0), ones(20000, 0xFF);
    refuse(zeros.data(), zeros.size(), "zeros");
    refuse(ones.data(), ones.size(), "bytes of 0xFF");
    // Layer I and Layer II streams, and the reserved version
    for (unsigned char second : {(unsigned char)0xFD, (unsigned char)0xFF, (unsigned char)0xEB}) {
        std::vector<unsigned char> b;
        for (int f = 0; f < 20; ++f) {
            b.insert(b.end(), {0xFF, second, 0x90, 0x64});
            b.insert(b.end(), 413, 0);
        }
        refuse(b.data(), b.size(), "a stream that is not Layer III");
    }
    // random bytes: with three headers in a row required, noise does not pass for MP3
    Rand rng(7);
    int decoded = 0;
    for (int trial = 0; trial < kTrials; ++trial) {
        std::vector<unsigned char> b((size_t)(1 + rng.next() % 9000));
        for (auto& v : b) v = (unsigned char)rng.next();
        error.clear();
        const auto s = decodeExact(b, &error);
        CHECK(sane(s, error, 4000000));
        if (s) ++decoded;
    }
    CHECK_MSG(decoded == 0, "%d of %d buffers of random bytes were decoded", decoded, kTrials);
    // Free format headers (which do not say how long their frames are) scattered through other bytes.
    // Where a few of them happen to sit at even distances that is a stream, as far as anyone can tell;
    // whatever comes out must be sane, and finding out must not take long.
    const auto t0 = std::chrono::steady_clock::now();
    for (int spacing : {41, 97, 400}) {
        std::vector<unsigned char> b(300000);
        for (auto& v : b) v = (unsigned char)(rng.next() % 200);            // no 0xFF by chance
        for (size_t i = 0; i + 4 < b.size(); i += (size_t)spacing + rng.next() % 300) {
            b[i] = 0xFF; b[i + 1] = 0xFB; b[i + 2] = (unsigned char)(rng.next() & 0x02); b[i + 3] = (unsigned char)((rng.next() & 0xC0) | 0x04);
        }
        error.clear();
        const auto s = decodeExact(b, &error);
        CHECK(sane(s, error, 4000000));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK_MSG(seconds < 10.0, "%.1f s to look through 900 KB of scattered free format headers", seconds);
    // an Info frame alone is an MP3 file with no audio in it
    const auto bytes = encoded(Sig::Sine, 1, 3000, 128);
    const auto frames = readFrames(bytes);
    CHECK(frames.size() > 2);
    const auto empty = readMp3Memory(bytes.data(), (size_t)frames[0].bytes, &error);
    CHECK(empty && empty->frames() == 0 && empty->channels == 1);
}

// A file that stops early gives the frames that are whole, exactly as the whole file gives them.
TEST(mp3_truncated_file_gives_the_whole_frames_before_the_cut) {
    for (int channels = 1; channels <= 2; ++channels) {
        const auto bytes = encoded(Sig::Music, channels, 9000, channels == 1 ? 96 : 160);
        const auto frames = readFrames(bytes);
        const auto whole = readMp3Memory(bytes.data(), bytes.size());
        CHECK(whole && whole->frames() == 9000);
        if (!whole || frames.size() < 5) continue;
        for (size_t cut = 0; cut < bytes.size(); cut += (cut < 700 ? 1 : (size_t)kCutStep)) {
            std::string error;
            const auto s = decodeExact(bytes.data(), cut, &error);
            // how many audio frames are whole before the cut (frame 0 is the Info frame)
            size_t have = 0;
            for (size_t i = 1; i < frames.size(); ++i) if (frames[i].pos + (size_t)frames[i].bytes <= cut) ++have;
            if (cut < (size_t)frames[0].bytes) {
                CHECK_MSG(s == nullptr && !error.empty(), "cut at %zu, inside the first frame, gave audio", cut);
                continue;
            }
            CHECK_MSG(s != nullptr, "cut at %zu: %s", cut, error.c_str());
            if (!s) continue;
            // the Info frame's padding no longer applies: everything decoded is given, less the delays in front
            const long long want = std::max<long long>(0, (long long)have * 1152 - 528 - 529);
            CHECK_MSG(s->frames() == want && s->channels == channels, "cut at %zu with %zu whole frames: %lld frames, expected %lld", cut, have, (long long)s->frames(), want);
            bool same = true;
            for (int c = 0; c < channels && same; ++c)
                for (size_t i = 0; i < s->ch[c].size() && i < whole->ch[c].size(); ++i)
                    if (s->ch[c][i] != whole->ch[c][i]) { same = false; break; }
            CHECK_MSG(same, "cut at %zu: the frames before the cut decoded differently", cut);
        }
    }
    // the same for streams from other encoders, of every kind: any prefix decodes or is refused, without harm
    for (const auto& r : kMp3RefStreams) {
        const auto whole = readMp3Memory(r.data, r.size);
        if (!whole) continue;
        for (size_t cut = 0; cut <= r.size; cut += (size_t)kCutStep * 3 + 1) {
            std::string error;
            const auto s = decodeExact(r.data, cut, &error);
            CHECK_MSG(sane(s, error, whole->frames() + 4608), "%s cut at %zu", r.name, cut);
            if (s) CHECK_MSG(s->frames() <= whole->frames() + 4096, "%s cut at %zu: %lld frames from part of a file of %lld", r.name, cut, (long long)s->frames(), (long long)whole->frames());
        }
    }
}

// Damage inside frames cannot be seen in MP3 (there are no checksums to speak of), so it decodes to
// something; but only the frames that were hit, and those that lean on them, may change.
TEST(mp3_damage_inside_frames_stays_where_it_is) {
    const auto bytes = encoded(Sig::Music, 2, 40000, 128);
    const auto frames = readFrames(bytes);
    const auto clean = readMp3Memory(bytes.data(), bytes.size());
    CHECK(clean && frames.size() > 20);
    if (!clean || frames.size() <= 20) return;
    Rand rng(21);
    for (int trial = 0; trial < kTrials / 4; ++trial) {
        auto b = bytes;
        // spoil some bytes of the main data of one frame in the middle (not its header or side information)
        const size_t victim = 8 + rng.next() % 6;
        const FrameInfo& f = frames[victim];
        const int count = 1 + (int)(rng.next() % 40);
        for (int k = 0; k < count; ++k) b[f.pos + 4 + (size_t)f.sideBytes + rng.next() % (size_t)(f.bytes - 4 - f.sideBytes)] = (unsigned char)rng.next();
        std::string error;
        const auto s = decodeExact(b, &error);
        CHECK_MSG(s != nullptr, "%s", error.c_str());
        if (!s) continue;
        CHECK(s->frames() == clean->frames() && allFinite(*s));
        // Bytes in frame `victim` belong to that frame or, through the reservoir, to the next ones; the
        // filters carry a frame's sound one granule on. Audio frame k covers samples from (k - 1) * 1152 - 1057.
        const size_t before = (victim - 1) * 1152 - 1057, after = (victim + 3) * 1152;
        bool same = true;
        for (int c = 0; c < 2; ++c) {
            for (size_t i = 0; i < before; ++i) if (s->ch[c][i] != clean->ch[c][i]) same = false;
            for (size_t i = after; i < s->ch[c].size(); ++i) if (s->ch[c][i] != clean->ch[c][i]) same = false;
        }
        CHECK_MSG(same, "trial %d: damage in frame %zu changed audio outside frames %zu to %zu", trial, victim, victim, victim + 2);
    }
}

// A frame whose header is destroyed is lost; the decoder finds the next frames and carries on.
TEST(mp3_decoder_picks_the_stream_up_again_after_a_destroyed_header) {
    const auto bytes = encoded(Sig::Sine, 1, 40000, 128);
    const auto frames = readFrames(bytes);
    const auto clean = readMp3Memory(bytes.data(), bytes.size());
    CHECK(clean && frames.size() > 20);
    if (!clean || frames.size() <= 20) return;
    {
        auto b = bytes;
        const FrameInfo& f = frames[10];
        std::memset(b.data() + f.pos, 0, 4);
        const auto s = decodeExact(b);
        CHECK(s != nullptr);
        if (s) {
            // one frame fewer; the Info frame's count no longer matches, so nothing is trimmed from the end
            CHECK_MSG(s->frames() == (long long)(frames.size() - 2) * 1152 - 528 - 529, "%lld frames", (long long)s->frames());
            CHECK(allFinite(*s));
            // before the lost frame the audio is untouched, and after it the tone is back (one frame earlier)
            bool same = true;
            for (size_t i = 0; i < 8 * 1152 - 1057; ++i) if (s->ch[0][i] != clean->ch[0][i]) same = false;
            CHECK(same);
            const size_t from = 14 * 1152, to = 20 * 1152;
            CHECK_MSG(std::fabs(rmsOf(s->ch[0], from, to) - 0.5 / std::sqrt(2.0)) < 0.02, "level after the gap %g", rmsOf(s->ch[0], from, to));
            CHECK_MSG(std::fabs(measureHz(s->ch[0], from, to, 44100.0) - 440.0) < 1.0, "pitch after the gap %g", measureHz(s->ch[0], from, to, 44100.0));
        }
    }
    {   // a stretch of the file replaced by noise: the rest is still decoded
        auto b = bytes;
        Rand rng(3);
        for (size_t i = frames[6].pos + 7; i < frames[12].pos + 20; ++i) b[i] = (unsigned char)rng.next();
        std::string error;
        const auto s = decodeExact(b, &error);
        CHECK_MSG(s != nullptr, "%s", error.c_str());
        if (s) {
            CHECK(allFinite(*s) && s->frames() > 20 * 1152 && s->frames() <= clean->frames() + 1152);
            const size_t n = (size_t)s->frames();
            CHECK_MSG(std::fabs(rmsOf(s->ch[0], n - 12000, n - 4000) - 0.5 / std::sqrt(2.0)) < 0.02, "level near the end %g", rmsOf(s->ch[0], n - 12000, n - 4000));
        }
    }
    {   // everything after the first frames destroyed: what was decoded before is returned
        auto b = bytes;
        std::memset(b.data() + frames[8].pos, 0x55, b.size() - frames[8].pos);
        const auto s = decodeExact(b);
        CHECK(s != nullptr);
        if (s) {
            CHECK_MSG(s->frames() == 7 * 1152 - 528 - 529, "%lld frames", (long long)s->frames());
            bool same = true;
            for (size_t i = 0; i < 5 * 1152; ++i) if (s->ch[0][i] != clean->ch[0][i]) same = false;
            CHECK(same);
        }
    }
}

// Anything may be wrong anywhere: bits flipped, bytes overwritten, pieces cut out or doubled, in
// streams of every kind. Whatever comes back is either nothing or finite audio of believable size.
TEST(mp3_decoder_survives_random_damage_without_reading_out_of_bounds) {
    Rand rng(1234);
    int decoded = 0, refused = 0, streams = 0;
    for (const auto& r : kMp3RefStreams) {
        ++streams;
        const std::vector<unsigned char> good(r.data, r.data + r.size);
        const auto whole = readMp3Memory(good.data(), good.size());
        CHECK(whole != nullptr);
        const long long limit = (whole ? whole->frames() : 0) * 2 + 20000;
        for (int trial = 0; trial < kTrials / 8; ++trial) {
            auto b = good;
            const int kind = (int)(rng.next() % 6);
            const size_t at = rng.next() % b.size();
            if (kind == 0) {                                    // flip a few bits anywhere
                for (int k = 0; k < 1 + (int)(rng.next() % 8); ++k) b[rng.next() % b.size()] ^= (unsigned char)(1u << (rng.next() % 8));
            } else if (kind == 1) {                             // overwrite a run with noise
                for (size_t i = at; i < b.size() && i < at + 1 + rng.next() % 300; ++i) b[i] = (unsigned char)rng.next();
            } else if (kind == 2) {                             // cut a piece out
                b.erase(b.begin() + (std::ptrdiff_t)at, b.begin() + (std::ptrdiff_t)std::min(b.size(), at + 1 + rng.next() % 500));
            } else if (kind == 3) {                             // double a piece
                const size_t len = std::min(b.size() - at, (size_t)(1 + rng.next() % 500));
                const std::vector<unsigned char> piece(b.begin() + (std::ptrdiff_t)at, b.begin() + (std::ptrdiff_t)(at + len));
                b.insert(b.begin() + (std::ptrdiff_t)at, piece.begin(), piece.end());
            } else if (kind == 4) {                             // spoil the side information of some frames
                for (const auto& f : readFrames(good))
                    if (rng.next() % 3 == 0) for (int k = 0; k < 3; ++k) b[f.pos + 4 + rng.next() % (size_t)f.sideBytes] = (unsigned char)rng.next();
            } else {                                            // spoil header bytes of some frames
                for (const auto& f : readFrames(good))
                    if (rng.next() % 4 == 0) b[f.pos + 1 + rng.next() % 3] ^= (unsigned char)(1u << (rng.next() % 8));
            }
            if (b.empty()) continue;
            std::string error;
            const auto s = decodeExact(b, &error);
            CHECK_MSG(sane(s, error, limit), "%s, damage of kind %d at %zu: %s", r.name, kind, at, s ? "a result that makes no sense" : "refused without a reason");
            if (s) ++decoded; else ++refused;
        }
    }
    CHECK(streams >= 20);
    CHECK_MSG(decoded > refused, "%d damaged streams decoded, %d refused", decoded, refused);
}

// The Info frame may itself say things that cannot be.
TEST(mp3_info_frame_with_impossible_values_is_not_believed_blindly) {
    const auto bytes = encoded(Sig::Sine, 2, 10000, 128);
    const auto frames = readFrames(bytes);
    CHECK(frames.size() > 5);
    if (frames.size() <= 5) return;
    const size_t tag = 4 + (size_t)frames[0].sideBytes;
    const size_t ext = tag + 120;
    const long long audio = (long long)frames.size() - 1;
    {   // more delay and padding than there is audio
        auto b = bytes;
        b[ext + 21] = 0xFF; b[ext + 22] = 0xFF; b[ext + 23] = 0xFF;
        const auto s = decodeExact(b);
        CHECK(s != nullptr);
        if (s) CHECK_MSG(s->frames() == std::max<long long>(0, audio * 1152 - 4095 - 4095), "%lld frames", (long long)s->frames());
    }
    {   // a delay longer than everything a short file decodes to
        auto b = encoded(Sig::Sine, 2, 700, 128);
        const auto f = readFrames(b);
        CHECK(f.size() == 3);
        const size_t e = 4 + (size_t)f[0].sideBytes + 120;
        b[e + 21] = 0xFF; b[e + 22] |= 0xF0;
        const auto s = decodeExact(b);
        CHECK(s && s->frames() == 0 && s->channels == 2);
        // and one that leaves only the last few samples, which are still inside the decoder's filters
        b[e + 21] = 0x90; b[e + 22] = 0x00; b[e + 23] = 0x00;                // delay 2304, no padding
        const auto t = decodeExact(b);
        CHECK(t != nullptr);
        if (t) CHECK_MSG(t->frames() == 0 && allFinite(*t), "%lld frames", (long long)t->frames());
        b[e + 21] = 0x70; b[e + 22] = 0x00;                                   // delay 1792, no padding: 512 samples are left
        const auto u = decodeExact(b);
        CHECK(u != nullptr);
        if (u) CHECK_MSG(u->frames() == 512 && allFinite(*u), "%lld frames", (long long)u->frames());
    }
    {   // no padding at all: the end of the audio is still inside the decoder, which has to bring it out
        auto b = bytes;
        b[ext + 22] &= 0xF0; b[ext + 23] = 0;
        const auto s = decodeExact(b);
        CHECK(s != nullptr);
        if (s) CHECK_MSG(s->frames() == audio * 1152 - 528 && allFinite(*s), "%lld frames", (long long)s->frames());
    }
    {   // a frame count far beyond the file: taken as a file cut short
        auto b = bytes;
        b[tag + 8] = 0x7F;
        const auto s = decodeExact(b);
        CHECK(s != nullptr);
        if (s) CHECK_MSG(s->frames() == audio * 1152 - 528 - 529, "%lld frames", (long long)s->frames());
    }
    {   // flags that promise fields the frame has no room for
        auto b = bytes;
        b[tag + 4] = b[tag + 5] = b[tag + 6] = b[tag + 7] = 0xFF;
        std::string error;
        const auto s = decodeExact(b, &error);
        CHECK(sane(s, error, 100000));
    }
    {   // an Info frame that claims a checksum: LAME leaves the tag where it is; an encoder may also move it two bytes on
        auto b = bytes;
        b[1] = 0xFA;
        const auto s = decodeExact(b);
        CHECK(s && s->frames() == 10000);
        b.insert(b.begin() + 4, 2, (unsigned char)0);           // two bytes of checksum after the header ...
        b.erase(b.begin() + frames[0].bytes, b.begin() + frames[0].bytes + 2);         // ... taken from the end of the frame's empty space
        const auto t = decodeExact(b);
        CHECK(t && t->frames() == 10000);
        if (s && t) CHECK(identical(*s, *t));
    }
    {   // the extension's name removed: no delays are read, only the decoder's own 529 samples go
        auto b = bytes;
        std::memset(b.data() + ext, 0, 9);
        const auto s = decodeExact(b);
        CHECK(s != nullptr);
        if (s) CHECK_MSG(s->frames() == audio * 1152 - 529, "%lld frames", (long long)s->frames());
    }
}
