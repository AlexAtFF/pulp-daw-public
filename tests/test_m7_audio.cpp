// Milestone 7 (core): audio clips on the playlist, hardware input, monitoring, count-in, recording.
#include "Util.h"
#include "pulp/Effects.h"
#include <filesystem>
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0, kBpm = 120.0;
int64 at(double ticks) { return (int64)std::llround(ticks * samplesPerTick(kBpm, kSr)); }

std::shared_ptr<SampleData> noiseClip(double seconds, double sr = kSr, uint32 seed = 77u) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = 2;
    dsp::Noise a(seed), b(seed * 3 + 1);
    const size_t n = (size_t)(seconds * sr);
    s->ch[0].resize(n); s->ch[1].resize(n);
    for (size_t i = 0; i < n; ++i) { s->ch[0][i] = 0.3f * a.next(); s->ch[1][i] = 0.3f * b.next(); }
    return s;
}
void song(Project& p) { p.global->tempo.set((float)kBpm); p.songMode = true; }
std::shared_ptr<SampleData> render(Project& p, int64 len, int block = 512, int threads = 1) {
    RenderOptions o;
    o.lengthSamples = len;
    o.blockSize = block;
    o.threads = threads;
    return renderOffline(p, o);
}
}

TEST(audio_clip_plays_on_the_exact_sample) {
    auto src = noiseClip(0.5);
    Project p;
    song(p);
    const int id = p.addAudio(src, "noise");
    p.addAudioClip(id, 0, 960);
    CHECK(p.clips[0].length == p.secondsToTicks(0.5));
    auto out = render(p, 48000);
    const int64 on = at(960);
    CHECK(peakOf(out->ch[0], 0, (size_t)on) == 0.0f);
    bool same = true;
    for (int64 i = 0; i < 20000; ++i) same = same && out->ch[0][(size_t)(on + i)] == src->ch[0][(size_t)i] && out->ch[1][(size_t)(on + i)] == src->ch[1][(size_t)i];
    CHECK(same);
    CHECK(peakOf(out->ch[0], (size_t)(on + 24000 + 10), 48000) == 0.0f);

    // gain, a mixer track with its fader, and a head offset in frames
    p.clips[0].gain = 0.5f;
    p.clips[0].mixerTrack = 4;
    p.mixer[4].live->pan.set(-1.0f);
    p.clips[0].sourceOffsetFrames = 1000;
    auto o2 = render(p, 48000);
    double worst = 0.0;
    for (int64 i = 400; i < 20000; ++i) worst = std::max(worst, (double)std::fabs(o2->ch[0][(size_t)(on + i)] - 0.5f * src->ch[0][(size_t)(i + 1000)]));
    CHECK_MSG(worst < 1e-7, "difference %g", worst);
    CHECK(peakOf(o2->ch[1]) < 1e-7f);
    // a clip that starts in the middle of its file fades in over 2 ms instead of clicking
    CHECK(std::fabs(o2->ch[0][(size_t)on]) < 1e-6f);
    // pattern mode does not play the playlist's audio
    p.songMode = false;
    CHECK(peakOf(render(p, 48000)->ch[0]) == 0.0f);
}

TEST(audio_clip_trim_fades_reverse_and_resampling) {
    auto tone = sine(1000, 1.0, kSr, 0.5f);
    {   // shortened clip stops, with fades at both ends
        Project p;
        song(p);
        const int id = p.addAudio(tone, "tone");
        auto& c = p.addAudioClip(id, 0, 0);
        c.length = 960;                 // half a second
        c.fadeIn = 240; c.fadeOut = 240;
        auto out = render(p, 36000);
        CHECK(peakOf(out->ch[0], (size_t)at(960) + 2, 36000) == 0.0f);
        CHECK(peakOf(out->ch[0], 0, 600) < 0.06f);                      // fading in
        CHECK_NEAR(peakOf(out->ch[0], 9000, 15000), 0.5, 0.01);         // full level in the middle
        CHECK(peakOf(out->ch[0], (size_t)at(960) - 600, (size_t)at(960)) < 0.06f);
    }
    {   // slip offset in ticks: the clip starts one beat into the audio
        Project p;
        song(p);
        auto src = noiseClip(1.0);
        const int id = p.addAudio(src, "n");
        auto& c = p.addAudioClip(id, 0, 0);
        c.offset = 960;
        auto out = render(p, 24000);
        double worst = 0.0;
        for (int64 i = 400; i < 20000; ++i) worst = std::max(worst, (double)std::fabs(out->ch[0][(size_t)i] - src->ch[0][(size_t)(i + at(960))]));
        CHECK(worst < 1e-7);
    }
    {   // reverse plays the file backwards
        Project p;
        song(p);
        auto src = noiseClip(0.25);
        const int id = p.addAudio(src, "n");
        p.addAudioClip(id, 0, 0).reverse = true;
        auto out = render(p, 12000);
        bool same = true;
        for (size_t i = 0; i < 11000; ++i) same = same && out->ch[0][i] == src->ch[0][src->ch[0].size() - 1 - i];
        CHECK(same);
    }
    {   // a 44.1 kHz file keeps its pitch at 48 kHz
        Project p;
        song(p);
        const int id = p.addAudio(sine(441, 1.0, 44100.0, 0.5f), "t");
        p.addAudioClip(id, 0, 0);
        auto out = render(p, 40000);
        CHECK_NEAR(measureHz(out->ch[0], 2000, 38000, kSr), 441.0, 0.05);
    }
}

TEST(audio_clips_save_reload_loop_and_stay_block_independent) {
    namespace fs = std::filesystem;
    const std::string dir = t::tmpDir() + "/audioproj";
    fs::remove_all(dir);
    fs::create_directories(dir);
    writeWavFile(dir + "/take.wav", *noiseClip(0.7), WavFormat::Float32);
    writeWavFile(dir + "/kick.wav", *drums::makeKick({}, kSr), WavFormat::Float32);
    auto build = [&](Project& p) {
        song(p);
        p.projectDir = dir;
        const int id = p.addAudio(dir + "/take.wav");
        CHECK(id != 0);
        auto& a = p.addAudioClip(id, 0, 480, 2);
        a.sourceOffsetFrames = 333; a.gain = 0.8f; a.fadeOut = 120;
        p.addAudioClip(id, 1, 3840 + 240, 3).reverse = true;
        const int kickId = p.addSamplerChannel(dir + "/kick.wav", "Kick").id;      // on disk, so the reloaded project has it too
        for (int s : {0, 4, 8, 12}) Project::setStep(p.patterns[0].notesFor(kickId), s, true);
        p.addPatternClip(0, 2, 0);
        p.addPatternClip(0, 2, 3840);
        p.setSlot(2, 0, std::make_shared<Grit>());      // latency on one audio track: compensation must hold
        p.loopStart = 0; p.loopEnd = 7680;
    };
    std::shared_ptr<SampleData> ref;
    for (auto cs : std::vector<std::pair<int, int>>{{512, 1}, {33, 1}, {64, 4}, {2048, 3}}) {
        Project p;
        build(p);
        RenderOptions o;
        o.blockSize = cs.first; o.threads = cs.second; o.loop = true; o.lengthSamples = at(7680) * 2 + 5000;
        const int64 before = Rt::allocationsInRt.load();
        auto out = renderOffline(p, o);
        CHECK(Rt::allocationsInRt.load() == before);
        if (!ref) { ref = out; CHECK(peakOf(out->ch[0]) > 0.2f); continue; }
        CHECK_MSG(identical(*ref, *out), "block %d threads %d differs", cs.first, cs.second);
    }
    // the loop repeats the clips: second pass equals the first
    const int64 loopLen = at(7680);
    double worst = 0.0;
    for (int64 i = 4000; i < loopLen - 100; ++i) worst = std::max(worst, (double)std::fabs(ref->ch[0][(size_t)i] - ref->ch[0][(size_t)(i + loopLen)]));
    CHECK_MSG(worst < 2e-3, "second pass differs by %g", worst);

    Project a, b;
    build(a);
    CHECK(a.save(dir + "/song.pulp"));
    std::string err;
    CHECK_MSG(b.load(dir + "/song.pulp", &err), "%s", err.c_str());
    CHECK(b.audios.size() == 1 && b.clips.size() == 4 && b.clips[0].sourceOffsetFrames == 333 && b.clips[1].reverse);
    RenderOptions o;
    auto ra = renderOffline(a, o), rb = renderOffline(b, o);
    CHECK(ra && rb && identical(*ra, *rb));
    CHECK(a.fileRefs().size() == 2);        // the take and the kick
}

TEST(wav_stream_writer_is_readable_before_and_after_closing) {
    const std::string path = t::tmpDir() + "/stream.wav";
    auto src = noiseClip(0.3);
    std::vector<float> inter(src->ch[0].size() * 2);
    for (size_t i = 0; i < src->ch[0].size(); ++i) { inter[i * 2] = src->ch[0][i]; inter[i * 2 + 1] = src->ch[1][i]; }
    WavStreamWriter w;
    CHECK(w.open(path, kSr, 2));
    CHECK(w.write(inter.data(), 5000));
    w.flush();
    // mid-take (as after a crash): the file already holds what was flushed
    auto partial = readWavFile(path);
    CHECK(partial && partial->frames() == 5000 && partial->channels == 2);
    if (partial) CHECK(partial->ch[1][4999] == src->ch[1][4999]);
    CHECK(w.write(inter.data() + 10000, (int)src->ch[0].size() - 5000));
    w.close();
    auto full = readWavFile(path);
    CHECK(full && full->frames() == (int64)src->ch[0].size());
    if (full) CHECK(full->ch[0] == src->ch[0]);
    // a mono 24-bit take
    WavStreamWriter m;
    CHECK(m.open(t::tmpDir() + "/stream24.wav", 44100.0, 1, WavFormat::Pcm24));
    CHECK(m.write(src->ch[0].data(), 1000));
    m.close();
    auto f24 = readWavFile(t::tmpDir() + "/stream24.wav");
    CHECK(f24 && f24->frames() == 1000 && f24->sampleRate == 44100.0);
    if (f24) CHECK(std::fabs(f24->ch[0][500] - src->ch[0][500]) < 1.0f / 8000000.0f);
}

TEST(input_monitoring_follows_the_track_setting) {
    auto run = [](int input, bool monitor, std::vector<float>& l, std::vector<float>& r) {
        Project p;
        p.mixer[3].input = input;
        p.mixer[3].monitor = monitor;
        p.prepare(kSr);
        Engine e;
        e.prepare(kSr);
        e.setState(p.compile());
        const int n = 9600;
        std::vector<float> a((size_t)n), b((size_t)n);
        for (int i = 0; i < n; ++i) { a[(size_t)i] = 0.4f * (float)std::sin(kTwoPi * 300.0 * i / kSr); b[(size_t)i] = 0.2f * (float)std::sin(kTwoPi * 700.0 * i / kSr); }
        l.assign((size_t)n, 0.0f); r.assign((size_t)n, 0.0f);
        const float* in[2] = {a.data(), b.data()};
        float* out[2] = {l.data(), r.data()};
        for (int i = 0; i < n; i += 240) {
            const float* ib[2] = {in[0] + i, in[1] + i};
            float* ob[2] = {out[0] + i, out[1] + i};
            e.process(ib, 2, ob, 2, 240);
        }
        CHECK(e.inputPeak(0) > 0.39f && e.inputPeak(1) > 0.19f);
        return a;
    };
    std::vector<float> l, r;
    auto a = run(1, true, l, r);         // input 1, mono: the same on both sides, late by exactly the input delay
    bool exact = true;
    for (size_t i = Engine::kInputDelay; i < l.size(); ++i) exact = exact && l[i] == a[i - Engine::kInputDelay] && r[i] == l[i];
    CHECK(exact);
    run(2, true, l, r);                  // input 2, mono
    CHECK_NEAR(measureHz(l, 200, 9000, kSr), 700.0, 1.0);
    CHECK(l == r);
    run(3, true, l, r);                  // stereo: 1 left, 2 right
    CHECK_NEAR(measureHz(l, 200, 9000, kSr), 300.0, 1.0);
    CHECK_NEAR(measureHz(r, 200, 9000, kSr), 700.0, 1.0);
    run(1, false, l, r);                 // direct monitoring: PULP stays silent
    CHECK(peakOf(l) == 0.0f && peakOf(r) == 0.0f);
}

TEST(count_in_clicks_then_starts_on_the_bar) {
    Project p;
    p.global->tempo.set((float)kBpm);
    auto click = std::make_shared<SampleData>();
    click->sampleRate = kSr; click->channels = 1; click->ch[0].assign(64, 0.0f); click->ch[0][0] = 0.9f;
    auto ch = addSampleChannel(p, click);
    Project::setStep(p.pattern().notesFor(ch.id), 0, true);
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setState(p.compile());
    e.startRecording();
    e.play(3840.0);                      // one bar of count-in
    const int n = 110000;
    std::vector<float> l((size_t)n), r((size_t)n);
    float* out[2] = {l.data(), r.data()};
    e.process(nullptr, 0, out, 2, n);
    const int64 bar = at(3840);
    // four count-in clicks, the first one higher
    for (int beat = 0; beat < 4; ++beat) CHECK_MSG(peakOf(l, (size_t)(beat * 24000), (size_t)(beat * 24000 + 300)) > 0.1f, "no count-in click on beat %d", beat);
    CHECK(measureHz(l, 0, 700, kSr) > measureHz(l, 24000, 24700, kSr) * 1.3);
    // then the pattern's own note exactly on the bar, and no more clicks (the metronome itself is off)
    CHECK_NEAR(l[(size_t)bar], 0.9, 1e-6);
    CHECK(peakOf(l, (size_t)bar + 200, (size_t)bar + 12000) < 1e-4f);
    // recording began only when the count-in was over
    CHECK(e.recordStarted());
    CHECK_MSG(e.recordStartTick() >= 0.0 && e.recordStartTick() < 64.0 / samplesPerTick(kBpm, kSr) + 1e-6, "recording started at tick %.2f", e.recordStartTick());
    std::vector<float> rec((size_t)n * 2);
    const int got = e.recordRead(rec.data(), n);
    CHECK_MSG(std::abs(got - (int)(n - bar)) <= 128, "recorded %d frames, expected about %d", got, (int)(n - bar));
}

TEST(count_in_survives_a_mode_switch_in_the_same_block) {
    // Recording switches to song mode and presses play in one go; the count-in must not be lost.
    Project p;
    p.global->tempo.set((float)kBpm);
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setState(p.compile());
    std::vector<float> l(4800), r(4800);
    float* out[2] = {l.data(), r.data()};
    e.process(nullptr, 0, out, 2, 4800);        // running in pattern mode
    p.songMode = true;
    e.setState(p.compile());
    e.startRecording();
    e.play(3840.0);
    e.process(nullptr, 0, out, 2, 4800);
    CHECK_MSG(e.positionTicks() < -3000.0, "position %.1f: the count-in was dropped", e.positionTicks());
    CHECK(!e.recordStarted());
}

TEST(a_recorded_take_lands_exactly_where_it_was_performed) {
    // The performer hears the engine through `outLat` samples of output latency and the microphone reaches the
    // engine `inLat` samples late. A performance p[] made in time with the song therefore arrives as in[c] = p[c - L].
    // Recorded and placed with the right offset, the take must play back as p[c] exactly.
    for (int latency : {0, 137, 1024}) {
        auto perf = noiseClip(1.2, kSr, 9u);
        Project p;
        song(p);
        p.mixer[1].input = 1;
        p.prepare(kSr);
        Engine e;
        e.prepare(kSr);
        e.setState(p.compile());
        e.startRecording();
        e.play();
        const int total = 48000, block = 256;
        std::vector<float> in0((size_t)block), in1((size_t)block, 0.0f), o0((size_t)block), o1((size_t)block);
        std::vector<float> take, chunk((size_t)8192 * 2);
        for (int c = 0; c < total; c += block) {
            for (int i = 0; i < block; ++i) { const int src = c + i - latency; in0[(size_t)i] = src >= 0 ? perf->ch[0][(size_t)src] : 0.0f; }
            const float* in[2] = {in0.data(), in1.data()};
            float* out[2] = {o0.data(), o1.data()};
            e.process(in, 2, out, 2, block);
            for (int got; (got = e.recordRead(chunk.data(), 8192)) > 0;)
                for (int i = 0; i < got; ++i) take.push_back(chunk[(size_t)i * 2]);         // input 1, mono
        }
        e.stopRecording();
        CHECK(e.recordDropped() == 0);
        CHECK(e.recordStartTick() == 0.0);
        CHECK((int)take.size() >= total - 64);

        // place the take: it starts where recording started, skipping the frames that are only latency
        auto rec = std::make_shared<SampleData>();
        rec->sampleRate = kSr; rec->channels = 1; rec->ch[0] = take;
        const int id = p.addAudio(rec, "take");
        auto& clip = p.addAudioClip(id, 0, (int64)e.recordStartTick(), 1);
        clip.sourceOffsetFrames = Engine::kInputDelay + latency;
        auto out = render(p, total - 4000);
        double worst = 0.0;
        for (size_t i = 300; i < out->ch[0].size() - 200; ++i) worst = std::max(worst, (double)std::fabs(out->ch[0][i] - perf->ch[0][i]));
        CHECK_MSG(worst < 1e-7, "latency %d: the take is off by %g", latency, worst);
        // and it would be audibly wrong without the compensation
        p.clips[0].sourceOffsetFrames = 0;
        auto late = render(p, total - 4000);
        CHECK(maxAbsDiff(late->ch[0], perf->ch[0]) > 0.1);
    }
}
