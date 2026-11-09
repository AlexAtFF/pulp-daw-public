// Milestone 1: files, clock, sequencer, sampler.
#include "Util.h"
using namespace pulp;
using namespace tu;

TEST(json_round_trip) {
    Json j = Json::object();
    j.set("name", "PULP \"quoted\" \\ and\nnewline");
    j.set("pi", 3.14159265358979);
    j.set("f", 0.78f);
    j.set("n", -42);
    j.set("yes", true);
    Json arr = Json::array();
    arr.push(1); arr.push(2.5); arr.push("three"); arr.push(Json());
    j.set("arr", arr);
    Json inner = Json::object();
    inner.set("k", Json::array());
    j.set("obj", inner);
    for (int indent : {0, 2}) {
        std::string err;
        Json back = Json::parse(j.dump(indent), &err);
        CHECK(err.empty());
        CHECK(back["name"].str() == j["name"].str());
        CHECK(back["pi"].num() == 3.14159265358979);
        CHECK(back["f"].f() == 0.78f);
        CHECK(back["n"].i() == -42);
        CHECK(back["yes"].b());
        CHECK(back["arr"].size() == 4);
        CHECK(back["arr"].at(2).str() == "three");
        CHECK(back["arr"].at(3).isNull());
        CHECK(back["obj"]["k"].isArray());
    }
    std::string err;
    Json::parse("{\"a\": [1, 2", &err);
    CHECK(!err.empty());
}

TEST(wav_round_trip) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = 44100;
    s->channels = 2;
    for (int i = 0; i < 1000; ++i) {
        s->ch[0].push_back(0.9f * (float)std::sin(i * 0.05));
        s->ch[1].push_back(-0.5f * (float)std::cos(i * 0.021));
    }
    struct { WavFormat fmt; double tol; } cases[] = {{WavFormat::Float32, 0.0}, {WavFormat::Pcm24, 1.0 / 8388608.0}, {WavFormat::Pcm16, 1.0 / 32768.0}};
    for (auto& c : cases) {
        auto bytes = encodeWav(*s, c.fmt);
        std::string err;
        auto back = readWavMemory(bytes.data(), bytes.size(), &err);
        CHECK(back != nullptr);
        if (!back) continue;
        CHECK(back->channels == 2);
        CHECK(back->sampleRate == 44100);
        CHECK(back->frames() == 1000);
        CHECK(maxAbsDiff(back->ch[0], s->ch[0]) <= c.tol);
        CHECK(maxAbsDiff(back->ch[1], s->ch[1]) <= c.tol);
    }
    const std::string path = t::tmpDir() + "/roundtrip.wav";
    CHECK(writeWavFile(path, *s, WavFormat::Pcm24));
    auto f = readWavFile(path);
    CHECK(f && f->frames() == 1000);
}

// The milestone proof: four-on-the-floor, every hit on exactly the right sample.
static void kickPatternCase(double bpm, double sr) {
    auto kick = drums::makeKick({}, sr);
    Project p;
    p.sampleRate = sr;
    p.global->tempo.set((float)bpm);
    p.global->masterVolume.set(0.8f);
    auto ch = addSampleChannel(p, kick, "Kick");
    auto& notes = p.pattern().notesFor(ch.id);
    for (int step : {0, 4, 8, 12}) Project::setStep(notes, step, true);

    const double spt = samplesPerTick((double)p.global->tempo.get(), sr);
    RenderOptions o;
    o.sampleRate = sr;
    o.lengthSamples = (int64)std::llround(16 * kTicksPerStep * spt) + kick->frames();
    auto out = renderOffline(p, o);
    CHECK(out != nullptr);
    if (!out) return;

    std::vector<float> expected((size_t)o.lengthSamples, 0.0f);
    for (int step : {0, 4, 8, 12}) {
        const int64 onset = (int64)std::llround(step * kTicksPerStep * spt);
        for (int64 i = 0; i < kick->frames() && onset + i < o.lengthSamples; ++i)
            expected[(size_t)(onset + i)] += kick->ch[0][(size_t)i];
        // The sample before each onset must not contain the new hit's click.
        CHECK_MSG(std::fabs(out->ch[0][(size_t)onset] - expected[(size_t)onset]) < 1.0e-6, "bpm %.2f sr %.0f step %d", bpm, sr, step);
    }
    const double d = maxAbsDiff(out->ch[0], expected);
    CHECK_MSG(d < 1.0e-6, "bpm %.2f sr %.0f: max difference %g", bpm, sr, d);
    CHECK(out->ch[0] == out->ch[1]);
    // A render shifted by one sample would not match: prove the comparison is sharp.
    std::vector<float> shifted(expected.size(), 0.0f);
    for (size_t i = 1; i < expected.size(); ++i) shifted[i] = expected[i - 1];
    CHECK(maxAbsDiff(out->ch[0], shifted) > 0.01);
}

TEST(kick_pattern_is_sample_accurate) {
    for (double bpm : {140.0, 60.0, 97.3, 174.25, 522.0, 33.0})
        kickPatternCase(bpm, 48000.0);
    kickPatternCase(140.0, 44100.0);
    kickPatternCase(128.0, 96000.0);
    kickPatternCase(10.0, 44100.0);
}

static Project* makeBeat(Project& p, double sr, double bpm) {
    p.sampleRate = sr;
    p.global->tempo.set((float)bpm);
    auto k = addSampleChannel(p, drums::makeKick({}, sr), "Kick");
    drums::KickSpec hi; hi.startHz = 900; hi.endHz = 300; hi.ampDecay = 0.05; hi.length = 0.12;
    auto h = addSampleChannel(p, drums::makeKick(hi, 44100.0), "Blip");      // different rate: uses interpolation
    h.live->pan.set(0.4f);
    h.live->volume.set(0.6f);
    for (int s : {0, 4, 8, 12}) Project::setStep(p.pattern().notesFor(k.id), s, true);
    for (int s : {2, 3, 6, 10, 11, 14, 15}) Project::setStep(p.pattern().notesFor(h.id), s, true);
    p.pattern().notesFor(h.id)[1].key = 67;
    p.pattern().notesFor(h.id)[2].pan = -0.7f;
    p.pattern().notesFor(h.id)[3].velocity = 0.4f;
    return &p;
}

TEST(render_is_identical_at_any_block_size) {
    std::shared_ptr<SampleData> ref;
    for (int block : {512, 1, 17, 64, 127, 256, 4096}) {
        Project p;
        makeBeat(p, 48000.0, 133.7);
        RenderOptions o;
        o.blockSize = block;
        o.lengthSamples = 48000 * 2;
        auto out = renderOffline(p, o);
        CHECK(out != nullptr);
        if (!out) return;
        if (!ref) { ref = out; CHECK(peakOf(out->ch[0]) > 0.3f); continue; }
        CHECK_MSG(identical(*ref, *out), "block size %d differs from 512", block);
    }
}

TEST(audio_thread_does_not_allocate) {
    Project p;
    makeBeat(p, 48000.0, 140.0);
    RenderOptions o;
    o.lengthSamples = 48000 * 3;
    o.loop = true;
    const int64 before = Rt::allocationsInRt.load();
    auto out = renderOffline(p, o);
    CHECK(out != nullptr);
    CHECK_MSG(Rt::allocationsInRt.load() == before, "%lld allocations inside the audio callback", (long long)(Rt::allocationsInRt.load() - before));
}

TEST(pattern_loops_on_the_exact_sample) {
    const double sr = 48000.0, bpm = 151.3;
    auto kick = drums::makeKick({}, sr);
    Project p;
    p.sampleRate = sr;
    p.global->tempo.set((float)bpm);
    auto ch = addSampleChannel(p, kick);
    Project::setStep(p.pattern().notesFor(ch.id), 0, true);
    Project::setStep(p.pattern().notesFor(ch.id), 6, true);
    const double spt = samplesPerTick((double)p.global->tempo.get(), sr);
    const int64 bar = (int64)std::llround(16 * kTicksPerStep * spt);
    RenderOptions o;
    o.loop = true;
    o.lengthSamples = bar * 3;
    auto out = renderOffline(p, o);
    CHECK(out != nullptr);
    if (!out) return;
    std::vector<float> expected((size_t)o.lengthSamples, 0.0f);
    for (int b = 0; b < 3; ++b)
        for (int step : {0, 6}) {
            const int64 onset = b * bar + (int64)std::llround(step * kTicksPerStep * spt);
            for (int64 i = 0; i < kick->frames() && onset + i < o.lengthSamples; ++i) expected[(size_t)(onset + i)] += kick->ch[0][(size_t)i];
        }
    CHECK_MSG(maxAbsDiff(out->ch[0], expected) < 1.0e-6, "difference %g", maxAbsDiff(out->ch[0], expected));
}

TEST(sampler_tuning) {
    const double c5 = keyToHz(60);
    struct { double srcRate, engRate; int key; } cases[] = {
        {48000, 48000, 60}, {48000, 48000, 72}, {48000, 48000, 48}, {48000, 48000, 67},
        {44100, 48000, 60}, {44100, 96000, 64}, {48000, 44100, 55}};
    for (auto& c : cases) {
        Project p;
        p.sampleRate = c.engRate;
        auto ch = addSampleChannel(p, sine(c5, 2.0, c.srcRate));
        Note n; n.key = c.key; n.length = kPPQ * 4;
        p.pattern().notesFor(ch.id).push_back(n);
        RenderOptions o;
        o.sampleRate = c.engRate;
        o.lengthSamples = (int64)(0.5 * c.engRate);
        auto out = renderOffline(p, o);
        CHECK(out != nullptr);
        if (!out) continue;
        const double hz = measureHz(out->ch[0], 200, (size_t)o.lengthSamples - 200, c.engRate);
        const double want = keyToHz(c.key);
        CHECK_MSG(std::fabs(1200.0 * std::log2(hz / want)) < 0.5, "key %d: %.3f Hz, expected %.3f Hz", c.key, hz, want);
    }
}

TEST(project_save_and_reload_renders_the_same) {
    const std::string dir = t::tmpDir() + "/proj1";
    auto kick = drums::makeKick({}, 48000.0);
    CHECK(writeWavFile(dir + "/kick.wav", *kick, WavFormat::Float32));
    Project a;
    a.projectDir = dir;
    a.name = "Round trip";
    a.global->tempo.set(123.0f);
    a.timeSigNum = 3;
    auto& ch = a.addSamplerChannel("kick.wav", "Kick");
    ch.live->volume.set(0.5f);
    ch.live->pan.set(-0.25f);
    ch.color = 0xff112233;
    static_cast<Sampler*>(ch.live->plugin.get())->pFine->set(12.0f);
    for (int s : {0, 3, 6, 9}) Project::setStep(a.pattern().notesFor(ch.id), s, true);
    a.pattern().notesFor(ch.id)[1].velocity = 0.5f;
    a.pattern().swing = 0.3f;
    a.addPattern("Second");
    a.clips.push_back({Clip::PatternClip, 2, 0, 3840, 0, a.patterns[0].id, false});
    a.tracks[2].name = "Drums";
    a.markers.push_back({1920, "Drop"});
    CHECK(a.save(dir + "/song.pulp"));

    Project b;
    std::string err;
    CHECK_MSG(b.load(dir + "/song.pulp", &err), "%s", err.c_str());
    CHECK(b.toJson() == a.toJson());
    CHECK(b.name == "Round trip");
    CHECK(b.channels.size() == 1 && b.patterns.size() == 2 && b.clips.size() == 1);
    CHECK(b.tracks[2].name == "Drums");

    RenderOptions o;
    o.lengthSamples = 48000;
    auto ra = renderOffline(a, o);
    auto rb = renderOffline(b, o);
    CHECK(ra && rb);
    if (ra && rb) {
        CHECK(peakOf(ra->ch[0]) > 0.1f);
        CHECK(identical(*ra, *rb));
    }
    // A damaged file is refused with a message, not a crash.
    CHECK(writeTextFile(dir + "/bad.pulp", "{\"format\": \"pulp-project\", \"version\": "));
    Project c;
    CHECK(!c.load(dir + "/bad.pulp", &err));
    CHECK(!err.empty());
}

TEST(live_notes_sound_while_stopped) {
    Project p;
    auto ch = addSampleChannel(p, drums::makeKick({}, 48000.0));
    (void)ch;
    p.prepare(48000.0);
    Engine e;
    e.prepare(48000.0);
    e.setState(p.compile());
    std::vector<float> l(4800), r(4800);
    float* out[2] = {l.data(), r.data()};
    e.process(nullptr, 0, out, 2, 4800);
    CHECK(peakOf(l) == 0.0f);
    e.noteOn(0, 60, 0.8f);
    e.process(nullptr, 0, out, 2, 4800);
    CHECK(peakOf(l) > 0.3f);
    CHECK(!e.isPlaying());
    CHECK(e.positionTicks() == 0.0);
}
