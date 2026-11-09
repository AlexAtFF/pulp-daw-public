// Milestone 7: audio played from disk, time-stretching, pitch shifting and tempo detection.
#include "Util.h"
#include "pulp/Stream.h"
#include "pulp/Stretch.h"
#include <filesystem>
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0, kBpm = 120.0;
int64 at(double ticks) { return (int64)std::llround(ticks * samplesPerTick(kBpm, kSr)); }

std::shared_ptr<SampleData> noise(double seconds, int channels = 2, uint32 seed = 5u) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = kSr;
    s->channels = channels;
    dsp::Noise a(seed), b(seed * 7 + 3);
    const size_t n = (size_t)(seconds * kSr);
    for (int c = 0; c < channels; ++c) s->ch[c].resize(n);
    for (size_t i = 0; i < n; ++i) { s->ch[0][i] = 0.3f * a.next(); if (channels > 1) s->ch[1][i] = 0.3f * b.next(); }
    return s;
}
std::shared_ptr<SampleData> render(Project& p, int64 len, int block = 512, int threads = 1) {
    RenderOptions o;
    o.lengthSamples = len;
    o.blockSize = block;
    o.threads = threads;
    return renderOffline(p, o);
}
// Streaming settings small enough that a few seconds of audio span many pages.
struct SmallPages {
    SmallPages() { AudioStream::setPageFrames(4096); AudioStream::setStreamAbove(48000); }
    ~SmallPages() { AudioStream::setPageFrames(65536); AudioStream::setStreamAbove((int64)48000 * 45); }
};
// A click every beat: a 3 ms burst of noise that decays.
std::shared_ptr<SampleData> clickLoop(double bpm, int beats, double sr = kSr) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = 1;
    const double beat = 60.0 / bpm * sr;
    s->ch[0].assign((size_t)std::llround(beat * beats), 0.0f);
    dsp::Noise nz(11u);
    for (int b = 0; b < beats; ++b) {
        const size_t start = (size_t)std::llround(b * beat);
        const float level = b % 4 == 0 ? 0.8f : 0.5f;
        for (int i = 0; i < (int)(sr * 0.03) && start + (size_t)i < s->ch[0].size(); ++i)
            s->ch[0][start + (size_t)i] = level * nz.next() * (float)std::exp(-i / (sr * 0.004));
    }
    return s;
}
// Sample index of the first loud sample at or after `from`.
int64 onsetAfter(const std::vector<float>& d, int64 from, float threshold) {
    for (int64 i = std::max<int64>(0, from); i < (int64)d.size(); ++i) if (std::fabs(d[(size_t)i]) > threshold) return i;
    return -1;
}
}

TEST(streamed_audio_is_the_same_audio_as_loaded_audio) {
    namespace fs = std::filesystem;
    SmallPages small;
    const std::string dir = t::tmpDir() + "/stream";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto src = noise(6.0);
    writeWavFile(dir + "/long.wav", *src, WavFormat::Float32);
    auto build = [&](Project& p, bool fromDisk) {
        p.global->tempo.set((float)kBpm);
        p.songMode = true;
        const int id = fromDisk ? p.addAudio(dir + "/long.wav") : p.addAudio(src, "long");
        CHECK(id != 0);
        if (fromDisk) { CHECK(p.audioById(id)->stream != nullptr); CHECK(p.audioById(id)->sample == nullptr); }
        p.addAudioClip(id, 0, 0, 1);
        auto& b = p.addAudioClip(id, 1, 960, 2);
        b.offset = 1440; b.length = 4000; b.gain = 0.7f; b.fadeIn = 300; b.fadeOut = 500; b.sourceOffsetFrames = 777;
        auto& c = p.addAudioClip(id, 2, 2000, 3);
        c.reverse = true; c.length = 3000;
    };
    Project mem, disk;
    build(mem, false);
    build(disk, true);
    const int64 len = (int64)(6.5 * kSr);
    auto ref = render(mem, len);
    for (auto cs : std::vector<std::pair<int, int>>{{512, 1}, {37, 1}, {64, 4}, {4096, 3}}) {
        Project d2;
        build(d2, true);
        const int64 before = Rt::allocationsInRt.load();
        auto out = render(d2, len, cs.first, cs.second);
        CHECK(Rt::allocationsInRt.load() == before);
        CHECK_MSG(identical(*ref, *out), "streamed render differs from the loaded one (block %d, threads %d)", cs.first, cs.second);
    }
    CHECK(peakOf(ref->ch[0]) > 0.2f);
    // the overview and the peak come from the whole file
    const auto* item = disk.audioById(1);
    CHECK_NEAR(item->peak(), src->peak(), 1e-7);
    float mn, mx;
    item->minMax(1000, 20000, mn, mx);
    CHECK(mn < -0.25f && mx > 0.25f);
    CHECK(item->frames() == src->frames() && item->channels() == 2);

    // 16-bit and 24-bit files decode to the same values through a stream as when loaded whole
    for (auto fmt : {WavFormat::Pcm16, WavFormat::Pcm24}) {
        const std::string path = dir + (fmt == WavFormat::Pcm16 ? "/p16.wav" : "/p24.wav");
        writeWavFile(path, *src, fmt);
        auto whole = readWavFile(path);
        auto st = AudioStream::open(path);
        CHECK(st && whole);
        if (!st || !whole) continue;
        std::vector<float> l(9000), r(9000);
        CHECK(st->read(50000, 9000, l.data(), r.data(), true));
        bool same = true;
        for (int i = 0; i < 9000; ++i) same = same && l[(size_t)i] == whole->ch[0][(size_t)(50000 + i)] && r[(size_t)i] == whole->ch[1][(size_t)(50000 + i)];
        CHECK(same);
        // before the start and after the end there is silence
        CHECK(st->read(-100, 200, l.data(), r.data(), true));
        CHECK(l[0] == 0.0f && l[99] == 0.0f && l[100] == whole->ch[0][0]);
        CHECK(st->read(st->frames - 50, 100, l.data(), r.data(), true));
        CHECK(l[49] == whole->ch[0][(size_t)(st->frames - 1)] && l[50] == 0.0f);
    }
    // saved and reloaded, the long file is streamed again and plays the same
    CHECK(disk.save(dir + "/song.pulp"));
    Project again;
    std::string err;
    CHECK_MSG(again.load(dir + "/song.pulp", &err), "%s", err.c_str());
    CHECK(again.audios.size() == 1 && again.audios[0].stream != nullptr);
    CHECK(identical(*ref, *render(again, len)));
}

TEST(live_playback_from_disk_never_waits_or_allocates) {
    namespace fs = std::filesystem;
    SmallPages small;
    const std::string dir = t::tmpDir() + "/streamlive";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto src = noise(8.0, 2, 21u);
    writeWavFile(dir + "/long.wav", *src, WavFormat::Float32);
    Project p;
    p.global->tempo.set((float)kBpm);
    p.songMode = true;
    const int id = p.addAudio(dir + "/long.wav");
    p.addAudioClip(id, 0, 0, 1);
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setLooping(false);
    e.setState(p.compile());
    auto stream = p.audioById(id)->stream;
    CHECK(stream != nullptr);
    if (!stream) return;
    stream->service();                       // what the loader thread does: the clip's first pages are kept ready
    CHECK(stream->pagesLoaded() >= 1);
    e.play();
    const int block = 256, total = (int)(7.5 * kSr);
    std::vector<float> l((size_t)total), r((size_t)total);
    const int64 before = Rt::allocationsInRt.load();
    int mostPages = 0;
    for (int c = 0; c + block <= total; c += block) {
        float* out[2] = {l.data() + c, r.data() + c};
        e.process(nullptr, 0, out, 2, block);
        stream->service();
        mostPages = std::max(mostPages, stream->pagesLoaded());
    }
    CHECK(Rt::allocationsInRt.load() == before);
    CHECK(e.streamMisses() == 0);
    bool same = true;
    for (int i = 0; i < total - block; ++i) same = same && l[(size_t)i] == src->ch[0][(size_t)i] && r[(size_t)i] == src->ch[1][(size_t)i];
    CHECK(same);
    // only a window of the file is ever in memory
    const int allPages = (int)(src->frames() / 4096) + 1;
    CHECK_MSG(mostPages < allPages / 2, "%d of %d pages were in memory at once", mostPages, allPages);

    // a jump to a place that is not loaded plays silence (counted) instead of blocking, and recovers once the loader has run
    e.stop();
    e.seek(960.0 * 12);                      // six seconds in
    for (int i = 0; i < 700; ++i) stream->service();      // long enough for the old pages to be let go
    e.play();
    std::vector<float> a((size_t)block), b((size_t)block);
    float* out[2] = {a.data(), b.data()};
    const int64 missesBefore = e.streamMisses();
    const int64 allocBefore = Rt::allocationsInRt.load();
    e.process(nullptr, 0, out, 2, block);
    CHECK(e.streamMisses() > missesBefore);
    CHECK(Rt::allocationsInRt.load() == allocBefore);
    CHECK(peakOf(a) == 0.0f);
    stream->service();
    e.process(nullptr, 0, out, 2, block);
    CHECK(peakOf(a) > 0.05f);
    // prefetching before the jump avoids the gap altogether
    e.stop();
    for (int i = 0; i < 700; ++i) stream->service();
    e.seek(960.0 * 4);
    p.prefetchAudio(960.0 * 4);
    stream->service();
    e.play();
    const int64 m2 = e.streamMisses();
    e.process(nullptr, 0, out, 2, block);
    CHECK(e.streamMisses() == m2);
    CHECK(peakOf(a) > 0.05f);
}

TEST(resampling_moves_pitch_and_rejects_aliases) {
    auto tone = sine(1000.0, 1.0, kSr, 0.5f);
    auto slow = stretch::resample(*tone, 2.0);          // twice as long, an octave down
    CHECK(slow->frames() == 96000);
    CHECK_NEAR(measureHz(slow->ch[0], 4000, 90000, kSr), 500.0, 0.05);
    CHECK_NEAR(peakOf(slow->ch[0], 4000, 90000), 0.5, 0.003);
    auto fast = stretch::resample(*tone, 0.5);
    CHECK(fast->frames() == 24000);
    CHECK_NEAR(measureHz(fast->ch[0], 2000, 22000, kSr), 2000.0, 0.2);
    CHECK_NEAR(peakOf(fast->ch[0], 2000, 22000), 0.5, 0.003);
    // 15 kHz read twice as fast would be 30 kHz, which cannot exist at 48 kHz: it must be filtered out, not folded to 18 kHz
    auto high = sine(15000.0, 0.5, kSr, 0.5f);
    auto folded = stretch::resample(*high, 0.5);
    CHECK_MSG(peakOf(folded->ch[0], 500, 11000) < 0.5f * 0.002f, "alias level %g", peakOf(folded->ch[0], 500, 11000));
    // a ratio of exactly one gives the audio back
    auto same = stretch::resample(*tone, 1.0);
    CHECK(maxAbsDiff(same->ch[0], tone->ch[0]) < 1e-5);
}

TEST(tonal_stretch_keeps_pitch_and_level) {
    auto tone = sine(440.0, 2.0, kSr, 0.5f);
    for (double ratio : {1.5, 0.75, 2.0, 1.07}) {
        auto out = stretch::tonal(*tone, ratio);
        const int64 want = (int64)std::llround(96000 * ratio);
        CHECK(out->frames() == want);
        CHECK_MSG(std::fabs(measureHz(out->ch[0], 6000, (size_t)(want - 6000), kSr) - 440.0) < 0.5, "ratio %.2f: %.2f Hz", ratio, measureHz(out->ch[0], 6000, (size_t)(want - 6000), kSr));
        const double rms = rmsOf(out->ch[0], 6000, (size_t)(want - 6000));
        CHECK_MSG(std::fabs(20.0 * std::log10(rms / (0.5 / std::sqrt(2.0)))) < 0.5, "ratio %.2f: level off by %.2f dB", ratio, 20.0 * std::log10(rms / (0.5 / std::sqrt(2.0))));
    }
    // a chord stays a chord: each partial is still there at its own frequency
    auto chord = std::make_shared<SampleData>();
    chord->sampleRate = kSr; chord->channels = 2;
    for (int c = 0; c < 2; ++c) {
        chord->ch[c].resize(96000);
        for (int i = 0; i < 96000; ++i)
            chord->ch[c][(size_t)i] = 0.2f * (float)(std::sin(kTwoPi * 261.63 * i / kSr) + std::sin(kTwoPi * 329.63 * i / kSr) + std::sin(kTwoPi * 392.0 * i / kSr));
    }
    auto out = stretch::tonal(*chord, 1.3);
    CHECK(out->channels == 2);
    const int N = 32768;
    std::vector<float> re((size_t)N), im((size_t)N, 0.0f);
    for (int i = 0; i < N; ++i) re[(size_t)i] = out->ch[0][(size_t)(20000 + i)] * (float)(0.5 - 0.5 * std::cos(kTwoPi * i / N));
    dsp::fft(re.data(), im.data(), N, false);
    auto level = [&](double hz) {
        const int k = (int)std::lround(hz * N / kSr);
        float m = 0.0f;
        for (int j = k - 2; j <= k + 2; ++j) m = std::max(m, std::sqrt(re[(size_t)j] * re[(size_t)j] + im[(size_t)j] * im[(size_t)j]));
        return m / (N / 4.0f);
    };
    for (double hz : {261.63, 329.63, 392.0}) CHECK_MSG(std::fabs(level(hz) - 0.2f) < 0.03f, "%.1f Hz came out at %.3f", hz, level(hz));
    CHECK(level(300.0) < 0.01f && level(360.0) < 0.01f && level(500.0) < 0.01f);
}

TEST(rhythmic_stretch_keeps_hits_sharp_and_in_place) {
    auto loop = clickLoop(120.0, 8);         // a hit every 24000 samples
    for (double ratio : {1.25, 0.8, 1.5}) {
        auto out = stretch::rhythmic(*loop, ratio);
        CHECK(out->frames() == (int64)std::llround(loop->frames() * ratio));
        for (int b = 0; b < 8; ++b) {
            const int64 expect = (int64)std::llround(b * 24000 * ratio);
            const int64 on = onsetAfter(out->ch[0], expect - 1200, 0.12f);
            CHECK_MSG(on >= 0 && std::llabs(on - expect) < 600, "ratio %.2f beat %d: hit at %lld, expected %lld", ratio, b, (long long)on, (long long)expect);
            // still a hit, not a smear: it is loud at once and gone 40 ms later
            if (on >= 0) {
                CHECK(peakOf(out->ch[0], (size_t)on, (size_t)on + 400) > 0.3f);
                CHECK(peakOf(out->ch[0], (size_t)on + 2600, (size_t)on + 6000) < 0.05f);
            }
        }
    }
    // pitch is untouched: a steady tone comes out at the same frequency
    auto tone = sine(330.0, 1.5, kSr, 0.5f);
    auto t2 = stretch::rhythmic(*tone, 1.4);
    CHECK_NEAR(measureHz(t2->ch[0], 3000, 90000, kSr), 330.0, 1.0);
    CHECK(peakOf(t2->ch[0], 3000, 90000) < 0.6f && rmsOf(t2->ch[0], 3000, 90000) > 0.3);
    // no stretch at all is the identity
    auto same = stretch::rhythmic(*loop, 1.0);
    CHECK(maxAbsDiff(same->ch[0], loop->ch[0]) < 1e-6);
}

TEST(pitch_shift_keeps_length) {
    auto tone = sine(440.0, 1.5, kSr, 0.5f);
    for (int mode : {(int)stretch::Rhythmic, (int)stretch::Tonal}) {
        for (double semis : {7.0, -5.0, 12.0}) {
            auto out = stretch::process(*tone, mode, 1.0, semis);
            CHECK(out->frames() == tone->frames());
            const double want = 440.0 * std::pow(2.0, semis / 12.0);
            const double got = measureHz(out->ch[0], 6000, 66000, kSr);
            CHECK_MSG(std::fabs(got / want - 1.0) < 0.004, "mode %d, %+.0f semitones: %.2f Hz, expected %.2f", mode, semis, got, want);
        }
    }
    // in resample mode pitch and length are one control
    auto up = stretch::process(*tone, stretch::Resample, 1.0, 12.0);
    CHECK(up->frames() == tone->frames() / 2);
    CHECK_NEAR(measureHz(up->ch[0], 2000, 34000, kSr), 880.0, 0.2);
    CHECK_NEAR(stretch::lengthRatio(stretch::Resample, 1.0, 12.0), 0.5, 1e-12);
    CHECK_NEAR(stretch::lengthRatio(stretch::Tonal, 1.3, 12.0), 1.3, 1e-12);
}

TEST(tempo_is_detected_from_audio) {
    // loops cut to whole bars give their tempo exactly
    for (double bpm : {90.0, 120.0, 128.0, 140.0, 174.0}) {
        auto loop = clickLoop(bpm, 8);
        double conf = 0.0;
        const double got = stretch::detectTempo(*loop, &conf);
        CHECK_MSG(std::fabs(got - bpm) < 0.02, "%.0f BPM loop detected as %.3f (confidence %.2f)", bpm, got, conf);
    }
    // a recording that is not cut to the bar is still within half a beat per minute
    for (double bpm : {97.0, 133.0}) {
        auto loop = clickLoop(bpm, 13);
        loop->ch[0].resize(loop->ch[0].size() - 9000);
        const double got = stretch::detectTempo(*loop);
        CHECK_MSG(std::fabs(got - bpm) < 0.5, "%.0f BPM detected as %.3f", bpm, got);
    }
    // 44.1 kHz works the same
    CHECK_NEAR(stretch::detectTempo(*clickLoop(110.0, 8, 44100.0)), 110.0, 0.05);
    // steady noise and silence have no tempo
    CHECK(stretch::detectTempo(*noise(3.0, 1)) == 0.0);
    auto quiet = std::make_shared<SampleData>();
    quiet->sampleRate = kSr; quiet->ch[0].assign(96000, 0.0f);
    CHECK(stretch::detectTempo(*quiet) == 0.0);
}

TEST(a_synced_clip_follows_the_project_tempo) {
    auto loop = clickLoop(100.0, 4);         // one bar at 100 BPM
    for (int mode : {(int)stretch::Rhythmic, (int)stretch::Tonal, (int)stretch::Resample}) {
        Project p;
        p.global->tempo.set(125.0f);
        p.songMode = true;
        const int id = p.addAudio(loop, "loop");
        CHECK_NEAR(p.detectAudioTempo(id), 100.0, 0.02);
        p.addAudioClip(id, 0, 960);          // starts on beat two
        CHECK(p.clips[0].length == p.secondsToTicks(2.4));
        p.setClipStretch(0, mode, true, 1.0f, 0.0f);
        // one bar of audio is one bar of song at any tempo
        CHECK_MSG(std::llabs(p.clips[0].length - 3840) <= 1, "length %lld", (long long)p.clips[0].length);
        CHECK_NEAR(p.clipLengthRatio(p.clips[0]), 0.8, 1e-4);
        auto out = render(p, (int64)(3.0 * kSr));
        const double beat = 60.0 / 125.0 * kSr;
        for (int b = 0; b < 4; ++b) {
            const int64 expect = (int64)std::llround((b + 1) * beat);
            const int64 on = onsetAfter(out->ch[0], expect - 1500, 0.1f);
            CHECK_MSG(on >= 0 && std::llabs(on - expect) < 600, "mode %d beat %d: hit at %lld, expected %lld", mode, b, (long long)on, (long long)expect);
        }
        // changing the tempo re-fits the clip: at 80 BPM the same four hits land on 80 BPM beats
        p.global->tempo.set(80.0f);
        auto slow = render(p, (int64)(4.5 * kSr));
        const double beat80 = 60.0 / 80.0 * kSr;
        for (int b = 0; b < 4; ++b) {
            const int64 expect = (int64)std::llround((b + 1) * beat80);
            const int64 on = onsetAfter(slow->ch[0], expect - 1500, 0.1f);
            CHECK_MSG(on >= 0 && std::llabs(on - expect) < 800, "mode %d at 80 BPM, beat %d: hit at %lld, expected %lld", mode, b, (long long)on, (long long)expect);
        }
    }
}

TEST(stretched_clips_save_reload_and_do_not_depend_on_block_size) {
    namespace fs = std::filesystem;
    const std::string dir = t::tmpDir() + "/stretchproj";
    fs::remove_all(dir);
    fs::create_directories(dir);
    writeWavFile(dir + "/loop.wav", *clickLoop(100.0, 8), WavFormat::Float32);
    writeWavFile(dir + "/tone.wav", *sine(220.0, 2.0, kSr, 0.4f), WavFormat::Float32);
    auto build = [&](Project& p) {
        p.global->tempo.set(130.0f);
        p.songMode = true;
        p.projectDir = dir;
        const int loopId = p.addAudio(dir + "/loop.wav");
        const int toneId = p.addAudio(dir + "/tone.wav");
        p.detectAudioTempo(loopId);
        p.addAudioClip(loopId, 0, 0, 1);
        p.setClipStretch(0, stretch::Rhythmic, true, 1.0f, 0.0f);
        p.addAudioClip(toneId, 1, 480, 2);
        p.setClipStretch(1, stretch::Tonal, false, 1.5f, 700.0f);
        p.clips[1].sourceOffsetFrames = 4800;
        p.addAudioClip(toneId, 2, 960, 0);
        p.setClipStretch(2, stretch::Resample, false, 1.0f, -1200.0f);
    };
    std::shared_ptr<SampleData> ref;
    for (auto cs : std::vector<std::pair<int, int>>{{512, 1}, {29, 1}, {64, 3}}) {
        Project p;
        build(p);
        auto out = render(p, (int64)(4.0 * kSr), cs.first, cs.second);
        if (!ref) { ref = out; CHECK(peakOf(out->ch[0]) > 0.1f); continue; }
        CHECK_MSG(identical(*ref, *out), "block %d threads %d differs", cs.first, cs.second);
    }
    Project a, b;
    build(a);
    // the tone clip: a fifth up and half as long again, and the octave-down one twice as long
    CHECK(a.clips[1].length == (int64)std::llround(a.secondsToTicks(2.0) * 1.5));
    CHECK(std::llabs(a.clips[2].length - a.secondsToTicks(2.0) * 2) <= 1);
    CHECK(a.save(dir + "/song.pulp"));
    std::string err;
    CHECK_MSG(b.load(dir + "/song.pulp", &err), "%s", err.c_str());
    CHECK(b.clips.size() == 3 && b.clips[0].sync && b.clips[0].stretchMode == stretch::Rhythmic);
    CHECK(b.clips[1].stretchMode == stretch::Tonal && b.clips[1].timeMul == 1.5f && b.clips[1].pitchCents == 700.0f);
    CHECK_NEAR(b.audios[0].bpm, 100.0, 0.02);
    CHECK(identical(*render(a, (int64)(4.0 * kSr)), *render(b, (int64)(4.0 * kSr))));
    // the pitched clip really is a fifth up
    Project solo;
    build(solo);
    solo.clips[0].muted = true;
    solo.clips[2].muted = true;
    auto out = render(solo, (int64)(3.0 * kSr));
    CHECK_NEAR(measureHz(out->ch[0], 30000, 100000, kSr) / (220.0 * std::pow(2.0, 7.0 / 12.0)), 1.0, 0.004);
}
