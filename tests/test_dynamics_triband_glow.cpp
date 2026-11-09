// Triband (three-band compressor and maximizer) and Glow (one-knob enhancer) against known responses.
#include "DynTestUtil.h"
using namespace pulp;
using namespace tu;
using namespace dt;

namespace {
const double kSr = 48000.0;

// every band at ratio 1: no gain reduction anywhere
void neutral(Triband& tb) {
    for (int b = 0; b < Triband::kBands; ++b) tb.pRatio[b]->set(1.0f);
    tb.pCeiling->set(0.0f);
}
}

TEST(triband_bands_sum_to_an_all_pass_of_the_input) {
    struct Case { double sr; float xl, xh; } cases[] = {
        {48000, 160, 3200}, {48000, 40, 800}, {48000, 1500, 14000}, {44100, 300, 5000}, {96000, 200, 6000}, {48000, 1000, 1000}};
    double worstFlat = 0.0, worstNull = -400.0;
    for (auto& cs : cases) {
        Triband tb;
        tb.prepare(cs.sr);
        tb.pXLow->set(cs.xl); tb.pXHigh->set(cs.xh);
        neutral(tb);
        const int lat = tb.latencySamples();
        CHECK(lat == (int)std::lround(0.004 * cs.sr));
        CHECK(tb.crossoverHigh() >= tb.crossoverLow() * 1.5);

        // flat magnitude: the spectrum of the impulse response is 1 everywhere
        const int n = 65536;
        Stereo imp = silence((double)n / cs.sr, cs.sr);
        imp.l.resize((size_t)n, 0.0f); imp.r.resize((size_t)n, 0.0f);
        imp.l[0] = imp.r[0] = 0.5f;
        auto ir = run(tb, imp, cs.sr);
        std::vector<float> re = ir.l, im((size_t)n, 0.0f);
        dsp::fft(re.data(), im.data(), n);
        double flat = 0.0;
        for (int k = 0; k <= n / 2; ++k) flat = std::max(flat, std::fabs(db(std::sqrt((double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k]) / 0.5)));
        CHECK_MSG(flat < 0.01, "%.0f Hz / %.0f Hz at %.0f: magnitude off by %.5f dB", cs.xl, cs.xh, cs.sr, flat);
        worstFlat = std::max(worstFlat, flat);

        // null: the output is the input through the two all-pass filters the crossovers amount to, late by the latency
        tb.reset();
        auto in = noise(0.5, 0.25f, cs.sr);        // quiet enough that the limiter has nothing to do
        auto out = run(tb, in, cs.sr);
        CHECK(tb.meter(3) == 0.0f);
        dsp::Biquad ap1[2], ap2[2];
        for (int c = 0; c < 2; ++c) { ap1[c].allpass(tb.crossoverLow(), 0.70710678118654752, cs.sr); ap2[c].allpass(tb.crossoverHigh(), 0.70710678118654752, cs.sr); }
        Stereo ref = in;
        for (size_t i = 0; i < ref.l.size(); ++i) { ref.l[i] = ap2[0].process(ap1[0].process(in.l[i])); ref.r[i] = ap2[1].process(ap1[1].process(in.r[i])); }
        const double resid = db(std::max(maxDiffDelayed(out.l, ref.l, lat, 0), maxDiffDelayed(out.r, ref.r, lat, 0)) / 0.25);
        CHECK_MSG(resid < -100.0, "%.0f Hz / %.0f Hz at %.0f: residual %.1f dB", cs.xl, cs.xh, cs.sr, resid);
        worstNull = std::max(worstNull, resid);
        // and it is not simply the input: the all-pass turns the phase
        CHECK(maxDiffDelayed(out.l, in.l, lat, 0) > 0.02);
        CHECK(peakOf(out.l, 0, (size_t)lat) == 0.0f);       // nothing comes out before the latency has passed
    }
    FIG("Triband crossover: magnitude within %.5f dB of flat, null against the all-pass reference %.1f dB", worstFlat, worstNull);
    CHECK(Triband().latencySamples() == 192);
}

TEST(triband_static_curve_in_each_band) {
    struct Case { int band; double hz; float xl, xh, inDb, thr, ratio, knee; double want; } cases[] = {
        {0, 60, 400, 4000, -6, -20, 4, 0, -20 + 14.0 / 4},
        {0, 60, 400, 4000, -12, -30, 2, 0, -30 + 18.0 / 2},
        {1, 1000, 100, 8000, -6, -24, 10, 0, -24 + 18.0 / 10},
        {1, 1000, 100, 8000, -30, -20, 4, 0, -30},                      // under the threshold: untouched
        {1, 1000, 100, 8000, -20, -20, 4, 12, -20 - 0.75 * 36 / 24},    // in the middle of a 12 dB knee
        {2, 9000, 150, 1000, -6, -20, 3, 0, -20 + 14.0 / 3},
        {2, 9000, 150, 1000, -20, -30, 0.5f, 0, -30 + 10.0 / 0.5},      // a ratio below 1 expands upwards
        {0, 60, 400, 4000, -10, -30, 0.5f, 0, -10 + 12.0},              // ... by 12 dB at the most
    };
    for (auto& cs : cases) {
        Triband tb;
        tb.prepare(kSr);
        tb.pXLow->set(cs.xl); tb.pXHigh->set(cs.xh);
        tb.pLimit->set(0.0f);
        const int b = cs.band;
        tb.pThresh[b]->set(cs.thr); tb.pRatio[b]->set(cs.ratio); tb.pKnee[b]->set(cs.knee); tb.pAttack[b]->set(0.1f); tb.pRelease[b]->set(2000.0f);
        auto out = run(tb, tone(cs.hz, 1.0, dbToGain(cs.inDb)));
        const double outDb = db(peakOf(out.l, out.l.size() / 2));
        CHECK_MSG(std::fabs(outDb - cs.want) < 0.4, "band %d in %.0f dB thr %.0f ratio %.1f knee %.0f: out %.2f dB, expected %.2f dB", b, cs.inDb, cs.thr, cs.ratio, cs.knee, outDb, cs.want);
        // the band's meter shows the reduction, the other two show none
        CHECK_MSG(std::fabs(tb.meter(b) - (cs.inDb - cs.want)) < 0.5, "meter %.2f, expected %.2f", tb.meter(b), cs.inDb - cs.want);
        for (int o = 0; o < Triband::kBands; ++o) if (o != b) CHECK(std::fabs(tb.meter(o)) < 0.05f);
        CHECK(tb.meter(3) == 0.0f);
    }
}

TEST(triband_attack_and_release_time_constants) {
    // A DC step sits wholly in the low band, so the band gain can be read straight off the output.
    Triband tb;
    tb.prepare(kSr);
    tb.pXLow->set(1500.0f);
    tb.pThresh[0]->set(-20.0f); tb.pRatio[0]->set(4.0f); tb.pKnee[0]->set(0.0f); tb.pAttack[0]->set(50.0f); tb.pRelease[0]->set(200.0f);
    const int lat = tb.latencySamples();
    const size_t n0 = 4800, n1 = n0 + 24000;
    Stereo in = silence(0.1);
    append(in, level(0.5, 0.5f));
    append(in, level(1.2, 0.05f));
    auto out = run(tb, in);
    auto gainDb = [&](size_t n) { return db(out.l[n + (size_t)lat] / in.l[n]); };
    const double full = (db(0.5) + 20.0) * 0.75;        // static reduction for -6 dB in: 10.48 dB
    CHECK_NEAR(gainDb(n1 - 1), -full, 0.05);
    // attack: 63 % of the way after 50 ms, 86 % after 100 ms
    CHECK_NEAR(gainDb(n0 + 2400), -full * (1.0 - std::exp(-1.0)), 0.25);
    CHECK_NEAR(gainDb(n0 + 4800), -full * (1.0 - std::exp(-2.0)), 0.25);
    // release: 37 % left after 200 ms, 14 % after 400 ms
    CHECK_NEAR(gainDb(n1 + 9600), -full * std::exp(-1.0), 0.2);
    CHECK_NEAR(gainDb(n1 + 19200), -full * std::exp(-2.0), 0.2);
    FIG("Triband attack 50 ms: %.2f dB at 50 ms (ideal %.2f); release 200 ms: %.2f dB at 200 ms (ideal %.2f)",
        gainDb(n0 + 2400), -full * (1.0 - std::exp(-1.0)), gainDb(n1 + 9600), -full * std::exp(-1.0));
}

TEST(triband_mute_solo_and_makeup) {
    auto level1k = [](const std::function<void(Triband&)>& setup) {
        Triband tb;
        tb.prepare(kSr);
        tb.pXLow->set(100.0f); tb.pXHigh->set(10000.0f);
        neutral(tb);
        tb.pLimit->set(0.0f);
        setup(tb);
        auto out = run(tb, tone(1000, 0.5, 0.1f));
        return db(peakOf(out.l, out.l.size() / 2) / 0.1);
    };
    CHECK_NEAR(level1k([](Triband&) {}), 0.0, 0.02);
    CHECK_NEAR(level1k([](Triband& t) { t.pMakeup[1]->set(6.0f); }), 6.0, 0.05);
    CHECK_NEAR(level1k([](Triband& t) { t.pMakeup[1]->set(-12.0f); }), -12.0, 0.05);
    CHECK(level1k([](Triband& t) { t.pMute[1]->set(1.0f); }) < -70.0);
    CHECK(level1k([](Triband& t) { t.pSolo[0]->set(1.0f); }) < -70.0);
    CHECK(level1k([](Triband& t) { t.pSolo[2]->set(1.0f); }) < -70.0);
    CHECK_NEAR(level1k([](Triband& t) { t.pSolo[1]->set(1.0f); }), 0.0, 0.02);
    CHECK_NEAR(level1k([](Triband& t) { t.pSolo[1]->set(1.0f); t.pSolo[2]->set(1.0f); t.pMute[0]->set(1.0f); }), 0.0, 0.02);
    CHECK(level1k([](Triband& t) { t.pSolo[1]->set(1.0f); t.pMute[1]->set(1.0f); }) < -70.0);
    // muting a band in the middle of a tone fades it out over a quantum instead of cutting it
    Triband tb;
    tb.prepare(kSr);
    neutral(tb);
    tb.pLimit->set(0.0f);
    auto in = tone(1000, 0.2, 0.5f);
    Stereo a = in, b = in;
    a.l.resize(4800); a.r.resize(4800);
    b.l.erase(b.l.begin(), b.l.begin() + 4800); b.r.erase(b.r.begin(), b.r.begin() + 4800);
    auto o1 = run(tb, a);
    tb.pMute[1]->set(1.0f);
    auto o2 = run(tb, b);
    append(o1, o2);
    CHECK(maxStep(o1.l) <= maxStep(in.l) * 1.05f);
    CHECK(peakOf(o1.l, 6000) < 0.02f);
}

TEST(triband_maximizer_holds_its_ceiling) {
    Triband tb;
    tb.prepare(kSr);
    tb.pMaximize->set(12.0f);
    tb.pCeiling->set(-1.0f);
    const float ceiling = dbToGain(-1.0f);
    auto in = noise(1.0, 0.9f);
    for (size_t i = 10000; i < 14000; ++i) in.l[i] = in.r[i] = (float)std::sin(i * 0.3);
    in.l[30000] = 4.0f; in.r[30011] = -6.0f;
    auto out = run(tb, in);
    CHECK(allFinite(out));
    CHECK_MSG(peakOf(out.l) <= ceiling + 1e-6f && peakOf(out.r) <= ceiling + 1e-6f, "peak %.6f over ceiling %.6f", std::max(peakOf(out.l), peakOf(out.r)), ceiling);
    CHECK(rmsOf(out.l, 2000) > 0.35 * ceiling);         // it limits, it does not just turn everything down
    CHECK(tb.meter(3) > 1.0f);

    // maximize is gain into the limiter: a quiet signal comes up by that much, a loud one stops at the ceiling
    auto loud = [](float maximize, float amp) {
        Triband t;
        t.prepare(kSr);
        neutral(t);
        t.pCeiling->set(-0.5f);
        t.pMaximize->set(maximize);
        auto o = run(t, groove(2.0, amp));
        return std::pair<double, double>(rmsOf(o.l, 9600), peakOf(o.l));
    };
    const auto q0 = loud(0.0f, 0.05f), q9 = loud(9.0f, 0.05f);
    CHECK_NEAR(db(q9.first / q0.first), 9.0, 0.05);
    const auto l0 = loud(0.0f, 0.5f), l12 = loud(12.0f, 0.5f);
    CHECK(l12.second <= dbToGain(-0.5f) + 1e-6f);
    CHECK_MSG(db(l12.first / l0.first) > 6.0, "only %.2f dB louder", db(l12.first / l0.first));
    FIG("Triband maximize 12 dB on a -6 dBFS groove: %.2f dB more RMS, peak %.3f dBFS", db(l12.first / l0.first), db(l12.second));

    // limiter release: after a loud stretch the gain comes back with the release time constant
    {
        Triband t;
        t.prepare(kSr);
        neutral(t);
        t.pXLow->set(1500.0f);
        t.pCeiling->set(-6.0f);
        t.pLimRelease->set(100.0f);
        const size_t lat = (size_t)t.latencySamples(), n1 = 14400;
        Stereo in = level(0.1, 0.25f);
        append(in, level(0.2, 1.0f));               // 6 dB over the ceiling for 200 ms (the step itself rings a little higher)
        append(in, level(0.6, 0.25f));
        const auto o = run(t, in);
        auto gain = [&](size_t n) { return (double)o.l[n + lat] / (double)in.l[n]; };
        CHECK(gain(4000) == 1.0);
        CHECK_NEAR(gain(n1 - 1000), dbToGain(-6.0f), 1e-6);      // held exactly at the ceiling
        const size_t m0 = n1 + 400;
        CHECK(gain(m0) < 0.6);
        CHECK_NEAR((1.0 - gain(m0 + 4800)) / (1.0 - gain(m0)), std::exp(-1.0), 0.01);
        CHECK_NEAR((1.0 - gain(m0 + 9600)) / (1.0 - gain(m0)), std::exp(-2.0), 0.01);
        FIG("Triband limiter release 100 ms: %.4f of the reduction left after 100 ms (ideal 0.3679)", (1.0 - gain(m0 + 4800)) / (1.0 - gain(m0)));
    }

    // limiter switched off: no ceiling, same latency
    Triband off;
    off.prepare(kSr);
    neutral(off);
    off.pLimit->set(0.0f);
    off.pMaximize->set(12.0f);
    CHECK(off.latencySamples() == tb.latencySamples());
    CHECK(peakOf(run(off, tone(1000, 0.3, 0.5f)).l, 4800) > 1.9f);
}

// ------------------------------------------------------------------ Glow

TEST(glow_at_zero_is_transparent_and_reports_its_latency) {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        Glow g;
        g.prepare(sr);
        g.pAmount->set(0.0f);
        const int lat = g.latencySamples();
        CHECK(lat == (int)std::lround(0.0015 * sr) + 32 + (int)std::lround(0.002 * sr));
        for (int character = 0; character < 3; ++character) {
            g.pCharacter->set((float)character);
            g.reset();
            auto in = groove(0.6, 0.9f, sr);
            addTo(in, noise(0.6, 0.05f, sr));
            auto out = run(g, in, sr);
            CHECK_MSG(maxDiffDelayed(out.l, in.l, lat, 0) == 0.0 && maxDiffDelayed(out.r, in.r, lat, 0) == 0.0,
                      "character %d at %.0f: differs by %g", character, sr, maxDiffDelayed(out.l, in.l, lat, 0));
            CHECK(peakOf(out.l, 0, (size_t)lat) == 0.0f);
        }
    }
    CHECK(Glow().latencySamples() == 200);
    // with the knob up the latency is the same: a click still comes out 200 samples later
    Glow g;
    g.prepare(kSr);
    g.pAmount->set(0.6f);
    Stereo imp = silence(0.1);
    imp.l[1000] = imp.r[1000] = 0.5f;
    auto out = run(g, imp);
    size_t at = 0;
    for (size_t i = 0; i < out.l.size(); ++i) if (std::fabs(out.l[i]) > std::fabs(out.l[at])) at = i;
    CHECK_MSG(at == 1200, "click at %d", (int)at);
}

TEST(glow_raises_loudness_without_clipping) {
    // The same music at a fixed peak: every step up the knob gives more RMS, and more RMS for the same peak.
    const auto in = groove(3.0, 0.5f);
    const double inRms = rmsOf(in.l, 9600), inCrest = inRms / peakOf(in.l);
    double lastRms = 0.0, lastCrest = 0.0, firstCrest = 0.0;
    for (float amount : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        Glow g;
        g.prepare(kSr);
        g.pAmount->set(amount);
        auto out = run(g, in);
        CHECK(allFinite(out));
        const double rms = rmsOf(out.l, 9600), peak = peakOf(out.l), crest = rms / peak;
        CHECK_MSG(peak <= 1.0, "amount %.2f peaks at %.4f", amount, peak);
        if (amount == 0.0f) { CHECK_NEAR(db(rms / inRms), 0.0, 0.01); CHECK_NEAR(db(crest / inCrest), 0.0, 0.01); firstCrest = crest; }
        else {
            CHECK_MSG(rms > lastRms * 1.05, "amount %.2f: RMS %.4f after %.4f", amount, rms, lastRms);
            CHECK_MSG(crest > lastCrest * 1.01, "amount %.2f: RMS at matched peak %.4f after %.4f", amount, crest, lastCrest);
        }
        FIG("Glow amount %3.0f %%: RMS %+.2f dB, peak %.2f dBFS, RMS at matched peak %+.2f dB", amount * 100.0, db(rms / inRms), db(peak), db(crest / inCrest));
        lastRms = rms; lastCrest = crest;
    }
    CHECK_MSG(db(lastCrest / firstCrest) > 2.0, "full amount only gives %.2f dB at matched peak", db(lastCrest / firstCrest));
    CHECK(db(lastRms / inRms) > 6.0);

    // full-scale input, every character, flat out: never over 0 dBFS, and not flattened against it either
    const auto hot = groove(2.0, 1.0f);
    for (int character = 0; character < 3; ++character) {
        Glow g;
        g.prepare(kSr);
        g.pAmount->set(1.0f);
        g.pCharacter->set((float)character);
        auto out = run(g, hot);
        CHECK_MSG(peakOf(out.l) <= 1.0f && peakOf(out.r) <= 1.0f, "character %d peaks at %.5f", character, peakOf(out.l));
        size_t atCeiling = 0;
        for (float v : out.l) if (std::fabs(v) >= 0.9999f) ++atCeiling;
        CHECK_MSG((double)atCeiling < 0.01 * (double)out.l.size(), "%d samples sit on the ceiling", (int)atCeiling);
        CHECK(g.meter(0) >= 0.0f);
        const double hotCrest = rmsOf(hot.l, 9600) / peakOf(hot.l), outCrest = rmsOf(out.l, 9600) / peakOf(out.l);
        FIG("Glow flat out on a 0 dBFS groove, character %d: RMS %+.2f dB, peak %.2f dBFS, RMS at matched peak %+.2f dB, %d samples at the ceiling",
            character, db(rmsOf(out.l, 9600) / rmsOf(hot.l, 9600)), db(peakOf(out.l)), db(outCrest / hotCrest), (int)atCeiling);
        CHECK(outCrest > hotCrest * 1.2);               // louder for the same peak here too
    }
    // output gain is plain gain after everything
    Glow a, b;
    a.prepare(kSr); b.prepare(kSr);
    a.pAmount->set(0.5f); b.pAmount->set(0.5f); b.pOut->set(-6.0f);
    CHECK_NEAR(db(rmsOf(run(b, in).l, 9600) / rmsOf(run(a, in).l, 9600)), -6.0, 0.01);
}

TEST(glow_characters_tilt_the_spectrum) {
    // A quiet tone is under every threshold, so what is left is the make-up gain and the shelving.
    auto gainAt = [](int character, double hz) {
        Glow g;
        g.prepare(kSr);
        g.pAmount->set(1.0f);
        g.pCharacter->set((float)character);
        const float amp = 0.002f;
        auto out = run(g, tone(hz, 0.5, amp));
        return db(rmsOf(out.l, 12000) / (amp / std::sqrt(2.0)));
    };
    for (int character = 0; character < 3; ++character) {
        const double low = gainAt(character, 40.0), mid = gainAt(character, 1000.0), high = gainAt(character, 14000.0);
        FIG("Glow character %d at full: %+.2f dB at 40 Hz, %+.2f dB at 1 kHz, %+.2f dB at 14 kHz", character, low, mid, high);
        CHECK_NEAR(mid, 16.0, 0.6);
        if (character == 0) { CHECK(low - mid > 3.0 && high - mid < 1.5 && high - mid > 0.3); }
        if (character == 1) { CHECK(low - mid > 1.8 && high - mid > 1.8 && std::fabs(low - high) < 0.8); }
        if (character == 2) { CHECK(high - mid > 3.2 && low - mid < 1.5 && low - mid > 0.3); }
    }
    // the saturation adds odd harmonics to a loud tone and no even ones
    Glow g;
    g.prepare(kSr);
    g.pAmount->set(1.0f);
    auto out = run(g, tone(1000, 0.6, 0.5f));
    const double f1 = levelDbAt(out.l, 16000, 8192, 1000.0);
    CHECK(levelDbAt(out.l, 16000, 8192, 3000.0) - f1 > -50.0);
    CHECK(levelDbAt(out.l, 16000, 8192, 2000.0) - f1 < -70.0);
}
