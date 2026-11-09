// Milestone 2: channel settings, step sequencer model, factory drums, preview.
#include "Util.h"
using namespace pulp;
using namespace tu;

namespace {
// A short click: one full-scale sample then silence, so onsets are unmistakable.
std::shared_ptr<SampleData> click(double sr = 48000.0, int len = 64) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = 1;
    s->ch[0].assign((size_t)len, 0.0f);
    s->ch[0][0] = 0.5f;
    return s;
}
// Sample positions where the signal jumps from silence to at least thr.
std::vector<int64> onsets(const std::vector<float>& d, float thr = 0.01f) {
    std::vector<int64> o;
    for (size_t i = 0; i < d.size(); ++i)
        if (std::fabs(d[i]) >= thr && (i == 0 || std::fabs(d[i - 1]) < thr * 0.1f)) o.push_back((int64)i);
    return o;
}
int64 stepSample(double step, double bpm, double sr) { return (int64)std::llround(step * kTicksPerStep * samplesPerTick(bpm, sr)); }
}

TEST(factory_drums_are_clean_and_reproducible) {
    auto a = drums::makeFactoryDrums(48000.0);
    auto b = drums::makeFactoryDrums(48000.0);
    CHECK(a.size() >= 25);
    CHECK(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        const auto& s = *a[i].audio;
        CHECK_MSG(s.peak() > 0.25f && s.peak() <= 0.9f, "%s peak %.3f", a[i].path.c_str(), s.peak());
        CHECK_MSG(std::fabs(s.ch[0].back()) < 1.0e-3f, "%s does not end at zero", a[i].path.c_str());
        CHECK_MSG(std::fabs(s.ch[0].front()) < 0.2f, "%s starts with a jump", a[i].path.c_str());
        CHECK(s.ch[0] == b[i].audio->ch[0]);
        CHECK(a[i].licence == "CC0-1.0");
        bool finite = true;
        for (float v : s.ch[0]) finite = finite && std::isfinite(v);
        CHECK(finite);
    }
    CHECK(sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(step_sequencer_model) {
    std::vector<Note> notes;
    Project::setStep(notes, 4, true);
    Project::setStep(notes, 0, true);
    Project::setStep(notes, 12, true);
    CHECK(notes.size() == 3 && notes[0].tick == 0 && notes[1].tick == 4 * kTicksPerStep);
    CHECK(Project::hasStep(notes, 4) && !Project::hasStep(notes, 5));
    CHECK(Project::isStepSequence(notes));
    Project::stepNote(notes, 4)->tick += 100;          // shifted within its step by the graph editor
    CHECK(Project::hasStep(notes, 4) && Project::isStepSequence(notes));
    Project::setStep(notes, 4, false);
    CHECK(notes.size() == 2 && !Project::hasStep(notes, 4));
    notes[0].length = 960;                              // a long note needs the piano roll
    CHECK(!Project::isStepSequence(notes));
}

TEST(cut_group_silences_the_other_channel) {
    const double sr = 48000.0, bpm = 120.0;
    Project p;
    p.global->tempo.set((float)bpm);
    auto open = addSampleChannel(p, sine(1000.0, 2.0, sr, 0.5f), "Open hat");
    auto closed = addSampleChannel(p, click(sr), "Closed hat");
    open.live->cutBy.set(1);
    closed.live->cut.set(1);
    closed.live->mute.set(0);
    closed.live->volume.set(0.0f);                      // only listen to the open hat
    Project::setStep(p.pattern().notesFor(open.id), 0, true);
    Project::setStep(p.pattern().notesFor(closed.id), 4, true);
    RenderOptions o;
    o.lengthSamples = 48000;
    auto out = renderOffline(p, o);
    CHECK(out != nullptr);
    if (!out) return;
    const int64 cutAt = stepSample(4, bpm, sr);
    CHECK(rmsOf(out->ch[0], 1000, (size_t)cutAt - 10) > 0.3);
    CHECK_MSG(peakOf(out->ch[0], (size_t)cutAt + 200, 48000) < 1.0e-4f, "still sounding after the cut: %g", peakOf(out->ch[0], (size_t)cutAt + 200, 48000));
    // The cut is a short fade, not a click: no sample-to-sample jump bigger than the sine's own slope.
    float jump = 0.0f;
    for (size_t i = (size_t)cutAt - 5; i < (size_t)cutAt + 200; ++i) jump = std::max(jump, std::fabs(out->ch[0][i] - out->ch[0][i - 1]));
    CHECK(jump < 0.08f);
}

TEST(polyphony_limit_and_mono) {
    const double sr = 48000.0, bpm = 120.0;
    for (int mode = 0; mode < 3; ++mode) {          // 0 unlimited, 1 max polyphony 1, 2 mono
        Project p;
        p.global->tempo.set((float)bpm);
        auto ch = addSampleChannel(p, sine(440.0, 2.0, sr, 0.25f));
        if (mode == 1) ch.live->poly.set(1);
        if (mode == 2) ch.live->mono.set(1);
        Project::setStep(p.pattern().notesFor(ch.id), 0, true);
        Project::setStep(p.pattern().notesFor(ch.id), 4, true);
        RenderOptions o;
        o.lengthSamples = 48000;
        auto out = renderOffline(p, o);
        CHECK(out != nullptr);
        if (!out) return;
        const double late = rmsOf(out->ch[0], 30000, 40000);
        const double one = 0.25 / std::sqrt(2.0);
        // Two overlapping sines of the same frequency are not simply double, so compare to a single voice.
        if (mode == 0) CHECK(std::fabs(late - one) > 0.02);
        else CHECK_MSG(std::fabs(late - one) < 0.005, "mode %d rms %.4f, expected one voice %.4f", mode, late, one);
    }
}

TEST(arpeggiator_walks_the_chord_in_time) {
    const double sr = 48000.0, bpm = 120.0;
    Project p;
    p.global->tempo.set((float)bpm);
    auto ch = addSampleChannel(p, sine(keyToHz(60), 3.0, sr, 0.3f));
    static_cast<Sampler*>(ch.live->plugin.get())->pEnvOn->set(1);
    static_cast<Sampler*>(ch.live->plugin.get())->pRelease->set(1.0f);
    ch.live->arpDir.set(1);         // up
    ch.live->arpTime.set(2.0f);     // two steps per note
    ch.live->arpGate.set(0.8f);
    for (int key : {60, 64, 67}) {
        Note n; n.key = key; n.length = 16 * kTicksPerStep;
        p.pattern().notesFor(ch.id).push_back(n);
    }
    RenderOptions o;
    o.lengthSamples = stepSample(16, bpm, sr);
    auto out = renderOffline(p, o);
    CHECK(out != nullptr);
    if (!out) return;
    const int expect[8] = {60, 64, 67, 60, 64, 67, 60, 64};
    for (int i = 0; i < 8; ++i) {
        const int64 a = stepSample(i * 2, bpm, sr), b = stepSample(i * 2 + 1.5, bpm, sr);
        const double hz = measureHz(out->ch[0], (size_t)a + 100, (size_t)b, sr);
        CHECK_MSG(std::fabs(1200.0 * std::log2(hz / keyToHz(expect[i]))) < 5.0, "arp step %d: %.2f Hz, expected key %d (%.2f Hz)", i, hz, expect[i], keyToHz(expect[i]));
        // gate 80%: silent in the last tenth of each arp step
        const int64 gapA = stepSample(i * 2 + 1.8, bpm, sr), gapB = stepSample(i * 2 + 2, bpm, sr) - 4;
        CHECK(peakOf(out->ch[0], (size_t)gapA, (size_t)gapB) < 0.01f);
    }
    // down and chord modes
    ch.live->arpDir.set(2);
    ch.live->arpChord.set(4);       // minor triad from the lowest held note
    ch.live->arpRange.set(2);
    ch.live->arpTime.set(1.0f);
    auto out2 = renderOffline(p, o);
    const int expect2[6] = {79, 75, 72, 67, 63, 60};
    for (int i = 0; i < 6; ++i) {
        const double hz = measureHz(out2->ch[0], (size_t)stepSample(i, bpm, sr) + 60, (size_t)stepSample(i + 0.8, bpm, sr), sr);
        CHECK_MSG(std::fabs(1200.0 * std::log2(hz / keyToHz(expect2[i]))) < 8.0, "down/minor step %d: %.2f Hz, expected key %d", i, hz, expect2[i]);
    }
}

TEST(echo_repeats_on_time_and_fades) {
    const double sr = 48000.0, bpm = 100.0;
    Project p;
    p.global->tempo.set((float)bpm);
    auto ch = addSampleChannel(p, click(sr));
    ch.live->echoCount.set(3);
    ch.live->echoTime.set(2.0f);
    ch.live->echoFeed.set(0.5f);
    Project::setStep(p.pattern().notesFor(ch.id), 1, true);
    RenderOptions o;
    o.lengthSamples = stepSample(16, bpm, sr);
    auto out = renderOffline(p, o);
    CHECK(out != nullptr);
    if (!out) return;
    auto on = onsets(out->ch[0], 0.001f);
    CHECK_MSG(on.size() == 4, "found %d onsets", (int)on.size());
    if (on.size() != 4) return;
    const int64 et = (int64)std::llround(2.0 * kTicksPerStep * samplesPerTick((double)p.global->tempo.get(), sr));
    float level = 0.5f;
    for (int k = 0; k < 4; ++k) {
        CHECK_MSG(on[(size_t)k] == stepSample(1, (double)p.global->tempo.get(), sr) + k * et, "echo %d at %lld", k, (long long)on[(size_t)k]);
        CHECK_NEAR(out->ch[0][(size_t)on[(size_t)k]], level, 1.0e-4);
        level *= 0.25f;         // velocity halves, level follows velocity squared
    }
}

TEST(swing_shift_and_gate) {
    const double sr = 48000.0, bpm = 120.0;
    {   // swing delays the off-beat sixteenths
        Project p;
        p.global->tempo.set((float)bpm);
        auto ch = addSampleChannel(p, click(sr));
        for (int s : {0, 1, 2, 3}) Project::setStep(p.pattern().notesFor(ch.id), s, true);
        p.pattern().swing = 0.5f;
        RenderOptions o;
        o.lengthSamples = stepSample(8, bpm, sr);
        auto out = renderOffline(p, o);
        auto on = onsets(out->ch[0]);
        CHECK(on.size() == 4);
        if (on.size() == 4) {
            const int64 d = (int64)std::llround(0.25 * kTicksPerStep * samplesPerTick(bpm, sr));   // 50% of half a step
            CHECK(on[0] == stepSample(0, bpm, sr));
            CHECK(std::llabs(on[1] - (stepSample(1, bpm, sr) + d)) <= 1);
            CHECK(on[2] == stepSample(2, bpm, sr));
            CHECK(std::llabs(on[3] - (stepSample(3, bpm, sr) + d)) <= 1);
        }
        ch.live->swingMix.set(0.0f);        // this channel opts out
        auto straight = renderOffline(p, o);
        auto on2 = onsets(straight->ch[0]);
        CHECK(on2.size() == 4 && on2[1] == stepSample(1, bpm, sr));
    }
    {   // time shift delays every note by a fraction of a step
        Project p;
        p.global->tempo.set((float)bpm);
        auto ch = addSampleChannel(p, click(sr));
        ch.live->shift.set(0.5f);
        Project::setStep(p.pattern().notesFor(ch.id), 2, true);
        RenderOptions o;
        o.lengthSamples = stepSample(8, bpm, sr);
        auto on = onsets(renderOffline(p, o)->ch[0]);
        CHECK(on.size() == 1 && on[0] == stepSample(2.5, bpm, sr));
    }
    {   // gate cuts a held note short
        Project p;
        p.global->tempo.set((float)bpm);
        auto ch = addSampleChannel(p, sine(440, 2.0, sr, 0.3f));
        static_cast<Sampler*>(ch.live->plugin.get())->pEnvOn->set(1);
        static_cast<Sampler*>(ch.live->plugin.get())->pRelease->set(1.0f);
        ch.live->gate.set(2.0f);
        Note n; n.length = 12 * kTicksPerStep;
        p.pattern().notesFor(ch.id).push_back(n);
        RenderOptions o;
        o.lengthSamples = stepSample(8, bpm, sr);
        auto out = renderOffline(p, o);
        CHECK(rmsOf(out->ch[0], 100, (size_t)stepSample(1.9, bpm, sr)) > 0.15);
        CHECK(peakOf(out->ch[0], (size_t)stepSample(2.2, bpm, sr), (size_t)o.lengthSamples) < 1.0e-3f);
    }
}

TEST(layer_channel_plays_its_children) {
    const double sr = 48000.0, bpm = 120.0;
    Project p;
    p.global->tempo.set((float)bpm);
    auto a = addSampleChannel(p, sine(440, 1.0, sr, 0.2f), "A");
    auto b = addSampleChannel(p, sine(660, 1.0, sr, 0.2f), "B");
    b.live->pan.set(1.0f);
    a.live->pan.set(-1.0f);
    const int idA = a.id, idB = b.id;
    auto& layer = p.addChannel(std::make_shared<LayerInstrument>(), "Layer");
    layer.layerChildren = {idA, idB};
    Project::setStep(p.pattern().notesFor(layer.id), 0, true);
    RenderOptions o;
    o.lengthSamples = 24000;
    auto out = renderOffline(p, o);
    CHECK(out != nullptr);
    if (!out) return;
    CHECK_NEAR(measureHz(out->ch[0], 100, 20000, sr), 440.0, 0.5);
    CHECK_NEAR(measureHz(out->ch[1], 100, 20000, sr), 660.0, 0.5);
    // and it survives a save / load
    Project q;
    CHECK(q.fromJson(p.toJson()));
    CHECK(q.channels.size() == 3 && q.channels[2].layerChildren.size() == 2);
    CHECK(q.channels[0].live->pan.get() == -1.0f);
}

TEST(portamento_glides_from_the_last_note) {
    const double sr = 48000.0, bpm = 120.0;
    Project p;
    p.global->tempo.set((float)bpm);
    auto ch = addSampleChannel(p, sine(keyToHz(60), 4.0, sr, 0.3f));
    ch.live->porta.set(1);
    ch.live->mono.set(1);
    ch.live->portaTime.set(200.0f);
    Note a; a.key = 60; a.length = 4 * kTicksPerStep;
    Note b; b.key = 72; b.tick = 4 * kTicksPerStep; b.length = 8 * kTicksPerStep;
    p.pattern().notesFor(ch.id) = {a, b};
    RenderOptions o;
    o.lengthSamples = stepSample(12, bpm, sr);
    auto out = renderOffline(p, o);
    CHECK(out != nullptr);
    if (!out) return;
    const size_t s4 = (size_t)stepSample(4, bpm, sr);
    const double early = measureHz(out->ch[0], s4 + 300, s4 + 1500, sr);         // just after the second note starts
    const double late = measureHz(out->ch[0], s4 + 14400, s4 + 19200, sr);       // after the 200 ms glide
    CHECK_MSG(early > keyToHz(60) * 1.01 && early < keyToHz(66), "early pitch %.1f Hz", early);
    CHECK_MSG(std::fabs(1200.0 * std::log2(late / keyToHz(72))) < 3.0, "late pitch %.1f Hz", late);
}

TEST(mod_x_and_y_drive_the_sampler_filter) {
    const double sr = 48000.0;
    auto bright = sine(6000.0, 1.0, sr, 0.3f);
    auto renderWith = [&](float modX) {
        Project p;
        auto ch = addSampleChannel(p, bright);
        static_cast<Sampler*>(ch.live->plugin.get())->pCutoff->set(4000.0f);
        Note n; n.modX = modX;
        p.pattern().notesFor(ch.id).push_back(n);
        RenderOptions o;
        o.lengthSamples = 24000;
        return rmsOf(renderOffline(p, o)->ch[0], 2000, 20000);
    };
    const double mid = renderWith(0.5f), low = renderWith(0.2f), high = renderWith(0.9f);
    CHECK_MSG(low < mid * 0.1 && high > mid * 1.5, "rms low %.4f mid %.4f high %.4f", low, mid, high);
}

TEST(channel_features_keep_block_size_invariance_and_do_not_allocate) {
    std::shared_ptr<SampleData> ref;
    for (int block : {512, 3, 64, 100, 1024}) {
        Project p;
        p.global->tempo.set(137.0f);
        auto a = addSampleChannel(p, drums::makeKick({}, 48000.0), "Kick");
        auto b = addSampleChannel(p, sine(330, 1.0, 44100.0, 0.2f), "Tone");
        auto c = addSampleChannel(p, click(), "Hat");
        static_cast<Sampler*>(b.live->plugin.get())->pEnvOn->set(1);
        b.live->arpDir.set(3); b.live->arpRange.set(2); b.live->arpTime.set(0.5f);
        b.live->echoCount.set(4); b.live->echoTime.set(1.5f); b.live->echoPan.set(0.3f); b.live->echoPitch.set(7);
        b.live->porta.set(1); b.live->poly.set(3);
        c.live->cut.set(2); b.live->cutBy.set(2);
        c.live->shift.set(0.3f);
        p.pattern().swing = 0.4f;
        for (int s : {0, 4, 8, 12}) Project::setStep(p.pattern().notesFor(a.id), s, true);
        for (int s : {2, 7, 10, 15}) Project::setStep(p.pattern().notesFor(c.id), s, true);
        for (int k : {57, 60, 64}) { Note n; n.key = k; n.length = 10 * kTicksPerStep; n.tick = 120; p.pattern().notesFor(b.id).push_back(n); }
        sortNotes(p.pattern().notesFor(b.id));
        RenderOptions o;
        o.blockSize = block;
        o.loop = true;
        o.lengthSamples = 48000 * 2;
        const int64 before = Rt::allocationsInRt.load();
        auto out = renderOffline(p, o);
        CHECK(Rt::allocationsInRt.load() == before);
        CHECK(out != nullptr);
        if (!out) return;
        if (!ref) { ref = out; CHECK(peakOf(out->ch[0]) > 0.2f); continue; }
        CHECK_MSG(identical(*ref, *out), "block size %d differs", block);
    }
}

TEST(master_volume_scales_the_whole_render_from_the_first_sample) {
    auto renderAt = [](float master) {
        Project p;
        p.global->masterVolume.set(master);
        auto ch = addSampleChannel(p, drums::makeKick({}, 48000.0));
        Project::setStep(p.pattern().notesFor(ch.id), 0, true);
        RenderOptions o;
        o.lengthSamples = 12000;
        return renderOffline(p, o);
    };
    auto unity = renderAt(0.8f), quiet = renderAt(0.2f);
    const float g = std::pow(0.25f, 2.89f);
    double worst = 0.0;
    for (size_t i = 0; i < unity->ch[0].size(); ++i) worst = std::max(worst, (double)std::fabs(quiet->ch[0][i] - unity->ch[0][i] * g));
    CHECK_MSG(worst < 1.0e-6, "difference %g (a jump at the start means the gain ramped in)", worst);
    CHECK(peakOf(quiet->ch[0]) < 0.02f);
}

TEST(preview_player_plays_once) {
    Engine e;
    e.prepare(48000.0);
    std::vector<float> l(9600), r(9600);
    float* out[2] = {l.data(), r.data()};
    e.process(nullptr, 0, out, 2, 9600);
    CHECK(peakOf(l) == 0.0f);
    e.preview.play(sine(500, 0.1, 44100.0, 0.5f), 0.8f);
    e.process(nullptr, 0, out, 2, 9600);
    CHECK(peakOf(l) > 0.3f && peakOf(l) < 0.45f);
    CHECK_NEAR(measureHz(l, 100, 4000, 48000.0), 500.0, 1.0);
    e.process(nullptr, 0, out, 2, 9600);
    CHECK(peakOf(l) == 0.0f);
    CHECK(!e.preview.isPlaying());
    e.preview.play(sine(500, 2.0, 48000.0, 0.5f), 1.0f);
    e.process(nullptr, 0, out, 2, 4800);
    e.preview.stop();
    e.process(nullptr, 0, out, 2, 9600);
    CHECK(peakOf(l, 2000) == 0.0f);
}
