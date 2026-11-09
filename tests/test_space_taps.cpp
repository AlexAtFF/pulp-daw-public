// Taps (multitap delay) against known responses.
#include "SpaceTest.h"
using namespace pulp;
using namespace st;

namespace {
const double kSr = 48000.0;

// every sample of a channel that is not zero, as (position, value)
std::vector<std::pair<size_t, float>> hits(const std::vector<float>& x, float floor = 0.0f) {
    std::vector<std::pair<size_t, float>> h;
    for (size_t i = 0; i < x.size(); ++i) if (std::fabs(x[i]) > floor) h.push_back({i, x[i]});
    return h;
}
std::shared_ptr<Taps> bare(double sr = kSr) {       // wet only, no feedback, every tap in the middle
    auto t = std::make_shared<Taps>();
    t->prepare(sr);
    t->pMix->set(1.0f);
    t->pFeedback->set(0.0f);
    for (int i = 0; i < Taps::kTaps; ++i) { t->pPan[i]->set(0.0f); t->pTapFb[i]->set(0.0f); t->pTone[i]->set(0.0f); }
    return t;
}
double energy(const std::vector<float>& x, size_t a, size_t b) {
    double e = 0.0;
    for (size_t i = a; i < std::min(b, x.size()); ++i) e += (double)x[i] * x[i];
    return e;
}
}

TEST(taps_land_on_the_exact_sample) {
    {   // free times in milliseconds, all eight taps
        auto t = bare();
        t->pSync->set(0.0f);
        t->pCount->set(8.0f);
        const float ms[8] = {10.5f, 125.0f, 333.25f, 20.0f, 1000.0f, 71.0f, 2.0f, 480.0f};
        std::vector<std::pair<size_t, float>> want;
        for (int i = 0; i < 8; ++i) {
            t->pTime[i]->set(ms[i]);
            t->pLevel[i]->set(0.1f * (float)(i + 1));
            want.push_back({(size_t)std::lround(ms[i] * 48.0), 0.1f * (float)(i + 1)});
        }
        std::sort(want.begin(), want.end());
        auto out = run(*t, impulse(1.2));
        auto got = hits(out.l);
        CHECK(got.size() == 8);
        for (size_t i = 0; i < got.size() && i < want.size(); ++i) {
            CHECK_MSG(got[i].first == want[i].first, "tap at %d, expected %d", (int)got[i].first, (int)want[i].first);
            CHECK_NEAR(got[i].second, want[i].second, 1e-6);
        }
        CHECK(out.l == out.r);
        CHECK_NEAR(t->tapSamples(2, 120.0), 333.25 * 48.0, 1e-3);
    }
    // tempo sync: steps of a grid, at several tempos, grids and sample rates
    struct Case { double sr, tempo; int grid; int ticks; float swing; } cases[] = {
        {48000.0, 140.0, 2, 240, 0.0f}, {44100.0, 97.3, 4, 480, 0.0f}, {96000.0, 174.25, 1, 160, 0.0f},
        {48000.0, 60.0, 0, 120, 0.0f}, {48000.0, 120.0, 3, 320, 0.0f}, {48000.0, 128.0, 6, 960, 0.0f},
        {48000.0, 120.0, 4, 480, 1.0f}, {44100.0, 133.0, 2, 240, 0.4f}, {48000.0, 90.0, 5, 640, 0.0f}};
    for (auto& cs : cases) {
        auto t = bare(cs.sr);
        t->pSync->set(1.0f);
        t->pGrid->set((float)cs.grid);
        t->pSwing->set(cs.swing);
        t->pCount->set(8.0f);
        const int steps[8] = {1, 2, 3, 5, 6, 7, 9, 4};
        std::vector<size_t> want;
        for (int i = 0; i < 8; ++i) {
            t->pSteps[i]->set((float)steps[i]);
            t->pLevel[i]->set(0.5f);
            double ticks = (double)steps[i] * cs.ticks;
            if (steps[i] & 1) ticks += (double)cs.swing * cs.ticks / 3.0;
            const double samples = ticks * 60.0 * cs.sr / (cs.tempo * kPPQ);
            CHECK_NEAR(t->tapSamples(i, cs.tempo), samples, 1e-6);
            want.push_back((size_t)std::llround(samples));
        }
        std::sort(want.begin(), want.end());
        Transport tr;
        tr.sr = cs.sr;
        tr.tempo = cs.tempo;
        auto out = run(*t, impulse(std::min(7.9, 9.5 * cs.ticks * 60.0 / (cs.tempo * kPPQ) + 0.1), cs.sr), tr);
        auto got = hits(out.l);
        CHECK_MSG(got.size() == want.size(), "%d taps heard at %.0f Hz, %.2f BPM", (int)got.size(), cs.sr, cs.tempo);
        for (size_t i = 0; i < got.size() && i < want.size(); ++i) {
            CHECK_MSG(got[i].first == want[i], "%.0f Hz %.2f BPM grid %d: tap at %d, expected %d", cs.sr, cs.tempo, cs.grid, (int)got[i].first, (int)want[i]);
            CHECK(got[i].second == 0.5f);       // a whole-sample tap is an exact copy
        }
    }
    {   // only the taps in use sound
        auto t = bare();
        t->pCount->set(3.0f);
        CHECK(hits(run(*t, impulse(3.0)).l).size() == 3);
        // full swing on an eighth-note grid puts the off-beats on the triplet: 2/3 of the way through the beat
        t->pGrid->set(4.0f);
        t->pSwing->set(1.0f);
        t->pSteps[0]->set(1.0f);
        CHECK_NEAR(t->tapSamples(0, 120.0), 24000.0 * 2.0 / 3.0, 1e-6);
        // times longer than the line holds are clamped, not wrapped
        t->pGrid->set(6.0f);
        t->pSteps[1]->set(32.0f);
        t->pSteps[2]->set(3.0f);
        t->reset();
        Transport slow;
        slow.tempo = 60.0;
        auto out = run(*t, impulse(8.5), slow);
        auto late = hits(out.l);
        CHECK_MSG(allFinite(out) && late.size() == 3, "%d taps heard", (int)late.size());
        if (late.size() == 3) CHECK_MSG(late[0].first == 64000 && late[1].first == 160000 && late[2].first == 384000, "at %d, %d, %d", (int)late[0].first, (int)late[1].first, (int)late[2].first);
    }
}

TEST(taps_feedback_repeats_the_pattern) {
    {   // global feedback goes round the longest tap: the whole pattern comes back, quieter
        auto t = bare();
        t->pCount->set(2.0f);
        t->pSteps[0]->set(1.0f); t->pSteps[1]->set(4.0f);          // 6000 and 24000 samples at 120 BPM
        t->pLevel[0]->set(0.8f); t->pLevel[1]->set(0.6f);
        t->pFeedback->set(0.5f);
        t->pLowCut->set(20.0f); t->pHighCut->set(20000.0f);
        auto out = run(*t, impulse(2.0));
        CHECK_NEAR(out.l[6000], 0.8, 1e-6);
        CHECK_NEAR(out.l[24000], 0.6, 1e-6);
        CHECK(peak(out, 0, 6000) == 0.0f && peak(out, 6001, 24000) == 0.0f);
        const double first = energy(out.l, 6000, 6100), second = energy(out.l, 30000, 30100), third = energy(out.l, 54000, 54100);
        CHECK_MSG(second / first > 0.19 && second / first < 0.26, "second pass energy ratio %.3f", second / first);     // 0.5 squared, less a little treble
        CHECK_MSG(third / second > 0.17 && third / second < 0.26, "third pass energy ratio %.3f", third / second);
        CHECK(peak(out, 24100, 29990) < 1e-4f);
        // the feedback filters: with a 1 kHz high cut every pass is duller than the last
        t->pHighCut->set(1000.0f);
        t->reset();
        Stereo burst = silence(2.0);
        dsp::Noise nz(8u);
        for (size_t i = 0; i < 2000; ++i) burst.l[i] = burst.r[i] = 0.5f * nz.next();
        auto dull = run(*t, burst);
        auto bright = [&](size_t from) {
            std::vector<float> seg(dull.l.begin() + (std::ptrdiff_t)from, dull.l.begin() + (std::ptrdiff_t)from + 2000);
            double hi = 0.0, lo = 0.0;
            for (double hz = 4000; hz < 12000; hz += 500) hi += std::pow(magAt(seg, hz, kSr), 2.0);
            for (double hz = 100; hz < 600; hz += 50) lo += std::pow(magAt(seg, hz, kSr), 2.0);
            return hi / lo;
        };
        CHECK(bright(30000) < 0.1 * bright(6000));
        CHECK(bright(54000) < 0.3 * bright(30000));
        // and the low cut takes the bottom out of each pass
        t->pHighCut->set(20000.0f);
        t->pLowCut->set(1500.0f);
        t->reset();
        Stereo hum = silence(2.0);
        for (size_t i = 0; i < 4000; ++i) hum.l[i] = hum.r[i] = 0.5f * (float)std::sin(kTwoPi * 150.0 * (double)i / kSr);
        auto thin = run(*t, hum);
        CHECK(tu::rmsOf(thin.l, 30500, 33500) < 0.2 * 0.5 * tu::rmsOf(thin.l, 6500, 9500));
    }
    {   // a tap's own feedback repeats that tap alone
        auto t = bare();
        t->pCount->set(2.0f);
        t->pSteps[0]->set(1.0f); t->pSteps[1]->set(5.0f);
        t->pLevel[0]->set(1.0f); t->pLevel[1]->set(0.0f);
        t->pTapFb[0]->set(0.5f);
        t->pLowCut->set(20.0f); t->pHighCut->set(20000.0f);
        auto out = run(*t, impulse(1.0));
        CHECK_NEAR(out.l[6000], 1.0, 1e-6);
        const double e1 = energy(out.l, 6000, 6100), e2 = energy(out.l, 12000, 12100), e3 = energy(out.l, 18000, 18100);
        CHECK(e2 / e1 > 0.19 && e2 / e1 < 0.26 && e3 / e2 > 0.17 && e3 / e2 < 0.26);
    }
    {   // every feedback control at its maximum still dies away: the sum is held below one
        auto t = bare();
        t->pCount->set(8.0f);
        t->pFeedback->set(0.95f);
        for (int i = 0; i < 8; ++i) { t->pTapFb[i]->set(0.95f); t->pLevel[i]->set(1.0f); t->pSteps[i]->set((float)(i + 1)); }
        t->pLowCut->set(20.0f); t->pHighCut->set(20000.0f);
        Stereo in = silence(30.0);
        dsp::Noise nz(2u);
        for (size_t i = 0; i < 48000; ++i) in.l[i] = in.r[i] = 0.5f * nz.next();
        auto out = run(*t, in);
        CHECK(allFinite(out));
        CHECK_MSG(peak(out) < 40.0f, "peak %.1f", peak(out));
        CHECK(tu::rmsOf(out.l, 29 * 48000) < 0.5 * tu::rmsOf(out.l, 9 * 48000, 10 * 48000));
        CHECK(t->tailSeconds() > 10.0 && t->tailSeconds() <= 60.0);
    }
    {   // the tail it reports: the longest tap with no feedback, longer with it
        auto t = bare();
        t->pCount->set(4.0f);
        run(*t, silence(0.01));          // at 120 BPM
        CHECK_NEAR(t->tailSeconds(), 12.0 * 0.125, 1e-3);
        t->pFeedback->set(0.5f);
        CHECK_NEAR(t->tailSeconds(), 1.5 * (1.0 + std::log(0.001) / std::log(0.5)), 1e-2);
    }
}

TEST(taps_pan_tone_and_mix) {
    {   // a mono signal follows the mixer's pan law; spread scales every pan
        auto t = bare();
        t->pCount->set(3.0f);
        t->pSteps[0]->set(1.0f); t->pSteps[1]->set(2.0f); t->pSteps[2]->set(3.0f);
        for (int i = 0; i < 3; ++i) t->pLevel[i]->set(0.8f);
        t->pPan[0]->set(-1.0f); t->pPan[1]->set(0.5f); t->pPan[2]->set(0.0f);
        auto out = run(*t, impulse(0.5));
        CHECK_NEAR(out.l[6000], 0.8, 1e-6); CHECK(out.r[6000] == 0.0f);
        CHECK_NEAR(out.l[12000], 0.4, 1e-6); CHECK_NEAR(out.r[12000], 0.8, 1e-6);
        CHECK_NEAR(out.l[18000], 0.8, 1e-6); CHECK_NEAR(out.r[18000], 0.8, 1e-6);
        t->pSpread->set(0.5f);
        t->reset();
        out = run(*t, impulse(0.5));
        CHECK_NEAR(out.l[6000], 0.8, 1e-6); CHECK_NEAR(out.r[6000], 0.4, 1e-6);
        t->pSpread->set(0.0f);
        t->reset();
        out = run(*t, impulse(0.5));
        CHECK(out.l == out.r);
        // a stereo signal: in the middle each side stays where it was; panned hard right, the left side is folded in, not lost
        t->pSpread->set(1.0f);
        t->pPan[0]->set(1.0f);
        t->reset();
        Stereo leftOnly = impulse(0.5);
        leftOnly.r[0] = 0.0f;
        out = run(*t, leftOnly);
        CHECK(out.l[6000] == 0.0f); CHECK_NEAR(out.r[6000], 0.4, 1e-6);
        CHECK_NEAR(out.l[18000], 0.8, 1e-6); CHECK(out.r[18000] == 0.0f);
    }
    {   // tone: negative darkens a tap, positive thins it, zero leaves it alone
        auto t = bare();
        t->pCount->set(3.0f);
        t->pSteps[0]->set(1.0f); t->pSteps[1]->set(2.0f); t->pSteps[2]->set(3.0f);
        for (int i = 0; i < 3; ++i) t->pLevel[i]->set(1.0f);
        t->pTone[0]->set(-0.7f); t->pTone[1]->set(0.0f); t->pTone[2]->set(0.7f);
        Stereo in = silence(0.6);
        dsp::Noise nz(4u);
        for (size_t i = 0; i < 3000; ++i) in.l[i] = in.r[i] = 0.5f * nz.next();
        auto out = run(*t, in);
        auto band = [&](size_t from, double lo, double hi) {
            std::vector<float> seg(out.l.begin() + (std::ptrdiff_t)from, out.l.begin() + (std::ptrdiff_t)from + 3000);
            double e = 0.0;
            for (double hz = lo; hz < hi; hz *= 1.1) e += std::pow(magAt(seg, hz, kSr), 2.0);
            return e;
        };
        CHECK(band(6000, 5000, 15000) < 0.02 * band(12000, 5000, 15000));       // dark
        CHECK(band(6000, 100, 300) > 0.7 * band(12000, 100, 300));
        CHECK(band(18000, 100, 300) < 0.1 * band(12000, 100, 300));             // thin
        CHECK(band(18000, 5000, 15000) > 0.7 * band(12000, 5000, 15000));
        for (size_t i = 0; i < 3000; ++i) CHECK(out.l[12000 + i] == in.l[i]);   // untouched
    }
    {   // dry / wet: dry only and bit-exact at 0, both at full level in the middle
        auto in = noise(0.4, 0.4f);
        auto t = std::make_shared<Taps>();
        t->prepare(kSr);
        t->pMix->set(0.0f);
        auto dry = run(*t, in);
        CHECK(dry.l == in.l && dry.r == in.r);
        auto w = std::make_shared<Taps>();
        w->prepare(kSr);
        w->pMix->set(1.0f);
        auto wet = run(*w, in);
        auto m = std::make_shared<Taps>();
        m->prepare(kSr);
        m->pMix->set(0.5f);
        auto both = run(*m, in);
        double worst = 0.0;
        for (size_t i = 0; i < both.l.size(); ++i) worst = std::max(worst, (double)std::fabs(both.l[i] - (in.l[i] + wet.l[i])));
        CHECK(worst < 1e-6);
        CHECK(t->latencySamples() == 0);
    }
}

TEST(taps_rhythm_presets_fall_on_their_grid) {
    Taps probe;
    const auto presets = probe.factoryPresets();
    CHECK(presets.size() >= 10);
    // tap positions in samples at 120 BPM (a sixteenth is 6000 samples), in time order
    struct Want { const char* name; std::vector<double> at; };
    const std::vector<Want> wants = {
        {"Dotted eighths", {18000, 36000, 54000, 72000}},
        {"Swung eighths", {16000, 24000, 40000, 48000}},                    // the off-beats sit on the triplet
        {"Gallop", {12000, 18000, 24000, 36000, 42000, 48000}},             // eighth, two sixteenths, twice
        {"Ping-pong spread", {12000, 24000, 36000, 48000, 60000, 72000}},
        {"Slapback cluster", {2976, 4176, 4992, 6288}},
        {"Triplet cascade", {8000, 16000, 24000, 32000, 40000, 48000}},
        {"Quarter-note build", {24000, 48000, 72000, 96000}},
        {"Sixteenth roll", {6000, 12000, 18000, 24000, 30000, 36000, 42000, 48000}},
        {"Wide doubler", {528, 816, 1104, 1392}},
        {"Dub scatter", {18000, 42000, 60000, 84000}},
    };
    for (auto& w : wants) {
        const Preset* pr = nullptr;
        for (auto& p : presets) if (p.name == w.name) pr = &p;
        CHECK_MSG(pr != nullptr, "no preset called %s", w.name);
        if (!pr) continue;
        Taps t;
        t.prepare(kSr);
        t.loadState(pr->state);
        t.pMix->set(1.0f);
        t.pFeedback->set(0.0f);
        for (int i = 0; i < Taps::kTaps; ++i) { t.pTapFb[i]->set(0.0f); t.pTone[i]->set(0.0f); }
        auto out = run(t, impulse(2.2));
        std::vector<size_t> got;
        for (size_t i = 0; i < out.l.size(); ++i) if (out.l[i] != 0.0f || out.r[i] != 0.0f) got.push_back(i);
        CHECK_MSG(got.size() == w.at.size(), "%s: %d taps", w.name, (int)got.size());
        for (size_t i = 0; i < got.size() && i < w.at.size(); ++i) CHECK_MSG((double)got[i] == w.at[i], "%s: tap at %d, expected %.0f", w.name, (int)got[i], w.at[i]);
    }
    {   // the ping-pong really alternates sides, the build really builds
        Taps t;
        t.prepare(kSr);
        for (auto& p : presets) if (p.name == "Ping-pong spread") t.loadState(p.state);
        t.pMix->set(1.0f); t.pFeedback->set(0.0f);
        auto out = run(t, impulse(1.6));
        CHECK(out.l[12000] > 0.5f && out.r[12000] == 0.0f);
        CHECK(out.r[24000] > 0.5f && out.l[24000] == 0.0f);
        CHECK(out.l[36000] > out.r[36000] && out.r[48000] > out.l[48000]);
        Taps b;
        b.prepare(kSr);
        for (auto& p : presets) if (p.name == "Quarter-note build") b.loadState(p.state);
        b.pMix->set(1.0f);
        auto o2 = run(b, impulse(2.2));
        const float a1 = peak(o2, 24000, 24001), a2 = peak(o2, 48000, 48001), a3 = peak(o2, 72000, 72001), a4 = peak(o2, 96000, 96001);
        CHECK(a1 < a2 && a2 < a3 && a3 < a4);
    }
}

TEST(taps_changes_while_running_do_not_click) {
    auto t = std::make_shared<Taps>();
    t->prepare(kSr);
    t->pMix->set(1.0f);
    Stereo in = tone(220.0, 4.0, 0.5f);
    Stereo out = in;
    Transport tr;
    for (size_t i = 0; i < in.l.size(); i += kQuantum) {
        const size_t k = i / kQuantum;
        if (k % 300 == 100) t->pSteps[0]->set((float)(1 + (k / 300) % 7));      // a tap moves: it is crossfaded to its new place
        if (k % 300 == 200) t->pCount->set((float)(1 + (k / 300) % 8));         // taps come and go
        if (k % 500 == 250) t->pSync->set((float)((k / 500) % 2));
        t->pPan[1]->set((float)std::sin(k * 0.02));
        t->pLevel[2]->set(0.5f + 0.5f * (float)std::sin(k * 0.013));
        t->pMix->set(0.75f + 0.25f * (float)std::sin(k * 0.031));
        t->pFeedback->set(0.3f + 0.3f * (float)std::sin(k * 0.007));
        ProcessContext c;
        c.tempo = 120.0 + 20.0 * std::sin(k * 0.001);                            // and the tempo drifts under it
        c.io[0] = out.l.data() + i;
        c.io[1] = out.r.data() + i;
        t->process(c);
    }
    CHECK(allFinite(out));
    float step = 0.0f;
    for (size_t i = 1; i < out.l.size(); ++i) step = std::max(step, std::max(std::fabs(out.l[i] - out.l[i - 1]), std::fabs(out.r[i] - out.r[i - 1])));
    // 220 Hz at this level moves about 0.015 per sample per tap; several taps and feedback add up, a click would be far larger
    CHECK_MSG(step < 0.2f, "largest step %.3f (peak %.2f)", step, peak(out));
}
