// Milestone 8: automation clips, event automation, controllers and links.
#include "Util.h"
#include "pulp/Automation.h"
#include "pulp/Controllers.h"
#include "pulp/Effects.h"
#include "pulp/Formula.h"
#include <filesystem>
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0, kBpm = 120.0;       // 25 samples per tick

std::shared_ptr<SampleData> dc(double seconds, float level = 0.5f) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = kSr;
    s->channels = 1;
    s->ch[0].assign((size_t)(seconds * kSr), level);
    return s;
}
std::shared_ptr<SampleData> click() {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = kSr; s->channels = 1;
    s->ch[0].assign(64, 0.9f);
    return s;
}
std::shared_ptr<SampleData> render(Project& p, int64 len, int block = 512, int threads = 1, double startTick = 0.0) {
    RenderOptions o;
    o.lengthSamples = len;
    o.blockSize = block;
    o.threads = threads;
    o.startTick = startTick;
    return renderOffline(p, o);
}
// A steady level through insert 1, whose fader an automation clip can then move.
int steadyLevel(Project& p, double seconds = 3.0) {
    p.global->tempo.set((float)kBpm);
    p.songMode = true;
    const int id = p.addAudio(dc(seconds), "level");
    p.addAudioClip(id, 0, 0, 1);
    return id;
}
AutoClip& curve(Project& p, const std::string& target, int64 start, std::vector<AutoPoint> pts, int track = 5) {
    Clip& c = p.addAutomationClip(target, track, start, pts.back().tick);
    AutoClip& a = *p.autoById(c.autoId);
    a.points = std::move(pts);
    return a;
}
float tempoNorm(double bpm) { return (float)((bpm - 10.0) / 512.0); }
// Runs a live engine for n samples and throws the sound away.
void run(Engine& e, int n, int block = 256) {
    std::vector<float> l((size_t)block), r((size_t)block);
    float* out[2] = {l.data(), r.data()};
    for (int c = 0; c < n; c += block) e.process(nullptr, 0, out, 2, std::min(block, n - c));
}
}

TEST(formulas_compile_and_evaluate) {
    auto ev = [](const std::string& text, double x = 0.0) {
        std::string err;
        auto f = Formula::compile(text, {"x", "beat"}, &err);
        CHECK_MSG(f != nullptr, "'%s': %s", text.c_str(), err.c_str());
        const double v[2] = {x, 6.0};
        return f ? f->eval(v) : -999.0;
    };
    CHECK_NEAR(ev("1 + 2 * 3"), 7.0, 1e-12);
    CHECK_NEAR(ev("(1 + 2) * 3"), 9.0, 1e-12);
    CHECK_NEAR(ev("-2 ^ 2"), -4.0, 1e-12);
    CHECK_NEAR(ev("2 ^ 3 ^ 2"), 512.0, 1e-9);
    CHECK_NEAR(ev("10 / 4 - 1"), 1.5, 1e-12);
    CHECK_NEAR(ev("7 % 4"), 3.0, 1e-12);
    CHECK_NEAR(ev("1 - x", 0.25), 0.75, 1e-12);
    CHECK_NEAR(ev("x * x", 0.5), 0.25, 1e-12);
    CHECK_NEAR(ev("sin(pi / 2) + cos(0)"), 2.0, 1e-12);
    CHECK_NEAR(ev("min(3, max(1, 2))"), 2.0, 1e-12);
    CHECK_NEAR(ev("clamp(x * 4, 0, 1)", 0.5), 1.0, 1e-12);
    CHECK_NEAR(ev("if(x > 0.5, 1, 0.2)", 0.4), 0.2, 1e-12);
    CHECK_NEAR(ev("if(x >= 0.4, 1, 0.2)", 0.4), 1.0, 1e-12);
    CHECK_NEAR(ev("frac(beat / 4)"), 0.5, 1e-12);
    CHECK_NEAR(ev("sine(0.5)"), 1.0, 1e-12);
    CHECK_NEAR(ev("tri(0.25) + saw(0.75) + square(0.25)"), 0.5 + 0.75 + 1.0, 1e-12);
    CHECK_NEAR(ev("SQRT(16) + Abs(-1) + floor(2.7) + round(2.5)"), 4 + 1 + 2 + 3, 1e-12);
    CHECK_NEAR(ev("1 / 0"), 0.0, 1e-12);                 // never a NaN or an infinity
    CHECK_NEAR(ev(" 0.5e1 "), 5.0, 1e-12);
    for (const char* bad : {"", "1 +", "foo", "sin(", "sin(1, 2)", "min(1)", "(1 + 2", "1 2", "x $ 2", "nothing(1)"}) {
        std::string err;
        CHECK_MSG(Formula::compile(bad, {"x"}, &err) == nullptr, "'%s' should be refused", bad);
        CHECK(!err.empty());
    }
}

TEST(automation_curve_shapes) {
    // single curve: straight with no tension, bowed with it
    CHECK_NEAR(autoSegment(0.2f, 0.8f, 0.5, ShapeCurve, 0.0f), 0.5, 1e-6);
    CHECK(autoSegment(0.0f, 1.0f, 0.5, ShapeCurve, 0.6f) < 0.2f);
    CHECK(autoSegment(0.0f, 1.0f, 0.5, ShapeCurve, -0.6f) > 0.8f);
    for (int shape = 0; shape < NumAutoShapes; ++shape) {
        for (float tension : {-1.0f, -0.3f, 0.0f, 0.5f, 1.0f}) {
            CHECK_NEAR(autoSegment(0.25f, 0.75f, 0.0, shape, tension), 0.25, 1e-6);            // every shape starts on its first point
            for (double x = 0.0; x <= 1.0; x += 0.01) {
                const float v = autoSegment(0.25f, 0.75f, x, shape, tension);
                CHECK(v >= 0.25f - 1e-6f && v <= 0.75f + 1e-6f);                                // and stays between its two points
            }
        }
    }
    // double curve passes through the middle; hold keeps the first value to the end
    CHECK_NEAR(autoSegment(0.0f, 1.0f, 0.5, ShapeDouble, 0.7f), 0.5, 1e-6);
    CHECK(autoSegment(0.0f, 1.0f, 0.25, ShapeDouble, 0.7f) < 0.1f);
    CHECK(autoSegment(0.3f, 0.9f, 0.999, ShapeHold, 0.0f) == 0.3f);
    // stairs: tension 0 gives nine steps, the last on the second value
    CHECK_NEAR(autoSegment(0.0f, 1.0f, 0.999, ShapeStairs, 0.0f), 1.0, 1e-6);
    CHECK_NEAR(autoSegment(0.0f, 1.0f, 0.5, ShapeStairs, -1.0f), 1.0, 1e-6);                   // two steps: low, then high
    CHECK_NEAR(autoSegment(0.0f, 1.0f, 0.49, ShapeStairs, -1.0f), 0.0, 1e-6);
    // pulse alternates; wave swings and arrives on the second value
    CHECK(autoSegment(0.1f, 0.9f, 0.1, ShapePulse, -1.0f) == 0.1f && autoSegment(0.1f, 0.9f, 0.6, ShapePulse, -1.0f) == 0.9f);
    CHECK_NEAR(autoSegment(0.1f, 0.9f, 1.0, ShapeWave, 0.0f), 0.9, 1e-5);
    CHECK_NEAR(autoSegment(0.1f, 0.9f, 1.0, ShapeWave, 1.0f), 0.9, 1e-5);

    // a whole curve: before the first point, between points, after the last
    std::vector<AutoPoint> pts = {{100, 0.2f, 0.0f, ShapeCurve}, {300, 0.6f, 0.0f, ShapeHold}, {500, 1.0f, 0.0f, ShapeCurve}};
    AutoLfo off;
    CHECK(evalAuto(pts, off, 0.0) == 0.2f);
    CHECK_NEAR(evalAuto(pts, off, 200.0), 0.4, 1e-6);
    CHECK(evalAuto(pts, off, 300.0) == 0.6f && evalAuto(pts, off, 499.0) == 0.6f);
    CHECK(evalAuto(pts, off, 500.0) == 1.0f && evalAuto(pts, off, 9000.0) == 1.0f);
    // LFO mode swings around the curve and never leaves 0..1
    AutoLfo lfo;
    lfo.on = true; lfo.periodSteps = 4.0f; lfo.depth = 0.2f;
    std::vector<AutoPoint> flat = {{0, 0.5f, 0.0f, ShapeCurve}, {3840, 0.5f, 0.0f, ShapeCurve}};
    CHECK_NEAR(evalAuto(flat, lfo, 0.0), 0.5, 1e-6);
    CHECK_NEAR(evalAuto(flat, lfo, 240.0), 0.7, 1e-6);          // a quarter of the way through a four-step cycle
    CHECK_NEAR(evalAuto(flat, lfo, 720.0), 0.3, 1e-6);
    CHECK_NEAR(evalAuto(flat, lfo, 960.0), 0.5, 1e-5);
    lfo.depth = 1.0f;
    for (double t = 0; t < 2000; t += 13) { const float v = evalAuto(flat, lfo, t); CHECK(v >= 0.0f && v <= 1.0f); }
}

TEST(event_automation_holds_and_ramps) {
    std::vector<AutoEvent> ev = {{480, 0.2f, false}, {960, 0.8f, true}, {1920, 0.4f, false}};
    CHECK(evalEvents(ev, 0.0) == 0.2f);                  // before the first event the control already has its value
    CHECK(evalEvents(ev, 700.0) == 0.2f);
    CHECK(evalEvents(ev, 960.0) == 0.8f);
    CHECK_NEAR(evalEvents(ev, 1440.0), 0.6, 1e-6);       // gliding to the next event
    CHECK(evalEvents(ev, 1920.0) == 0.4f && evalEvents(ev, 99999.0) == 0.4f);
    // recorded movement is thinned without changing its shape by more than the tolerance
    std::vector<AutoEvent> rec;
    for (int i = 0; i <= 200; ++i) rec.push_back({i * 10, (float)(0.5 + 0.4 * std::sin(i * 0.05)), true});
    auto thin = rec;
    simplifyEvents(thin, 0.01f);
    CHECK_MSG(thin.size() < rec.size() / 4 && thin.size() >= 4, "%d of %d points kept", (int)thin.size(), (int)rec.size());
    double worst = 0.0;
    for (auto& e : rec) worst = std::max(worst, (double)std::fabs(evalEvents(thin, e.tick) - e.value));
    CHECK_MSG(worst <= 0.0101, "thinning moved the curve by %g", worst);
    CHECK(thin.front().tick == 0 && thin.back().tick == 2000);
}

TEST(parameter_addresses_round_trip) {
    Project p;
    auto a = addSampleChannel(p, click(), "Kick");
    auto b = addSampleChannel(p, click(), "Snare");
    p.setSlot(3, 2, std::make_shared<Squeeze>());
    p.setRoute(3, 7, true);
    p.mixer[3].name = "Drums";
    std::vector<const Param*> all = {&p.global->tempo, &p.global->masterVolume, &p.global->masterPitch, &a.live->volume, &b.live->pan, &b.live->arpTime,
                                     a.sampler()->params()[0].get(), &p.mixer[3].live->volume, &p.mixer[0].live->eqGain[1], &p.mixer[3].live->slotMix[2],
                                     p.mixer[3].slots[2]->param("thresh"), p.mixer[3].routeTo(7)->level.get()};
    for (const Param* q : all) {
        const std::string addr = p.addressOf(q);
        CHECK_MSG(!addr.empty(), "%s has no address", q->info.name.c_str());
        std::shared_ptr<void> keep;
        CHECK_MSG(p.findParam(addr, &keep) == q, "%s does not lead back to its parameter", addr.c_str());
        CHECK(keep != nullptr);
    }
    CHECK(p.addressOf(&b.live->pan) == "ch:" + std::to_string(b.id) + ":pan");
    CHECK(p.addressOf(p.mixer[3].slots[2]->param("thresh")) == "fx:3:2:thresh");
    CHECK(p.addressOf(p.mixer[3].routeTo(7)->level.get()) == "send:3:7");
    CHECK(p.paramLabel("fx:3:2:thresh") == "Drums - Squeeze - Threshold");
    CHECK(p.paramLabel("ch:" + std::to_string(a.id) + ":volume") == "Kick - Channel volume");
    CHECK(p.paramLabel("tempo") == "Tempo");
    Param stranger(ParamInfo{});
    CHECK(p.addressOf(&stranger).empty());
    for (const char* bad : {"", "nope", "ch:999:volume", "ch:1:nope", "fx:3:5:thresh", "fx:3:2:nope", "mix:999:volume", "send:3:9"}) CHECK(p.findParam(bad) == nullptr);
}

TEST(automation_clip_moves_a_fader_exactly) {
    Project p;
    steadyLevel(p);
    curve(p, "mix:1:volume", 0, {{0, 0.0f, 0.0f, ShapeCurve}, {3840, 1.0f, 0.0f, ShapeCurve}});
    const int64 allocs = Rt::allocationsInRt.load();
    auto out = render(p, 110000);
    CHECK(Rt::allocationsInRt.load() == allocs);
    // At the end of every 64-sample quantum the fader is exactly where the curve says; in between it ramps.
    for (int k : {2, 10, 100, 500, 1000, 1400, 1500, 1700}) {
        const int n = k * 64;
        const double tick = n / 25.0;
        const float v = (float)std::min(1.0, tick / 3840.0);
        const float want = 0.5f * TrackLive::faderToGain(v);
        CHECK_MSG(std::fabs(out->ch[0][(size_t)(n - 1)] - want) <= 2e-6f + want * 1e-5f, "sample %d: %.7f, expected %.7f", n - 1, out->ch[0][(size_t)(n - 1)], want);
    }
    // it rises all the way without a step
    for (size_t i = 1; i < 96000; ++i) CHECK(out->ch[0][i] >= out->ch[0][i - 1] - 1e-7f);
    float biggest = 0.0f;
    for (size_t i = 200; i < 96000; ++i) biggest = std::max(biggest, out->ch[0][i] - out->ch[0][i - 1]);
    CHECK_MSG(biggest < 1e-4f, "largest jump between two samples %g", biggest);
    // after the clip the fader stays where the curve left it
    CHECK_NEAR(out->ch[0][109000], 0.5f * TrackLive::faderToGain(1.0f), 1e-5);

    // a hold: the jump is over within one quantum of its tick
    Project q;
    steadyLevel(q);
    curve(q, "mix:1:volume", 0, {{0, 0.8f, 0.0f, ShapeHold}, {1920, 0.2f, 0.0f, ShapeCurve}, {3840, 0.2f, 0.0f, ShapeCurve}});
    auto h = render(q, 60000);
    CHECK_NEAR(h->ch[0][47935], 0.5, 1e-6);                                              // 0.8 is unity
    CHECK_NEAR(h->ch[0][47999], 0.5f * TrackLive::faderToGain(0.2f), 1e-6);
    CHECK_NEAR(h->ch[0][59000], 0.5f * TrackLive::faderToGain(0.2f), 1e-6);
    // the clip's range maps the curve onto part of the fader's travel
    Project r;
    steadyLevel(r);
    auto& a = curve(r, "mix:1:volume", 0, {{0, 0.0f, 0.0f, ShapeCurve}, {3840, 1.0f, 0.0f, ShapeCurve}});
    a.min = 0.4f; a.max = 0.8f;
    auto m = render(r, 100000);
    CHECK_NEAR(m->ch[0][64 * 2 - 1], 0.5f * TrackLive::faderToGain(0.4f + 0.4f * (float)(128 / 25.0 / 3840.0)), 1e-5);
    CHECK_NEAR(m->ch[0][99000], 0.5, 1e-6);
}

TEST(automation_depends_only_on_the_song_position) {
    auto build = [](Project& p) {
        steadyLevel(p);
        curve(p, "mix:1:volume", 960, {{0, 0.2f, 0.3f, ShapeCurve}, {1920, 0.9f, 0.0f, ShapeWave}, {3840, 0.5f, 0.0f, ShapeCurve}});
        curve(p, "mix:1:pan", 0, {{0, 0.0f, 0.0f, ShapeStairs}, {3840, 1.0f, 0.0f, ShapeCurve}}, 6);
    };
    Project a;
    build(a);
    auto full = render(a, 140000);
    // wherever the knobs were left, the result is the same
    Project b;
    build(b);
    b.mixer[1].live->volume.set(0.03f);
    b.mixer[1].live->pan.set(0.9f);
    CHECK(identical(*full, *render(b, 140000)));
    // before the first clip starts the control already sits on the clip's first value
    {
        auto st = b.compile();
        for (auto& t : st->autoTargets) if (t.param == &b.mixer[1].live->volume) CHECK_NEAR(t.valueAt(500.0), 0.2, 1e-6);
    }
    // Playing from the middle gives exactly what playing through gives there. (The start positions are whole
    // quanta into the song, so both runs work the curve out at the same moments.)
    for (double from : {704.0, 1984.0, 3328.0, 4480.0}) {
        Project c;
        build(c);
        auto part = render(c, 20000, 512, 1, from);
        const size_t off = (size_t)(from * 25.0);
        double worst = 0.0;
        for (size_t i = 128; i < 20000; ++i)
            worst = std::max(worst, std::max((double)std::fabs(part->ch[0][i] - full->ch[0][off + i]), (double)std::fabs(part->ch[1][i] - full->ch[1][off + i])));
        CHECK_MSG(worst < 1e-6, "from tick %.0f the render differs by %g", from, worst);
    }
    // block size and thread count make no difference, and a small-block "live" run equals the export
    for (auto cs : std::vector<std::pair<int, int>>{{64, 1}, {33, 1}, {4096, 6}, {300, 3}}) {
        Project c;
        build(c);
        CHECK_MSG(identical(*full, *render(c, 140000, cs.first, cs.second)), "block %d threads %d differs", cs.first, cs.second);
    }
}

TEST(overlapping_and_finished_clips) {
    Project p;
    steadyLevel(p);
    curve(p, "mix:1:volume", 0, {{0, 0.1f, 0.0f, ShapeCurve}, {1920, 0.1f, 0.0f, ShapeCurve}});
    curve(p, "mix:1:volume", 960, {{0, 0.9f, 0.0f, ShapeCurve}, {480, 0.7f, 0.0f, ShapeCurve}}, 6);
    curve(p, "mix:1:volume", 4800, {{0, 0.4f, 0.0f, ShapeCurve}, {960, 0.6f, 0.0f, ShapeCurve}}, 7);
    auto st = p.compile();
    CHECK(st->autoTargets.size() == 1);
    if (st->autoTargets.size() != 1) return;
    const auto& t = st->autoTargets[0];
    CHECK(t.segments.size() == 3 && t.param == &p.mixer[1].live->volume);
    CHECK_NEAR(t.valueAt(500.0), 0.1, 1e-6);            // only the first clip has started
    CHECK_NEAR(t.valueAt(960.0), 0.9, 1e-6);            // the clip that started last holds the control
    CHECK_NEAR(t.valueAt(1200.0), 0.8, 1e-6);
    CHECK_NEAR(t.valueAt(1700.0), 0.7, 1e-6);           // ... and keeps it at its end value once it is over
    CHECK_NEAR(t.valueAt(4000.0), 0.7, 1e-6);
    CHECK_NEAR(t.valueAt(5280.0), 0.5, 1e-6);
    CHECK_NEAR(t.valueAt(90000.0), 0.6, 1e-6);
    // a muted clip, or one on a muted track, drives nothing
    p.clips[2].muted = true;
    p.tracks[7].mute = true;
    st = p.compile();
    CHECK(st->autoTargets[0].segments.size() == 1);
    // a clip trimmed from its left edge plays the later part of its curve
    Project q;
    steadyLevel(q);
    curve(q, "mix:1:volume", 0, {{0, 0.0f, 0.0f, ShapeCurve}, {3840, 1.0f, 0.0f, ShapeCurve}});
    q.clips[1].start = 960; q.clips[1].offset = 960; q.clips[1].length = 960;
    auto s2 = q.compile();
    CHECK_NEAR(s2->autoTargets[0].valueAt(0.0), 0.25, 1e-6);
    CHECK_NEAR(s2->autoTargets[0].valueAt(1440.0), 0.375, 1e-6);
    CHECK_NEAR(s2->autoTargets[0].valueAt(5000.0), 0.5, 1e-6);
    // a clip whose parameter is gone is ignored
    Project r;
    steadyLevel(r);
    curve(r, "ch:42:volume", 0, {{0, 0.0f, 0.0f, ShapeCurve}, {3840, 1.0f, 0.0f, ShapeCurve}});
    CHECK(r.compile()->autoTargets.empty());
    CHECK(render(r, 4000) != nullptr);
}

TEST(pattern_events_drive_controls) {
    Project p;
    p.global->tempo.set((float)kBpm);
    auto ch = addSampleChannel(p, click());
    const std::string vol = "ch:" + std::to_string(ch.id) + ":volume";
    const std::string pan = "ch:" + std::to_string(ch.id) + ":pan";
    p.pattern().events[vol] = {{0, 0.78f, false}, {1920, 0.39f, false}};
    p.pattern().events[pan] = {{0, 0.0f, true}, {3840, 1.0f, false}};       // a glide from hard left to hard right over the bar
    CHECK(p.isAutomated(vol) && !p.isAutomated("mastervol"));
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setState(p.compile());
    ch.live->volume.set(0.1f);
    e.play();
    run(e, 25 * 1000);                           // tick 1000
    CHECK_NEAR(ch.live->volume.get(), 0.78, 1e-6);
    CHECK_NEAR(ch.live->pan.get(), -1.0 + 2.0 * (1000.0 / 3840.0), 0.04);
    run(e, 25 * 1500);                           // tick 2500
    CHECK_NEAR(ch.live->volume.get(), 0.39, 1e-6);
    run(e, 25 * 1500);                           // tick 4000: the pattern has looped
    CHECK_NEAR(ch.live->volume.get(), 0.78, 1e-6);
    CHECK(ch.live->pan.get() < -0.85f);
    // stopped, the controls are free again
    e.stop();
    run(e, 512);
    ch.live->volume.set(0.5f);
    run(e, 512);
    CHECK(ch.live->volume.get() == 0.5f);

    // in song mode the events belong to the pattern's clips
    p.songMode = true;
    p.addPatternClip(0, 0, 3840);                // the pattern plays in bar two
    e.setState(p.compile());
    e.play();
    run(e, 25 * 2000);                           // bar one: no clip yet, the control waits on the first event's value
    CHECK_NEAR(ch.live->volume.get(), 0.78, 1e-6);
    run(e, 25 * 4000);                           // tick 6000 = 2160 into the clip
    CHECK_NEAR(ch.live->volume.get(), 0.39, 1e-6);
    CHECK_NEAR(ch.live->pan.get(), -1.0 + 2.0 * (2160.0 / 3840.0), 0.04);
    // an automation clip for the same control, starting later, takes over from the pattern's events
    curve(p, vol, 5760, {{0, 0.6f, 0.0f, ShapeCurve}, {960, 0.6f, 0.0f, ShapeCurve}});
    e.setState(p.compile());
    run(e, 256);
    CHECK_NEAR(ch.live->volume.get(), 0.6, 1e-6);
}

TEST(sidechain_ducking_with_a_follower) {
    // a silent trigger on insert 1 ducks a bass tone on insert 2 on every beat
    Project p;
    p.global->tempo.set((float)kBpm);
    p.songMode = true;
    auto kick = addSampleChannel(p, click(), "Trigger");
    p.channels[0].mixerTrack = 1;
    for (int s : {0, 4, 8, 12}) Project::setStep(p.pattern().notesFor(kick.id), s, true);
    p.addPatternClip(0, 0, 0);
    const int bass = p.addAudio(sine(110.0, 2.0, kSr, 0.4f), "bass");
    p.addAudioClip(bass, 1, 0, 2);
    auto follower = std::make_shared<Follower>();
    follower->loadState(follower->factoryPresets()[4].state);       // "Silent trigger"
    CHECK(follower->pMute->getBool());
    p.setSlot(1, 0, follower);
    ParamLink link;
    link.target = "mix:2:volume";
    link.kind = ParamLink::Controller;
    link.track = 1; link.slot = 0; link.output = 0;
    link.min = 0.0f; link.max = 0.8f;
    p.setLink(link);
    CHECK(p.isAutomated("mix:2:volume"));
    const int64 allocs = Rt::allocationsInRt.load();
    auto out = render(p, 96000);
    CHECK(Rt::allocationsInRt.load() == allocs);
    for (int beat = 0; beat < 4; ++beat) {
        const size_t at = (size_t)(beat * 24000);
        const double ducked = rmsOf(out->ch[0], at + 300, at + 1500), back = rmsOf(out->ch[0], at + 19000, at + 23500);
        CHECK_MSG(back > 0.2, "beat %d: the bass never came back (%.3f)", beat, back);
        CHECK_MSG(ducked < back * 0.3, "beat %d: ducked %.3f, recovered %.3f", beat, ducked, back);
    }
    CHECK(peakOf(out->ch[0]) < 0.45f);          // the trigger itself is not heard
    // without the link the bass is steady
    p.removeLink("mix:2:volume");
    p.mixer[2].live->volume.set(0.8f);
    auto flat = render(p, 96000);
    CHECK_NEAR(rmsOf(flat->ch[0], 300, 1500) / rmsOf(flat->ch[0], 19000, 23500), 1.0, 0.05);

    // the same recipe with the follower on its own track, listening to the kick through a sidechain route
    Project q;
    q.global->tempo.set((float)kBpm);
    q.songMode = true;
    auto k2 = addSampleChannel(q, click(), "Kick");
    q.channels[0].mixerTrack = 1;
    for (int s : {0, 8}) Project::setStep(q.pattern().notesFor(k2.id), s, true);
    q.addPatternClip(0, 0, 0);
    q.addAudioClip(q.addAudio(sine(110.0, 2.0, kSr, 0.4f), "bass"), 1, 0, 2);
    auto f2 = std::make_shared<Follower>();
    f2->loadState(f2->factoryPresets()[2].state);                    // "Ducking from the sidechain"
    q.setSlot(3, 0, f2);
    CHECK(q.setRoute(1, 3, true, true));
    link.track = 3;
    q.setLink(link);
    auto o2 = render(q, 96000);
    CHECK(rmsOf(o2->ch[0], 600, 1500) < rmsOf(o2->ch[0], 40000, 47000) * 0.4);          // ducked just after the kick on beat one
    CHECK_NEAR(rmsOf(o2->ch[0], 24600, 25500) / rmsOf(o2->ch[0], 40000, 47000), 1.0, 0.25);  // beat two has no kick: no ducking
    // identical on any number of threads and at any block size
    for (auto cs : std::vector<std::pair<int, int>>{{64, 1}, {4096, 4}, {77, 3}}) CHECK(identical(*o2, *render(q, 96000, cs.first, cs.second)));
}

TEST(hardware_controller_links) {
    Project p;
    p.global->tempo.set((float)kBpm);
    auto ch = addSampleChannel(p, click());
    const std::string vol = "ch:" + std::to_string(ch.id) + ":volume";
    ParamLink link;
    link.target = vol;
    link.kind = ParamLink::Midi;
    link.midiChannel = -1;
    link.cc = 7;
    link.min = 0.2f; link.max = 0.6f;
    p.setLink(link);
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setState(p.compile());
    run(e, 256);
    CHECK(ch.live->volume.get() == 0.78f);              // nothing moves until the hardware control does
    CHECK(e.ccValue(-1, 7) < 0.0f);
    e.setCc(3, 7, 0.5f);
    run(e, 128);
    CHECK_NEAR(ch.live->volume.get(), 0.4, 1e-6);       // any channel: the middle of the range
    CHECK_NEAR(e.ccValue(3, 7), 0.5, 1e-6);
    e.setCc(9, 7, 1.0f);
    run(e, 128);
    CHECK_NEAR(ch.live->volume.get(), 0.6, 1e-6);
    // a link to one channel ignores the others
    link.midiChannel = 3;
    p.setLink(link);
    e.setState(p.compile());
    e.setCc(9, 7, 0.0f);
    run(e, 128);
    CHECK_NEAR(ch.live->volume.get(), 0.4, 1e-6);       // channel 4 is still at 0.5
    // a mapping formula and an inverted range
    link.midiChannel = -1;
    link.formula = "x * x";
    link.min = 1.0f; link.max = 0.0f;
    p.setLink(link);
    e.setState(p.compile());
    e.setCc(0, 7, 0.5f);
    run(e, 128);
    CHECK_NEAR(ch.live->volume.get(), 0.75, 1e-6);
    // smoothing: the control glides to the new value
    link.formula.clear();
    link.min = 0.0f; link.max = 1.0f;
    link.smoothMs = 100.0f;
    p.setLink(link);
    e.setState(p.compile());
    e.setCc(0, 7, 0.0f);
    run(e, 48000);
    CHECK(ch.live->volume.get() < 0.001f);
    e.setCc(0, 7, 1.0f);
    run(e, 4800);                                        // one time constant
    CHECK_NEAR(ch.live->volume.get(), 0.632, 0.02);
    run(e, 48000);
    CHECK(ch.live->volume.get() > 0.999f);
    // while a control is being dragged the link leaves it alone
    e.setTouched(&ch.live->volume);
    ch.live->volume.set(0.3f);
    run(e, 512);
    CHECK(ch.live->volume.get() == 0.3f);
    e.setTouched(nullptr);
    run(e, 128);
    CHECK(ch.live->volume.get() > 0.99f);
    // a render never sees the hardware: the link does nothing offline
    ch.live->volume.set(0.5f);
    CHECK(render(p, 2000) != nullptr);
    CHECK(ch.live->volume.get() == 0.5f);
}

TEST(tempo_automation) {
    Project p;
    p.global->tempo.set((float)kBpm);
    p.songMode = true;
    auto kick = addSampleChannel(p, click());
    for (int s : {0, 4, 8, 12}) Project::setStep(p.pattern().notesFor(kick.id), s, true);
    p.addPatternClip(0, 0, 0);
    p.addPatternClip(0, 0, 3840);
    // bar one at 120 BPM, bar two at 240
    curve(p, "tempo", 0, {{0, tempoNorm(120), 0.0f, ShapeHold}, {3840, tempoNorm(240), 0.0f, ShapeCurve}, {7680, tempoNorm(240), 0.0f, ShapeCurve}});
    p.global->tempo.set(97.0f);                  // wherever the tempo box was left
    RenderOptions o;
    o.tail = RenderOptions::Cut;
    auto out = renderOffline(p, o);
    CHECK_MSG(out->frames() == 96000 + 48000, "%lld samples", (long long)out->frames());
    for (int b = 0; b < 4; ++b) CHECK_NEAR(out->ch[0][(size_t)(b * 24000)], 0.9, 1e-5);                 // 120 BPM: half a second apart
    for (int b = 0; b < 4; ++b) CHECK_NEAR(out->ch[0][(size_t)(96000 + b * 12000)], 0.9, 1e-5);         // 240 BPM: a quarter second apart
    CHECK(peakOf(out->ch[0], 24000 + 200, 48000 - 10) == 0.0f);
    for (int block : {64, 100, 4096}) {
        Project q;
        q.fromJson(p.toJson());
        // samples that only live in memory are not in the document: rebuild the channel's sample
        static_cast<Sampler*>(q.channels[0].live->plugin.get())->setSample(click());
        o.blockSize = block;
        auto again = renderOffline(q, o);
        CHECK_MSG(identical(*out, *again), "block %d differs", block);
    }
    // a gliding tempo: the bar takes as long as the average of its beats says
    Project g;
    g.global->tempo.set((float)kBpm);
    g.songMode = true;
    auto k2 = addSampleChannel(g, click());
    Project::setStep(g.pattern().notesFor(k2.id), 0, true);
    g.addPatternClip(0, 0, 0);
    curve(g, "tempo", 0, {{0, tempoNorm(100), 0.0f, ShapeCurve}, {3840, tempoNorm(200), 0.0f, ShapeCurve}});
    o.blockSize = 512;
    auto glide = renderOffline(g, o);
    const double expectSeconds = 4.0 * 60.0 / 100.0 * std::log(2.0);       // four beats while the tempo doubles linearly in beats
    CHECK_MSG(std::fabs((double)glide->frames() / kSr - expectSeconds) < 0.004, "%.4f s, expected %.4f", (double)glide->frames() / kSr, expectSeconds);
}

TEST(controllers_put_out_what_they_should) {
    // every controller lets the sound through untouched
    auto tone = sine(440.0, 0.5, kSr, 0.4f);
    auto build = [&](Project& p, std::shared_ptr<Plugin> fx) {
        p.global->tempo.set((float)kBpm);
        p.songMode = true;
        p.addAudioClip(p.addAudio(tone, "t"), 0, 0, 1);
        if (fx) p.setSlot(1, 0, fx);
    };
    Project plain;
    build(plain, nullptr);
    auto ref = render(plain, 24000);
    for (const char* type : {"ctl_lfo", "ctl_peak", "ctl_xy", "ctl_formula"}) {
        Project p;
        auto fx = PluginRegistry::global().create(type);
        CHECK(fx != nullptr && fx->numControlOutputs() >= 1 && !fx->isInstrument());
        build(p, fx);
        CHECK_MSG(identical(*ref, *render(p, 24000)), "%s changes the sound", type);
    }
    // Wobble follows the song position: a sine, one cycle per bar, on an otherwise empty track
    Project p;
    steadyLevel(p);
    auto lfo = std::make_shared<Wobble>();
    lfo->pSteps->set(16.0f); lfo->pBase->set(0.5f); lfo->pDepth->set(0.8f); lfo->pShape->set(0.0f); lfo->pSync->set(1.0f);
    p.setSlot(9, 0, lfo);
    ParamLink link;
    link.target = "mix:1:pan";
    link.kind = ParamLink::Controller;
    link.track = 9; link.slot = 0; link.output = 0;
    p.setLink(link);
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setState(p.compile());
    e.play();
    for (int tick : {480, 960, 1920, 2880, 3500}) {
        // run up to the tick: the link carries what the LFO put out one quantum (2.56 ticks) earlier
        const int64 target = (int64)tick * 25;
        run(e, (int)(target - e.clock()), 64);
        const double at = (double)(tick) - 2.0 * 64.0 / 25.0;
        const double want = 0.5 + 0.4 * std::sin(kTwoPi * at / 3840.0);
        CHECK_MSG(std::fabs(p.mixer[1].live->pan.getNorm() - want) < 0.004, "tick %d: pan %.4f, expected %.4f", tick, p.mixer[1].live->pan.getNorm(), want);
    }
    CHECK_NEAR(Wobble::wave(1, 0.25), 1.0, 1e-9);
    CHECK_NEAR(Wobble::wave(2, 0.75), -1.0, 1e-9);
    CHECK_NEAR(Wobble::wave(3, 0.5), 0.0, 1e-9);
    CHECK_NEAR(Wobble::wave(4, 0.0), 1.0, 1e-9);
    CHECK(Wobble::wave(5, 3.2) == Wobble::wave(5, 3.9) && Wobble::wave(5, 3.2) != Wobble::wave(5, 4.2));

    // Pad XY: two outputs, with smoothing or without
    Project q;
    steadyLevel(q);
    auto pad = std::make_shared<PadXy>();
    pad->pSmooth->set(0.0f);
    q.setSlot(4, 3, pad);
    link.target = "mix:1:pan"; link.track = 4; link.slot = 3; link.output = 1;
    q.setLink(link);
    q.prepare(kSr);
    Engine e2;
    e2.prepare(kSr);
    e2.setState(q.compile());
    pad->pX->set(0.1f); pad->pY->set(0.9f);
    run(e2, 192, 64);
    CHECK_NEAR(pad->controlOutput(0), 0.1, 1e-6);
    CHECK_NEAR(q.mixer[1].live->pan.getNorm(), 0.9, 1e-6);
    pad->pSmooth->set(200.0f);
    pad->pY->set(0.1f);
    run(e2, 9600, 64);                          // one time constant
    CHECK_NEAR(pad->controlOutput(1), 0.9 - 0.8 * 0.632, 0.02);

    // Formula: typed expressions of the knobs and the song position
    auto f = std::make_shared<FormulaCtl>();
    std::string err;
    CHECK(f->setFormula("a * 0.5 + 0.25", &err));
    CHECK(!f->setFormula("a +", &err) && !err.empty());
    CHECK(f->formula() == "a * 0.5 + 0.25");     // a bad formula is refused and the old one stays
    Project r;
    steadyLevel(r);
    r.setSlot(2, 0, f);
    r.prepare(kSr);
    Engine e3;
    e3.prepare(kSr);
    e3.setState(r.compile());
    f->pA->set(0.5f);
    run(e3, 128, 64);
    CHECK_NEAR(f->controlOutput(0), 0.5, 1e-6);
    CHECK(f->setFormula("frac(beat / 4)"));
    e3.play();
    run(e3, 25 * 960 + 64, 64);                 // one beat in
    CHECK_NEAR(f->controlOutput(0), 0.25, 0.002);
    auto g = PluginRegistry::global().create("ctl_formula");
    g->loadState(f->saveState());
    CHECK(static_cast<FormulaCtl*>(g.get())->formula() == "frac(beat / 4)");
    // the follower: its output rises with the level and falls back
    auto fol = std::make_shared<Follower>();
    Project s;
    steadyLevel(s, 0.5);
    s.setSlot(1, 0, fol);
    s.prepare(kSr);
    Engine e4;
    e4.prepare(kSr);
    e4.setState(s.compile());
    e4.play();
    run(e4, 12000, 64);
    CHECK_NEAR(fol->controlOutput(0), 0.5, 0.01);
    run(e4, 48000, 64);                          // the clip ended after half a second
    CHECK(fol->controlOutput(0) < 0.01f);
}

TEST(automation_saves_and_reloads) {
    namespace fs = std::filesystem;
    const std::string dir = t::tmpDir() + "/autoproj";
    fs::remove_all(dir);
    fs::create_directories(dir);
    writeWavFile(dir + "/tone.wav", *sine(220.0, 3.0, kSr, 0.4f), WavFormat::Float32);
    writeWavFile(dir + "/click.wav", *click(), WavFormat::Float32);
    auto build = [&](Project& p) {
        p.global->tempo.set((float)kBpm);
        p.songMode = true;
        p.projectDir = dir;
        const int id = p.addAudio(dir + "/tone.wav");
        p.addAudioClip(id, 0, 0, 1);
        auto& ch = p.addSamplerChannel(dir + "/click.wav", "Click");
        const int cid = ch.id;
        for (int s : {0, 4, 8, 12}) Project::setStep(p.pattern().notesFor(cid), s, true);
        p.pattern().events["ch:" + std::to_string(cid) + ":pan"] = {{0, 0.0f, true}, {1920, 1.0f, false}, {2880, 0.5f, false}};
        p.addPatternClip(0, 1, 0);
        p.addPatternClip(0, 1, 3840);
        p.setSlot(1, 0, std::make_shared<Sweep>());
        auto& a = curve(p, "fx:1:0:cutoff", 0, {{0, 0.2f, 0.4f, ShapeCurve}, {1920, 0.9f, -0.5f, ShapeDouble}, {3840, 0.3f, 1.0f, ShapeStairs}, {7680, 0.6f, 0.0f, ShapeCurve}});
        a.lfo.on = true; a.lfo.shape = 1; a.lfo.periodSteps = 2.0f; a.lfo.depth = 0.1f; a.lfo.phase = 0.25f;
        a.min = 0.1f; a.max = 0.9f; a.name = "Filter sweep";
        p.setSlot(2, 0, std::make_shared<Wobble>());
        ParamLink l;
        l.target = "mix:1:pan"; l.kind = ParamLink::Controller; l.track = 2; l.slot = 0; l.output = 0; l.min = 0.3f; l.max = 0.7f; l.smoothMs = 20.0f; l.formula = "1 - x";
        p.setLink(l);
        ParamLink m;
        m.target = "mastervol"; m.kind = ParamLink::Midi; m.midiChannel = 2; m.cc = 74;
        p.setLink(m);
    };
    Project a, b;
    build(a);
    CHECK(a.save(dir + "/song.pulp"));
    std::string err;
    CHECK_MSG(b.load(dir + "/song.pulp", &err), "%s", err.c_str());
    CHECK(b.autos.size() == 1 && b.links.size() == 2 && b.clips.size() == 4);
    if (b.autos.size() == 1) {
        const auto& x = b.autos[0];
        CHECK(x.target == "fx:1:0:cutoff" && x.name == "Filter sweep" && x.points.size() == 4);
        CHECK(x.points[1].shape == ShapeDouble && x.points[1].tension == -0.5f && x.points[2].shape == ShapeStairs && x.points[2].tick == 3840);
        CHECK(x.lfo.on && x.lfo.shape == 1 && x.lfo.periodSteps == 2.0f && x.lfo.phase == 0.25f && x.min == 0.1f && x.max == 0.9f);
    }
    CHECK(b.patterns[0].events.size() == 1 && b.patterns[0].events.begin()->second.size() == 3 && b.patterns[0].events.begin()->second[0].ramp);
    if (b.links.size() == 2) {
        CHECK(b.links[0].formula == "1 - x" && b.links[0].smoothMs == 20.0f && b.links[0].track == 2 && b.links[0].min == 0.3f);
        CHECK(b.links[1].kind == ParamLink::Midi && b.links[1].midiChannel == 2 && b.links[1].cc == 74);
    }
    // saved twice, the file is the same
    CHECK(b.save(dir + "/again.pulp"));
    std::string t1, t2;
    readTextFile(dir + "/song.pulp", t1);
    readTextFile(dir + "/again.pulp", t2);
    CHECK(t1 == t2);
    auto ra = render(a, 200000), rb = render(b, 200000);
    CHECK(identical(*ra, *rb));
    CHECK(peakOf(ra->ch[0]) > 0.1f);
    // a copy of a clip can be given its own curve
    const int before = (int)a.autos.size();
    const int id = a.makeAutoClipUnique(3);
    CHECK(id != 0 && (int)a.autos.size() == before + 1 && a.clips[3].autoId == id && a.autoById(id)->points.size() == 4);
    // all of it together: any block size, any thread count, no allocation on the audio thread
    for (auto cs : std::vector<std::pair<int, int>>{{64, 1}, {4096, 5}, {211, 2}}) {
        Project c;
        build(c);
        const int64 allocs = Rt::allocationsInRt.load();
        auto out = render(c, 200000, cs.first, cs.second);
        CHECK(Rt::allocationsInRt.load() == allocs);
        CHECK_MSG(identical(*ra, *out), "block %d threads %d differs", cs.first, cs.second);
    }
}
