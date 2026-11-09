// Quarter Inch (tape), Glass (tube), Splat (fuzz) and Coarse (bit depth and sample rate reduction).
#include "ToneTest.h"
#include <set>
using namespace pulp;
using namespace tu;
using namespace tt;

namespace {
const double kSr = 48000.0;

double poleLpDb(double hz, double corner, double sr = kSr) {
    const double a = 1.0 - std::exp(-kTwoPi * corner / sr), w = kTwoPi * hz / sr;
    return db(a / std::sqrt(1.0 + (1.0 - a) * (1.0 - a) - 2.0 * (1.0 - a) * std::cos(w)));
}
double poleHpDb(double hz, double corner, double sr = kSr) {
    const double a = 1.0 - std::exp(-kTwoPi * corner / sr), w = kTwoPi * hz / sr;
    return db((1.0 - a) * 2.0 * std::fabs(std::sin(0.5 * w)) / std::sqrt(1.0 + (1.0 - a) * (1.0 - a) - 2.0 * (1.0 - a) * std::cos(w)));
}
// Where the largest sample of a signal is.
size_t peakIndex(const std::vector<float>& x) {
    size_t at = 0;
    for (size_t i = 0; i < x.size(); ++i) if (std::fabs(x[i]) > std::fabs(x[at])) at = i;
    return at;
}
// The frequency of a tone cycle by cycle (from its upward zero crossings), as a ratio to `hz`, with the time of each cycle.
struct Cycles { std::vector<double> ratio, time; };
Cycles cycleFrequencies(const std::vector<float>& x, size_t from, double hz, double sr = kSr) {
    Cycles c;
    double last = -1.0;
    for (size_t i = from + 1; i < x.size(); ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double at = (double)(i - 1) + (double)(-x[i - 1]) / (double)(x[i] - x[i - 1]);
            if (last >= 0.0) { c.ratio.push_back(sr / (at - last) / hz); c.time.push_back(0.5 * (at + last) / sr); }
            last = at;
        }
    }
    return c;
}
double peakDeviation(const Cycles& c) {
    double worst = 0.0;
    for (double r : c.ratio) worst = std::max(worst, std::fabs(r - 1.0));
    return worst;
}
// How much of a rate there is in the cycle-by-cycle frequency (amplitude of that component of the deviation).
double deviationAt(const Cycles& c, double rateHz) {
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < c.ratio.size(); ++i) { re += (c.ratio[i] - 1.0) * std::cos(kTwoPi * rateHz * c.time[i]); im += (c.ratio[i] - 1.0) * std::sin(kTwoPi * rateHz * c.time[i]); }
    return 2.0 * std::sqrt(re * re + im * im) / (double)c.ratio.size();
}
}

// ------------------------------------------------------------------ Quarter Inch

TEST(quarter_inch_saturation_is_odd_soft_and_does_not_fold_back) {
    {
        QuarterInch t;
        t.prepare(kSr);
        t.pDrive->set(12.0f); t.pBump->set(0.0f); t.pSpeed->set(3);
        const auto out = run(t, sineWave(1000, 0.7, 0.5f));
        const double h2 = harmonicDb(out.l, 1000, 2), h3 = harmonicDb(out.l, 1000, 3), h4 = harmonicDb(out.l, 1000, 4), h5 = harmonicDb(out.l, 1000, 5);
        CHECK_MSG(h3 > -30.0 && h5 > -50.0, "3rd %.1f dB, 5th %.1f dB", h3, h5);
        CHECK_MSG(h2 < -90.0 && h4 < -90.0, "2nd %.1f dB, 4th %.1f dB", h2, h4);
        FIG("quarter inch, 1 kHz at 0.5, drive 12 dB: 2nd %.1f, 3rd %.1f, 4th %.1f, 5th %.1f dB", h2, h3, h4, h5);
    }
    // soft: twenty decibels more in gives far less than twenty more out, and never more than the curve's ceiling
    {
        double outDb[3];
        int k = 0;
        for (float amp : {0.01f, 0.1f, 1.0f}) {
            QuarterInch t;
            t.prepare(kSr);
            t.pDrive->set(12.0f); t.pBump->set(0.0f); t.pHyst->set(0.0f);
            const auto out = run(t, sineWave(500, 0.5, amp));
            outDb[k++] = db(peakOf(out.l, 12000));
            CHECK(peakOf(out.l) <= 1.0f / std::sqrt(dbToGain(12.0f)) * 1.15f);
        }
        CHECK_NEAR(outDb[0], db(0.01) + 6.0, 0.3);       // quiet: drive 12 dB comes out as 6 (the stage turns down by half of what it turns up)
        CHECK(outDb[2] - outDb[1] < 10.0 && outDb[2] - outDb[1] > 2.0);
        FIG("quarter inch, drive 12 dB: 500 Hz at -40 / -20 / 0 dB comes out at %.1f / %.1f / %.1f dB", outDb[0], outDb[1], outDb[2]);
    }
    // hysteresis: the quietest signals are held back (tape with too little bias), loud ones are not
    {
        double quiet[2], loud[2];
        int k = 0;
        for (float h : {0.0f, 1.0f}) {
            QuarterInch t;
            t.prepare(kSr);
            t.pDrive->set(0.0f); t.pBump->set(0.0f); t.pHyst->set(h);
            quiet[k] = gainDbAt(t, 1000, 0.002f);
            loud[k++] = gainDbAt(t, 1000, 0.7f);
        }
        CHECK_NEAR(quiet[1] - quiet[0], db(1.0 - QuarterInch::kHystMax), 0.4);
        CHECK(std::fabs(loud[1] - loud[0]) < 1.5);
        // and it is a loop, not a curve: the output depends on which way the input is going
        double p[3] = {0, 0, 0};
        for (double u = -1.0; u < 0.3; u += 0.001) QuarterInch::hysteresis(u, 1.0, p);
        const double rising = QuarterInch::hysteresis(0.3, 1.0, p);
        for (double u = 1.0; u > 0.3; u -= 0.001) QuarterInch::hysteresis(u, 1.0, p);
        const double falling = QuarterInch::hysteresis(0.3, 1.0, p);
        CHECK_MSG(falling - rising > 0.1 && falling - rising < 0.2, "loop is %.3f wide at 0.3", falling - rising);
        FIG("quarter inch hysteresis at full: a -54 dB tone is %.1f dB quieter than with none, a -3 dB tone %.2f dB; the loop is %.3f wide", quiet[0] - quiet[1], loud[0] - loud[1], falling - rising);
    }
    // 9 kHz driven hard, against the same curve run without oversampling
    {
        QuarterInch t;
        t.prepare(kSr);
        t.pDrive->set(18.0f); t.pBump->set(0.0f); t.pSpeed->set(3);
        const auto in = sineWave(9000, 0.7, 0.5f);
        const auto out = run(t, in);
        dsp::Biquad pre, de;
        pre.highShelf(QuarterInch::kEmphasisHz, 0.7071, QuarterInch::kEmphasisDb, kSr);
        de.highShelf(QuarterInch::kEmphasisHz, 0.7071, -QuarterInch::kEmphasisDb, kSr);
        std::vector<float> naive(in.l.size());
        double p[3] = {0, 0, 0};
        const double g = dbToGain(18.0f);
        for (size_t i = 0; i < naive.size(); ++i) naive[i] = de.process((float)tonefx::Sig::f(QuarterInch::hysteresis(g * pre.process(in.l[i]), 0.3, p), 1.0));
        const double ours = aliasDb(out.l), plain = aliasDb(naive);
        CHECK_MSG(ours < plain - 30.0 && ours < -45.0, "aliases %.1f dB, without oversampling %.1f dB", ours, plain);
        FIG("quarter inch, 9 kHz at 0.5, drive 18 dB: aliases %.1f dB against %.1f dB without oversampling (%.1f dB better)", ours, plain, plain - ours);
    }
}

TEST(quarter_inch_head_bump_and_speed_roll_off) {
    // A signal too quiet to saturate (and no hysteresis): what is left is the head bump, the high roll-off and the low roll-off.
    double worst = 0.0;
    for (int speed = 0; speed < QuarterInch::kSpeeds; ++speed) {
        QuarterInch t;
        t.prepare(kSr);
        t.pDrive->set(0.0f); t.pHyst->set(0.0f); t.pBump->set(4.0f); t.pSpeed->set((float)speed);
        double high, bump, low;
        QuarterInch::speedCorners(speed, high, bump, low);
        high = std::min(high, 0.45 * kSr);
        dsp::Biquad pk, lp;
        pk.peak(bump, 1.0, 4.0, kSr);
        lp.lowpass(high, 0.62, kSr);
        for (double hz : {bump * 0.5, bump, bump * 2.0, 1000.0, high * 0.5, high}) {
            if (hz > 19000.0) continue;
            // (the last term is what the anti-aliased stage takes off the top by averaging two fast samples: 0.3 dB at 16 kHz)
            const double want = db(pk.magnitude(hz, kSr)) + db(lp.magnitude(hz, kSr)) + poleHpDb(hz, low) + db(std::cos(kPi * hz / (4.0 * kSr)));
            const double got = gainDbAt(t, hz, 0.001f);
            CHECK_MSG(std::fabs(got - want) < 0.1, "speed %d, %.0f Hz: measured %.2f dB, expected %.2f dB", speed, hz, got, want);
            worst = std::max(worst, std::fabs(got - want));
        }
    }
    // the bump is a boost of the stated size at the stated place, the roll-off comes down as the tape slows
    QuarterInch a, b;
    a.prepare(kSr); b.prepare(kSr);
    for (auto* t : {&a, &b}) { t->pDrive->set(0.0f); t->pHyst->set(0.0f); t->pSpeed->set(2); }
    a.pBump->set(0.0f); b.pBump->set(6.0f);
    CHECK_NEAR(gainDbAt(b, 80, 0.001f) - gainDbAt(a, 80, 0.001f), 6.0, 0.1);
    CHECK_NEAR(gainDbAt(b, 2000, 0.001f) - gainDbAt(a, 2000, 0.001f), 0.0, 0.05);
    double at8k[QuarterInch::kSpeeds];
    for (int speed = 0; speed < QuarterInch::kSpeeds; ++speed) { a.pSpeed->set((float)speed); at8k[speed] = gainDbAt(a, 8000, 0.001f); }
    CHECK(at8k[0] < at8k[1] - 4.0 && at8k[1] < at8k[2] - 1.0 && at8k[2] < at8k[3]);
    FIG("quarter inch: quiet-signal response within %.2f dB of bump + roll-offs at every speed; 8 kHz at 3.75 / 7.5 / 15 / 30 ips: %.1f / %.1f / %.1f / %.1f dB", worst, at8k[0], at8k[1], at8k[2], at8k[3]);
    // changing speed in the middle of a tone glides
    QuarterInch g;
    g.prepare(kSr);
    g.pDrive->set(0.0f);
    const auto in = sineWave(3000, 1.0, 0.2f);
    const auto out = run(g, in, {}, [&](size_t i) { if (i == 9600) g.pSpeed->set(0); if (i == 28800) g.pSpeed->set(3); });
    CHECK_MSG(maxStep(out.l) <= maxStep(in.l) * 1.2f, "step %.5f, the tone's own %.5f", maxStep(out.l), maxStep(in.l));
}

TEST(quarter_inch_wow_and_flutter_depth) {
    const auto measure = [](float wow, float flutter, double seconds) {
        QuarterInch t;
        t.prepare(kSr);
        t.pDrive->set(0.0f); t.pHyst->set(0.0f); t.pBump->set(0.0f); t.pWow->set(wow); t.pFlutter->set(flutter);
        return cycleFrequencies(run(t, sineWave(1000, seconds, 0.2f)).l, 4800, 1000.0);
    };
    // none: the pitch does not move at all
    const auto still = measure(0.0f, 0.0f, 2.0);
    CHECK_MSG(peakDeviation(still) < 2.0e-6, "deviation %.2g with wow and flutter at zero", peakDeviation(still));
    // wow: slow, up to the stated depth, and in proportion to the control
    const auto wowFull = measure(1.0f, 0.0f, 8.0), wowHalf = measure(0.5f, 0.0f, 8.0);
    const double w1 = peakDeviation(wowFull), w2 = peakDeviation(wowHalf);
    CHECK_MSG(w1 <= QuarterInch::kMaxWow * 1.02 && w1 > QuarterInch::kMaxWow * 0.8, "wow at full: %.4f %% (%.4f stated)", 100.0 * w1, 100.0 * QuarterInch::kMaxWow);
    CHECK_NEAR(w2 / w1, 0.5, 0.02);
    CHECK(deviationAt(wowFull, 0.55) > 5.0 * deviationAt(wowFull, 3.0));
    CHECK_NEAR(deviationAt(wowFull, 0.55), QuarterInch::kMaxWow * 0.7, QuarterInch::kMaxWow * 0.12);
    // flutter: fast and smaller
    const auto flutFull = measure(0.0f, 1.0f, 4.0), flutHalf = measure(0.0f, 0.5f, 4.0);
    const double f1 = peakDeviation(flutFull), f2 = peakDeviation(flutHalf);
    CHECK_MSG(f1 <= QuarterInch::kMaxFlutter * 1.03 && f1 > QuarterInch::kMaxFlutter * 0.8, "flutter at full: %.4f %% (%.4f stated)", 100.0 * f1, 100.0 * QuarterInch::kMaxFlutter);
    CHECK_NEAR(f2 / f1, 0.5, 0.03);
    CHECK_NEAR(deviationAt(flutFull, 7.1), QuarterInch::kMaxFlutter * 0.5, QuarterInch::kMaxFlutter * 0.08);
    CHECK(deviationAt(flutFull, 7.1) > 5.0 * deviationAt(flutFull, 0.55));
    FIG("quarter inch: pitch deviation with wow and flutter at zero %.1e; wow at full %.3f %% (0.6 stated), at half %.3f %%; flutter at full %.3f %% (0.3 stated), at half %.3f %%",
        peakDeviation(still), 100.0 * w1, 100.0 * w2, 100.0 * f1, 100.0 * f2);
    // the transport starts from the same place every time: two machines, and one that was reset, give the same sound
    QuarterInch a, b;
    a.prepare(kSr); b.prepare(kSr);
    for (auto* t : {&a, &b}) { t->pWow->set(0.8f); t->pFlutter->set(0.7f); }
    const auto in = groove(0.5, 0.6f);
    const auto first = run(a, in);
    CHECK(run(b, in).l == first.l);
    a.reset();
    CHECK(run(a, in).l == first.l);
    CHECK(maxAbsDiff(first.l, in.l) > 0.01);
}

TEST(quarter_inch_hiss_is_off_by_default_and_at_its_stated_level) {
    QuarterInch off;
    off.prepare(kSr);
    const auto quiet = run(off, silence(0.5));
    CHECK(peakOf(quiet.l) == 0.0f && peakOf(quiet.r) == 0.0f);
    double levels[2];
    int k = 0;
    for (float dbSet : {-60.0f, -40.0f}) {
        QuarterInch t;
        t.prepare(kSr);
        t.pHiss->set(dbSet);
        const auto out = run(t, silence(1.0));
        levels[k++] = db(rmsOf(out.l, 4800));
        CHECK(maxAbsDiff(out.l, out.r) > dbToGain(dbSet) * 0.3);      // the two channels hiss separately
        QuarterInch again;
        again.prepare(kSr);
        again.pHiss->set(dbSet);
        CHECK(run(again, silence(1.0)).l == out.l);                    // and the same way every time
        CHECK(peakOf(out.l) <= dbToGain(dbSet) * 1.7f);
        // brighter than white noise: more above 4 kHz than below 1 kHz, band for band
        CHECK(bandDb(out.l, 8192, 32768, 4000, 5000) > bandDb(out.l, 8192, 32768, 200, 1200) + 3.0);
    }
    CHECK_NEAR(levels[1] - levels[0], 20.0, 0.05);
    CHECK_MSG(levels[0] > -68.0 && levels[0] < -60.0, "hiss set to -60 dB (peak) measures %.1f dB RMS", levels[0]);
    FIG("quarter inch hiss: set to -60 dB it measures %.1f dB RMS; off by default (exact silence)", levels[0]);
}

TEST(quarter_inch_latency) {
    const int expected[3] = {120, 128, 224};
    int k = 0;
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        QuarterInch t;
        t.prepare(sr);
        CHECK(t.latencySamples() == expected[k++]);
        const int lat = t.latencySamples();
        // the dry path: exact
        t.pMix->set(0.0f);
        const auto in = noise(0.3, 0.7f, sr);
        const auto out = runAt(t, in, sr);
        CHECK(maxDiffDelayed(out.l, in.l, lat, 0) == 0.0 && peakOf(out.l, 0, (size_t)lat) == 0.0f);
        // the wet path: a click comes out where the latency says, with and without the transport wobbling about it
        for (float wow : {0.0f, 1.0f}) {
            QuarterInch w;
            w.prepare(sr);
            w.pDrive->set(0.0f); w.pHyst->set(0.0f); w.pBump->set(0.0f); w.pSpeed->set(3); w.pWow->set(wow);
            const auto click = runAt(w, impulse(0.1, sr, 100, 0.01f), sr);
            const long long at = (long long)peakIndex(click.l) - 100;
            const long long slack = wow > 0.0f ? (long long)(0.0016 * sr) : 1;       // wow moves it by up to 1.6 ms either way
            CHECK_MSG(std::llabs(at - lat) <= slack, "%.0f Hz, wow %.0f: click comes out %lld samples late, latency says %d", sr, wow, at, lat);
        }
    }
}

// ------------------------------------------------------------------ Glass

namespace {
// The stage chain as Glass runs it, but at the project rate and with the plain curve: no oversampling at all.
std::vector<float> glassNaive(const std::vector<float>& in, int stages, double driveDb, double bias) {
    std::vector<float> out(in.size());
    const double b = bias * 0.9, kn = Glass::kKneeNeg;
    const double gs = std::pow(10.0, driveDb / 20.0 / stages), comp = 1.0 / std::sqrt(std::pow(10.0, driveDb / 20.0));
    const double rest = tonefx::Sig::f(b, kn), inv = 1.0 / tonefx::Sig::slope(b, kn);
    for (size_t i = 0; i < in.size(); ++i) {
        double v = in[i];
        for (int s = 0; s < stages; ++s) v = -(tonefx::Sig::f(b + gs * v, kn) - rest) * inv;
        out[i] = (float)(v * comp * ((stages & 1) ? -1.0 : 1.0));
    }
    return out;
}
std::shared_ptr<Glass> glass(int stages, float drive, float bias, float sag = 0.0f) {
    auto g = std::make_shared<Glass>();
    g->prepare(kSr);
    g->pStages->set((float)(stages - 1)); g->pDrive->set(drive); g->pBias->set(bias); g->pSag->set(sag); g->pOut->set(0.0f);
    return g;
}
}

TEST(glass_stages_are_asymmetric_and_bias_adds_even_harmonics) {
    // one stage, the same drive, more and more bias: the second harmonic grows
    double h2[3], h3[3];
    int k = 0;
    for (float bias : {0.0f, 0.4f, 1.0f}) {
        auto g = glass(1, 12.0f, bias);
        const auto out = run(*g, sineWave(1000, 0.7, 0.25f));
        h2[k] = harmonicDb(out.l, 1000, 2);
        h3[k++] = harmonicDb(out.l, 1000, 3);
    }
    CHECK_MSG(h2[0] > -40.0, "no bias: 2nd harmonic %.1f dB (the stage is asymmetric by itself)", h2[0]);
    CHECK_MSG(h2[1] > h2[0] + 3.0 && h2[2] > h2[1] + 3.0, "2nd harmonic %.1f / %.1f / %.1f dB at bias 0 / 0.4 / 1", h2[0], h2[1], h2[2]);
    CHECK(h2[2] > h3[2]);       // with the bias up, the even harmonic leads
    FIG("glass, one stage, 1 kHz at 0.25, drive 12 dB: 2nd harmonic %.1f / %.1f / %.1f dB and 3rd %.1f / %.1f / %.1f dB at bias 0 / 40 / 100 %%", h2[0], h2[1], h2[2], h3[0], h3[1], h3[2]);
    // one, two or three stages share the drive between them: a quiet signal comes out the same, the right way up,
    // and a loud one is squeezed harder the more stages there are
    double thd[3];
    for (int stages = 1; stages <= 3; ++stages) {
        auto g = glass(stages, 12.0f, 0.35f);
        const auto in = sineWave(1000, 0.5, 0.0005f);
        const auto out = run(*g, in);
        CHECK_NEAR(levelDbAt(out.l, 8000, 8192, 1000.0) - db(0.0005), 6.0, 0.1);         // 12 dB of drive comes out as 6
        double dot = 0.0, a = 0.0, b = 0.0;
        for (size_t i = 4800; i < out.l.size(); ++i) { dot += (double)out.l[i] * in.l[i - 32]; a += (double)out.l[i] * out.l[i]; b += (double)in.l[i - 32] * in.l[i - 32]; }
        CHECK_MSG(dot / std::sqrt(a * b) > 0.99, "%d stages: the output is not in phase with the input (%.4f)", stages, dot / std::sqrt(a * b));
        g->reset();
        const auto loud = run(*g, sineWave(1000, 0.7, 0.5f));
        double sum = 0.0;
        for (int h = 2; h <= 9; ++h) sum += std::pow(10.0, harmonicDb(loud.l, 1000, h) / 10.0);
        thd[stages - 1] = 10.0 * std::log10(sum);
    }
    // (not simply more with every stage: each stage turns the signal over, so part of what one adds the next takes off)
    for (double v : thd) CHECK(v > -20.0 && v < -8.0);
    CHECK(std::fabs(thd[1] - thd[0]) > 0.5 && std::fabs(thd[2] - thd[1]) > 0.5);
    FIG("glass, 1 kHz at 0.5, drive 12 dB, bias 35 %%: harmonics 2 to 9 together %.1f / %.1f / %.1f dB for one / two / three stages", thd[0], thd[1], thd[2]);
    // changing the number of stages under a tone does not click
    auto g = glass(1, 18.0f, 0.35f);
    const auto in = sineWave(220, 1.0, 0.3f);
    const auto out = run(*g, in, {}, [&](size_t i) { if ((i / kQuantum) % 60 == 30) g->pStages->set((float)(((i / kQuantum) / 60) % 3)); });
    CHECK_MSG(maxStep(out.l) < maxStep(in.l) * dbToGain(9.0f) * 2.0f, "largest step %.4f", maxStep(out.l));
}

TEST(glass_folds_back_far_less_than_the_plain_curve) {
    double worstGain = 1000.0;
    for (int stages = 1; stages <= 3; ++stages) {
        auto g = glass(stages, 24.0f, 0.35f);
        const auto in = sineWave(9000, 0.7, 0.5f);
        const auto out = run(*g, in);
        const double ours = aliasDb(out.l), plain = aliasDb(glassNaive(in.l, stages, 24.0, 0.35));
        CHECK_MSG(ours < plain - 30.0, "%d stages: aliases %.1f dB, without oversampling %.1f dB", stages, ours, plain);
        worstGain = std::min(worstGain, plain - ours);
        FIG("glass, %d stage(s), 9 kHz at 0.5, drive 24 dB: aliases %.1f dB against %.1f dB without oversampling (%.1f dB better)", stages, ours, plain, plain - ours);
    }
    CHECK(worstGain >= 30.0);
}

TEST(glass_sag_is_a_slow_level_dependent_compression) {
    // a loud tone: the level sinks over the first tenth of a second and stays down; a quiet one is left alone
    double early[2], late[2];
    int k = 0;
    for (float sag : {0.0f, 1.0f}) {
        auto g = glass(2, 6.0f, 0.35f, sag);
        const auto out = run(*g, sineWave(400, 1.0, 0.5f));
        early[k] = db(rmsOf(out.l, 240, 480));
        late[k++] = db(rmsOf(out.l, 40000, 47000));
        if (sag > 0.0f) {
            // the supply is down by what the read-out says: the follower sits between the tone's average and its peak
            const double lvl = 0.5 * dbToGain(6.0f);
            const double deepest = -db(1.0 - Glass::kSagDepth * lvl / (1.0 + lvl)), shallowest = -db(1.0 - Glass::kSagDepth * 0.6 * lvl / (1.0 + 0.6 * lvl));
            CHECK_MSG(g->meter(0) < deepest + 0.05 && g->meter(0) > shallowest, "sag read-out %.2f dB, expected between %.2f and %.2f", g->meter(0), shallowest, deepest);
        } else CHECK(g->meter(0) == 0.0f);
    }
    CHECK_NEAR(early[0], late[0], 0.6);      // (without sag only the coupling between the stages settles)
    CHECK_MSG((early[1] - late[1]) - (early[0] - late[0]) > 1.5, "with sag the level goes from %.2f dB to %.2f dB, without from %.2f to %.2f", early[1], late[1], early[0], late[0]);
    // slow: 5 ms in it has hardly started, 40 ms in (one attack time) it is most of the way
    auto g = glass(2, 6.0f, 0.35f, 1.0f);
    std::vector<float> sagDb;
    run(*g, sineWave(400, 0.6, 0.5f), {}, [&](size_t i) { if (i > 0) sagDb.push_back(g->meter(0)); });
    const double settled = sagDb.back();
    CHECK(sagDb[(size_t)(0.005 * kSr / kQuantum)] < 0.3 * settled);
    CHECK(sagDb[(size_t)(0.040 * kSr / kQuantum)] > 0.5 * settled && sagDb[(size_t)(0.040 * kSr / kQuantum)] < 0.95 * settled);
    auto q = glass(2, 6.0f, 0.35f, 1.0f);
    run(*q, sineWave(400, 0.5, 0.001f));
    CHECK(q->meter(0) < 0.01f);
    FIG("glass sag at full, 400 Hz at 0.5, drive 6 dB: output %.2f dB at the start, %.2f dB once settled; supply down %.2f dB; %.0f %% of the way after 40 ms",
        early[1], late[1], settled, 100.0 * sagDb[(size_t)(0.040 * kSr / kQuantum)] / settled);
}

TEST(glass_tone_and_latency) {
    // tone: a shelf above 2 kHz, nine decibels either way
    for (float tone : {-1.0f, 0.5f, 1.0f}) {
        auto g = glass(1, 0.0f, 0.0f);
        g->pTone->set(tone);
        dsp::Biquad sh;
        sh.highShelf(2000.0, 0.7071, tone * 9.0, kSr);
        for (double hz : {200.0, 2000.0, 10000.0}) CHECK_NEAR(gainDbAt(*g, hz, 0.0005f), db(sh.magnitude(hz, kSr)), 0.15);
        CHECK_NEAR(gainDbAt(*g, 10000.0, 0.0005f), tone * 9.0, 0.5);
    }
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        Glass dry;
        dry.prepare(sr);
        CHECK(dry.latencySamples() == 32);
        dry.pMix->set(0.0f);
        const auto in = noise(0.3, 0.7f, sr);
        const auto out = runAt(dry, in, sr);
        CHECK(maxDiffDelayed(out.l, in.l, 32, 0) == 0.0 && peakOf(out.l, 0, 32) == 0.0f);
        // the wet path, whatever the number of stages: a quiet click comes out exactly 32 samples later, and evenly
        // about that sample (the stages are half a fast sample late each, and the way down makes up for exactly that)
        for (int stages = 1; stages <= 3; ++stages) {
            Glass g;
            g.prepare(sr);
            g.pStages->set((float)(stages - 1)); g.pDrive->set(0.0f); g.pBias->set(0.0f); g.pSag->set(0.0f); g.pOut->set(0.0f);
            const auto click = runAt(g, impulse(0.05, sr, 100, 0.001f), sr);
            CHECK_MSG(peakIndex(click.l) == 132, "%.0f Hz, %d stages: click comes out %d samples late", sr, stages, (int)peakIndex(click.l) - 100);
            CHECK_MSG(std::fabs(click.l[131] - click.l[133]) < 0.02f * std::fabs(click.l[132]), "%.0f Hz, %d stages: lopsided about the peak (%.3g, %.3g, %.3g)", sr, stages, click.l[131], click.l[132], click.l[133]);
        }
    }
    // a quiet tone nulls against the input delayed by the latency (and not by one sample more or less)
    auto g = glass(3, 0.0f, 0.3f);
    const auto in = sineWave(3000, 0.3, 0.0005f);
    const auto out = run(*g, in);
    const double at = maxDiffDelayed(out.l, in.l, 32, 4800), before = maxDiffDelayed(out.l, in.l, 31, 4800), after = maxDiffDelayed(out.l, in.l, 33, 4800);
    CHECK_MSG(at < 0.0005 * 0.02 && before > 10.0 * at && after > 10.0 * at, "null %.3g at 32, %.3g at 31, %.3g at 33", at, before, after);
    FIG("glass, three stages: a quiet 3 kHz tone nulls to %.1f dB against the input 32 samples late (31: %.1f dB, 33: %.1f dB)", db(at / 0.0005), db(before / 0.0005), db(after / 0.0005));
}

// ------------------------------------------------------------------ Splat

TEST(splat_curve_and_its_antiderivative) {
    // the static curve: clipping levels, dead zone, octave fold
    const auto plain = Splat::shapeFor(0.0f, 0.0f, 0.0f);
    CHECK(plain.f(0.3) == 0.3 && plain.f(-0.3) == -0.3 && plain.f(5.0) == 1.0 && plain.f(-5.0) == -1.0);
    const auto lop = Splat::shapeFor(0.0f, 0.0f, 1.0f);
    CHECK_NEAR(lop.f(5.0), 1.0, 1e-12);
    CHECK_NEAR(lop.f(-5.0), -0.4, 1e-12);
    const auto gate = Splat::shapeFor(0.5f, 0.0f, 0.0f);
    CHECK(gate.f(0.39) == 0.0 && gate.f(-0.64) == 0.0);
    CHECK_NEAR(gate.f(0.9), 0.5, 1e-12);
    CHECK_NEAR(gate.f(-0.9), -0.25, 1e-12);
    const auto oct = Splat::shapeFor(0.0f, 1.0f, 0.0f);
    CHECK_NEAR(oct.f(-0.4), 0.4, 1e-12);        // full-wave: the negative half is turned up
    const auto half = Splat::shapeFor(0.0f, 0.5f, 0.0f);
    CHECK(half.f(-0.4) == 0.0 && half.f(0.4) == 0.4);
    // F really is the integral of f, for every corner of the controls
    dsp::Noise rnd(77u);
    double worst = 0.0;
    for (int trial = 0; trial < 400; ++trial) {
        const auto sh = Splat::shapeFor(trial % 5 == 0 ? 0.0f : rnd.uni(), trial % 7 == 0 ? 0.5f : rnd.uni(), rnd.uni());
        CHECK(sh.F(0.0) == 0.0);
        double integral = 0.0;
        const double step = 0.0005;
        for (double x = 0.0; x < 3.0; x += step) integral += step * sh.f(x + 0.5 * step);
        worst = std::max(worst, std::fabs(integral - sh.F(3.0)));
        integral = 0.0;
        for (double x = 0.0; x > -3.0; x -= step) integral -= step * sh.f(x - 0.5 * step);
        worst = std::max(worst, std::fabs(integral - sh.F(-3.0)));
    }
    CHECK_MSG(worst < 1.0e-6, "antiderivative off by %.3g", worst);
}

TEST(splat_harmonics_octave_sputter_and_tone) {
    const auto fuzz = [](float gain, float asym, float sputter, float octave, float tone) {
        auto f = std::make_shared<Splat>();
        f->prepare(kSr);
        f->pFuzz->set(gain); f->pAsym->set(asym); f->pSputter->set(sputter); f->pOctave->set(octave); f->pTone->set(tone); f->pLevel->set(0.0f);
        return f;
    };
    // symmetric clipping gives odd harmonics only; the asymmetry brings in the even ones
    {
        const auto sym = run(*fuzz(30, 0, 0, 0, 0.5f), sineWave(1000, 0.7, 0.5f)), lop = run(*fuzz(30, 1, 0, 0, 0.5f), sineWave(1000, 0.7, 0.5f));
        const double s2 = harmonicDb(sym.l, 1000, 2), s3 = harmonicDb(sym.l, 1000, 3), l2 = harmonicDb(lop.l, 1000, 2);
        CHECK_MSG(s2 < -80.0 && s3 > -14.0, "symmetric: 2nd %.1f dB, 3rd %.1f dB", s2, s3);
        CHECK_MSG(l2 > -12.0, "asymmetric: 2nd %.1f dB", l2);
        FIG("splat, 1 kHz at 0.5, fuzz 30 dB: symmetric 2nd %.1f dB, 3rd %.1f dB; fully asymmetric 2nd %.1f dB", s2, s3, l2);
        // hard: driven this far the wave is flat on top and underneath, at the two clipping levels (2 apart, or 1.4
        // with the asymmetry full on)
        for (float asym : {0.0f, 1.0f}) {
            const auto sq = run(*fuzz(40, asym, 0, 0, 0.0f), sineWave(500, 0.5, 0.5f));
            float top = -10.0f, bottom = 10.0f;
            for (size_t i = 12000; i < sq.l.size(); ++i) { top = std::max(top, sq.l[i]); bottom = std::min(bottom, sq.l[i]); }
            const float apart = 2.0f - 0.6f * asym;
            // (the tone filter rounds the corners off, so a little under; never over)
            CHECK_MSG(top - bottom > apart * 0.88f && top - bottom <= apart * 1.001f, "asymmetry %.0f: peak to peak %.3f, the clipping levels are %.1f apart", asym, top - bottom, apart);
        }
        // the bias follows the size of the signal, so the even harmonics are there at any drive
        for (float gain : {12.0f, 44.0f}) {
            const auto o = run(*fuzz(gain, 1, 0, 0, 0.5f), sineWave(1000, 0.7, 0.5f));
            CHECK_MSG(harmonicDb(o.l, 1000, 2) > -12.0, "fuzz %.0f dB: 2nd %.1f dB", gain, harmonicDb(o.l, 1000, 2));
        }
    }
    // octave up: the rectified signal has twice the frequency
    {
        const auto out = run(*fuzz(24, 0, 0, 1, 0.5f), sineWave(1000, 0.7, 0.3f));
        const double f1 = harmonicDb(out.l, 2000, 1), fund = levelDbAt(out.l, out.l.size() - 16384, 16384, 1000.0) - levelDbAt(out.l, out.l.size() - 16384, 16384, 2000.0);
        (void)f1;
        CHECK_MSG(fund < -60.0, "with the octave full on, 1 kHz is %.1f dB below 2 kHz", -fund);
        const auto halfway = run(*fuzz(24, 0, 0, 0.5f, 0.5f), sineWave(1000, 0.7, 0.3f));
        const double both = levelDbAt(halfway.l, halfway.l.size() - 16384, 16384, 2000.0) - levelDbAt(halfway.l, halfway.l.size() - 16384, 16384, 1000.0);
        CHECK(both > -30.0 && both < 5.0);
        FIG("splat octave: full on, the 1 kHz that went in is %.1f dB below the 2 kHz that comes out; half on, 2 kHz is %.1f dB against 1 kHz", -fund, both);
    }
    // sputter: below the dead zone nothing comes out at all, above it the fuzz is there
    {
        const float gain = dbToGain(30.0f);
        const double threshold = 0.8 * 0.5 / gain;       // sputter at half: the dead zone is 0.4 after the gain
        auto f = fuzz(30, 0.0f, 0.5f, 0, 0.5f);
        const auto under = run(*f, sineWave(500, 1.0, (float)(threshold * 0.9)));
        CHECK_MSG(peakOf(under.l, 24000) == 0.0f, "a tone under the dead zone gives %g", peakOf(under.l, 24000));
        f->reset();
        const auto over = run(*f, sineWave(500, 0.5, (float)(threshold * 3.0)));
        CHECK(rmsOf(over.l, 12000) > 0.1);
        // a note dying away: the fuzz cuts out where the note crosses the dead zone, long before the note is gone
        f->reset();
        Stereo in = sineWave(500, 2.0, 0.5f);
        for (size_t i = 0; i < in.l.size(); ++i) { const float e = (float)std::exp(-(double)i / (0.25 * kSr)); in.l[i] *= e; in.r[i] *= e; }
        const auto out = run(*f, in);
        size_t lastSound = 0;
        for (size_t i = 0; i < out.l.size(); ++i) if (std::fabs(out.l[i]) > 1.0e-4f) lastSound = i;
        const double crossing = 0.25 * std::log(0.5 / threshold);       // when the note's level reaches the dead zone
        CHECK_MSG(std::fabs((double)lastSound / kSr - crossing) < 0.06, "the fuzz stops at %.3f s, the note crosses the dead zone at %.3f s", (double)lastSound / kSr, crossing);
        CHECK(peakOf(in.l, lastSound + 4800) > 1.0e-4f);
        FIG("splat sputter at half, fuzz 30 dB: a decaying note cuts out at %.3f s (it crosses the dead zone at %.3f s; the note itself is still %.1f dB)",
            (double)lastSound / kSr, crossing, db(peakOf(in.l, lastSound, lastSound + 480)));
    }
    // tone: all the way down is a low-pass, all the way up a high-pass, the middle is a scoop (no clipping here: fuzz at 0 dB)
    {
        auto dark = fuzz(0, 0, 0, 0, 0.0f), bright = fuzz(0, 0, 0, 0, 1.0f), mid = fuzz(0, 0, 0, 0, 0.5f);
        for (double hz : {100.0, 720.0, 5000.0}) CHECK_NEAR(gainDbAt(*dark, hz, 0.3f), poleLpDb(hz, Splat::kToneLowHz) + poleHpDb(hz, 25.0), 0.1);
        for (double hz : {100.0, 1400.0, 5000.0}) CHECK_NEAR(gainDbAt(*bright, hz, 0.3f), poleHpDb(hz, Splat::kToneHighHz) + poleHpDb(hz, 25.0), 0.1);
        CHECK(gainDbAt(*mid, 1000, 0.3f) < gainDbAt(*mid, 120, 0.3f) - 1.5 && gainDbAt(*mid, 1000, 0.3f) < gainDbAt(*mid, 9000, 0.3f) - 1.5);
        FIG("splat tone: dark %.1f dB at 5 kHz, bright %.1f dB at 100 Hz, middle %.1f dB at 1 kHz", gainDbAt(*dark, 5000, 0.3f), gainDbAt(*bright, 100, 0.3f), gainDbAt(*mid, 1000, 0.3f));
        // level
        dark->pLevel->set(-12.0f);
        CHECK_NEAR(gainDbAt(*dark, 200, 0.3f), poleLpDb(200, Splat::kToneLowHz) + poleHpDb(200, 25.0) - 12.0, 0.1);
    }
}

TEST(splat_folds_back_far_less_than_the_plain_curve_and_is_32_samples_late) {
    struct Case { float gain, asym, sputter, octave; } cases[] = {{30, 0.5f, 0, 0}, {40, 0, 0, 0}, {30, 1, 0.4f, 0}, {12, 0, 0, 1}};
    for (auto& cs : cases) {
        Splat f;
        f.prepare(kSr);
        f.pFuzz->set(cs.gain); f.pAsym->set(cs.asym); f.pSputter->set(cs.sputter); f.pOctave->set(cs.octave); f.pTone->set(1.0f); f.pLevel->set(0.0f);
        const auto in = sineWave(9000, 0.7, 0.5f);
        const auto out = run(f, in);
        // the same curve, sample by sample, and the same two filters after it
        tonefx::Pole dc, hi;
        dc.setHz(25.0, kSr); hi.setHz(Splat::kToneHighHz, kSr);
        std::vector<float> naive(in.l.size());
        const double g = dbToGain(cs.gain), rel = 1.0 - std::exp(-1.0 / (0.001 * Splat::kBiasReleaseMs * kSr));
        double env = 0.0;
        for (size_t i = 0; i < naive.size(); ++i) {
            const double size = g * std::fabs(in.l[i]);
            env = size > env ? size : env - rel * env;
            naive[i] = (float)hi.hp(dc.hp(Splat::shapeFor(cs.sputter, cs.octave, cs.asym, env).f(g * in.l[i])));
        }
        // (with the octave full on there is no 9 kHz left to measure against, so there the two are compared as they are)
        const double ref = cs.octave >= 1.0f ? levelDbAt(out.l, out.l.size() - 16384, 16384, 9000.0) : 0.0;
        const double refNaive = cs.octave >= 1.0f ? levelDbAt(naive, naive.size() - 16384, 16384, 9000.0) : 0.0;
        const double ours = aliasDb(out.l) + ref, plain = aliasDb(naive) + refNaive;
        CHECK_MSG(ours < plain - 30.0, "fuzz %.0f asym %.1f sputter %.1f octave %.0f: aliases %.1f dB, without oversampling %.1f dB", cs.gain, cs.asym, cs.sputter, cs.octave, ours, plain);
        FIG("splat, 9 kHz at 0.5, fuzz %.0f dB, asymmetry %.1f, sputter %.1f, octave %.0f: aliases %.1f dB against %.1f dB without oversampling (%.1f dB better)",
            cs.gain, cs.asym, cs.sputter, cs.octave, ours, plain, plain - ours);
    }
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        Splat dry;
        dry.prepare(sr);
        CHECK(dry.latencySamples() == 32);
        dry.pMix->set(0.0f);
        const auto in = noise(0.3, 0.7f, sr);
        const auto out = runAt(dry, in, sr);
        CHECK(maxDiffDelayed(out.l, in.l, 32, 0) == 0.0 && peakOf(out.l, 0, 32) == 0.0f);
        Splat f;
        f.prepare(sr);
        f.pFuzz->set(0.0f); f.pAsym->set(0.0f); f.pTone->set(0.0f); f.pLevel->set(0.0f);
        const auto click = runAt(f, impulse(0.05, sr, 100, 0.5f), sr);
        CHECK_MSG(peakIndex(click.l) == 132, "%.0f Hz: click comes out %d samples late", sr, (int)peakIndex(click.l) - 100);
    }
}

// ------------------------------------------------------------------ Coarse

TEST(coarse_bit_depth_gives_the_stated_step) {
    // a slow ramp from -1 to 1: every output value is a whole number of steps, there are 2^bits + 1 of them, and none is more than half a step off
    Stereo ramp = silence(1.0);
    for (size_t i = 0; i < ramp.l.size(); ++i) ramp.l[i] = ramp.r[i] = (float)(-1.0 + 2.0 * (double)i / (double)(ramp.l.size() - 1));
    for (float bits : {1.0f, 2.0f, 4.0f, 8.0f, 12.0f, 6.5f}) {
        Coarse c;
        c.prepare(kSr);
        c.pBits->set(bits);
        const double q = Coarse::stepFor(bits);
        CHECK_NEAR(q, 2.0 / std::pow(2.0, bits), 1e-12);
        const auto out = run(c, ramp);
        std::set<long long> levels;
        double worstOff = 0.0, worstErr = 0.0, errSq = 0.0;
        for (size_t i = 0; i < out.l.size(); ++i) {
            const double steps = out.l[i] / q;
            worstOff = std::max(worstOff, std::fabs(steps - std::round(steps)));
            levels.insert((long long)std::llround(steps));
            worstErr = std::max(worstErr, (double)std::fabs(out.l[i] - ramp.l[i]));
            errSq += (double)(out.l[i] - ramp.l[i]) * (out.l[i] - ramp.l[i]);
        }
        CHECK_MSG(worstOff < 1.0e-3, "%.1f bits: output is %.3g of a step off the grid", bits, worstOff);
        CHECK_MSG(worstErr <= q * 0.5 * 1.0001 + 1e-7, "%.1f bits: error %.4g, half a step is %.4g", bits, worstErr, q * 0.5);
        const long long expected = 2 * (long long)std::floor(1.0 / q + 0.5) + 1;
        CHECK_MSG((long long)levels.size() == expected, "%.1f bits: %d levels, expected %lld", bits, (int)levels.size(), expected);
        if (bits >= 4.0f) CHECK_NEAR(std::sqrt(errSq / (double)out.l.size()) / (q / std::sqrt(12.0)), 1.0, 0.03);
        if (bits == 8.0f) FIG("coarse at 8 bits: step %.6f, %d levels over -1..1, largest error %.6f (half a step is %.6f), RMS error %.3f of step / sqrt(12)",
                              q, (int)levels.size(), worstErr, q * 0.5, std::sqrt(errSq / (double)out.l.size()) / (q / std::sqrt(12.0)));
    }
    // 24 bits is off: bit for bit what went in
    Coarse off;
    off.prepare(kSr);
    off.pBits->set(24.0f);
    const auto in = noise(0.2, 0.7f);
    CHECK(run(off, in).l == in.l);
}

TEST(coarse_dither_trades_distortion_for_noise) {
    const double q = Coarse::stepFor(8.0f);
    // a tone smaller than half a step: without dither it vanishes, with dither it is all there
    {
        Coarse plain, dithered;
        plain.prepare(kSr); dithered.prepare(kSr);
        dithered.pDither->set(1);
        const auto in = sineWave(1000, 1.5, (float)(0.4 * q));
        const auto a = run(plain, in), b = run(dithered, in);
        CHECK(peakOf(a.l) == 0.0f);
        CHECK_NEAR(levelDbAt(b.l, 4096, 65536, 1000.0), db(0.4 * q), 0.5);
    }
    // a tone of a step and a half: the third harmonic of the staircase goes, noise of a quarter of a step squared comes
    {
        Coarse plain, dithered;
        plain.prepare(kSr); dithered.prepare(kSr);
        dithered.pDither->set(1);
        const auto in = sineWave(1000, 1.5, (float)(1.5 * q));
        const auto a = run(plain, in), b = run(dithered, in);
        const double h3plain = levelDbAt(a.l, 4096, 65536, 3000.0) - levelDbAt(a.l, 4096, 65536, 1000.0);
        const double h3dith = levelDbAt(b.l, 4096, 65536, 3000.0) - levelDbAt(b.l, 4096, 65536, 1000.0);
        CHECK_MSG(h3plain > -25.0 && h3dith < h3plain - 20.0, "third harmonic %.1f dB without dither, %.1f dB with", h3plain, h3dith);
        double errSq = 0.0;
        for (size_t i = 0; i < b.l.size(); ++i) errSq += (double)(b.l[i] - in.l[i]) * (b.l[i] - in.l[i]);
        CHECK_NEAR(errSq / (double)b.l.size() / (q * q / 4.0), 1.0, 0.05);
        FIG("coarse at 8 bits, a 1 kHz tone 1.5 steps high: third harmonic %.1f dB without dither, %.1f dB with; dithered error power %.3f of step^2 / 4",
            h3plain, h3dith, errSq / (double)b.l.size() / (q * q / 4.0));
    }
    // digital silence stays silent even with the dither on
    Coarse c;
    c.prepare(kSr);
    c.pDither->set(1); c.pBits->set(4.0f);
    const auto out = run(c, silence(0.2));
    CHECK(peakOf(out.l) == 0.0f && peakOf(out.r) == 0.0f);
}

TEST(coarse_rate_reduction_holds_and_makes_the_stated_images) {
    const auto crush = [](float rate, int mode) {
        auto c = std::make_shared<Coarse>();
        c->prepare(kSr);
        c->pBits->set(24.0f); c->pRate->set(rate); c->pMode->set((float)mode);
        return c;
    };
    // What holding each value for eight samples does to a frequency (x is the frequency over the reduced rate).
    const auto sinc = [](double x) { return std::fabs(std::sin(kPi * x) / (8.0 * std::sin(kPi * x / 8.0))); };
    const size_t n = 16384;
    // hold at 6 kHz (one sample in eight): each value is held for eight samples
    {
        const auto in = sineWave(1000, 0.7, 0.5f);
        const auto out = run(*crush(6000, Coarse::Hold), in);
        size_t changes = 0, wrongRun = 0, runLength = 1;
        for (size_t i = 1; i < 4800; ++i) {
            if (out.l[i] != out.l[i - 1]) { if (runLength != 8 && changes > 0) ++wrongRun; ++changes; runLength = 1; } else ++runLength;
        }
        CHECK_MSG(changes >= 590 && changes <= 600 && wrongRun == 0, "%d changes in 4800 samples, %d runs not 8 long", (int)changes, (int)wrongRun);
        // the images of a 1 kHz tone sit at 6k -/+ 1k and 12k -/+ 1k, at the levels a hold gives them
        const size_t from = out.l.size() - n;
        const double f1 = levelDbAt(out.l, from, n, 1000.0);
        CHECK_NEAR(f1 - db(0.5), db(sinc(1.0 / 6.0)), 0.1);
        double worst = 0.0;
        for (double hz : {5000.0, 7000.0, 11000.0, 13000.0}) {
            const double got = levelDbAt(out.l, from, n, hz) - f1, want = db(sinc(hz / 6000.0) / sinc(1.0 / 6.0));
            CHECK_MSG(std::fabs(got - want) < 0.2, "image at %.0f Hz: %.2f dB, a hold gives %.2f dB", hz, got, want);
            worst = std::max(worst, std::fabs(got - want));
        }
        CHECK(levelDbAt(out.l, from, n, 3000.0) - f1 < -80.0);       // and nothing where no image belongs
        FIG("coarse, 1 kHz held at 6 kHz: images at 5 / 7 kHz %.2f / %.2f dB re the tone (a hold gives %.2f / %.2f); all four images within %.2f dB",
            levelDbAt(out.l, from, n, 5000.0) - f1, levelDbAt(out.l, from, n, 7000.0) - f1, db(sinc(5.0 / 6.0) / sinc(1.0 / 6.0)), db(sinc(7.0 / 6.0) / sinc(1.0 / 6.0)), worst);
        // interpolated: straight lines between the held values, so the images are squared down
        const auto lin = run(*crush(6000, Coarse::Interpolated), in);
        const double l1 = levelDbAt(lin.l, from, n, 1000.0);
        CHECK_NEAR(levelDbAt(lin.l, from, n, 5000.0) - l1, 2.0 * db(sinc(5.0 / 6.0) / sinc(1.0 / 6.0)), 0.3);
        CHECK_NEAR(levelDbAt(lin.l, from, n, 7000.0) - l1, 2.0 * db(sinc(7.0 / 6.0) / sinc(1.0 / 6.0)), 0.3);
        // filtered both ways: the images are taken off again by the low-pass after the hold
        const auto filt = run(*crush(6000, Coarse::Filtered), in);
        const double ff = levelDbAt(filt.l, from, n, 1000.0);
        CHECK(levelDbAt(filt.l, from, n, 5000.0) - ff < -33.0 && levelDbAt(filt.l, from, n, 7000.0) - ff < -45.0);
        CHECK_NEAR(ff - db(0.5), db(sinc(1.0 / 6.0)), 0.5);
        FIG("coarse, 1 kHz at 6 kHz: image at 5 kHz %.1f dB held, %.1f dB interpolated, %.1f dB filtered both ways",
            levelDbAt(out.l, from, n, 5000.0) - f1, levelDbAt(lin.l, from, n, 5000.0) - l1, levelDbAt(filt.l, from, n, 5000.0) - ff);
    }
    // a 5 kHz tone is above half of 6 kHz: held raw it turns into 1 kHz; filtered first, most of it is stopped before the hold
    {
        const auto in = sineWave(5000, 0.7, 0.5f);
        const auto raw = run(*crush(6000, Coarse::Hold), in), safe = run(*crush(6000, Coarse::HoldFiltered), in);
        const size_t from = raw.l.size() - n;
        const double rawAlias = levelDbAt(raw.l, from, n, 1000.0) - db(0.5), safeAlias = levelDbAt(safe.l, from, n, 1000.0) - db(0.5);
        CHECK_NEAR(rawAlias, db(sinc(1.0 / 6.0)), 0.2);
        CHECK_MSG(safeAlias < rawAlias - 18.0, "alias at 1 kHz: %.1f dB raw, %.1f dB filtered first", rawAlias, safeAlias);
        FIG("coarse, 5 kHz at a rate of 6 kHz: the alias at 1 kHz is %.1f dB held raw and %.1f dB filtered first", rawAlias, safeAlias);
    }
    // a rate that does not divide the project rate still puts its image in the right place
    {
        const auto out = run(*crush(7000, Coarse::Hold), sineWave(1000, 0.7, 0.5f));
        const size_t from = out.l.size() - n;
        const auto ideal = [](double x) { return std::fabs(std::sin(kPi * x) / (kPi * x)); };
        CHECK_NEAR(levelDbAt(out.l, from, n, 6000.0) - levelDbAt(out.l, from, n, 1000.0), db(ideal(6.0 / 7.0) / ideal(1.0 / 7.0)), 1.5);
    }
    // the highest rate is off, and so is any rate at or above the project's: bit for bit what went in
    const auto in = noise(0.2, 0.7f);
    CHECK(run(*crush(48000, Coarse::Hold), in).l == in.l);
    CHECK(run(*crush(48000, Coarse::Filtered), in).l == in.l);
    Coarse low;
    low.prepare(32000.0);
    low.pBits->set(24.0f); low.pRate->set(40000.0f);
    CHECK(runAt(low, in, 32000.0).l == in.l);
    // changing the mode or the rate under a tone: the output moves by no more than the crushed tone itself does
    Coarse c;
    c.prepare(kSr);
    c.pBits->set(24.0f); c.pRate->set(12000.0f);
    const auto tone220 = sineWave(220, 1.0, 0.3f);
    const auto out = run(c, tone220, {}, [&](size_t i) { const size_t q = i / kQuantum; if (q % 50 == 25) c.pMode->set((float)((q / 50) % 4)); if (q % 50 == 0) c.pRate->set(q % 100 == 0 ? 12000.0f : 3000.0f); });
    // held at 3 kHz a 220 Hz tone at 0.3 steps by up to 0.3 * 2 pi * 220 / 3000 = 0.138 at once; a switch adds little to that
    CHECK_MSG(maxStep(out.l) < 0.2f, "largest step %.4f", maxStep(out.l));
    FIG("coarse: mode and rate switched every half second under a 220 Hz tone: largest step %.3f (the 3 kHz hold alone steps by 0.138)", maxStep(out.l));
}
