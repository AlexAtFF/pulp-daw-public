// MP3 encoding: what the encoder writes is decoded again (by this decoder; ffmpeg was used by hand,
// see NOTES.md) and measured against what went in, and the frames it writes are read back with the
// test's own reader.
#include "Mp3TestUtil.h"
#include "pulp/Stretch.h"
#include <filesystem>
using namespace pulp;
using namespace tu;
using namespace mp3t;

namespace {

constexpr double kSeconds = 1.0;        // how long the signals for the quality measurements are

std::shared_ptr<SampleData> roundTrip(const SampleData& src, const Mp3EncodeOptions& o, std::vector<unsigned char>* bytesOut = nullptr) {
    const auto bytes = encodeMp3(src, o);
    if (bytesOut) *bytesOut = bytes;
    std::string error;
    auto s = readMp3Memory(bytes.data(), bytes.size(), &error);
    CHECK_MSG(s != nullptr, "did not decode: %s", error.c_str());
    return s;
}
Mp3EncodeOptions at(int kbps) { Mp3EncodeOptions o; o.kbps = kbps; return o; }

// The level of the error in the 2 to 12 ms before each click of a Clicks signal, as a fraction of the
// click's peak (the worst click counts). This is where pre-echo would be.
double preEcho(const SampleData& src, const SampleData& got) {
    const auto& x = src.ch[0];
    const auto& y = got.ch[0];
    const double sr = src.sampleRate;
    double worst = 0.0;
    for (int k = 0;; ++k) {
        const size_t at = clickAt(k, sr);
        if (at + (size_t)(0.02 * sr) + 8 >= std::min(x.size(), y.size())) break;
        const size_t a = at - (size_t)(0.012 * sr), b = at - (size_t)(0.002 * sr);
        double e = 0.0;
        for (size_t i = a; i < b; ++i) e += ((double)y[i] - x[i]) * ((double)y[i] - x[i]);
        worst = std::max(worst, std::sqrt(e / (double)(b - a)) / peakOf(x, at, at + (size_t)(0.02 * sr)));
    }
    return worst;
}

unsigned crc16Reflected(const unsigned char* p, size_t n) {        // polynomial 0xA001, as the Info frame uses
    unsigned crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}
unsigned crc16Frame(const unsigned char* p, size_t n, unsigned crc) {       // polynomial 0x8005, most significant bit first
    for (size_t i = 0; i < n; ++i) {
        crc ^= (unsigned)p[i] << 8;
        for (int k = 0; k < 8; ++k) crc = ((crc & 0x8000) ? ((crc << 1) ^ 0x8005) : (crc << 1)) & 0xFFFF;
    }
    return crc;
}

} // namespace

// The quality table. Every signal at every bitrate, mono and stereo, must decode to its exact length
// and with at least the signal-to-noise ratio below. The floors are 2 to 5 dB under what was measured
// (the test prints the measurements; NOTES.md has them beside LAME's on the same signals, and says
// what they do and do not show):
//   sine and sweep: one tone at a time, so nearly all the bits go to it. The stereo figures at the low
//     rates are held down by the first two granules, where the tones start from nothing and the encoder
//     spends its bits on the onset (25 to 40 dB there, 65 to 80 dB afterwards);
//   music-like: notes, hits and a noise floor; its error is mostly in the hits and the floor, which is
//     where a perceptual coder puts it;
//   noise: white noise to the top of the band cannot be coded well at any MP3 bitrate (and everything
//     above the encoder's low-pass is lost outright), so the floors are low and only say "not broken".
TEST(mp3_round_trip_quality_for_every_signal_and_bitrate) {
    struct Row { Sig kind; double floor[2][4]; };          // [mono, stereo][96, 128, 192, 320]
    const Row rows[] = {
        {Sig::Sine, {{65, 70, 72, 72}, {38, 45, 58, 62}}},
        {Sig::Sweep, {{48, 51, 52, 52}, {31, 34, 39, 46}}},
        {Sig::Music, {{21, 25, 28.5, 29}, {15, 16.5, 20.5, 27}}},
        {Sig::Noise, {{5, 7.5, 9.5, 10}, {2, 3, 5, 9}}},
    };
    const int rates[4] = {96, 128, 192, 320};
    const size_t n = (size_t)(kSeconds * 44100.0);
    for (const Row& row : rows) {
        for (int channels = 1; channels <= 2; ++channels) {
            const auto src = signal(row.kind, channels, n);
            char line[200];
            int col = snprintf(line, sizeof line, "    %-10s %s:", sigName(row.kind), channels == 1 ? "mono  " : "stereo");
            for (int k = 0; k < 4; ++k) {
                const auto got = roundTrip(*src, at(rates[k]));
                if (!got) continue;
                CHECK_MSG(got->frames() == src->frames() && got->channels == channels && got->sampleRate == 44100.0, "%s %d ch %d kbps: %lld frames of %d channels",
                          sigName(row.kind), channels, rates[k], (long long)got->frames(), got->channels);
                CHECK(allFinite(*got));
                const double snr = snrDb(*src, *got);
                CHECK_MSG(snr >= row.floor[channels - 1][k], "%s %d ch at %d kbps: %.2f dB, floor %.1f", sigName(row.kind), channels, rates[k], snr, row.floor[channels - 1][k]);
                col += snprintf(line + col, sizeof line - (size_t)col, "  %3dk %5.1f dB", rates[k], snr);
            }
            printf("%s\n", line);
        }
    }
}

// More bits must not make things worse, and the highest rate must be clearly better than the lowest.
TEST(mp3_quality_rises_with_the_bitrate) {
    const size_t n = (size_t)(kSeconds * 44100.0);
    for (int channels = 1; channels <= 2; ++channels) {
        const auto src = signal(Sig::Music, channels, n, 44100.0, 5);
        double last = -100.0, first = 0.0;
        for (int kbps : {96, 128, 160, 192, 256, 320}) {
            const auto got = roundTrip(*src, at(kbps));
            if (!got) continue;
            const double snr = snrDb(*src, *got);
            CHECK_MSG(snr > last - 1.0, "%d channels: %.2f dB at %d kbps after %.2f dB at the rate below", channels, snr, kbps, last);
            if (kbps == 96) first = snr;
            last = snr;
        }
        CHECK_MSG(last > first + 4.0, "%d channels: %.2f dB at 320 kbps, %.2f dB at 96", channels, last, first);
    }
}

TEST(mp3_silence_stays_silent_and_small) {
    for (int channels = 1; channels <= 2; ++channels) {
        const auto src = signal(Sig::Silence, channels, 30000);
        std::vector<unsigned char> bytes;
        const auto got = roundTrip(*src, at(192), &bytes);
        if (!got) continue;
        CHECK(got->frames() == 30000);
        for (int c = 0; c < channels; ++c) CHECK_MSG(peakOf(got->ch[c]) == 0.0f, "channel %d of silence decodes to a peak of %g", c, peakOf(got->ch[c]));
        // constant bitrate: the frames are as large as ever, but they hold nothing
        for (const auto& f : readFrames(bytes))
            for (int g = 0; g < 2; ++g)
                for (int c = 0; c < channels; ++c) CHECK(f.gr[g][c].part23 == 0 && f.gr[g][c].bigValues == 0);
    }
    // sound, then true silence, then sound: no leftovers in the gap beyond the filters' reach
    auto src = signal(Sig::Sine, 1, 60000);
    for (size_t i = 20000; i < 40000; ++i) src->ch[0][i] = 0.0f;
    const auto got = roundTrip(*src, at(128));
    if (got) CHECK_MSG(peakOf(got->ch[0], 24000, 36000) < 1.0e-4f, "the gap holds %g", peakOf(got->ch[0], 24000, 36000));
}

// Any length comes back exactly, down to nothing at all.
TEST(mp3_round_trip_keeps_the_exact_length) {
    const size_t lengths[] = {0, 1, 2, 31, 100, 527, 575, 576, 577, 1056, 1057, 1151, 1152, 1153, 1728, 2303, 2304, 2305, 4409, 10000};
    for (int channels = 1; channels <= 2; ++channels)
        for (size_t n : lengths) {
            const auto src = signal(Sig::Sine, channels, n);
            std::vector<unsigned char> bytes;
            const auto got = roundTrip(*src, at(channels == 1 ? 96 : 192), &bytes);
            if (!got) continue;
            CHECK_MSG((size_t)got->frames() == n && got->channels == channels, "%zu frames of %d channels came back as %lld of %d", n, channels, (long long)got->frames(), got->channels);
            CHECK(allFinite(*got));
            // as few frames as hold the audio and the 1057 samples the two filterbanks delay it by (but two at least), after the Info frame
            const auto frames = readFrames(bytes);
            CHECK_MSG(frames.size() == 1 + std::max<size_t>(2, (n + 1057 + 1151) / 1152), "%zu samples in %zu frames", n, frames.size());
            if (n >= 4409) CHECK_MSG(snrDb(*src, *got) > 40.0, "%zu frames: %.1f dB", n, snrDb(*src, *got));
        }
    // very short inputs keep their place and most of their energy: a single full-scale sample ...
    for (int kbps : {96, 128, 192, 320}) {
        auto one = blank(1, 1);
        one->ch[0][0] = 0.9f;
        const auto got = roundTrip(*one, at(kbps));
        CHECK(got && got->frames() == 1);
        if (got && got->frames() == 1) CHECK_MSG(got->ch[0][0] > 0.6f && got->ch[0][0] < 1.1f, "at %d kbps one sample of 0.9 came back as %g", kbps, got->ch[0][0]);
        // ... and a click in the middle of less than a frame
        auto click = blank(2, 700);
        click->ch[0][300] = 0.8f;
        click->ch[1][300] = -0.8f;
        const auto back = roundTrip(*click, at(kbps));
        CHECK(back && back->frames() == 700);
        if (back && back->frames() == 700) {
            CHECK_MSG(back->ch[0][300] > 0.5f && back->ch[1][300] < -0.5f, "at %d kbps the click came back as %g and %g", kbps, back->ch[0][300], back->ch[1][300]);
            printf("    a click of 0.8 at %d kbps: back as %.3f, peak before it %.4f; one sample of 0.9 back as %.3f\n", kbps, back->ch[0][300], peakOf(back->ch[0], 0, 250),
                   got ? got->ch[0][0] : 0.0f);
            CHECK_MSG(peakOf(back->ch[0], 0, 250) < 0.04f, "at %d kbps there is %g before the click", kbps, peakOf(back->ch[0], 0, 250));
        }
    }
}

// The frames themselves: MPEG-1 Layer III headers with the right bitrate and rate, the sizes that go
// with them (padded at 44.1 kHz so the average is exact), an Info frame first.
TEST(mp3_frame_headers_and_sizes_are_right_for_the_bitrate) {
    static const int index[8][2] = {{96, 7}, {112, 8}, {128, 9}, {160, 10}, {192, 11}, {224, 12}, {256, 13}, {320, 14}};
    for (double rate : {32000.0, 44100.0, 48000.0})
        for (const auto& br : index)
            for (int channels = 1; channels <= 2; ++channels) {
                if (channels == 1 && (br[0] == 112 || br[0] == 224)) continue;        // enough combinations
                const size_t n = (size_t)(0.4 * rate) + 123;
                const auto src = signal(Sig::Music, channels, n, rate);
                const auto bytes = encodeMp3(*src, br[0]);
                const auto frames = readFrames(bytes);
                const size_t audio = std::max<size_t>(2, (n + 1057 + 1151) / 1152);
                CHECK_MSG(frames.size() == audio + 1, "%g Hz %d kbps: %zu frames, expected %zu", rate, br[0], frames.size(), audio + 1);
                if (frames.size() != audio + 1) continue;
                size_t total = 0, padded = 0;
                for (size_t i = 0; i < frames.size(); ++i) {
                    const FrameInfo& f = frames[i];
                    const unsigned char* h = bytes.data() + f.pos;
                    CHECK_MSG(h[0] == 0xFF && h[1] == 0xFB, "frame %zu starts %02X %02X", i, h[0], h[1]);         // sync, MPEG-1, Layer III, no CRC
                    CHECK_MSG(f.version == 1 && f.bitrateIndex == br[1] && f.kbps == br[0] && f.rate == (int)rate, "frame %zu: %d kbps at %d Hz", i, f.kbps, f.rate);
                    CHECK_MSG(f.channels == channels && f.mode == (channels == 1 ? 3 : 1), "frame %zu: mode %d", i, f.mode);
                    CHECK((h[3] & 0x0B) == 0);                                  // no copyright bit, no emphasis
                    const int base = 144000 * br[0] / (int)rate;
                    CHECK_MSG(f.bytes == base + (f.padding ? 1 : 0), "frame %zu is %d bytes", i, f.bytes);
                    CHECK(f.pos == total);
                    total += (size_t)f.bytes;
                    if (f.padding) ++padded;
                    if (i == 0) {
                        CHECK(isInfoFrame(bytes.data(), f) && !f.padding && f.mainDataBegin == 0);
                    } else {
                        CHECK(!isInfoFrame(bytes.data(), f));
                        if (i == 1) CHECK(f.mainDataBegin == 0);                // nothing to borrow from yet
                        int bits = 0;
                        for (int g = 0; g < 2; ++g)
                            for (int c = 0; c < channels; ++c) {
                                const GranuleInfo& G = f.gr[g][c];
                                CHECK(G.bigValues <= 288 && G.part23 <= 4095 && !(G.switching && G.blockType == 0));
                                for (int t = 0; t < 3; ++t) CHECK(G.table[t] != 4 && G.table[t] != 14);
                                bits += G.part23;
                            }
                        // a frame's data fits in its own room plus what it borrowed
                        CHECK_MSG(bits <= 8 * (f.bytes - 4 - f.sideBytes + f.mainDataBegin), "frame %zu: %d bits of data", i, bits);
                    }
                }
                CHECK_MSG(total == bytes.size(), "frames cover %zu of %zu bytes", total, bytes.size());
                // the padding makes the average frame exactly 144000 * kbps / rate bytes long
                const double exact = 144000.0 * br[0] / rate * (double)audio;
                size_t audioBytes = total - (size_t)frames[0].bytes;
                CHECK_MSG(std::fabs((double)audioBytes - exact) < 1.0, "%g Hz %d kbps: %zu bytes of audio frames, exactly %.1f wanted", rate, br[0], audioBytes, exact);
                if (rate != 44100.0) CHECK(padded == 0);
            }
    // bitrates outside the table go to the nearest one in it; the default is 192
    const auto src = signal(Sig::Sine, 1, 3000);
    struct { int asked, got; } snaps[] = {{0, 96}, {-5, 96}, {64, 96}, {100, 96}, {105, 112}, {121, 128}, {150, 160}, {180, 192}, {200, 192}, {300, 320}, {1000, 320}};
    for (const auto& s : snaps) {
        const auto frames = readFrames(encodeMp3(*src, s.asked));
        CHECK_MSG(!frames.empty() && frames[0].kbps == s.got, "asked for %d kbps, got %d", s.asked, frames.empty() ? 0 : frames[0].kbps);
    }
    const auto def = readFrames(encodeMp3(*src));
    CHECK(!def.empty() && def[0].kbps == 192);
}

TEST(mp3_info_frame_states_length_delay_and_padding) {
    for (int channels = 1; channels <= 2; ++channels)
        for (size_t n : {(size_t)1, (size_t)5000, (size_t)44100}) {
            const auto src = signal(Sig::Music, channels, n);
            const auto bytes = encodeMp3(*src, 128);
            const auto frames = readFrames(bytes);
            CHECK(frames.size() >= 2);
            if (frames.size() < 2) continue;
            const FrameInfo& f = frames[0];
            const unsigned char* p = bytes.data() + 4;
            for (int i = 0; i < f.sideBytes; ++i) CHECK(p[i] == 0);            // the Info frame carries no audio
            const unsigned char* t = p + f.sideBytes;
            auto be32 = [](const unsigned char* q) { return ((uint32)q[0] << 24) | ((uint32)q[1] << 16) | ((uint32)q[2] << 8) | (uint32)q[3]; };
            CHECK(std::memcmp(t, "Info", 4) == 0);                              // constant bitrate: "Info", not "Xing"
            CHECK(be32(t + 4) == 0x0F);
            CHECK_MSG(be32(t + 8) == frames.size() - 1, "says %u frames, there are %zu", be32(t + 8), frames.size() - 1);
            CHECK(be32(t + 12) == bytes.size());
            for (int i = 0; i < 100; ++i) CHECK(t[16 + i] == i * 256 / 100);    // the seek table of a constant bitrate file is a straight line
            const unsigned char* x = t + 120;
            CHECK(std::memcmp(x, "LAME", 4) == 0);                              // the marker decoders look for before they trust the delays
            const int delay = (x[21] << 4) | (x[22] >> 4), padding = ((x[22] & 15) << 8) | x[23];
            CHECK_MSG(delay == 528, "delay %d", delay);
            CHECK_MSG((size_t)padding == (frames.size() - 1) * 1152 - n - 528, "padding %d for %zu samples in %zu frames", padding, n, frames.size() - 1);
            CHECK(padding >= 529);                                              // the decoder's own delay fits in it
            CHECK(be32(x + 28) == bytes.size());
            const size_t tagBytes = (size_t)(x + 34 - bytes.data());
            CHECK((((unsigned)x[34] << 8) | x[35]) == crc16Reflected(bytes.data(), tagBytes));
            CHECK((((unsigned)x[32] << 8) | x[33]) == crc16Reflected(bytes.data() + f.bytes, bytes.size() - (size_t)f.bytes));
            CHECK(tagBytes + 2 <= (size_t)f.bytes);
        }
    // without the Info frame the stream starts with audio and decodes to whole frames less the decoder's delay
    const auto src = signal(Sig::Sine, 2, 5000);
    Mp3EncodeOptions o;
    o.infoFrame = false;
    const auto bytes = encodeMp3(*src, o);
    const auto frames = readFrames(bytes);
    CHECK(!frames.empty() && !isInfoFrame(bytes.data(), frames[0]));
    const auto got = readMp3Memory(bytes.data(), bytes.size());
    CHECK(got != nullptr);
    if (!got) return;
    CHECK(got->frames() == (long long)frames.size() * 1152 - 529);
    // the audio then sits 528 samples in
    double sig = 0.0, err = 0.0;
    for (size_t i = 0; i < 5000; ++i) {
        const double a = src->ch[0][i], e = a - (double)got->ch[0][i + 528];
        sig += a * a;
        err += e * e;
    }
    CHECK_MSG(10.0 * std::log10(sig / err) > 40.0, "%.1f dB when read 528 samples in", 10.0 * std::log10(sig / err));
}

// Stereo that is nearly mono is cheaper as sum and difference; stereo with independent channels is not.
TEST(mp3_mid_side_is_used_where_it_helps) {
    const size_t n = (size_t)(kSeconds * 44100.0);
    auto share = [](const std::vector<unsigned char>& bytes) {
        int ms = 0, all = 0;
        for (const auto& f : readFrames(bytes)) {
            if (isInfoFrame(bytes.data(), f)) continue;
            ++all;
            if (f.mode == 1 && (f.modeExt & 2)) ++ms;
            CHECK(f.mode == 1 && (f.modeExt & 1) == 0);
        }
        return all ? (double)ms / all : 0.0;
    };
    const auto near = signal(Sig::NearMono, 2, n);
    std::vector<unsigned char> bytes;
    const auto joint = roundTrip(*near, at(128), &bytes);
    CHECK_MSG(share(bytes) > 0.9, "mid/side in %.0f%% of the frames of near-mono stereo", 100.0 * share(bytes));
    Mp3EncodeOptions lr = at(128);
    lr.stereo = Mp3EncodeOptions::LeftRight;
    const auto plain = roundTrip(*near, lr, &bytes);
    for (const auto& f : readFrames(bytes)) CHECK(f.mode == 0);
    if (joint && plain) CHECK_MSG(snrDb(*near, *joint) > snrDb(*near, *plain) + 1.0, "mid/side %.2f dB, left/right %.2f dB", snrDb(*near, *joint), snrDb(*near, *plain));
    // the small difference between the channels survives
    if (joint) {
        std::vector<float> side(n), sideBack(n);
        for (size_t i = 0; i < n; ++i) { side[i] = near->ch[0][i] - near->ch[1][i]; sideBack[i] = joint->ch[0][i] - joint->ch[1][i]; }
        CHECK_MSG(std::fabs(rmsOf(sideBack, 2000, n) / rmsOf(side, 2000, n) - 1.0) < 0.2, "difference signal: %g in, %g out", rmsOf(side, 2000, n), rmsOf(sideBack, 2000, n));
    }

    auto wide = signal(Sig::Music, 2, n);
    Rand rng(4);
    for (size_t i = 0; i < n; ++i) wide->ch[1][i] = 0.3f * rng.uni();           // music left, noise right
    roundTrip(*wide, at(128), &bytes);
    CHECK_MSG(share(bytes) < 0.1, "mid/side in %.0f%% of the frames of unrelated channels", 100.0 * share(bytes));
    // identical channels: all mid, and the two channels decode identically
    auto same = signal(Sig::Music, 2, n);
    same->ch[1] = same->ch[0];
    const auto back = roundTrip(*same, at(128), &bytes);
    CHECK(share(bytes) > 0.95);
    if (back) CHECK(maxAbsDiff(back->ch[0], back->ch[1]) < 1.0e-6);
}

// At an attack the encoder switches to short blocks, which keeps the quantisation noise from
// spreading back over the quiet before it.
TEST(mp3_short_blocks_at_attacks_keep_pre_echo_down) {
    const size_t n = (size_t)(1.0 * 44100.0);
    const auto clicks = signal(Sig::Clicks, 1, n);
    for (int kbps : {96, 128, 192, 320}) {
        std::vector<unsigned char> bytes;
        const auto withShort = roundTrip(*clicks, at(kbps), &bytes);
        int shortBlocks = 0, startBlocks = 0, stopBlocks = 0, last = 0;
        bool legal = true, first = true;
        for (const auto& f : readFrames(bytes)) {
            if (isInfoFrame(bytes.data(), f)) continue;
            for (int g = 0; g < 2; ++g) {
                const int type = f.gr[g][0].blockType;
                if (type == 2) ++shortBlocks;
                if (type == 1) ++startBlocks;
                if (type == 3) ++stopBlocks;
                // the windows must follow one another the way they overlap: after a long or stop block a long or
                // start block, after a start block a short one, after a short block a short or stop block
                // (a short block may open the file: there is nothing before it to overlap with)
                if (last == 1) legal = legal && type == 2;
                else if (last == 2) legal = legal && (type == 2 || type == 3);
                else legal = legal && (type == 0 || type == 1 || (first && type == 2));
                last = type;
                first = false;
            }
        }
        CHECK_MSG(shortBlocks >= 4 && shortBlocks <= 24, "%d short blocks for 4 clicks", shortBlocks);
        CHECK_MSG(startBlocks >= 3 && stopBlocks >= 3, "%d start and %d stop blocks", startBlocks, stopBlocks);
        CHECK_MSG(legal, "the block types do not follow one another legally at %d kbps", kbps);
        Mp3EncodeOptions o = at(kbps);
        o.shortBlocks = false;
        const auto longOnly = roundTrip(*clicks, o, &bytes);
        for (const auto& f : readFrames(bytes))
            for (int g = 0; g < 2; ++g) CHECK(f.gr[g][0].blockType == 0);
        if (!withShort || !longOnly) continue;
        const double a = preEcho(*clicks, *withShort), b = preEcho(*clicks, *longOnly);
        printf("    clicks at %d kbps: error before the attack %.1f dB below the click's peak with short blocks, %.1f dB with long blocks only\n", kbps, -20.0 * std::log10(a), -20.0 * std::log10(b));
        CHECK_MSG(a < 0.0056, "pre-echo at %d kbps is %.1f dB under the click's peak", kbps, -20.0 * std::log10(a));        // -45 dB
        CHECK_MSG(a < b, "short blocks %.1f dB, long blocks %.1f dB", -20.0 * std::log10(a), -20.0 * std::log10(b));
    }
    // steady signals stay in long blocks (the very start of the file may count as an attack)
    for (Sig kind : {Sig::Sine, Sig::Sweep, Sig::Noise}) {
        const auto bytes = encodeMp3(*signal(kind, 2, n), 128);
        int shortBlocks = 0;
        for (const auto& f : readFrames(bytes))         // not the first frames, and not the last, where the signal breaks off
            if (f.pos > 2000 && f.pos + 3000 < bytes.size()) for (int g = 0; g < 2; ++g) if (f.gr[g][0].blockType != 0) ++shortBlocks;
        CHECK_MSG(shortBlocks == 0, "%d switched blocks in a steady %s", shortBlocks, sigName(kind));
    }
    // both channels always switch together
    const auto bytes = encodeMp3(*signal(Sig::Music, 2, n), 128);
    for (const auto& f : readFrames(bytes))
        for (int g = 0; g < 2; ++g) CHECK(f.gr[g][0].blockType == f.gr[g][1].blockType);
}

// Bits a frame does not need are kept for a later one, within the limits the format sets.
TEST(mp3_bit_reservoir_is_used_and_stays_within_bounds) {
    const size_t n = (size_t)(kSeconds * 44100.0);
    const auto src = signal(Sig::Music, 2, n);
    for (int kbps : {96, 128, 192, 256, 320}) {
        const auto bytes = encodeMp3(*src, kbps);
        const auto frames = readFrames(bytes);
        int borrowed = 0, most = 0;
        for (const auto& f : frames) {
            if (f.mainDataBegin > 0) ++borrowed;
            most = std::max(most, f.mainDataBegin);
            // what is borrowed lies in the frames before: there must be that many bytes of them
            size_t before = 0;
            for (const auto& e : frames) if (e.pos < f.pos && !isInfoFrame(bytes.data(), e)) before += (size_t)(e.bytes - 4 - e.sideBytes);
            CHECK(before >= (size_t)f.mainDataBegin);
        }
        // a decoder need only buffer 7680 bits: a frame and what it borrows fit in that
        const int frameBits = 8 * (144000 * kbps / 44100 + 1);
        const int limit = std::max(0, std::min(511, (7680 - frameBits) / 8));
        CHECK_MSG(most <= limit, "%d kbps: a frame reaches back %d bytes, limit %d", kbps, most, limit);
        if (limit > 50) CHECK_MSG(borrowed > (int)frames.size() / 4, "%d kbps: %d of %zu frames use the reservoir", kbps, borrowed, frames.size());
        else CHECK(borrowed == 0 || most <= limit);
    }
}

// Rates MP3 does not have are resampled to 44.1 or 48 kHz first.
TEST(mp3_other_sample_rates_are_resampled) {
    struct { double rate; double target; } cases[] = {{8000, 48000}, {11025, 44100}, {16000, 48000}, {22050, 44100}, {24000, 48000}, {88200, 44100}, {96000, 48000}, {37800, 44100}, {50000, 48000}};
    for (const auto& c : cases) {
        const size_t n = (size_t)(0.5 * c.rate);
        const double hz = c.rate >= 22050 ? 1000.0 : 440.0;
        const auto src = sine(hz, 0.5, c.rate, 0.5f);
        const auto bytes = encodeMp3(*src, 128);
        const auto frames = readFrames(bytes);
        CHECK_MSG(!frames.empty() && frames[0].rate == (int)c.target, "%g Hz was encoded at %d Hz", c.rate, frames.empty() ? 0 : frames[0].rate);
        const auto got = readMp3Memory(bytes.data(), bytes.size());
        CHECK(got != nullptr);
        if (!got) continue;
        CHECK(got->sampleRate == c.target);
        const long long want = std::llround((double)n * c.target / c.rate);
        CHECK_MSG(std::llabs(got->frames() - want) <= 1, "%g Hz: %lld frames at %g Hz, expected %lld", c.rate, (long long)got->frames(), c.target, want);
        const size_t len = (size_t)got->frames();
        const double measured = measureHz(got->ch[0], len / 4, len * 3 / 4, c.target);
        CHECK_MSG(std::fabs(measured - hz) < 0.5, "%g Hz: the %g Hz tone came back at %.2f Hz", c.rate, hz, measured);
        CHECK_MSG(std::fabs(rmsOf(got->ch[0], len / 4, len * 3 / 4) - 0.5 / std::sqrt(2.0)) < 0.01, "%g Hz: level %g", c.rate, rmsOf(got->ch[0], len / 4, len * 3 / 4));
    }
    // the three rates MPEG-1 has are kept as they are
    for (double rate : {32000.0, 44100.0, 48000.0}) {
        const auto src = signal(Sig::Music, 2, (size_t)(0.3 * rate), rate);
        const auto got = roundTrip(*src, at(192));
        if (!got) continue;
        CHECK(got->sampleRate == rate && got->frames() == src->frames());
        CHECK_MSG(snrDb(*src, *got) > 16.0, "%g Hz: %.1f dB", rate, snrDb(*src, *got));
    }
}

// The other ways to store stereo, and frames with checksums.
TEST(mp3_encoder_options) {
    const size_t n = (size_t)(std::min(kSeconds, 0.6) * 44100.0);
    const auto src = signal(Sig::NearMono, 2, n);
    struct { Mp3EncodeOptions::Stereo stereo; int mode; int ext; double floor; const char* name; } modes[] = {
        {Mp3EncodeOptions::Auto, 1, -1, 22.0, "auto"},
        {Mp3EncodeOptions::LeftRight, 0, 0, 19.0, "left/right"},
        {Mp3EncodeOptions::MidSide, 1, 2, 22.0, "mid/side"},
        {Mp3EncodeOptions::Dual, 2, 0, 19.0, "dual channel"},
        {Mp3EncodeOptions::Intensity, 1, 1, 12.0, "intensity"},
    };
    for (const auto& m : modes)
        for (bool mixed : {false, true}) {
            Mp3EncodeOptions o = at(192);
            o.stereo = m.stereo;
            o.mixedBlocks = mixed;
            std::vector<unsigned char> bytes;
            const auto got = roundTrip(*src, o, &bytes);
            int mixedBlocks = 0, switched = 0;
            for (const auto& f : readFrames(bytes)) {
                if (isInfoFrame(bytes.data(), f)) continue;
                CHECK_MSG(f.mode == m.mode && (m.ext < 0 || f.modeExt == m.ext), "%s: mode %d extension %d", m.name, f.mode, f.modeExt);
                for (int g = 0; g < 2; ++g) {
                    if (f.gr[g][0].switching) ++switched;
                    if (f.gr[g][0].mixed) ++mixedBlocks;
                }
            }
            CHECK(switched > 0);
            CHECK_MSG(mixedBlocks == (mixed ? switched : 0), "%s: %d of %d switched blocks are mixed", m.name, mixedBlocks, switched);
            if (!got) continue;
            CHECK(got->frames() == src->frames() && got->channels == 2);
            const double snr = snrDb(*src, *got);
            CHECK_MSG(snr > m.floor, "%s%s: %.2f dB, floor %.1f", m.name, mixed ? " with mixed blocks" : "", snr, m.floor);
        }
    // intensity stereo keeps each channel's level in the bands it joins, though not their difference
    {
        auto hard = blank(2, n);
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)i / 44100.0;
            hard->ch[0][i] = 0.4f * (float)std::sin(kTwoPi * 6000.0 * t) + 0.2f * (float)std::sin(kTwoPi * 300.0 * t);
            hard->ch[1][i] = 0.1f * (float)std::sin(kTwoPi * 6000.0 * t) + 0.2f * (float)std::sin(kTwoPi * 450.0 * t);
        }
        Mp3EncodeOptions o = at(192);
        o.stereo = Mp3EncodeOptions::Intensity;
        const auto got = roundTrip(*hard, o);
        if (got) {
            const double l = rmsOf(got->ch[0], 4000, n), r = rmsOf(got->ch[1], 4000, n);
            CHECK_MSG(std::fabs(l - rmsOf(hard->ch[0], 4000, n)) < 0.03 && std::fabs(r - rmsOf(hard->ch[1], 4000, n)) < 0.03, "levels %g and %g, were %g and %g", l, r,
                      rmsOf(hard->ch[0], 4000, n), rmsOf(hard->ch[1], 4000, n));
        }
    }
    // checksums: two more bytes per frame, valid over the header's last two bytes and the side information
    Mp3EncodeOptions o = at(128);
    o.crc = true;
    std::vector<unsigned char> bytes;
    const auto got = roundTrip(*src, o, &bytes);
    int checked = 0;
    for (const auto& f : readFrames(bytes)) {
        if (isInfoFrame(bytes.data(), f)) continue;
        const unsigned char* h = bytes.data() + f.pos;
        CHECK(f.crc && h[1] == 0xFA);
        unsigned crc = crc16Frame(h + 2, 2, 0xFFFF);
        crc = crc16Frame(h + 6, (size_t)f.sideBytes, crc);
        CHECK_MSG(crc == (((unsigned)h[4] << 8) | h[5]), "frame at %zu: checksum %04X, stored %02X%02X", f.pos, crc, h[4], h[5]);
        ++checked;
    }
    CHECK(checked > 5);
    if (got) CHECK_MSG(snrDb(*src, *got) > 18.0, "with checksums: %.2f dB", snrDb(*src, *got));
}

// Input that is not well-behaved audio must still give a valid stream.
TEST(mp3_encoder_takes_loud_broken_and_lopsided_input) {
    const size_t n = 12000;
    // louder than full scale: kept, not clipped
    auto loud = signal(Sig::Sine, 1, n);
    for (auto& v : loud->ch[0]) v *= 6.0f;                 // peak 3.0
    auto got = roundTrip(*loud, at(192));
    if (got) {
        CHECK_MSG(snrDb(*loud, *got) > 50.0, "a sine of peak 3: %.1f dB", snrDb(*loud, *got));
        CHECK(peakOf(got->ch[0]) > 2.9f);
    }
    // very quiet: still there
    auto quiet = signal(Sig::Sine, 1, n);
    for (auto& v : quiet->ch[0]) v *= 2.0e-4f;             // peak 1e-4, 80 dB down
    got = roundTrip(*quiet, at(192));
    if (got) CHECK_MSG(snrDb(*quiet, *got) > 30.0, "a sine 80 dB down: %.1f dB", snrDb(*quiet, *got));
    // a constant
    auto dc = blank(2, n);
    for (auto& v : dc->ch[0]) v = 0.5f;
    for (auto& v : dc->ch[1]) v = -0.25f;
    got = roundTrip(*dc, at(128));
    if (got) {
        CHECK_MSG(std::fabs(got->ch[0][n / 2] - 0.5f) < 0.01f && std::fabs(got->ch[1][n / 2] + 0.25f) < 0.01f, "constants came back as %g and %g", got->ch[0][n / 2], got->ch[1][n / 2]);
    }
    // not-a-number and infinite samples, and absurd levels: a valid stream of finite samples comes out
    auto broken = signal(Sig::Music, 2, n);
    broken->ch[0][100] = std::numeric_limits<float>::quiet_NaN();
    broken->ch[0][2000] = std::numeric_limits<float>::infinity();
    broken->ch[1][3000] = -std::numeric_limits<float>::infinity();
    broken->ch[1][5000] = 1.0e30f;
    for (size_t i = 7000; i < 7100; ++i) broken->ch[0][i] = (i & 1) ? 1.0e12f : -1.0e12f;
    for (int kbps : {96, 320}) {
        got = roundTrip(*broken, at(kbps));
        if (got) {
            CHECK(got->frames() == (long long)n && allFinite(*got));
            CHECK_MSG(peakOf(got->ch[0]) < 200.0f, "peak %g", peakOf(got->ch[0]));          // such samples are clipped to 64 on the way in
        }
    }
    // a stereo sample whose channels differ in length, and one that says stereo but has no second channel
    auto lopsided = signal(Sig::Sine, 2, n);
    lopsided->ch[1].resize(n / 2);
    got = roundTrip(*lopsided, at(128));
    if (got) {
        CHECK(got->frames() == (long long)n && got->channels == 2);
        CHECK(peakOf(got->ch[1], n / 2 + 2000, n) < 0.01f && peakOf(got->ch[1], 1000, n / 2 - 1000) > 0.4f);
    }
    auto mono = signal(Sig::Sine, 1, n);
    mono->channels = 0;                                     // nonsense channel counts are taken as mono
    got = roundTrip(*mono, at(128));
    if (got) CHECK(got->channels == 1 && got->frames() == (long long)n);
    // a nonsense sample rate is taken as 44.1 kHz
    auto rateless = signal(Sig::Sine, 1, n);
    rateless->sampleRate = 0.0;
    got = roundTrip(*rateless, at(128));
    if (got) CHECK(got->sampleRate == 44100.0 && got->frames() == (long long)n);
}

TEST(mp3_encoder_gives_the_same_bytes_every_time) {
    const auto src = signal(Sig::Music, 2, 20000);
    const auto a = encodeMp3(*src, 160), b = encodeMp3(*src, 160);
    CHECK(a == b);
    Mp3EncodeOptions o;
    o.kbps = 160;
    CHECK(encodeMp3(*src, o) == a);
    CHECK(encodeMp3(*src) == encodeMp3(*src, 192));
}

TEST(mp3_file_round_trip_with_a_non_ascii_path) {
    namespace fs = std::filesystem;
    auto u8 = [](const std::string& p) { return fs::path(std::u8string(p.begin(), p.end())); };
    const std::string dir = t::tmpDir() + "/mp3 \xC3\xA9\xC3\xA0\xC3\xBC \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";     // "éàü 日本語"
    const std::string path = dir + "/\xD0\xBF\xD0\xB5\xD1\x81\xD0\xBD\xD1\x8F \xE2\x99\xAB.mp3";                 // "песня ♫.mp3"
    std::error_code ec;
    fs::remove_all(u8(dir), ec);
    const auto src = signal(Sig::Music, 2, 22050);
    std::string error;
    CHECK_MSG(writeMp3File(path, *src, 192, &error), "%s", error.c_str());
    CHECK(fs::exists(u8(path)));
    const auto size = fs::file_size(u8(path), ec);
    CHECK(size == encodeMp3(*src, 192).size());
    const auto got = readMp3File(path, &error);
    CHECK_MSG(got != nullptr, "%s", error.c_str());
    if (got) {
        CHECK(got->path == path);
        CHECK(got->frames() == src->frames() && got->channels == 2 && got->sampleRate == 44100.0);
        CHECK_MSG(snrDb(*src, *got) > 20.0, "%.1f dB", snrDb(*src, *got));
        const auto bytes = encodeMp3(*src, 192);
        const auto fromMemory = readMp3Memory(bytes.data(), bytes.size());
        CHECK(fromMemory && identical(*got, *fromMemory));
    }
    // the default bitrate, and a path that cannot be written
    CHECK(writeMp3File(dir + "/default.mp3", *src));
    CHECK(fs::file_size(u8(dir + "/default.mp3"), ec) == size);
    error.clear();
    CHECK(writeFileBytes(dir + "/blocker", "x", 1));
    CHECK(!writeMp3File(dir + "/blocker/file.mp3", *src, 128, &error));
    CHECK(error.find("cannot write") != std::string::npos);
    fs::remove_all(u8(dir), ec);
}
