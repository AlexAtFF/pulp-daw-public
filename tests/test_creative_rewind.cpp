// Rewind: time and volume curves locked to the song.
#include "CreativeTest.h"
#include <filesystem>
using namespace pulp;
using namespace ct;

namespace {
const double kSr = 48000.0;
const size_t kBeat = 24000, kBar = 96000;      // at 120 BPM
using Pt = Rewind::Point;
const int H = Rewind::Hold, Ln = Rewind::Line, Sm = Rewind::Smooth;

const Preset* presetNamed(const std::vector<Preset>& all, const char* name) {
    for (auto& p : all) if (p.name == name) return &p;
    return nullptr;
}
std::shared_ptr<Rewind> withPreset(const char* name) {
    auto r = std::make_shared<Rewind>();
    r->prepare(kSr);
    const auto all = r->factoryPresets();
    const Preset* p = presetNamed(all, name);
    CHECK_MSG(p != nullptr, "no preset called %s", name);
    if (p) r->loadState(p->state);
    return r;
}
}

TEST(rewind_with_flat_curves_is_the_input_bit_for_bit) {
    const auto in = busy(5.0);          // two and a half bars: the pattern comes round twice
    {   // preset one, playing and stopped
        Rewind r;
        r.prepare(kSr);
        const auto presets = r.factoryPresets();
        CHECK(presets.size() >= 16 && presets[0].name == "Nothing");
        r.loadState(presets[0].state);
        CHECK(r.timeCurve().empty() && r.volumeCurve().empty());
        CHECK(same(run(r, in), in));
        Transport stopped;
        stopped.playing = false;
        r.reset();
        CHECK(same(run(r, in, stopped), in));
    }
    {   // curves that do nothing drawn with points, any amounts, any mix, any pattern length, a tempo that is not round
        for (int len = 0; len < 4; ++len) {
            Rewind r;
            r.prepare(kSr);
            r.setTimeCurve({{0.0f, 0.0f, Ln}, {0.5f, 0.0f, H}, {0.75f, 0.0f, Sm}, {1.0f, 0.0f, H}});
            r.setVolumeCurve({{0.0f, 1.0f, Sm}, {0.3f, 1.0f, Ln}, {0.9f, 1.0f, H}});
            r.pLength->set((float)len); r.pMix->set(0.37f); r.pTime->set(0.6f); r.pVolume->set(0.8f);
            Transport t;
            t.tempo = 133.7;
            CHECK_MSG(same(run(r, in, t), in), "pattern length %d", len);
        }
    }
    {   // curves that do a lot, with both amounts at 0 or the mix at 0, and stopped whatever the settings
        auto r = withPreset("Stutter build");
        r->setVolumeCurve({{0.0f, 0.0f, H}, {0.5f, 1.0f, H}});
        r->pTime->set(0.0f); r->pVolume->set(0.0f);
        CHECK(same(run(*r, in), in));
        r->reset();
        r->pTime->set(1.0f); r->pVolume->set(1.0f); r->pMix->set(0.0f);
        CHECK(same(run(*r, in), in));
        r->reset();
        r->pMix->set(1.0f);
        Transport stopped;
        stopped.playing = false;
        CHECK(same(run(*r, in, stopped), in));
        CHECK(!same(run(*r, in), in));      // and playing, it does do something
    }
}

TEST(rewind_half_speed_curve_plays_the_first_half_of_the_bar_an_octave_down) {
    auto r = withPreset("Half speed");
    CHECK(r->latencySamples() == 0);
    const double f = 440.0;
    const auto in = tone(f, 4.0, 0.5f);
    const auto out = run(*r, in);
    // the pitch, in both bars
    const double hz1 = tu::measureHz(out.l, 4800, kBar - 4800, kSr), hz2 = tu::measureHz(out.l, kBar + 4800, 2 * kBar - 4800, kSr);
    CHECK_MSG(std::fabs(1200.0 * std::log2(hz1 / (f / 2))) < 0.5, "bar 1 at %.3f Hz", hz1);
    CHECK_MSG(std::fabs(1200.0 * std::log2(hz2 / (f / 2))) < 0.5, "bar 2 at %.3f Hz", hz2);
    // sample by sample it is the bar's own first half, stretched to twice the length: out(t) = in(bar + (t - bar) / 2)
    const size_t fade = (size_t)(r->pSmooth->get() * 0.001 * kSr) + 2;
    double worst = 0.0;
    for (size_t bar = 0; bar < 2; ++bar)
        for (size_t i = bar ? fade : 0; i < kBar; ++i) {
            const double src = (double)(bar * kBar) + 0.5 * (double)i;
            worst = std::max(worst, std::fabs((double)out.l[bar * kBar + i] - 0.5 * std::sin(kTwoPi * f * src / kSr)));
        }
    printf("    rewind half speed: %.3f / %.3f Hz for 220, worst difference from the stretched input %.2e (%.1f dB under the signal)\n", hz1, hz2, worst, db(worst / 0.5));
    CHECK_MSG(worst < 2.0e-4, "half speed is off by %g", worst);
    // every second sample lands on a sample of the input and is that sample exactly
    bool exact = true;
    for (size_t i = 0; i < kBar; i += 2) exact = exact && out.l[i] == in.l[i / 2];
    CHECK(exact);
    // the jump back at the bar line is a crossfade, not a click: no step beyond what a 440 Hz sine and a 4 ms fade give
    const float bound = 0.5f * (float)(kTwoPi * f / kSr) + 2.0f * 0.5f / 192.0f;
    CHECK_MSG(maxStep(out.l) <= bound, "largest step %.4f, bound %.4f", maxStep(out.l), bound);
    // the meters follow: half way through the bar the sound is one beat behind
    Rewind m;
    m.prepare(kSr);
    m.loadState(presetNamed(m.factoryPresets(), "Half speed")->state);
    run(m, slice(in, 0, kBar / 2));
    CHECK_NEAR(m.meter(0), 0.5, 0.001);
    CHECK_NEAR(m.meter(1), 1.0, 0.002);
    CHECK_NEAR(m.meter(2), 1.0, 1e-6);
}

TEST(rewind_stutter_curve_repeats_the_first_eighth_exactly) {
    auto r = withPreset("Stuck on the first eighth");
    const size_t eighth = kBeat / 2;
    const size_t fade = (size_t)std::lround(r->pSmooth->get() * 0.001 * kSr);     // 2 ms: 96 samples
    CHECK(fade == 96);
    const auto in = noise(4.0, 0.4f);
    const auto out = run(*r, in);
    for (size_t bar = 0; bar < 2; ++bar) {
        // the first eighth is live
        bool live = true;
        for (size_t i = bar ? fade : 0; i < eighth; ++i) live = live && out.l[bar * kBar + i] == in.l[bar * kBar + i];
        CHECK_MSG(live, "bar %d: the first eighth is not the input", (int)bar);
        // every later eighth is that same eighth again, to the bit, once the crossfade at its start is over
        for (size_t k = 1; k < 8; ++k) {
            size_t wrong = 0;
            for (size_t i = fade; i < eighth; ++i) {
                const size_t at = bar * kBar + k * eighth + i;
                wrong += out.l[at] != in.l[bar * kBar + i] || out.r[at] != in.r[bar * kBar + i];
            }
            CHECK_MSG(wrong == 0, "bar %d, eighth %d: %d samples differ", (int)bar, (int)k, (int)wrong);
        }
    }
    // inside the crossfade the sound is a mix of the two slices: never louder than both together
    CHECK(peak(out) <= 0.4f * 1.0001f * 2.0f);
    // with a sine, a jump of the playback point makes no step larger than the sine's own plus the fade's
    Rewind s;
    s.prepare(kSr);
    s.loadState(presetNamed(s.factoryPresets(), "Stuck on the first eighth")->state);
    const auto sine = run(s, tone(1000.0, 4.0, 0.5f));
    const float bound = 0.5f * (float)(kTwoPi * 1000.0 / kSr) + 2.0f * 0.5f / 96.0f;
    CHECK_MSG(maxStep(sine.l) <= bound, "largest step %.4f, bound %.4f", maxStep(sine.l), bound);
    printf("    rewind stutter: largest step with a 1 kHz sine %.4f (the sine alone: %.4f, bound %.4f)\n", maxStep(sine.l), 0.5 * kTwoPi * 1000.0 / kSr, bound);
}

TEST(rewind_volume_curve_gates_on_the_grid_without_a_click) {
    // open and closed on alternate eighths, 4 ms smoothing: the gain moves in a straight line over 192 samples
    Rewind r;
    r.prepare(kSr);
    std::vector<Pt> gate;
    for (int k = 0; k < 8; ++k) gate.push_back({(float)k / 8.0f, k % 2 == 0 ? 1.0f : 0.0f, H});
    r.setVolumeCurve(gate);
    r.pSmooth->set(4.0f);
    const size_t eighth = kBeat / 2, ramp = 192;
    const auto out = run(r, dc(4.0, 1.0f));
    CHECK(out.l[0] == 1.0f);
    float worstStep = 0.0f;
    for (size_t k = 1; k < 16; ++k) {
        const size_t g = k * eighth;
        const float before = k % 2 == 1 ? 1.0f : 0.0f, after = 1.0f - before;
        CHECK_MSG(out.l[g - 1] == before, "edge %d: the gain moved before the grid (%.6f)", (int)k, out.l[g - 1]);
        CHECK_MSG(out.l[g] != before, "edge %d: the gain did not start on the grid", (int)k);
        CHECK_NEAR(out.l[g], before + (after - before) / (float)ramp, 1e-6);
        CHECK_NEAR(out.l[g + ramp / 2 - 1], 0.5, 1e-4);
        CHECK_MSG(out.l[g + ramp] == after && out.l[g + eighth - 1] == after, "edge %d: not settled after the ramp", (int)k);
    }
    worstStep = maxStep(out.l);
    CHECK_MSG(worstStep <= 1.0f / (float)ramp + 1e-6f, "largest step of the gain %.6f, bound %.6f", worstStep, 1.0 / ramp);
    // on a sine: closed means silent, open means the input, and no step is larger than the sine's own plus 1/192 of its peak
    r.reset();
    const auto in = tone(1000.0, 4.0, 0.5f);
    const auto sine = run(r, in);
    bool silent = true, open = true;
    for (size_t k = 0; k < 16; ++k)
        for (size_t i = ramp; i < eighth; ++i) {
            const size_t at = k * eighth + i;
            if (k % 2 == 1) silent = silent && sine.l[at] == 0.0f;
            else open = open && sine.l[at] == in.l[at];
        }
    CHECK(silent && open);
    const float bound = 0.5f * (float)(kTwoPi * 1000.0 / kSr) + 0.5f / (float)ramp + 1e-6f;
    CHECK_MSG(maxStep(sine.l) <= bound, "largest step %.5f, bound %.5f", maxStep(sine.l), bound);
    printf("    rewind gate: gain moves %.6f a sample (bound 1/192 = %.6f); largest step on a 1 kHz sine %.5f (bound %.5f)\n", worstStep, 1.0 / 192.0, maxStep(sine.l), bound);
    // the volume amount scales the depth: at 50 % a closed step is at half
    r.reset();
    r.pVolume->set(0.5f);
    const auto half = run(r, dc(1.0, 1.0f));
    CHECK_NEAR(half.l[eighth + 4000], 0.5, 1e-6);
    // the pump preset dips at every beat and has recovered before the next
    auto p = withPreset("Pump on every beat");
    const auto pumped = run(*p, dc(4.0, 1.0f));
    for (size_t beat = 1; beat < 8; ++beat) {
        CHECK_MSG(pumped.l[beat * kBeat + 600] < 0.15f, "beat %d: %.3f just after the beat", (int)beat, pumped.l[beat * kBeat + 600]);
        CHECK(pumped.l[beat * kBeat - 100] == 1.0f);
    }
    CHECK(maxStep(pumped.l) <= 1.0f / (6.0f * 48.0f) + 1e-6f);
}

TEST(rewind_from_the_middle_of_the_song_matches_a_render_from_the_start) {
    // A plain delay of three beats plus a stutter: the sound always comes from further back than the bar.
    auto make = [] {
        auto r = std::make_shared<Rewind>();
        r->setTimeCurve({{0.0f, 3.0f, H}, {0.5f, 3.0f, H}, {0.625f, 3.5f, H}, {0.75f, 3.0f, Ln}, {1.0f, 3.5f, H}});
        r->setVolumeCurve({{0.0f, 1.0f, H}, {0.25f, 0.3f, Sm}, {0.5f, 1.0f, H}});
        r->pSmooth->set(3.0f);
        return r;
    };
    {   // the plugin alone, fed the same input
        const auto in = busy(8.0);          // four bars
        auto a = make(), b = make();
        a->prepare(kSr); b->prepare(kSr);
        const auto whole = run(*a, in);
        Transport mid;
        mid.startTick = 2.0 * 4.0 * kPPQ;   // bar three
        const auto part = run(*b, slice(in, 2 * kBar, 4 * kBar), mid);
        // nothing can come out before the tape holds what the curve asks for: three beats of silence, then a difference
        // of at most rounding (the song position is a sum of slightly different numbers in the two runs)
        CHECK(peak(part, 0, 3 * kBeat) == 0.0f);
        CHECK(peak(whole, 2 * kBar, 2 * kBar + 3 * kBeat) > 0.1f);
        const size_t filled = 3 * kBeat + kBeat / 2 + 200;      // the longest reach of the curve, and the smoothing
        double worst = 0.0;
        for (size_t i = filled; i < part.l.size(); ++i)
            worst = std::max(worst, std::max(std::fabs((double)part.l[i] - whole.l[2 * kBar + i]), std::fabs((double)part.r[i] - whole.r[2 * kBar + i])));
        printf("    rewind from bar 3: largest difference from the full render once the tape has filled %.2e\n", worst);
        CHECK_MSG(worst < 1.0e-6, "difference %g", worst);
    }
    {   // in a project, through the engine: an export from bar 3 against that stretch of an export from the start
        auto build = [&](Project& p) {
            p.global->tempo.set(120.0f);
            auto ch = tu::addSampleChannel(p, tu::sine(330.0, 0.1, kSr, 0.5f));
            p.channels.back().mixerTrack = 1;
            auto& notes = p.pattern().notesFor(ch.id);
            for (int step = 0; step < 64; ++step)
                if (step % 3 != 1) { Note n; n.tick = step * kTicksPerStep; n.length = kTicksPerStep / 2; n.key = 60 + (step * 5) % 12; notes.push_back(n); }
            p.setSlot(1, 0, make());
        };
        Project a, b;
        build(a); build(b);
        RenderOptions o;
        o.lengthSamples = 4 * (int64)kBar;
        auto whole = renderOffline(a, o);
        o.startTick = 2.0 * 4.0 * kPPQ;
        o.lengthSamples = 2 * (int64)kBar;
        auto part = renderOffline(b, o);
        CHECK(whole && part);
        if (!whole || !part) return;
        CHECK(tu::peakOf(whole->ch[0]) > 0.1f);
        const size_t filled = 3 * kBeat + kBeat / 2 + 200;
        double worst = 0.0;
        for (size_t i = filled; i < (size_t)part->frames(); ++i) worst = std::max(worst, std::fabs((double)part->ch[0][i] - whole->ch[0][2 * kBar + i]));
        printf("    rewind in a project, export from bar 3: largest difference %.2e (peak %.3f)\n", worst, tu::peakOf(whole->ch[0]));
        CHECK_MSG(worst < 1.0e-6, "difference %g", worst);
    }
}

TEST(rewind_curves_are_sorted_clamped_evaluated_and_survive_save_and_load) {
    Rewind r;
    r.prepare(kSr);
    // given out of order and out of range: sorted by position, clamped
    r.setTimeCurve({{0.75f, 99.0f, Ln}, {0.25f, 0.5f, Sm}, {-1.0f, -3.0f, 7}, {0.5f, 2.0f, H}});
    auto tc = r.timeCurve();
    CHECK(tc.size() == 4);
    if (tc.size() == 4) {
        CHECK(tc[0].pos == 0.0f && tc[0].value == 0.0f && tc[0].shape == Sm);
        CHECK(tc[1].pos == 0.25f && tc[2].pos == 0.5f && tc[3].pos == 0.75f && tc[3].value == Rewind::kMaxBeats);
    }
    // more points than fit: the first thirty-two are kept
    std::vector<Pt> many;
    for (int i = 0; i < 40; ++i) many.push_back({(float)i / 40.0f, (float)(i % 2), H});
    r.setVolumeCurve(many);
    CHECK(r.volumeCurve().size() == (size_t)Rewind::kMaxPoints);
    CHECK(r.timeCurve().size() == 4);       // setting one curve leaves the other alone
    // the three shapes
    const std::vector<Pt> shapes = {{0.0f, 1.0f, H}, {0.25f, 2.0f, Ln}, {0.5f, 4.0f, Sm}, {0.75f, 0.0f, H}};
    CHECK_NEAR(Rewind::valueAt(shapes, 0.1, 0.0f), 1.0, 1e-9);
    CHECK_NEAR(Rewind::valueAt(shapes, 0.375, 0.0f), 3.0, 1e-6);
    CHECK_NEAR(Rewind::valueAt(shapes, 0.625, 0.0f), 2.0, 1e-6);         // half way down a smooth stretch
    CHECK_NEAR(Rewind::valueAt(shapes, 0.5625, 0.0f), 4.0 - 4.0 * (0.25 * 0.25 * (3.0 - 0.5)), 1e-6);
    CHECK_NEAR(Rewind::valueAt(shapes, 0.9, 0.0f), 0.0, 1e-9);            // the last point holds to the end
    CHECK_NEAR(Rewind::valueAt({}, 0.3, 0.7f), 0.7, 1e-7);
    CHECK_NEAR(Rewind::valueAt({{0.5f, 3.0f, H}}, 0.1, 0.0f), 3.0, 1e-9); // before the first point: its value
    CHECK(Rewind::patternBeats(0) == 2.0 && Rewind::patternBeats(1) == 4.0 && Rewind::patternBeats(2) == 8.0 && Rewind::patternBeats(3) == 16.0);

    // save and load, straight and through the text of a project file
    r.setTimeCurve({{0.0f, 0.0f, Ln}, {0.3f, 1.25f, Sm}, {0.6f, 0.1f, H}, {0.6f, 2.0f, Ln}, {1.0f, 0.0f, H}});
    r.pLength->set(2.0f); r.pSmooth->set(7.5f); r.pTime->set(0.9f);
    const Json state = r.saveState();
    CHECK(state["extra"]["time"].isArray() && state["extra"]["time"].size() == 5 && state["extra"]["volume"].size() == 32);
    CHECK(state["extra"]["time"].at(1).isArray() && state["extra"]["time"].at(1).size() == 3);
    for (int pass = 0; pass < 2; ++pass) {
        Rewind q;
        q.prepare(kSr);
        q.loadState(pass == 0 ? state : Json::parse(state.dump()));
        CHECK(q.saveState() == state);
        const auto a = r.timeCurve(), b = q.timeCurve();
        CHECK(a.size() == b.size());
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) CHECK(a[i].pos == b[i].pos && a[i].value == b[i].value && a[i].shape == b[i].shape);
        CHECK(q.volumeCurve().size() == 32);
        const auto in = busy(2.5);
        Rewind fresh;
        fresh.prepare(kSr);
        fresh.loadState(state);
        CHECK(same(run(q, in), run(fresh, in)));
    }
    // a state with no curves in it gives flat curves
    Json bare = Json::object();
    bare.set("type", "timewarp");
    bare.set("params", Json::object());
    r.loadState(bare);
    CHECK(r.timeCurve().empty() && r.volumeCurve().empty());
    // in a project file on disk
    {
        namespace fs = std::filesystem;
        const std::string dir = t::tmpDir() + "/creative_rewind";
        fs::remove_all(dir);
        fs::create_directories(dir);
        writeWavFile(dir + "/beep.wav", *tu::sine(440.0, 0.3, kSr, 0.5f), WavFormat::Float32);
        Project p, q;
        p.projectDir = dir;
        auto ch = tu::addSampleChannel(p, SampleCache::global().load(dir + "/beep.wav"));
        p.channels.back().mixerTrack = 1;
        for (int s = 0; s < 16; s += 2) Project::setStep(p.pattern().notesFor(ch.id), s, true);
        auto fx = std::make_shared<Rewind>();
        fx->loadState(state);
        p.setSlot(1, 0, fx);
        std::string err;
        CHECK(p.save(dir + "/song.pulp"));
        CHECK_MSG(q.load(dir + "/song.pulp", &err), "%s", err.c_str());
        auto* back = dynamic_cast<Rewind*>(q.mixer[1].slots[0].get());
        CHECK(back != nullptr);
        if (back) CHECK(back->saveState() == state);
        RenderOptions o;
        o.lengthSamples = 96000;
        auto a = renderOffline(p, o), b = renderOffline(q, o);
        CHECK(a && b);
        if (a && b) { CHECK(tu::identical(*a, *b)); CHECK(tu::peakOf(a->ch[0]) > 0.1f); }
    }
}

TEST(rewind_presets_do_what_their_names_say) {
    const auto in = tone(500.0, 4.0, 0.5f);
    {   // the tape stop: the pitch falls through the last beat and the sound has faded by the bar line
        auto r = withPreset("Tape stop at the end of the bar");
        const auto out = run(*r, in);
        CHECK_NEAR(tu::measureHz(out.l, kBeat, 2 * kBeat, kSr), 500.0, 0.5);
        const double mid = tu::measureHz(out.l, 3 * kBeat + 11000, 3 * kBeat + 13000, kSr);    // half way: half speed
        CHECK_MSG(std::fabs(mid / 500.0 - 0.5) < 0.03, "half way through the stop the pitch is %.1f Hz", mid);
        CHECK(peak(out, kBar - 40, kBar - 1) < 0.02f);
    }
    {   // reverse: a rising sweep comes out falling in the last beat
        auto r = withPreset("Reverse swell into the bar");
        const auto out = run(*r, chirp(300.0, 200.0, 4.0, 0.5f));
        const double early = tu::measureHz(out.l, 3 * kBeat + 6000, 3 * kBeat + 9000, kSr), late = tu::measureHz(out.l, 3 * kBeat + 18000, 3 * kBeat + 21000, kSr);
        CHECK_MSG(late < early - 30.0, "%.1f Hz then %.1f Hz", early, late);
        // at the same speed backwards: the sweep falls as fast as it rose
        CHECK_NEAR((late - early) / (12000.0 / kSr), -200.0, 12.0);
    }
    {   // the scratch wiggle ends where it began: back on the live sound at the bar line, without a jump
        auto r = withPreset("Scratch wiggle on the last beat");
        const auto out = run(*r, in);
        CHECK(out.l[kBar + 500] == in.l[kBar + 500]);
        CHECK(maxStep(out.l) <= 0.5f * (float)(kTwoPi * 500.0 / kSr) * 4.0f);
    }
    {   // every preset: named, different, at most 32 points, positions in order
        Rewind r;
        const auto presets = r.factoryPresets();
        std::vector<std::string> seen;
        for (auto& pr : presets) {
            CHECK(!pr.name.empty() && std::find(seen.begin(), seen.end(), pr.name) == seen.end());
            seen.push_back(pr.name);
            for (const char* which : {"time", "volume"}) {
                const Json& c = pr.state["extra"][which];
                CHECK(c.isArray() && c.size() <= (size_t)Rewind::kMaxPoints);
                float last = 0.0f;
                for (auto& e : c.elements()) { CHECK(e.at(0).f() >= last && e.at(0).f() <= 1.0f); last = e.at(0).f(); }
            }
            if (&pr != &presets[0]) CHECK_MSG(pr.state["extra"]["time"].size() + pr.state["extra"]["volume"].size() > 0, "%s has no curves", pr.name.c_str());
        }
    }
}
