// Jet, Swirl, Flutter, Waver and Rotor against known responses.
#include "SpaceTest.h"
using namespace pulp;
using namespace st;

namespace {
const double kSr = 48000.0;

std::vector<float> head(const std::vector<float>& x, size_t n) { return std::vector<float>(x.begin(), x.begin() + (std::ptrdiff_t)std::min(n, x.size())); }
double corr(const std::vector<double>& a, const std::vector<double>& b, size_t from, size_t to) {
    double ma = 0.0, mb = 0.0;
    for (size_t i = from; i < to; ++i) { ma += a[i]; mb += b[i]; }
    ma /= (double)(to - from); mb /= (double)(to - from);
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = from; i < to; ++i) { ab += (a[i] - ma) * (b[i] - mb); aa += (a[i] - ma) * (a[i] - ma); bb += (b[i] - mb) * (b[i] - mb); }
    return ab / std::sqrt(aa * bb + 1e-300);
}
float largestStep(const std::vector<float>& x, size_t from = 1) {
    float m = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}
}

// ------------------------------------------------------------------ Jet

TEST(jet_comb_notches_sit_where_the_delay_puts_them) {
    auto make = [](double sr, float delayMs, float fb, float mix) {
        auto j = std::make_shared<Jet>();
        j->prepare(sr);
        j->pDelay->set(delayMs); j->pDepth->set(0.0f); j->pFeedback->set(fb); j->pMix->set(mix);
        return j;
    };
    {   // 1 ms is 48 samples: dry plus delayed cancels at 500, 1500, 2500 ... Hz
        auto h = head(run(*make(kSr, 1.0f, 0.0f, 0.5f), impulse(0.05)).l, 256);
        CHECK(h[0] == 1.0f && h[48] == 1.0f);
        auto n = notches(h, kSr, 100, 12000, 5, 25);
        CHECK(n.size() == 12);
        for (size_t k = 0; k < n.size(); ++k) CHECK_MSG(std::fabs(n[k] - (500.0 + 1000.0 * (double)k)) < 2.0, "notch %d at %.1f Hz", (int)k, n[k]);
        CHECK_NEAR(magAt(h, 1000.0, kSr), 2.0, 1e-3);       // and adds in between
    }
    {   // 0.25 ms: 2, 6, 10, 14, 18, 22 kHz
        auto n = notches(head(run(*make(kSr, 0.25f, 0.0f, 0.5f), impulse(0.05)).l, 256), kSr, 100, 23500, 10, 25);
        CHECK(n.size() == 6);
        for (size_t k = 0; k < n.size(); ++k) CHECK_MSG(std::fabs(n[k] - (2000.0 + 4000.0 * (double)k)) < 5.0, "notch %d at %.1f Hz", (int)k, n[k]);
    }
    {   // a delay between samples (0.73 ms = 35.04 samples), and another rate (2 ms at 44.1 kHz = 88.2 samples)
        auto n = notches(head(run(*make(kSr, 0.73f, 0.0f, 0.5f), impulse(0.05)).l, 256), kSr, 100, 8000, 2, 20);
        CHECK(n.size() >= 5);
        for (size_t k = 0; k < 5 && k < n.size(); ++k) CHECK_MSG(std::fabs(n[k] / ((k + 0.5) / 0.00073) - 1.0) < 0.003, "notch %d at %.1f Hz", (int)k, n[k]);
        Transport t;
        t.sr = 44100.0;
        auto m = notches(head(run(*make(44100.0, 2.0f, 0.0f, 0.5f), impulse(0.05, 44100.0), t).l, 512), 44100.0, 50, 4000, 1, 20);
        CHECK(m.size() >= 7);
        for (size_t k = 0; k < 7 && k < m.size(); ++k) CHECK_MSG(std::fabs(m[k] / ((k + 0.5) * 500.0) - 1.0) < 0.003, "notch %d at %.1f Hz", (int)k, m[k]);
    }
    {   // positive feedback resonates at multiples of 1 / delay and leaves the notches complete:
        // dry + wet is (1 + z^-D) / (1 - fb z^-D)
        auto pos = head(run(*make(kSr, 1.0f, 0.7f, 0.5f), impulse(0.2)).l, 8192);
        CHECK_NEAR(magAt(pos, 1000.0, kSr), 2.0 / 0.3, 0.02);
        CHECK_NEAR(magAt(pos, 3000.0, kSr), 2.0 / 0.3, 0.02);
        auto n = notches(pos, kSr, 100, 12000, 5, 40);
        CHECK(n.size() == 12);
        for (size_t k = 0; k < n.size(); ++k) CHECK_MSG(std::fabs(n[k] - (500.0 + 1000.0 * (double)k)) < 2.0, "with feedback: notch %d at %.1f Hz", (int)k, n[k]);
        CHECK_NEAR(magAt(pos, 750.0, kSr), std::sqrt(2.0 / 1.49), 0.01);       // |1 - j| / |1 + 0.7 j|
        // negative feedback resonates half-way between instead, on what were the notches: the wet path alone is z^-D / (1 + |fb| z^-D)
        auto neg = head(run(*make(kSr, 1.0f, -0.7f, 1.0f), impulse(0.2)).l, 8192);
        CHECK_NEAR(magAt(neg, 1500.0, kSr), 1.0 / 0.3, 0.01);
        CHECK_NEAR(magAt(neg, 2500.0, kSr), 1.0 / 0.3, 0.01);
        CHECK_NEAR(magAt(neg, 1000.0, kSr), 1.0 / 1.7, 0.01);
        auto both = head(run(*make(kSr, 1.0f, -0.7f, 0.5f), impulse(0.2)).l, 8192);
        CHECK_NEAR(magAt(both, 500.0, kSr), 0.7 / 0.3, 0.01);                   // 1 - 1 / (1 - 0.7)
        CHECK_NEAR(magAt(both, 1000.0, kSr), 2.7 / 1.7, 0.01);
    }
    {   // dry only is bit-exact, whatever else is set
        Jet j;
        j.prepare(kSr);
        j.pMix->set(0.0f);
        j.pFeedback->set(0.9f);
        auto in = noise(0.3, 0.5f);
        auto out = run(j, in);
        CHECK(out.l == in.l && out.r == in.r);
        CHECK(j.latencySamples() == 0);
    }
}

TEST(jet_sweeps_at_the_rate_and_depth_it_shows) {
    // Wet only, a 3 kHz tone in: the swept delay bends the pitch by f0 x (rate of change of the delay).
    auto make = [](float rate, float depthMs, int shape, int sync) {
        auto j = std::make_shared<Jet>();
        j->prepare(kSr);
        j->pDelay->set(1.0f); j->pDepth->set(depthMs); j->pRate->set(rate); j->pShape->set((float)shape);
        j->pSync->set((float)sync); j->pFeedback->set(0.0f); j->pMix->set(1.0f); j->pPhase->set(0.0f);
        return j;
    };
    const double f0 = 3000.0;
    const size_t a = 48000, b = 48000 * 4;
    {   // sine sweep of 4 ms at 2 Hz: delay = 1 + 4 (0.5 - 0.5 cos), so the pitch moves by f0 * 0.004 * pi * 2 = 75.4 Hz each way
        auto tr = demod(run(*make(2.0f, 4.0f, 0, 0), tone(f0, 4.5, 0.5f)).l, kSr, f0, 64);
        double mn, mx;
        minMax(tr.hz, a, b, mn, mx);
        CHECK_MSG(std::fabs((mx - f0) / 75.4 - 1.0) < 0.02 && std::fabs((f0 - mn) / 75.4 - 1.0) < 0.02, "pitch moves +%.1f / -%.1f Hz", mx - f0, f0 - mn);
        CHECK_NEAR(oscRate(tr.hz, a, b, kSr), 2.0, 0.01);
    }
    {   // triangle sweep: the delay moves at a constant 2 * depth * rate, so the pitch sits 48 Hz up, then 48 Hz down
        auto tr = demod(run(*make(2.0f, 4.0f, 1, 0), tone(f0, 4.5, 0.5f)).l, kSr, f0, 64);
        int up = 0, down = 0, other = 0;
        for (size_t i = a; i < b; i += 97) {
            if (std::fabs(tr.hz[i] - f0 - 48.0) < 1.5) ++up;
            else if (std::fabs(tr.hz[i] - f0 + 48.0) < 1.5) ++down;
            else ++other;
        }
        CHECK_MSG(other < (up + down) / 20 && std::abs(up - down) < (up + down) / 20, "%d up, %d down, %d neither", up, down, other);
        CHECK_NEAR(acRate(tr.hz, a, b, kSr, 0.5, 8.0), 2.0, 0.02);
    }
    {   // tempo sync: a cycle per quarter note at 150 BPM is 2.5 Hz, playing or stopped
        for (bool playing : {false, true}) {
            Transport t;
            t.tempo = 150.0;
            t.playing = playing;
            auto tr = demod(run(*make(0.1f, 3.0f, 0, 7), tone(f0, 4.5, 0.5f), t).l, kSr, f0, 64);
            CHECK_NEAR(oscRate(tr.hz, a, b, kSr), 2.5, 0.01);
        }
    }
    {   // stereo phase: at 180 degrees one side goes up while the other goes down; at 0 they are the same
        auto j = make(2.0f, 4.0f, 0, 0);
        j->pPhase->set(180.0f);
        auto out = run(*j, tone(f0, 3.0, 0.5f));
        auto l = demod(out.l, kSr, f0, 64), r = demod(out.r, kSr, f0, 64);
        CHECK(corr(l.hz, r.hz, a, 48000 * 2 + 24000) < -0.98);
        j->pPhase->set(90.0f);
        j->reset();
        out = run(*j, tone(f0, 3.0, 0.5f));
        l = demod(out.l, kSr, f0, 64); r = demod(out.r, kSr, f0, 64);
        CHECK(std::fabs(corr(l.hz, r.hz, a, 48000 * 2 + 24000)) < 0.05);
        j->pPhase->set(0.0f);
        j->reset();
        out = run(*j, tone(f0, 1.0, 0.5f));
        CHECK(out.l == out.r);
    }
    {   // the delay really runs from `delay` to `delay + depth`: with a very slow sweep the first notch travels between 1 / (2 * 5 ms) and 1 / (2 * 1 ms)
        auto j = std::make_shared<Jet>();
        j->prepare(kSr);
        j->pDelay->set(1.0f); j->pDepth->set(4.0f); j->pRate->set(0.5f); j->pShape->set(1.0f); j->pFeedback->set(0.0f); j->pMix->set(0.5f); j->pPhase->set(0.0f);
        auto out = run(*j, tone(500.0, 4.0, 0.5f));       // 500 Hz is nulled only when the delay is exactly 1 ms (and 3, 5 ms)
        auto tr = demod(out.l, kSr, 500.0, 96 * 4);
        double mn, mx;
        minMax(tr.amp, 48000, 48000 * 3, mn, mx);
        CHECK(mn < 0.02 && mx > 0.95);
    }
}

// ------------------------------------------------------------------ Swirl

TEST(swirl_has_half_as_many_notches_as_stages_and_is_all_pass_without_the_dry_path) {
    auto make = [](int stagesIndex, float mix, float fb) {
        auto s = std::make_shared<Swirl>();
        s->prepare(kSr);
        s->pStages->set((float)stagesIndex); s->pLow->set(400.0f); s->pHigh->set(2500.0f);      // centre: 1000 Hz
        s->pDepth->set(0.0f); s->pFeedback->set(fb); s->pMix->set(mix);
        return s;
    };
    const double fc = 1000.0;
    for (int idx = 0; idx < 6; ++idx) {
        const int stages = 2 * (idx + 1);
        auto h = head(run(*make(idx, 0.5f, 0.0f), impulse(0.1)).l, 2048);
        auto n = notches(h, kSr, 10, 23000, 2, 30);
        CHECK_MSG((int)n.size() == stages / 2, "%d stages: %d notches", stages, (int)n.size());
        // N equal first-order sections turn the phase by 180 degrees (2k + 1) times where tan(pi f / sr) = tan(pi fc / sr) * tan((2k + 1) pi / 2N)
        for (size_t k = 0; k < n.size(); ++k) {
            const double want = kSr / kPi * std::atan(std::tan(kPi * fc / kSr) * std::tan((2.0 * (double)k + 1.0) * kPi / (2.0 * stages)));
            CHECK_MSG(std::fabs(n[k] / want - 1.0) < 0.004, "%d stages: notch %d at %.1f Hz, expected %.1f Hz", stages, (int)k, n[k], want);
        }
        // without the dry path it is all-pass: flat to a hair at every frequency
        auto w = head(run(*make(idx, 1.0f, 0.0f), impulse(0.1)).l, 4096);
        double worst = 0.0;
        for (double hz = 20.0; hz < 23000.0; hz *= 1.07) worst = std::max(worst, std::fabs(db(magAt(w, hz, kSr))));
        CHECK_MSG(worst < 0.001, "%d stages wet only: %.5f dB from flat", stages, worst);
    }
    {   // Feedback: dry + wet is (1 + A) / (1 - fb A), A being the all-pass chain. The notches (A = -1) stay complete and
        // stay put; positive feedback grows peaks of 2 / (1 - fb) between them, negative feedback narrows the notches.
        auto plain = head(run(*make(1, 0.5f, 0.0f), impulse(0.3)).l, 12000);
        auto m = notches(plain, kSr, 10, 23000, 2, 30);
        CHECK(m.size() == 2);
        auto widthAt = [&](const std::vector<float>& h, double centre) {       // how wide the notch is 12 dB below the level between notches
            const double ref = magAt(h, 1000.0, kSr) * 0.25;
            double lo = centre, hi = centre;
            while (lo > 20.0 && magAt(h, lo, kSr) < ref) lo -= 1.0;
            while (hi < 20000.0 && magAt(h, hi, kSr) < ref) hi += 1.0;
            return hi - lo;
        };
        for (float fb : {0.7f, -0.7f}) {
            auto h = head(run(*make(1, 0.5f, fb), impulse(0.3)).l, 12000);
            auto n = notches(h, kSr, 10, 23000, 2, 30);
            CHECK_MSG(n.size() == 2, "feedback %.1f: %d notches", fb, (int)n.size());
            for (size_t k = 0; k < n.size() && k < m.size(); ++k) CHECK_MSG(std::fabs(n[k] / m[k] - 1.0) < 0.004, "feedback %.1f: notch %d at %.1f Hz, without feedback %.1f Hz", fb, (int)k, n[k], m[k]);
            double top = 0.0;
            for (double hz = 5.0; hz < 23000.0; hz *= 1.01) top = std::max(top, magAt(h, hz, kSr));
            CHECK_NEAR(top, 2.0 / (1.0 - fb), 0.03);                // at A = +1 (the lows, the highs and between the notches)
            CHECK_NEAR(magAt(h, 1000.0, kSr), 2.0 / (1.0 - fb), 0.03);     // 1 kHz is the centre: four stages turn the phase once round there
        }
        const double w0 = widthAt(plain, m[0]), wPos = widthAt(head(run(*make(1, 0.5f, 0.7f), impulse(0.3)).l, 12000), m[0]),
                     wNeg = widthAt(head(run(*make(1, 0.5f, -0.7f), impulse(0.3)).l, 12000), m[0]);
        CHECK_MSG(wNeg < 0.5 * w0 && wPos > w0, "notch width %.0f Hz; %.0f Hz with +0.7, %.0f Hz with -0.7", w0, wPos, wNeg);
        // the wet path alone is A (1 + fb) / (1 - fb A): between (1 + fb) / (1 + |fb|) ... and (1 + fb) / (1 - |fb|)
        auto wet = head(run(*make(1, 1.0f, 0.7f), impulse(0.3)).l, 12000);
        double lo = 1e9, hi = 0.0;
        for (double hz = 5.0; hz < 23000.0; hz *= 1.01) { lo = std::min(lo, magAt(wet, hz, kSr)); hi = std::max(hi, magAt(wet, hz, kSr)); }
        CHECK_NEAR(hi, 1.7 / 0.3, 0.03);
        CHECK_NEAR(lo, 1.0, 0.01);
    }
    {   // the centre follows the range, and dry only is bit-exact
        auto s = make(0, 0.5f, 0.0f);
        s->pLow->set(200.0f); s->pHigh->set(200.0f);
        auto n = notches(head(run(*s, impulse(0.1)).l, 4096), kSr, 10, 20000, 1, 30);
        CHECK(n.size() == 1);
        if (n.size() == 1) CHECK_NEAR(n[0], 200.0, 1.0);        // two stages: the notch is at the centre itself
        Swirl d;
        d.prepare(kSr);
        d.pMix->set(0.0f);
        auto in = noise(0.3, 0.5f);
        auto out = run(d, in);
        CHECK(out.l == in.l && out.r == in.r);
    }
}

TEST(swirl_sweeps_at_its_rate_over_its_range) {
    auto make = [](float rate, int sync) {
        auto s = std::make_shared<Swirl>();
        s->prepare(kSr);
        s->pStages->set(0.0f); s->pLow->set(300.0f); s->pHigh->set(3000.0f); s->pRate->set(rate); s->pSync->set((float)sync);
        s->pDepth->set(1.0f); s->pFeedback->set(0.0f); s->pMix->set(0.5f); s->pPhase->set(0.0f);
        return s;
    };
    // Two stages have one notch, at the sweep frequency itself. A tone is nulled whenever the sweep passes it.
    {   // the sweep covers exactly low to high: tones just inside are nulled, tones outside never are
        auto s = make(1.5f, 0);
        for (double hz : {320.0, 950.0, 2800.0}) {
            s->reset();
            auto tr = demod(run(*s, tone(hz, 3.0, 0.5f)).l, kSr, hz, (int)(kSr / hz * 4.0));
            double mn, mx;
            minMax(tr.amp, 24000, 120000, mn, mx);
            CHECK_MSG(mn < 0.03, "%.0f Hz: never drops below %.3f", hz, mn);
        }
        for (double hz : {240.0, 3750.0}) {
            s->reset();
            auto tr = demod(run(*s, tone(hz, 3.0, 0.5f)).l, kSr, hz, (int)(kSr / hz * 4.0));
            double mn, mx;
            minMax(tr.amp, 24000, 120000, mn, mx);
            CHECK_MSG(mn > 0.15, "%.0f Hz is outside the sweep but drops to %.3f", hz, mn);
        }
    }
    {   // rate: the pattern repeats 1.5 times a second (free), or once per eighth note at 120 BPM = 4 Hz (synced)
        auto tr = demod(run(*make(1.5f, 0), tone(600.0, 6.0, 0.5f)).l, kSr, 600.0, 320);
        CHECK_NEAR(acRate(tr.amp, 24000, 280000, kSr, 0.4, 6.0), 1.5, 0.015);
        Transport t;
        t.playing = true;
        auto ts = demod(run(*make(0.2f, 10), tone(600.0, 4.0, 0.5f), t).l, kSr, 600.0, 320);
        CHECK_NEAR(acRate(ts.amp, 24000, 180000, kSr, 1.0, 12.0), 4.0, 0.04);
    }
    {   // depth 0.5 sweeps half the range (in octaves) about the centre; stereo phase shifts one side's sweep in time
        auto s = make(1.5f, 0);
        s->pDepth->set(0.5f);
        auto tr = demod(run(*s, tone(400.0, 3.0, 0.5f)).l, kSr, 400.0, 480);     // sqrt(300 * 3000) = 949; half range reaches 949 / 10^0.25 = 533
        double mn, mx;
        minMax(tr.amp, 24000, 120000, mn, mx);
        CHECK(mn > 0.2);
        auto q = make(1.0f, 0);
        q->pPhase->set(90.0f);
        auto out = run(*q, tone(600.0, 5.0, 0.5f));
        auto l = demod(out.l, kSr, 600.0, 320), r = demod(out.r, kSr, 600.0, 320);
        // the right side is a quarter cycle ahead: its envelope matches the left one 0.25 s later
        double best = -2.0;
        int bestLag = 0;
        for (int lag = -24000; lag <= 24000; lag += 240) {
            std::vector<double> shifted(l.amp.size(), 0.0);
            for (size_t i = 48000; i < 200000; ++i) shifted[i] = r.amp[(size_t)((int)i + lag)];
            const double c = corr(l.amp, shifted, 48000, 200000);
            if (c > best) { best = c; bestLag = lag; }
        }
        CHECK_MSG(std::abs(std::abs(bestLag) - 12000) <= 480 && best > 0.95, "best match at %d samples (%.2f)", bestLag, best);
    }
}

// ------------------------------------------------------------------ Flutter

TEST(flutter_shapes_rate_depth_and_auto_pan) {
    auto make = [](int shape, float rate, float depth, float phase, float smoothMs) {
        auto f = std::make_shared<Flutter>();
        f->prepare(kSr);
        f->pShape->set((float)shape); f->pRate->set(rate); f->pDepth->set(depth); f->pPhase->set(phase); f->pSmooth->set(smoothMs);
        return f;
    };
    // A constant 1.0 in: the output is the gain itself.
    {   // sine at 4 Hz, 70 % deep: between 0.3 and 1.0, 12000 samples a cycle, starting at the top
        auto g = run(*make(Flutter::Sine, 4.0f, 0.7f, 0.0f, 4.0f), dc(2.0, 1.0f));
        CHECK(g.l == g.r);
        CHECK_NEAR(*std::min_element(g.l.begin(), g.l.end()), 0.3, 1e-5);
        CHECK_NEAR(*std::max_element(g.l.begin(), g.l.end()), 1.0, 1e-6);
        double worst = 0.0;
        for (size_t i = 0; i < g.l.size(); ++i) worst = std::max(worst, std::fabs(g.l[i] - (1.0 - 0.7 * (0.5 - 0.5 * std::cos(kTwoPi * 4.0 * (double)i / kSr)))));
        CHECK_MSG(worst < 1e-5, "sine: %g from the formula", worst);
        std::vector<double> gd(g.l.begin(), g.l.end());
        CHECK_NEAR(oscRate(gd, 0, gd.size(), kSr), 4.0, 0.001);
    }
    {   // triangle: straight lines between 1 and 1 - depth
        auto g = run(*make(Flutter::Triangle, 2.0f, 1.0f, 0.0f, 0.0f), dc(1.0, 1.0f));
        double worst = 0.0;
        for (size_t i = 0; i < g.l.size(); ++i) { const double p = std::fmod(2.0 * (double)i / kSr, 1.0); worst = std::max(worst, std::fabs(g.l[i] - std::fabs(2.0 * p - 1.0))); }
        CHECK_MSG(worst < 1e-5, "triangle: %g from the formula", worst);
    }
    {   // square: full for half the cycle, down for the other half; with smoothing the edge takes that long and nothing jumps
        auto hard = run(*make(Flutter::Square, 4.0f, 0.7f, 0.0f, 0.0f), dc(1.0, 1.0f));
        CHECK(hard.l[3000] == 1.0f && std::fabs(hard.l[9000] - 0.3f) < 1e-6f);
        CHECK_NEAR(largestStep(hard.l), 0.7, 1e-5);
        auto soft = run(*make(Flutter::Square, 4.0f, 0.7f, 0.0f, 4.0f), dc(1.0, 1.0f));
        CHECK(soft.l[3000] == 1.0f && std::fabs(soft.l[9000] - 0.3f) < 1e-6f);
        // a raised-cosine edge 4 ms (192 samples) long: steepest step is depth * (pi / 2) / 192
        CHECK_MSG(largestStep(soft.l) < 0.7 * 1.5708 / 192.0 * 1.01, "largest step %.5f", largestStep(soft.l));
        CHECK_NEAR(soft.l[6000], 0.65, 1e-4);           // half-way through the falling edge, which is centred on the half cycle
        int edge = 0;
        for (size_t i = 5000; i < 7000; ++i) if (soft.l[i] < 0.9999f && soft.l[i] > 0.3001f) ++edge;
        CHECK_MSG(std::abs(edge - 192) <= 4, "falling edge takes %d samples", edge);
        // very long smoothing cannot swallow the cycle: the edge is held to half of it
        auto slow = run(*make(Flutter::Square, 20.0f, 1.0f, 0.0f, 50.0f), dc(0.5, 1.0f));
        CHECK(*std::min_element(slow.l.begin(), slow.l.end()) < 0.01f && *std::max_element(slow.l.begin() + 100, slow.l.end()) > 0.99f);
    }
    {   // saw up climbs through the cycle and drops at its end; saw down jumps up at the start and falls
        auto up = run(*make(Flutter::SawUp, 2.0f, 1.0f, 0.0f, 5.0f), dc(1.0, 1.0f));
        CHECK(up.l[0] == 0.0f);
        CHECK_NEAR(up.l[6000], 6000.0 / (24000.0 - 240.0), 1e-4);
        CHECK_NEAR(up.l[23759], 1.0, 1e-3);
        CHECK(up.l[24001] < 0.001f);
        CHECK(largestStep(up.l) < 1.5708 / 240.0 * 1.01);
        auto down = run(*make(Flutter::SawDown, 2.0f, 1.0f, 0.0f, 5.0f), dc(1.0, 1.0f));
        CHECK(down.l[0] == 0.0f);
        CHECK_NEAR(down.l[241], 1.0, 1e-3);
        CHECK_NEAR(down.l[18000], 6000.0 / (24000.0 - 240.0), 1e-4);
        CHECK(largestStep(down.l) < 1.5708 / 240.0 * 1.01);
        auto raw = run(*make(Flutter::SawUp, 2.0f, 1.0f, 0.0f, 0.0f), dc(1.0, 1.0f));
        CHECK_NEAR(raw.l[12000], 0.5, 1e-5);
    }
    {   // stereo phase: 0 is tremolo (both sides together), 180 degrees is auto-pan (one up while the other is down)
        auto pan = run(*make(Flutter::Sine, 3.0f, 0.8f, 180.0f, 4.0f), dc(2.0, 1.0f));
        double worst = 0.0;
        for (size_t i = 0; i < pan.l.size(); ++i) worst = std::max(worst, std::fabs(pan.l[i] + pan.r[i] - 1.2));       // the two gains always add to 2 - depth
        CHECK(worst < 1e-5);
        CHECK(pan.l[0] > 0.999f && pan.r[0] < 0.201f);
        auto quarter = run(*make(Flutter::Sine, 3.0f, 0.8f, 90.0f, 4.0f), dc(2.0, 1.0f));
        CHECK_NEAR(quarter.r[0], 0.6, 1e-5);
        CHECK_NEAR(quarter.r[12000], quarter.l[16000], 1e-5);      // the right side runs a quarter cycle (4000 samples) ahead
    }
    {   // depth 0 is bit-exact; depth 1 reaches silence; it scales a signal rather than adding anything
        auto in = noise(0.3, 0.5f);
        auto out = run(*make(Flutter::Square, 7.0f, 0.0f, 180.0f, 3.0f), in);
        CHECK(out.l == in.l && out.r == in.r);
        auto full = run(*make(Flutter::Sine, 5.0f, 1.0f, 0.0f, 4.0f), in);
        CHECK(std::fabs(full.l[4800]) < 1e-6f);
        CHECK(Flutter().latencySamples() == 0 && Flutter().tailSeconds() == 0.0);
    }
    // the shape function itself: within 0..1, and continuous whenever there is smoothing
    for (int shape = 0; shape < Flutter::NumShapes; ++shape) {
        float prev = Flutter::shapeAt(shape, 0.0, 0.05), lo = 1.0f, hi = 0.0f, jump = 0.0f;
        for (int i = 1; i <= 20000; ++i) {
            const float v = Flutter::shapeAt(shape, (double)i / 10000.0, 0.05);
            lo = std::min(lo, v); hi = std::max(hi, v);
            jump = std::max(jump, std::fabs(v - prev));
            prev = v;
        }
        CHECK_MSG(lo >= 0.0f && lo < 1e-4f && hi <= 1.0f && hi > 0.9999f && jump < 0.004f, "shape %d: %.4f..%.4f, largest jump %.4f", shape, lo, hi, jump);
    }
}

TEST(flutter_follows_the_song_position) {
    auto make = [](int shape, int sync, float rate) {
        auto f = std::make_shared<Flutter>();
        f->prepare(kSr);
        f->pShape->set((float)shape); f->pSync->set((float)sync); f->pRate->set(rate); f->pDepth->set(0.8f); f->pSmooth->set(5.0f);
        return f;
    };
    const double tempo = 140.0, tps = tempo * kPPQ / (60.0 * kSr);
    Transport play;
    play.tempo = tempo;
    play.playing = true;
    for (int shape = 0; shape < Flutter::NumShapes; ++shape) {
        // synced to quarter notes: the gain at any sample is the shape at (song position / 960 ticks)
        const double rateHz = tempo / 60.0, width = 0.005 * rateHz;
        auto g = run(*make(shape, 7, 9.0f), dc(2.0, 1.0f), play);
        double worst = 0.0;
        for (size_t i = 0; i < g.l.size(); ++i) worst = std::max(worst, std::fabs(g.l[i] - (1.0 - 0.8 * (1.0 - Flutter::shapeAt(shape, (double)i * tps / 960.0, width)))));
        CHECK_MSG(worst < 2e-5, "shape %d: %g from the song position", shape, worst);
        // started somewhere else in the song (as playback from a marker is, where an export starts at the top):
        // the same song positions give the same samples, bit for bit
        Transport later = play;
        later.startTick = 19200.0 * tps;
        auto h = run(*make(shape, 7, 9.0f), dc(1.0, 1.0f), later);
        bool same = true;
        for (size_t i = 0; i < h.l.size(); ++i) same = same && h.l[i] == g.l[i + 19200];
        CHECK_MSG(same, "shape %d differs when playback starts mid-song", shape);
    }
    {   // a free rate in Hz is locked to the song time too
        auto g = run(*make(Flutter::Triangle, 0, 3.0f), dc(2.0, 1.0f), play);
        double worst = 0.0;
        for (size_t i = 0; i < g.l.size(); ++i) worst = std::max(worst, std::fabs(g.l[i] - (1.0 - 0.8 * (1.0 - Flutter::shapeAt(Flutter::Triangle, 3.0 * (double)i / kSr, 0.0)))));
        CHECK(worst < 2e-5);
        Transport later = play;
        later.startTick = 48000.0 * tps;
        auto h = run(*make(Flutter::Triangle, 0, 3.0f), dc(1.0, 1.0f), later);
        CHECK(std::fabs(h.l[1234] - g.l[48000 + 1234]) < 1e-6f);
    }
    {   // the rate follows the tempo when synced: an eighth note at 174 BPM is 5.8 Hz; stopped, it runs at the same rate from wherever it was
        Transport t;
        t.tempo = 174.0;
        t.playing = true;
        auto g = run(*make(Flutter::Sine, 10, 1.0f), dc(3.0, 1.0f), t);
        std::vector<double> gd(g.l.begin(), g.l.end());
        CHECK_NEAR(oscRate(gd, 0, gd.size(), kSr), 5.8, 0.002);
        t.playing = false;
        auto s = run(*make(Flutter::Sine, 10, 1.0f), dc(3.0, 1.0f), t);
        std::vector<double> sd(s.l.begin(), s.l.end());
        CHECK_NEAR(oscRate(sd, 0, sd.size(), kSr), 5.8, 0.002);
    }
    {   // jumping about in the song, stopping and starting, and sweeping a free rate: the gain never jumps
        auto f = make(Flutter::Sine, 7, 2.0f);
        Stereo out = dc(4.0, 1.0f);
        double tick = 0.0;
        for (size_t i = 0; i < out.l.size(); i += kQuantum) {
            const size_t k = i / kQuantum;
            ProcessContext c;
            c.tempo = tempo;
            c.playing = (k / 400) % 3 != 1;                 // stops for a while now and then
            if (k % 333 == 0) tick = (double)(k * 7919 % 50000);       // the transport jumps
            c.tickPos = tick;
            if (c.playing) tick += kQuantum * tps;
            if (k == 1500) f->pSync->set(10.0f);            // the division changes
            if (k > 2200) { f->pSync->set(0.0f); f->pRate->set(1.0f + 8.0f * (float)(k - 2200) / 800.0f); }      // then a free rate is swept
            c.io[0] = out.l.data() + i;
            c.io[1] = out.r.data() + i;
            f->process(c);
        }
        // a sine at up to 9 Hz, 80 % deep, moves 0.8 * pi * 9 / 48000 = 0.0005 a sample; a phase slide after a jump is faster but still smooth
        CHECK_MSG(largestStep(out.l) < 0.01f, "largest step %.5f", largestStep(out.l));
        CHECK(*std::min_element(out.l.begin(), out.l.end()) > 0.1999f);
    }
}

// ------------------------------------------------------------------ Waver

TEST(waver_reports_its_latency_and_nulls_against_it) {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        Waver w;
        w.prepare(sr);
        const int lat = w.latencySamples();
        // the centre of the swing: enough for 100 cents at 0.5 Hz, plus a few samples for the interpolator
        CHECK(lat == (int)std::ceil((std::pow(2.0, 100.0 / 1200.0) - 1.0) * sr / (kTwoPi * 0.5)) + 4);
        w.pDepth->set(0.0f);
        Transport t;
        t.sr = sr;
        auto in = noise(0.3, 0.5f, sr);
        auto out = run(w, in, t);
        bool same = true;
        for (size_t i = (size_t)lat; i < out.l.size(); ++i) same = same && out.l[i] == in.l[i - (size_t)lat] && out.r[i] == in.r[i - (size_t)lat];
        CHECK_MSG(same, "%.0f Hz: depth 0 is not a pure delay of %d samples", sr, lat);
        CHECK(peak(out, 0, (size_t)lat) == 0.0f);
        CHECK_NEAR(w.tailSeconds(), (double)lat / sr, 1e-9);
    }
    {   // with the vibrato on, the delay swings evenly about that centre: a slow tone stays in time on average
        Waver w;
        w.prepare(kSr);
        w.pRate->set(6.0f); w.pDepth->set(60.0f);
        const int lat = w.latencySamples();
        auto in = tone(100.0, 2.0, 0.5f);
        auto out = run(w, in);
        auto a = demod(out.l, kSr, 100.0, 960), b = demod(in.l, kSr, 100.0, 960);
        // average phase of output against input delayed by the latency
        double meanDiff = 0.0;
        int count = 0;
        for (size_t i = 24000; i < 72000; ++i, ++count) meanDiff += (a.hz[i] - b.hz[i - (size_t)lat]);
        CHECK(std::fabs(meanDiff / count) < 0.01);
        CHECK(lat == 913);
    }
}

TEST(waver_pitch_deviation_matches_the_depth) {
    struct Case { double sr, f0; int avg; float rate, cents; double seconds; } cases[] = {
        {48000.0, 3000.0, 64, 5.5f, 20.0f, 3.0}, {48000.0, 3000.0, 64, 5.5f, 50.0f, 3.0}, {48000.0, 3000.0, 64, 2.0f, 100.0f, 4.0},
        {48000.0, 3000.0, 64, 10.0f, 10.0f, 3.0}, {48000.0, 3000.0, 64, 0.5f, 100.0f, 9.0}, {48000.0, 3000.0, 64, 14.0f, 100.0f, 3.0},
        {44100.0, 2205.0, 80, 6.0f, 35.0f, 3.0}, {96000.0, 3000.0, 128, 6.0f, 35.0f, 3.0}, {48000.0, 3000.0, 64, 5.0f, 2.0f, 3.0}};
    for (auto& cs : cases) {
        Waver w;
        w.prepare(cs.sr);
        w.pRate->set(cs.rate); w.pDepth->set(cs.cents);
        Transport t;
        t.sr = cs.sr;
        auto tr = demod(run(w, tone(cs.f0, cs.seconds, 0.5f, cs.sr), t).l, cs.sr, cs.f0, cs.avg);
        const size_t a = (size_t)(0.5 * cs.sr), b = (size_t)((cs.seconds - 0.2) * cs.sr);
        double mn, mx;
        minMax(tr.hz, a, b, mn, mx);
        const double up = cents(mx, cs.f0), down = cents(mn, cs.f0);
        // the tape speed goes 1 +/- r with r = 2^(cents / 1200) - 1: exactly +cents at the top, a shade more than -cents at the bottom
        const double wantDown = 1200.0 * std::log2(2.0 - std::pow(2.0, cs.cents / 1200.0));
        CHECK_MSG(std::fabs(up / cs.cents - 1.0) < 0.02 && std::fabs(down / wantDown - 1.0) < 0.02,
                  "%.0f Hz, %.1f Hz, %.0f ct: measured %+.2f / %+.2f ct (expected %+.2f / %+.2f)", cs.sr, cs.rate, cs.cents, up, down, (double)cs.cents, wantDown);
        CHECK_MSG(std::fabs(oscRate(tr.hz, a, b, cs.sr) / cs.rate - 1.0) < 0.01, "rate %.3f Hz, set %.1f Hz", oscRate(tr.hz, a, b, cs.sr), cs.rate);
        // and the level does not wobble with it
        Waver again;
        again.prepare(cs.sr);
        again.pRate->set(cs.rate); again.pDepth->set(cs.cents);
        auto level = slidingRms(run(again, tone(cs.f0, cs.seconds, 0.5f, cs.sr), t).l, (size_t)(cs.sr / cs.f0 * 16.0));
        double lo, hi;
        minMax(level, a, b, lo, hi);
        CHECK_MSG(hi / lo < 1.03, "level moves by %.3f dB", db(hi / lo));
    }
    {   // stereo phase 180: the left goes sharp as the right goes flat
        Waver w;
        w.prepare(kSr);
        w.pRate->set(5.0f); w.pDepth->set(40.0f); w.pPhase->set(180.0f);
        auto out = run(w, tone(3000.0, 2.0, 0.5f));
        auto l = demod(out.l, kSr, 3000.0, 64), r = demod(out.r, kSr, 3000.0, 64);
        CHECK(corr(l.hz, r.hz, 24000, 86000) < -0.99);
    }
    {   // tempo sync: an eighth note at 120 BPM is 4 Hz; depth is kept
        Waver w;
        w.prepare(kSr);
        w.pSync->set(10.0f); w.pDepth->set(30.0f);
        Transport t;
        t.playing = true;
        auto tr = demod(run(w, tone(3000.0, 3.0, 0.5f), t).l, kSr, 3000.0, 64);
        double mn, mx;
        minMax(tr.hz, 24000, 130000, mn, mx);
        CHECK_NEAR(oscRate(tr.hz, 24000, 130000, kSr), 4.0, 0.02);
        CHECK_NEAR(cents(mx, 3000.0), 30.0, 0.6);
    }
    {   // delay and fade-in after silence: the note starts straight, then the vibrato comes in
        Waver w;
        w.prepare(kSr);
        w.pRate->set(6.0f); w.pDepth->set(50.0f); w.pOnset->set(300.0f); w.pFadeIn->set(200.0f);
        const size_t lat = (size_t)w.latencySamples();
        Stereo in = silence(0.5);
        append(in, tone(3000.0, 1.5, 0.5f));
        append(in, silence(0.5));
        append(in, tone(3000.0, 1.5, 0.5f));
        auto tr = demod(run(w, in).l, kSr, 3000.0, 64);
        for (size_t start : {(size_t)24000, (size_t)(24000 + 72000 + 24000)}) {
            const size_t s0 = start + lat;      // where the note comes out
            double mn, mx;
            minMax(tr.hz, s0 + 480, s0 + 14400 - 200, mn, mx);
            CHECK_MSG(std::fabs(cents(mx, 3000.0)) < 0.5 && std::fabs(cents(mn, 3000.0)) < 0.5, "moves %.2f ct during the delay", cents(mx, 3000.0));
            minMax(tr.hz, s0 + 14400 + 2400, s0 + 14400 + 7200, mn, mx);      // part-way up the fade
            CHECK(cents(mx, 3000.0) > 5.0 && cents(mx, 3000.0) < 45.0);
            minMax(tr.hz, s0 + 24000 + 2400, s0 + 60000, mn, mx);             // fully in
            CHECK_NEAR(cents(mx, 3000.0), 50.0, 1.0);
        }
    }
}

// ------------------------------------------------------------------ Rotor

namespace {
std::shared_ptr<Rotor> rotor(int horn, int drum, float doppler, float am, float spread = 0.0f, float distance = 0.0f, float drive = 0.0f) {
    auto r = std::make_shared<Rotor>();
    r->prepare(kSr);
    r->pHorn->set((float)horn); r->pDrum->set((float)drum); r->pDoppler->set(doppler); r->pAmDepth->set(am);
    r->pSpread->set(spread); r->pDistance->set(distance); r->pDrive->set(drive); r->pMix->set(1.0f);
    return r;
}
}

TEST(rotor_reports_its_latency_and_keeps_the_dry_path_in_step) {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        Rotor r;
        r.prepare(sr);
        const size_t lat = (size_t)r.latencySamples();
        CHECK(lat == 16 + (size_t)std::lround(0.0012 * sr));       // the oversampler plus the centre of the Doppler delay
        r.pMix->set(0.0f);
        Transport t;
        t.sr = sr;
        auto in = noise(0.3, 0.5f, sr);
        auto out = run(r, in, t);
        bool same = true;
        for (size_t i = lat; i < out.l.size(); ++i) same = same && out.l[i] == in.l[i - lat] && out.r[i] == in.r[i - lat];
        CHECK_MSG(same, "%.0f Hz: dry only is not a pure delay of %d samples", sr, (int)lat);
    }
    {   // the wet path arrives at the same moment: with the rotors at rest, a click through the cabinet peaks at the latency
        auto r = rotor(Rotor::Stop, Rotor::Stop, 0.0f, 0.0f);
        const size_t lat = (size_t)r->latencySamples();
        CHECK(lat == 74);
        auto out = run(*r, impulse(0.1));
        size_t at = 0;
        for (size_t i = 0; i < out.l.size(); ++i) if (std::fabs(out.l[i]) > std::fabs(out.l[at])) at = i;
        CHECK_MSG(std::abs((int)at - (int)lat) <= 1, "wet click at %d", (int)at);
        // and a low tone and a high tone each come out at about the level they went in (the crossover sums flat)
        for (double hz : {150.0, 400.0, 2500.0}) {
            r->reset();
            auto o = run(*r, tone(hz, 0.5, 0.2f));
            CHECK_MSG(std::fabs(db(tu::rmsOf(o.l, 12000) / (0.2 / std::sqrt(2.0)))) < 3.5, "%.0f Hz comes out %.1f dB different", hz, db(tu::rmsOf(o.l, 12000) / (0.2 / std::sqrt(2.0))));
        }
    }
}

TEST(rotor_modulates_treble_with_the_horn_and_bass_with_the_drum) {
    {   // amplitude: a 2 kHz tone pulses at the horn's speed, a 200 Hz tone at the drum's
        auto r = rotor(Rotor::Fast, Rotor::Fast, 0.0f, 0.8f);
        auto hi = demod(run(*r, tone(2000.0, 3.0, 0.3f)).l, kSr, 2000.0, 96);
        CHECK_NEAR(oscRate(hi.amp, 24000, 140000, kSr), 6.8, 0.03);
        double mn, mx;
        minMax(hi.amp, 24000, 140000, mn, mx);
        CHECK_MSG(mn / mx > 0.12 && mn / mx < 0.4, "horn level swings to %.2f of its peak", mn / mx);
        r->reset();
        auto lo = demod(run(*r, tone(200.0, 3.0, 0.3f)).l, kSr, 200.0, 960);
        CHECK_NEAR(oscRate(lo.amp, 24000, 140000, kSr), 5.8, 0.03);
        minMax(lo.amp, 24000, 140000, mn, mx);
        CHECK_MSG(mn / mx > 0.6 && mn / mx < 0.9, "drum level swings to %.2f of its peak", mn / mx);      // the drum is gentler
        CHECK_NEAR(r->meter(0), 6.8, 1e-4);
        CHECK_NEAR(r->meter(1), 5.8, 1e-4);
    }
    {   // Doppler: the horn's mouth travels a circle of 0.55 ms radius (in sound-time): at 6.8 turns a second that bends
        // a 3 kHz tone by 3000 * 2 pi * 6.8 * 0.00055 = 70.5 Hz (40 cents) each way
        auto r = rotor(Rotor::Fast, Rotor::Fast, 1.0f, 0.0f);
        auto hi = demod(run(*r, tone(3000.0, 3.0, 0.3f)).l, kSr, 3000.0, 64);
        double mn, mx;
        minMax(hi.hz, 24000, 140000, mn, mx);
        CHECK_MSG(std::fabs((mx - 3000.0) / 70.5 - 1.0) < 0.03 && std::fabs((3000.0 - mn) / 70.5 - 1.0) < 0.03, "horn bends +%.1f / -%.1f Hz", mx - 3000.0, 3000.0 - mn);
        CHECK_NEAR(oscRate(hi.hz, 24000, 140000, kSr), 6.8, 0.03);
        r->reset();
        auto level = slidingRms(run(*r, tone(3000.0, 3.0, 0.3f)).l, 256);
        double alo, ahi;
        minMax(level, 24000, 140000, alo, ahi);
        CHECK_MSG(ahi / alo < 1.03, "level moves by %.2f dB with the amplitude depth at zero", db(ahi / alo));
        // the drum: 0.2 ms radius at 5.8 turns a second bends 300 Hz by 2.19 Hz
        r->reset();
        auto lo = demod(run(*r, tone(300.0, 3.0, 0.3f)).l, kSr, 300.0, 640);
        minMax(lo.hz, 24000, 140000, mn, mx);
        CHECK_MSG(std::fabs((mx - 300.0) / 2.19 - 1.0) < 0.06, "drum bends +%.2f Hz", mx - 300.0);
        CHECK_NEAR(oscRate(lo.hz, 24000, 140000, kSr), 5.8, 0.03);
        // half the Doppler setting, half the bend
        auto h = rotor(Rotor::Fast, Rotor::Fast, 0.5f, 0.0f);
        auto half = demod(run(*h, tone(3000.0, 2.0, 0.3f)).l, kSr, 3000.0, 64);
        minMax(half.hz, 24000, 90000, mn, mx);
        CHECK_NEAR((mx - 3000.0) / 35.25, 1.0, 0.03);
    }
    {   // the slow speeds, and the rates are the user's to set
        auto r = rotor(Rotor::Slow, Rotor::Slow, 0.0f, 0.8f);
        auto hi = demod(run(*r, tone(2000.0, 8.0, 0.3f)).l, kSr, 2000.0, 96);
        CHECK_NEAR(acRate(hi.amp, 24000, 380000, kSr, 0.3, 3.0), 0.8, 0.01);
        r->reset();
        auto lo = demod(run(*r, tone(200.0, 9.0, 0.3f)).l, kSr, 200.0, 960);
        CHECK_NEAR(acRate(lo.amp, 24000, 430000, kSr, 0.3, 3.0), 0.67, 0.01);
        auto f = rotor(Rotor::Fast, Rotor::Fast, 0.0f, 0.8f);
        f->pHornFast->set(9.0f);
        CHECK_NEAR(oscRate(demod(run(*f, tone(2000.0, 2.0, 0.3f)).l, kSr, 2000.0, 96).amp, 24000, 90000, kSr), 9.0, 0.05);
    }
    {   // stopped: nothing moves, a tone comes out steady
        auto r = rotor(Rotor::Stop, Rotor::Stop, 1.0f, 1.0f, 120.0f, 0.5f);
        auto tr = demod(run(*r, tone(2000.0, 1.0, 0.3f)).l, kSr, 2000.0, 96);
        double mn, mx;
        minMax(tr.amp, 12000, 44000, mn, mx);
        CHECK(mx / mn < 1.0005);
        minMax(tr.hz, 12000, 44000, mn, mx);
        CHECK(mx - mn < 0.01);
        CHECK(r->meter(0) == 0.0f && r->meter(1) == 0.0f);
    }
    {   // microphones: with no spread the two sides are the same; 180 degrees apart, one hears the horn coming as the other hears it going
        auto r = rotor(Rotor::Fast, Rotor::Fast, 0.5f, 0.8f, 0.0f);
        auto same = run(*r, tone(2000.0, 1.0, 0.3f));
        CHECK(same.l == same.r);
        auto w = rotor(Rotor::Fast, Rotor::Fast, 0.0f, 0.8f, 180.0f);
        auto wide = run(*w, tone(2000.0, 2.0, 0.3f));
        auto l = demod(wide.l, kSr, 2000.0, 96), rr = demod(wide.r, kSr, 2000.0, 96);
        CHECK(corr(l.amp, rr.amp, 24000, 90000) < -0.9);
        // 90 degrees apart: the right microphone hears each pass a quarter turn (1 / 6.8 / 4 s = 1765 samples) later
        auto q = rotor(Rotor::Fast, Rotor::Fast, 0.0f, 0.8f, 90.0f);
        auto qo = run(*q, tone(2000.0, 2.0, 0.3f));
        auto ql = demod(qo.l, kSr, 2000.0, 96), qr = demod(qo.r, kSr, 2000.0, 96);
        double best = -2.0;
        int bestLag = 0;
        for (int lag = -3500; lag <= 3500; lag += 25) {
            std::vector<double> sh(ql.amp.size(), 0.0);
            for (size_t i = 24000; i < 80000; ++i) sh[i] = qr.amp[(size_t)((int)i + lag)];
            const double cc = corr(ql.amp, sh, 24000, 80000);
            if (cc > best) { best = cc; bestLag = lag; }
        }
        CHECK_MSG(std::abs(bestLag - 1765) <= 100, "right lags left by %d samples", bestLag);
        // distance: far microphones hear a shallower swing and more of each other
        auto nearMic = rotor(Rotor::Fast, Rotor::Fast, 0.0f, 0.8f, 180.0f, 0.0f), farMic = rotor(Rotor::Fast, Rotor::Fast, 0.0f, 0.8f, 180.0f, 1.0f);
        auto n = demod(run(*nearMic, tone(2000.0, 2.0, 0.3f)).l, kSr, 2000.0, 96), f = demod(run(*farMic, tone(2000.0, 2.0, 0.3f)).l, kSr, 2000.0, 96);
        double nmn, nmx, fmn, fmx;
        minMax(n.amp, 24000, 90000, nmn, nmx);
        minMax(f.amp, 24000, 90000, fmn, fmx);
        CHECK_MSG(fmn / fmx > 2.0 * nmn / nmx, "near %.2f, far %.2f", nmn / nmx, fmn / fmx);
    }
    {   // balance: all the way to one side leaves only that rotor
        auto r = rotor(Rotor::Stop, Rotor::Stop, 0.0f, 0.0f);
        r->pBalance->set(-1.0f);        // drum only
        CHECK(tu::rmsOf(run(*r, tone(3000.0, 0.5, 0.3f)).l, 12000) < 0.01);
        r->reset();
        CHECK(tu::rmsOf(run(*r, tone(200.0, 0.5, 0.3f)).l, 12000) > 0.15);
        r->pBalance->set(1.0f);         // horn only
        r->reset();
        CHECK(tu::rmsOf(run(*r, tone(200.0, 0.5, 0.3f)).l, 12000) < 0.01);
    }
}

TEST(rotor_takes_time_to_change_speed) {
    auto runFor = [](Rotor& r, double seconds) { Stereo s = silence(seconds); run(r, s); };
    {   // slow to fast: the horn is 63 % of the way after 0.45 s, the drum after 2.2 s
        auto r = rotor(Rotor::Slow, Rotor::Slow, 0.5f, 0.5f);
        runFor(*r, 0.1);
        CHECK_NEAR(r->meter(0), 0.8, 1e-4);          // starts at speed: no spin-up from rest at the top of a render
        CHECK_NEAR(r->meter(1), 0.67, 1e-4);
        r->pHorn->set(Rotor::Fast); r->pDrum->set(Rotor::Fast);
        runFor(*r, 0.45);
        CHECK_NEAR(r->meter(0), 0.8 + (6.8 - 0.8) * (1.0 - std::exp(-1.0)), 0.03);
        CHECK(r->meter(1) < 0.67 + (5.8 - 0.67) * 0.2);                         // the drum has barely started
        runFor(*r, 2.2 - 0.45);
        CHECK_NEAR(r->meter(1), 0.67 + (5.8 - 0.67) * (1.0 - std::exp(-1.0)), 0.03);
        CHECK(r->meter(0) > 6.8 * 0.98);                                         // the horn is long since there
        runFor(*r, 12.0);
        CHECK_NEAR(r->meter(1), 5.8, 0.03);
        // fast to slow: 0.75 s and 1.8 s
        r->pHorn->set(Rotor::Slow); r->pDrum->set(Rotor::Slow);
        runFor(*r, 0.75);
        CHECK_NEAR(r->meter(0), 0.8 + (6.8 - 0.8) * std::exp(-1.0), 0.03);
        runFor(*r, 1.8 - 0.75);
        CHECK_NEAR(r->meter(1), 0.67 + (5.8 - 0.67) * std::exp(-1.0), 0.04);
        // brake: both coast to a stop and then are exactly still
        r->pHorn->set(Rotor::Stop); r->pDrum->set(Rotor::Stop);
        runFor(*r, 30.0);
        CHECK(r->meter(0) == 0.0f && r->meter(1) == 0.0f);
        // one rotor can run while the other rests
        r->pHorn->set(Rotor::Fast);
        runFor(*r, 5.0);
        CHECK(r->meter(0) > 6.7f && r->meter(1) == 0.0f);
    }
    {   // inertia scales the times
        auto r = rotor(Rotor::Slow, Rotor::Slow, 0.5f, 0.5f);
        r->pInertia->set(2.0f);
        runFor(*r, 0.1);
        r->pHorn->set(Rotor::Fast);
        runFor(*r, 0.9);
        CHECK_NEAR(r->meter(0), 0.8 + (6.8 - 0.8) * (1.0 - std::exp(-1.0)), 0.03);
    }
    {   // the pitch of a tone glides with it: no jump in level or pitch at the switch
        auto r = rotor(Rotor::Slow, Rotor::Slow, 1.0f, 0.6f);
        Stereo in = tone(1500.0, 3.0, 0.3f), out = in;
        for (size_t i = 0; i < in.l.size(); i += kQuantum) {
            if (i == 48000) { r->pHorn->set(Rotor::Fast); r->pDrum->set(Rotor::Fast); }
            if (i == 96000) { r->pHorn->set(Rotor::Stop); r->pDrum->set(Rotor::Stop); }
            ProcessContext c;
            c.io[0] = out.l.data() + i;
            c.io[1] = out.r.data() + i;
            r->process(c);
        }
        // 1500 Hz at this level moves 0.3 * 2 pi * 1500 / 48000 = 0.06 a sample
        CHECK_MSG(largestStep(out.l) < 0.09f, "largest step %.4f", largestStep(out.l));
    }
}

TEST(rotor_drive_adds_harmonics_without_aliasing) {
    const size_t from = 12000, n = 8192;
    auto level = [&](const std::vector<float>& x, double hz) { return levelDb(x, from, n, hz, kSr); };
    {   // at no drive the valve stage is nearly clean
        auto r = rotor(Rotor::Stop, Rotor::Stop, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        auto out = run(*r, tone(1000.0, 0.5, 0.25f));
        CHECK_MSG(level(out.l, 3000.0) - level(out.l, 1000.0) < -60.0, "third harmonic %.1f dB", level(out.l, 3000.0) - level(out.l, 1000.0));
        CHECK(level(out.l, 2000.0) - level(out.l, 1000.0) < -80.0);
    }
    {   // driven hard: strong odd harmonics, no even ones, and the level stays in hand
        auto r = rotor(Rotor::Stop, Rotor::Stop, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        auto out = run(*r, tone(1000.0, 0.5, 0.25f));
        CHECK(level(out.l, 3000.0) - level(out.l, 1000.0) > -16.0);
        CHECK(level(out.l, 2000.0) - level(out.l, 1000.0) < -70.0);
        CHECK(peak(out) < 0.6f && tu::rmsOf(out.l, from) > 0.1);
        // quiet playing gets louder, loud playing is held back: that is what an overdriven amplifier does
        r->reset();
        auto quiet = run(*r, tone(1000.0, 0.5, 0.02f));
        CHECK(tu::rmsOf(quiet.l, from) / (0.02 / std::sqrt(2.0)) > 2.0);
    }
    {   // A 9 kHz tone: its fifth harmonic (45 kHz) would fold back to 3 kHz without oversampling
        const float g = 0.25f * std::pow(10.0f, 0.5f * 1.8f);
        auto r = rotor(Rotor::Stop, Rotor::Stop, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f);
        auto out = run(*r, tone(9000.0, 0.5, 0.6f));
        std::vector<float> naive(out.l.size());
        for (size_t i = 0; i < naive.size(); ++i) naive[i] = std::tanh((float)(0.6 * g * std::sin(kTwoPi * 9000.0 * (double)i / kSr)));
        const double ours = level(out.l, 3000.0) - level(out.l, 9000.0), plain = level(naive, 3000.0) - level(naive, 9000.0);
        CHECK_MSG(ours < -60.0 && ours < plain - 25.0, "alias at 3 kHz: %.1f dB (without oversampling %.1f dB)", ours, plain);
        printf("      Rotor drive, 9 kHz tone: alias at 3 kHz %.1f dB below the tone (a plain tanh: %.1f dB)\n", -ours, -plain);
    }
}

TEST(rotor_through_an_organ_chord) {
    // A drawbar-organ chord (three notes, six harmonics each) through the tremolo setting.
    Stereo in = silence(4.0);
    const double notes[3] = {220.0, 277.18, 329.63};
    const double bars[6] = {1.0, 2.0, 3.0, 4.0, 6.0, 8.0}, amps[6] = {1.0, 0.8, 0.6, 0.5, 0.3, 0.25};
    for (size_t i = 0; i < in.l.size(); ++i) {
        double v = 0.0;
        for (double f : notes) for (int h = 0; h < 6; ++h) v += amps[h] * std::sin(kTwoPi * f * bars[h] * (double)i / kSr);
        in.l[i] = in.r[i] = (float)(0.035 * v);
    }
    Rotor r;
    r.prepare(kSr);
    for (auto& p : r.factoryPresets()) if (p.name == "Tremolo") r.loadState(p.state);
    auto out = run(r, in);
    CHECK(allFinite(out));
    CHECK_MSG(peak(out) < 0.9f && peak(out) > 0.1f, "peak %.2f (in %.2f)", peak(out), peak(in));
    CHECK_NEAR(db(tu::rmsOf(out.l, 48000) / tu::rmsOf(in.l, 48000)), 0.0, 6.0);       // about as loud as it went in
    // the fundamental of the low note (220 Hz, in the drum) wavers at the drum's speed; a high harmonic (1760 Hz, in the horn) at the horn's
    auto lo = demod(out.l, kSr, 220.0, 2400), hi = demod(out.l, kSr, 1760.0, 1200);
    CHECK_NEAR(acRate(lo.amp, 48000, 180000, kSr, 2.0, 12.0), 5.8, 0.1);
    CHECK_NEAR(acRate(hi.amp, 48000, 180000, kSr, 2.0, 12.0), 6.8, 0.1);
    // the two sides differ (the microphones are 100 degrees apart) but carry the same energy
    CHECK(tu::maxAbsDiff(out.l, out.r) > 0.02);
    CHECK_NEAR(db(tu::rmsOf(out.l, 48000) / tu::rmsOf(out.r, 48000)), 0.0, 0.5);
    // and the chord is still the chord: every drawbar of every note is there, none drowned by distortion products
    for (double f : notes) CHECK(levelDb(out.l, 48000, 65536, f, kSr) > levelDb(out.l, 48000, 65536, f * 1.5 + 13.0, kSr) + 25.0);
}
