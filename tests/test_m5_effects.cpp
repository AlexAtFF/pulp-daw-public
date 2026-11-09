// Milestone 5: each core effect against a known response.
#include "Util.h"
#include "pulp/Effects.h"
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0;

struct Stereo { std::vector<float> l, r; };

// Runs a plugin over a signal in 64-sample quanta, as the engine does.
Stereo run(Plugin& p, const Stereo& in, double tempo = 120.0, const Stereo* side = nullptr) {
    Stereo out = in;
    const size_t n = in.l.size() / kQuantum * kQuantum;
    out.l.resize(n); out.r.resize(n);
    for (size_t i = 0; i < n; i += kQuantum) {
        ProcessContext c;
        c.sampleRate = kSr;
        c.tempo = tempo;
        c.io[0] = out.l.data() + i;
        c.io[1] = out.r.data() + i;
        if (side) { c.sidechain[0] = side->l.data() + i; c.sidechain[1] = side->r.data() + i; }
        p.process(c);
    }
    return out;
}
Stereo tone(double hz, double seconds, float amp, float ampR = -1.0f) {
    Stereo s;
    const size_t n = (size_t)(seconds * kSr);
    s.l.resize(n); s.r.resize(n);
    for (size_t i = 0; i < n; ++i) { const float v = (float)std::sin(kTwoPi * hz * (double)i / kSr); s.l[i] = amp * v; s.r[i] = (ampR < 0 ? amp : ampR) * v; }
    return s;
}
Stereo impulse(double seconds, float amp = 1.0f) {
    Stereo s;
    s.l.assign((size_t)(seconds * kSr), 0.0f);
    s.r = s.l;
    s.l[0] = s.r[0] = amp;
    return s;
}
Stereo noise(double seconds, float amp) {
    Stereo s;
    dsp::Noise a(11u), b(29u);
    s.l.resize((size_t)(seconds * kSr)); s.r.resize(s.l.size());
    for (size_t i = 0; i < s.l.size(); ++i) { s.l[i] = amp * a.next(); s.r[i] = amp * b.next(); }
    return s;
}
// Gain in dB a plugin applies to a steady sine, measured after it settles.
double gainDbAt(Plugin& p, double hz, float amp = 0.1f) {
    p.prepare(kSr);
    auto out = run(p, tone(hz, 0.6, amp));
    const size_t a = out.l.size() / 2, b = out.l.size();
    return 20.0 * std::log10(rmsOf(out.l, a, b) / (amp / std::sqrt(2.0)) + 1e-12);
}
// Level in dB of one frequency in a signal (windowed DFT at that single bin), relative to full scale sine.
double levelDbAt(const std::vector<float>& x, size_t from, size_t n, double hz) {
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * (double)i / (double)(n - 1));
        re += w * x[from + i] * std::cos(kTwoPi * hz * (double)i / kSr);
        im += w * x[from + i] * std::sin(kTwoPi * hz * (double)i / kSr);
        wsum += w;
    }
    return 20.0 * std::log10(2.0 * std::sqrt(re * re + im * im) / wsum + 1e-12);
}
bool allFinite(const Stereo& s) {
    for (float v : s.l) if (!std::isfinite(v)) return false;
    for (float v : s.r) if (!std::isfinite(v)) return false;
    return true;
}
}

TEST(contour_eq_matches_its_curve) {
    ContourEq eq;
    eq.prepare(kSr);
    eq.pType[0]->set(ContourEq::LowCut); eq.pFreq[0]->set(80); eq.pQ[0]->set(0.71f);
    eq.pFreq[2]->set(400); eq.pGain[2]->set(-9); eq.pQ[2]->set(2.0f);
    eq.pFreq[4]->set(3000); eq.pGain[4]->set(6); eq.pQ[4]->set(1.2f);
    eq.pGain[6]->set(4.5f);
    eq.pOut->set(-2.0f);
    for (double hz : {40.0, 80.0, 200.0, 400.0, 1000.0, 3000.0, 6000.0, 12000.0, 18000.0}) {
        const double measured = gainDbAt(eq, hz), curve = eq.magnitudeDb(hz);
        CHECK_MSG(std::fabs(measured - curve) < 0.15, "%.0f Hz: measured %.2f dB, curve %.2f dB", hz, measured, curve);
    }
    CHECK_NEAR(eq.magnitudeDb(400.0), -9.0 - 2.0, 0.6);
    CHECK_NEAR(eq.magnitudeDb(3000.0), 6.0 - 2.0, 0.8);
    // flat EQ is exactly transparent
    ContourEq flat;
    flat.prepare(kSr);
    auto in = noise(0.2, 0.5f);
    auto out = run(flat, in);
    in.l.resize(out.l.size());
    CHECK(out.l == in.l);
    // the scope tap holds the latest output
    float tap[256];
    CHECK(flat.readScope(tap, 256) == 256);
    CHECK_NEAR(tap[255], 0.5f * (out.l.back() + out.r.back()), 1e-6);
}

TEST(squeeze_static_ratio_and_sidechain) {
    struct Case { float inDb, thresh, ratio; } cases[] = {{-6, -20, 4}, {-6, -30, 2}, {-12, -24, 10}, {-30, -20, 4}};
    for (auto& cs : cases) {
        Squeeze comp;
        comp.prepare(kSr);
        comp.pThresh->set(cs.thresh); comp.pRatio->set(cs.ratio); comp.pKnee->set(0); comp.pAttack->set(0.1f); comp.pRelease->set(2000);
        const float amp = dbToGain(cs.inDb);
        auto out = run(comp, tone(1000, 1.0, amp));
        const double outDb = 20.0 * std::log10(peakOf(out.l, out.l.size() / 2));
        const double want = cs.inDb <= cs.thresh ? cs.inDb : cs.thresh + (cs.inDb - cs.thresh) / cs.ratio;
        CHECK_MSG(std::fabs(outDb - want) < 0.4, "in %.0f dB thr %.0f ratio %.0f: out %.2f dB, expected %.2f dB", cs.inDb, cs.thresh, cs.ratio, outDb, want);
    }
    // the knee is continuous and symmetric about the threshold
    CHECK_NEAR(Squeeze::curveDb(-20.0f, -20.0f, 4.0f, 12.0f), -20.0f + (0.25f - 1.0f) * 36.0f / 24.0f, 1e-4);
    CHECK_NEAR(Squeeze::curveDb(-26.0f, -20.0f, 4.0f, 12.0f), -26.0f, 1e-4);
    CHECK_NEAR(Squeeze::curveDb(-14.0f, -20.0f, 4.0f, 12.0f), -20.0f + 6.0f / 4.0f, 1e-4);
    // make-up gain
    Squeeze mk;
    mk.pThresh->set(0); mk.pMakeup->set(6);
    CHECK_NEAR(gainDbAt(mk, 1000, 0.05f), 6.0, 0.05);
    // sidechain: a quiet signal is turned down only while the sidechain is loud
    Squeeze sc;
    sc.prepare(kSr);
    sc.pThresh->set(-30); sc.pRatio->set(20); sc.pKnee->set(0); sc.pAttack->set(0.5f); sc.pRelease->set(50); sc.pSide->set(1);
    auto quiet = tone(500, 1.0, 0.02f);
    Stereo side = tone(100, 1.0, 0.8f);
    for (size_t i = side.l.size() / 2; i < side.l.size(); ++i) side.l[i] = side.r[i] = 0.0f;
    auto out = run(sc, quiet, 120.0, &side);
    CHECK(rmsOf(out.l, 4800, 20000) < 0.3 * rmsOf(out.l, 40000, 47000));
    CHECK(sc.meter(0) < 0.5f);
}

TEST(lid_never_exceeds_its_ceiling_and_reports_latency) {
    Lid lim;
    lim.prepare(kSr);
    CHECK(lim.latencySamples() == 239);
    lim.pCeiling->set(-1.0f);
    lim.pGain->set(12.0f);
    const float ceiling = dbToGain(-1.0f);
    // hot noise, a full-scale burst, and single-sample spikes
    auto in = noise(1.0, 0.9f);
    for (size_t i = 10000; i < 14000; ++i) in.l[i] = in.r[i] = (float)std::sin(i * 0.3) * 1.0f;
    in.l[30000] = 4.0f; in.r[30011] = -6.0f;
    auto out = run(lim, in);
    CHECK_MSG(peakOf(out.l) <= ceiling + 1e-6f && peakOf(out.r) <= ceiling + 1e-6f, "peak %.6f over ceiling %.6f", std::max(peakOf(out.l), peakOf(out.r)), ceiling);
    CHECK(rmsOf(out.l, 2000) > 0.4 * ceiling);          // it limits, it does not just turn everything down
    CHECK(lim.meter(0) > 1.0f);
    // below the ceiling the signal passes untouched, just late by the reported latency
    Lid clean;
    clean.prepare(kSr);
    clean.pCeiling->set(0.0f);
    auto q = noise(0.5, 0.25f);
    auto qo = run(clean, q);
    double worst = 0.0;
    for (size_t i = 239; i < qo.l.size(); ++i) worst = std::max(worst, (double)std::fabs(qo.l[i] - q.l[i - 239]));
    CHECK_MSG(worst < 1e-7, "difference %g", worst);
    // gate: below the threshold it closes
    Lid gate;
    gate.prepare(kSr);
    gate.pGate->set(-40.0f);
    auto low = tone(1000, 0.5, dbToGain(-60.0f));
    auto go = run(gate, low);
    CHECK(peakOf(go.l, go.l.size() / 2) < dbToGain(-90.0f));
    auto loud = tone(1000, 0.5, dbToGain(-20.0f));
    gate.reset();
    CHECK_NEAR(peakOf(run(gate, loud).l, 12000), dbToGain(-20.0f), 0.002);
}

TEST(chamber_decay_time_and_dry_wet) {
    for (float decay : {0.6f, 2.0f, 5.0f}) {
        Chamber rv;
        rv.prepare(kSr);
        rv.pDecay->set(decay); rv.pMix->set(1.0f); rv.pEarly->set(0.0f); rv.pDamp->set(0.0f); rv.pPre->set(0.0f); rv.pLowCut->set(20.0f);
        auto out = run(rv, impulse(decay * 1.6 + 1.0));
        CHECK(allFinite(out));
        // Schroeder backward integration: time for the energy curve to fall from -5 to -35 dB, doubled
        std::vector<double> e(out.l.size() + 1, 0.0);
        for (size_t i = out.l.size(); i-- > 0;) e[i] = e[i + 1] + (double)out.l[i] * out.l[i];
        size_t t5 = 0, t35 = 0;
        for (size_t i = 0; i < out.l.size(); ++i) {
            const double db = 10.0 * std::log10(e[i] / e[0] + 1e-30);
            if (t5 == 0 && db <= -5.0) t5 = i;
            if (t35 == 0 && db <= -35.0) { t35 = i; break; }
        }
        const double rt60 = 2.0 * (double)(t35 - t5) / kSr;
        CHECK_MSG(t35 > t5 && std::fabs(rt60 / decay - 1.0) < 0.3, "decay %.1f s: measured RT60 %.2f s", decay, rt60);
    }
    Chamber dry;
    dry.prepare(kSr);
    dry.pMix->set(0.0f);
    auto in = noise(0.3, 0.3f);
    auto out = run(dry, in);
    in.l.resize(out.l.size());
    CHECK(maxAbsDiff(out.l, in.l) < 1e-7);
    // stereo: a mono impulse gives different left and right tails when width is up
    Chamber wide;
    wide.prepare(kSr);
    wide.pMix->set(1.0f);
    auto w = run(wide, impulse(1.0));
    CHECK(maxAbsDiff(w.l, w.r) > 1e-3);
}

TEST(repeater_tap_time_is_exact) {
    {   // free time: 250 ms is 12000 samples
        Repeater d;
        d.prepare(kSr);
        d.pSync->set(0); d.pTime->set(250.0f); d.pMix->set(1.0f); d.pFeedback->set(0.5f); d.pHighCut->set(20000); d.pLowCut->set(20);
        auto out = run(d, impulse(1.0));
        size_t first = 0;
        for (size_t i = 0; i < out.l.size(); ++i) if (std::fabs(out.l[i]) > 0.5f) { first = i; break; }
        CHECK_MSG(first == 12000, "first echo at %d", (int)first);
        CHECK(peakOf(out.l, 0, 11990) < 1e-6f);
        CHECK(peakOf(out.l, 23990, 24020) > 0.3f && peakOf(out.l, 23990, 24020) < 0.55f);      // second echo, quieter
    }
    {   // tempo sync: an eighth note at 140 BPM
        Repeater d;
        d.prepare(kSr);
        d.pSync->set(3); d.pMix->set(1.0f);
        const double want = 480.0 * 60.0 * kSr / (140.0 * kPPQ);
        CHECK_NEAR(d.delaySamples(140.0), want, 1e-6);
        auto out = run(d, impulse(0.6), 140.0);
        size_t first = 0;
        float best = 0.0f;
        for (size_t i = 0; i < 12000; ++i) if (std::fabs(out.l[i]) > best) { best = std::fabs(out.l[i]); first = i; }
        CHECK_MSG(std::fabs((double)first - want) <= 1.0, "echo at %d, expected %.2f", (int)first, want);
    }
    {   // ping-pong alternates sides
        Repeater d;
        d.prepare(kSr);
        d.pSync->set(0); d.pTime->set(100.0f); d.pMix->set(1.0f); d.pPing->set(1); d.pFeedback->set(0.7f);
        auto out = run(d, impulse(0.5));
        CHECK(peakOf(out.l, 4790, 4810) > 0.3f && peakOf(out.r, 4790, 4810) < 1e-6f);
        CHECK(peakOf(out.r, 9590, 9620) > 0.2f && peakOf(out.l, 9590, 9620) < 1e-6f);
    }
}

TEST(shimmer_and_stereo_kit) {
    Shimmer ch;
    ch.prepare(kSr);
    ch.pMix->set(0.0f);
    auto in = tone(440, 0.5, 0.3f);
    auto dry = run(ch, in);
    in.l.resize(dry.l.size());
    CHECK(maxAbsDiff(dry.l, in.l) < 1e-7);
    ch.pMix->set(0.5f);
    ch.reset();
    auto wet = run(ch, in);
    CHECK(allFinite(wet) && maxAbsDiff(wet.l, in.l) > 0.05 && peakOf(wet.l) < 0.7f);
    CHECK(maxAbsDiff(wet.l, wet.r) > 0.01);     // the two sides are modulated differently

    StereoKit sk;
    sk.prepare(kSr);
    auto lr = tone(300, 0.2, 0.4f, 0.1f);       // louder on the left
    sk.pWidth->set(0.0f);
    auto mono = run(sk, lr);
    CHECK(maxAbsDiff(mono.l, mono.r) < 1e-7);
    CHECK_NEAR(peakOf(mono.l, 2000), 0.25, 0.002);
    StereoKit sw;
    sw.prepare(kSr);
    sw.pSwap->set(1); sw.pGain->set(6.0f);
    auto swapped = run(sw, lr);
    CHECK_NEAR(peakOf(swapped.l, 2000), 0.1 * dbToGain(6.0f), 0.002);
    CHECK_NEAR(peakOf(swapped.r, 2000), 0.4 * dbToGain(6.0f), 0.004);
    // mono below: a side-only low tone is removed, a side-only high tone is kept
    auto sideOnly = [](double hz) { auto s = tone(hz, 0.5, 0.3f); for (auto& v : s.r) v = -v; return s; };
    StereoKit mb;
    mb.prepare(kSr);
    mb.pMonoBelow->set(300.0f);
    CHECK(rmsOf(run(mb, sideOnly(60)).l, 12000) < 0.08 * 0.3 / std::sqrt(2.0));
    mb.reset();
    CHECK(rmsOf(run(mb, sideOnly(5000)).l, 12000) > 0.9 * 0.3 / std::sqrt(2.0));
}

TEST(grit_latency_harmonics_and_aliasing) {
    {   // nearly clean at low level: the input comes back 16 samples late
        Grit g;
        g.prepare(kSr);
        CHECK(g.latencySamples() == 16);
        g.pDrive->set(0.0f); g.pOut->set(0.0f);
        auto in = tone(1000, 0.3, 0.01f);
        auto out = run(g, in);
        double worst = 0.0;
        for (size_t i = 2000; i < out.l.size(); ++i) worst = std::max(worst, (double)std::fabs(out.l[i] - in.l[i - 16]));
        CHECK_MSG(worst < 0.01 * 0.01, "difference %g", worst);
    }
    {   // A 9 kHz tone: its 5th harmonic (45 kHz) would fold back to 3 kHz without oversampling.
        const size_t from = 8000, n = 8192;
        auto aliasDb = [&](const std::vector<float>& x) { return levelDbAt(x, from, n, 3000.0) - levelDbAt(x, from, n, 9000.0); };
        // soft clip, moderately driven
        Grit g;
        g.prepare(kSr);
        g.pDrive->set(12.0f); g.pType->set(0); g.pOut->set(0.0f);
        auto out = run(g, tone(9000, 0.5, 0.5f));
        std::vector<float> naive(out.l.size());
        for (size_t i = 0; i < naive.size(); ++i) naive[i] = std::tanh((float)(0.5 * 3.981 * std::sin(kTwoPi * 9000.0 * (double)i / kSr)));
        const double ours = aliasDb(out.l), plain = aliasDb(naive);
        CHECK_MSG(ours < -60.0 && ours < plain - 25.0, "soft clip alias %.1f dB (no oversampling: %.1f dB)", ours, plain);
        // hard clip, driven into a square wave: harmonics fall off slowly, so 4x oversampling helps less
        Grit h;
        h.prepare(kSr);
        h.pDrive->set(30.0f); h.pType->set(1); h.pOut->set(0.0f);
        auto hard = run(h, tone(9000, 0.5, 0.5f));
        for (size_t i = 0; i < naive.size(); ++i) naive[i] = clampT((float)(0.5 * 31.62 * std::sin(kTwoPi * 9000.0 * (double)i / kSr)), -1.0f, 1.0f);
        const double oursHard = aliasDb(hard.l), plainHard = aliasDb(naive);
        CHECK_MSG(oursHard < plainHard - 8.0, "hard clip alias %.1f dB (no oversampling: %.1f dB)", oursHard, plainHard);
        CHECK(levelDbAt(hard.l, from, n, 9000.0) > -6.0);
    }
    {   // a 1 kHz tone through the soft clipper gains a third harmonic but no second; bias adds the second
        Grit g;
        g.prepare(kSr);
        g.pDrive->set(18.0f);
        auto out = run(g, tone(1000, 0.5, 0.3f));
        const double f1 = levelDbAt(out.l, 8000, 8192, 1000.0);
        CHECK(levelDbAt(out.l, 8000, 8192, 3000.0) - f1 > -25.0);
        CHECK(levelDbAt(out.l, 8000, 8192, 2000.0) - f1 < -70.0);
        g.pBias->set(0.6f);
        g.reset();
        auto biased = run(g, tone(1000, 0.5, 0.3f));
        CHECK(levelDbAt(biased.l, 8000, 8192, 2000.0) - levelDbAt(biased.l, 8000, 8192, 1000.0) > -35.0);
        CHECK(std::fabs(biased.l.back()) < 1.0f);
    }
}

TEST(sweep_filter_responses) {
    auto make = [](int type, float cutoff, int slope, float reso = 0.0f) {
        auto f = std::make_shared<Sweep>();
        f->pType->set((float)type); f->pCutoff->set(cutoff); f->pSlope->set((float)slope); f->pReso->set(reso);
        return f;
    };
    auto lp = make(0, 1000, 0);
    CHECK_NEAR(gainDbAt(*lp, 100), 0.0, 0.1);
    CHECK_NEAR(gainDbAt(*lp, 1000), -3.0, 0.2);
    CHECK_NEAR(gainDbAt(*lp, 4000), -24.6, 1.0);
    auto lp24 = make(0, 1000, 1);
    CHECK_NEAR(gainDbAt(*lp24, 1000), -6.0, 0.3);
    CHECK_NEAR(gainDbAt(*lp24, 4000), -49.2, 2.0);
    auto hp = make(1, 1000, 0);
    CHECK_NEAR(gainDbAt(*hp, 8000), 0.0, 0.2);
    CHECK_NEAR(gainDbAt(*hp, 250), -24.1, 1.0);
    auto bp = make(2, 1000, 0, 0.5f);
    CHECK_NEAR(gainDbAt(*bp, 1000), 0.0, 0.2);
    CHECK(gainDbAt(*bp, 200) < -20.0 && gainDbAt(*bp, 5000) < -20.0);
    auto notch = make(3, 1000, 0, 0.3f);
    CHECK(gainDbAt(*notch, 1000) < -40.0);
    CHECK_NEAR(gainDbAt(*notch, 100), 0.0, 0.3);
    auto res = make(0, 1000, 0, 0.6f);
    CHECK(gainDbAt(*res, 1000) > 12.0);         // resonance peaks at the cutoff
}

TEST(every_effect_preset_is_stable_and_reloads) {
    int effects = 0;
    for (auto& t : PluginRegistry::global().types()) {
        if (t.instrument) continue;
        ++effects;
        auto probe = t.create();
        const auto presets = probe->factoryPresets();
        CHECK_MSG(presets.size() >= 3, "%s has %d presets", t.name.c_str(), (int)presets.size());
        for (auto& pr : presets) {
            auto p = t.create();
            p->prepare(kSr);
            p->loadState(pr.state);
            auto in = noise(0.5, 0.3f);
            for (size_t i = 0; i < 2000; ++i) in.l[i] = in.r[i] = 0.9f * (float)std::sin(i * 0.05);
            auto out = run(*p, in);
            CHECK_MSG(allFinite(out), "%s / %s is not finite", t.name.c_str(), pr.name.c_str());
            CHECK_MSG(peakOf(out.l) < 8.0f, "%s / %s runs away (peak %.1f)", t.name.c_str(), pr.name.c_str(), peakOf(out.l));
            // save, recreate, load: same state
            auto q = PluginRegistry::global().create(p->typeId());
            CHECK(q != nullptr);
            if (!q) continue;
            q->prepare(kSr);
            q->loadState(p->saveState());
            CHECK(q->saveState() == p->saveState());
        }
        // every parameter clamps to its range and has a readable value
        for (auto& prm : probe->params()) {
            prm->set(prm->info.max + 1000.0f);
            CHECK(prm->get() == prm->info.max);
            prm->set(prm->info.min - 1000.0f);
            CHECK(prm->get() == prm->info.min);
            CHECK(!prm->display().empty());
        }
    }
    CHECK(effects >= 9);
}
