// Snap (transient shaper), Curb (soft clipper) and the shared oversampler against known responses.
#include "DynTestUtil.h"
using namespace pulp;
using namespace tu;
using namespace dt;

namespace {
const double kSr = 48000.0;
const size_t kHitAt = 9600;     // the drum hit starts 0.2 s in

// A drum-like burst: a click of noise on a 180 Hz tone that dies away with an 80 ms time constant.
Stereo drumHit(float amp = 0.5f) {
    Stereo s = silence(1.4);
    dsp::Noise nz(77u);
    for (size_t i = kHitAt; i < s.l.size(); ++i) {
        const double t = (double)(i - kHitAt) / kSr;
        s.l[i] = s.r[i] = amp * (float)(std::sin(kTwoPi * 180.0 * t) * std::exp(-t / 0.08) + 0.6 * nz.next() * std::exp(-t / 0.003));
    }
    return s;
}
struct Parts { double first, body, tail; };
Parts parts(const std::vector<float>& x) {
    return {rmsOf(x, kHitAt, kHitAt + 240), rmsOf(x, kHitAt + 1920, kHitAt + 4800), rmsOf(x, kHitAt + 14400, kHitAt + 24000)};
}
Stereo snap(float attack, float sustain, const Stereo& in, float mix = 1.0f, float outDb = 0.0f) {
    Snap s;
    s.prepare(kSr);
    s.pAttack->set(attack); s.pSustain->set(sustain); s.pMix->set(mix); s.pOut->set(outDb);
    return run(s, in);
}
}

TEST(snap_at_zero_and_zero_is_transparent) {
    for (float mix : {1.0f, 0.3f}) {
        auto in = drumHit();
        addTo(in, noise(1.4, 0.02f));
        auto out = snap(0.0f, 0.0f, in, mix);
        CHECK(out.l == in.l && out.r == in.r);
    }
    CHECK(Snap().latencySamples() == 0);
    // output gain and mix do what they say
    auto in = drumHit();
    auto wet = snap(1.0f, 0.0f, in), half = snap(1.0f, 0.0f, in, 0.5f), dry = snap(1.0f, 0.0f, in, 0.0f), quiet = snap(0.0f, 0.0f, in, 1.0f, -6.0f);
    CHECK(dry.l == in.l);
    double worst = 0.0;
    for (size_t i = 0; i < in.l.size(); ++i) worst = std::max(worst, (double)std::fabs(half.l[i] - 0.5f * (in.l[i] + wet.l[i])));
    CHECK(worst < 1e-6);
    CHECK_NEAR(db(rmsOf(quiet.l) / rmsOf(in.l)), -6.0, 0.001);
}

TEST(snap_attack_and_sustain_shape_a_drum_hit) {
    const auto in = drumHit();
    const Parts dry = parts(in.l);
    double lastFirst = -1e9, lastTail = -1e9;
    for (float amount : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        // attack: the first 5 ms against the body (40 to 100 ms)
        const auto a = snap(amount, 0.0f, in);
        const Parts pa = parts(a.l);
        const double first = db(pa.first / pa.body) - db(dry.first / dry.body);
        CHECK_MSG(first > lastFirst + 1.0, "attack %.1f: first 5 ms against the body %+.2f dB, after %+.2f dB", amount, first, lastFirst);
        lastFirst = first;
        // ... and the tail is not touched by the attack control at all
        CHECK(std::equal(a.l.begin() + (long)(kHitAt + 14400), a.l.end(), in.l.begin() + (long)(kHitAt + 14400)));
        // sustain: the tail (300 to 500 ms) against the body
        const auto s = snap(0.0f, amount, in);
        const Parts ps = parts(s.l);
        const double tail = db(ps.tail / ps.body) - db(dry.tail / dry.body);
        CHECK_MSG(tail > lastTail + 1.0, "sustain %.1f: tail against the body %+.2f dB, after %+.2f dB", amount, tail, lastTail);
        lastTail = tail;
        // ... and the first milliseconds are not touched by the sustain control
        CHECK(std::equal(s.l.begin(), s.l.begin() + (long)(kHitAt + 240), in.l.begin()));
        FIG("Snap %+4.0f %%: attack moves the first 5 ms %+.2f dB against the body; sustain moves the tail %+.2f dB", amount * 100.0, first, tail);
        if (amount == -1.0f) { CHECK(first < -4.0 && tail < -4.0); }
        if (amount == 0.0f) { CHECK(first == 0.0 && tail == 0.0); }
        if (amount == 1.0f) { CHECK(first > 4.0 && tail > 4.0); }
        CHECK(allFinite(a) && allFinite(s));
    }
    // the meters say which way the gain went
    Snap sn;
    sn.prepare(kSr);
    sn.pAttack->set(1.0f);
    Stereo head = in;
    head.l.resize(kHitAt + 128); head.r.resize(kHitAt + 128);
    run(sn, head);
    CHECK(sn.meter(1) > 3.0f && sn.meter(0) == 0.0f);
}

TEST(snap_works_the_same_at_any_level) {
    // No threshold: 40 dB quieter in gives exactly the same shaping, 40 dB quieter.
    const auto loud = drumHit(0.5f), quiet = drumHit(0.005f);
    for (auto set : {std::pair<float, float>(1.0f, 0.0f), std::pair<float, float>(-1.0f, 0.0f), std::pair<float, float>(0.0f, 1.0f), std::pair<float, float>(0.6f, -0.8f)}) {
        const auto a = snap(set.first, set.second, loud), b = snap(set.first, set.second, quiet);
        double worst = 0.0;
        for (size_t i = 0; i < a.l.size(); ++i) worst = std::max(worst, (double)std::fabs(a.l[i] - 100.0f * b.l[i]));
        CHECK_MSG(worst < 1e-3 * peakOf(a.l), "attack %.1f sustain %.1f: differs by %g of the peak", set.first, set.second, worst / peakOf(a.l));
        CHECK(maxAbsDiff(a.l, loud.l) > 0.01);
    }
}

TEST(snap_leaves_a_steady_tone_alone) {
    // With nothing rising or dying away the two envelopes agree, so even the extreme settings do nothing.
    for (double hz : {60.0, 100.0, 1000.0, 7000.0}) {
        for (float amount : {1.0f, -1.0f}) {
            const auto in = tone(hz, 3.0, 0.3f);
            const auto out = snap(amount, amount, in);
            const size_t from = (size_t)(2.5 * kSr);
            double worst = 0.0;
            for (size_t i = from; i < out.l.size(); ++i) worst = std::max(worst, (double)std::fabs(out.l[i] - in.l[i]));
            CHECK_MSG(worst < 0.002 * 0.3, "%.0f Hz at %+.0f %%: off by %g", hz, amount * 100.0, worst);
        }
    }
}

// ------------------------------------------------------------------ the oversampler

TEST(oversampler_filter_is_flat_then_steep_and_32_samples_late) {
    const auto& t = dyn::Over4::tables();
    auto magDb = [&](double hz) {       // response of the 129-tap filter at the fast rate (192 kHz for a 48 kHz project)
        double re = 0.0, im = 0.0;
        for (int n = 0; n < dyn::Over4::kTaps; ++n) { re += t.fir[n] * std::cos(kTwoPi * hz * n / 192000.0); im -= t.fir[n] * std::sin(kTwoPi * hz * n / 192000.0); }
        return db(std::sqrt(re * re + im * im));
    };
    double ripple = 0.0, stop = -400.0;
    for (double hz = 0.0; hz <= 19500.0; hz += 250.0) ripple = std::max(ripple, std::fabs(magDb(hz)));
    for (double hz = 28500.0; hz <= 96000.0; hz += 125.0) stop = std::max(stop, magDb(hz));
    CHECK_MSG(ripple < 0.01, "pass band ripple %.4f dB", ripple);
    CHECK_MSG(stop < -80.0, "stop band %.1f dB", stop);
    CHECK_NEAR(magDb(24000.0), -6.02, 0.1);
    FIG("oversampler: within %.4f dB to 19.5 kHz, %.2f dB at 20 kHz, %.1f dB or lower from 28.5 kHz", ripple, magDb(20000.0), stop);
    // up and straight back down: the same signal 32 samples later
    dyn::Over4 os;
    const auto in = tone(1000, 0.1, 0.5f);
    double worst = 0.0;
    for (size_t i = 0; i < in.l.size(); ++i) {
        float u[4];
        os.up(in.l[i], u);
        const float y = os.down(u), d = os.delay(in.l[i]);
        if (i >= 400) worst = std::max(worst, (double)std::fabs(y - d));
        if (i >= 32) CHECK(d == in.l[i - 32]);
    }
    CHECK_MSG(worst < 1e-4, "round trip differs by %g", worst);
}

// ------------------------------------------------------------------ Curb

TEST(curb_never_exceeds_its_threshold) {
    auto in = noise(0.5, 2.0f);
    for (size_t i = 4000; i < 9000; ++i) in.l[i] = in.r[i] = (float)std::sin(i * 0.3) * 1.5f;
    for (size_t i = 12000; i < 16000; ++i) in.l[i] = in.r[i] = (float)std::sin(i * 0.004) * 3.0f;
    in.l[20000] = 40.0f; in.r[20011] = -60.0f;
    for (float thresh : {-12.0f, -3.0f, 0.0f})
        for (float knee : {0.0f, 0.3f, 1.0f})
            for (float over : {1.0f, 0.0f})
                for (float drive : {0.0f, 18.0f}) {
                    Curb cb;
                    cb.prepare(kSr);
                    cb.pThresh->set(thresh); cb.pKnee->set(knee); cb.pOver->set(over); cb.pDrive->set(drive);
                    auto out = run(cb, in);
                    const float limit = dbToGain(thresh);
                    CHECK(allFinite(out));
                    CHECK_MSG(peakOf(out.l) <= limit && peakOf(out.r) <= limit, "thr %.0f knee %.1f over %.0f drive %.0f: peak %.7f over %.7f", thresh, knee, over, drive, std::max(peakOf(out.l), peakOf(out.r)), limit);
                    CHECK(peakOf(out.l) > 0.9f * limit);
                    CHECK(cb.meter(0) >= 0.0f);
                }
    // the meter shows by how much the input was over
    Curb cb;
    cb.prepare(kSr);
    cb.pThresh->set(-6.0f);
    run(cb, tone(500, 0.2, 1.0f));
    CHECK_NEAR(cb.meter(0), 6.0, 0.1);
}

TEST(curb_is_transparent_below_the_knee_and_reports_its_latency) {
    CHECK(Curb().latencySamples() == 48);
    for (double sr : {44100.0, 96000.0}) { Curb c; c.prepare(sr); CHECK(c.latencySamples() == 32 + (int)std::lround(sr / 3000.0)); }
    for (float over : {1.0f, 0.0f}) {
        Curb cb;
        cb.prepare(kSr);
        cb.pThresh->set(-6.0f); cb.pKnee->set(0.5f); cb.pOver->set(over);      // the knee starts at -12 dBFS (0.25)
        const int lat = cb.latencySamples();
        auto in = tone(1000, 0.3, 0.2f);
        addTo(in, tone(5000, 0.3, 0.03f));
        addTo(in, noise(0.3, 0.004f));
        auto out = run(cb, in);
        CHECK_MSG(maxDiffDelayed(out.l, in.l, lat, 0) == 0.0 && maxDiffDelayed(out.r, in.r, lat, 0) == 0.0, "differs by %g", maxDiffDelayed(out.l, in.l, lat, 0));
        CHECK(peakOf(out.l, 0, (size_t)lat) == 0.0f);
        CHECK(cb.meter(0) == 0.0f);
    }
}

TEST(curb_static_curve_and_knee) {
    // the curve itself
    const float T = 0.5f;
    for (float knee : {0.0f, 0.2f, 0.5f, 1.0f}) {
        float last = -1.0f;
        for (int i = 0; i <= 4000; ++i) {
            const float x = (float)i * 0.001f, y = Curb::shape(x, T, knee);
            CHECK(y <= T && y >= last - 1e-7f);                 // never over the threshold, never falls
            CHECK(std::fabs(Curb::shape(-x, T, knee) + y) < 1e-7f);
            if (x <= T * (1.0f - knee)) CHECK(y == x);          // a straight line up to the knee
            if (i > 0) CHECK(y - last <= 0.001f + 1e-6f);       // no jump, no slope above 1
            last = y;
        }
        CHECK(Curb::shape(100.0f, T, knee) > 0.999f * T);
    }
    CHECK(Curb::shape(0.7f, T, 0.0f) == T);                     // hard: flat from the threshold on
    CHECK_NEAR(Curb::shape(T, T, 1.0f), T * std::tanh(1.0f), 1e-6);     // fully soft: tanh
    CHECK(Curb::shape(T, T, 0.5f) < Curb::shape(T, T, 0.2f));   // a softer knee starts earlier and turns more gently

    // through the effect: a slow tone comes out as the curve says, sample for sample
    for (float knee : {0.0f, 0.5f, 1.0f}) {
        const auto in = tone(100, 0.3, 1.0f);
        Curb plain, fine;
        plain.prepare(kSr); fine.prepare(kSr);
        for (Curb* c : {&plain, &fine}) { c->pThresh->set(-6.0f); c->pKnee->set(knee); }
        plain.pOver->set(0.0f);
        const auto a = run(plain, in), b = run(fine, in);
        const float thr = dbToGain(-6.0f);
        double worstPlain = 0.0, worstFine = 0.0;
        for (size_t i = 2000; i < a.l.size(); ++i) {
            const float want = Curb::shape(in.l[i - 48], thr, knee);
            worstPlain = std::max(worstPlain, (double)std::fabs(a.l[i] - want));
            worstFine = std::max(worstFine, (double)std::fabs(b.l[i] - want));
        }
        CHECK_MSG(worstPlain < 1e-6, "knee %.1f without oversampling: off the curve by %g", knee, worstPlain);
        CHECK_MSG(worstFine < (knee == 0.0f ? 0.01 : 0.004), "knee %.1f oversampled: off the curve by %g", knee, worstFine);
        CHECK_NEAR(peakOf(b.l, 2000), Curb::shape(1.0f, thr, knee), 0.002);
    }
}

TEST(curb_drive_output_and_level_match) {
    auto peakFor = [](float drive, float out, bool match, float amp) {
        Curb cb;
        cb.prepare(kSr);
        cb.pThresh->set(-6.0f); cb.pKnee->set(0.0f); cb.pDrive->set(drive); cb.pOut->set(out); cb.pMatch->set(match ? 1.0f : 0.0f);
        return (double)peakOf(run(cb, tone(200, 0.3, amp)).l, 4800);
    };
    CHECK_NEAR(db(peakFor(12, 0, false, 0.01f) / 0.01), 12.0, 0.01);       // drive is gain in front
    CHECK_NEAR(db(peakFor(12, -5, false, 0.01f) / 0.01), 7.0, 0.01);       // output gain is gain behind
    CHECK_NEAR(db(peakFor(12, 0, true, 0.01f) / 0.01), 0.0, 0.01);         // matched: what is not clipped keeps its level
    CHECK_NEAR(db(peakFor(12, 0, false, 0.5f)), -6.0, 0.01);               // driven into the threshold
    CHECK_NEAR(db(peakFor(12, 0, true, 0.5f)), -18.0, 0.01);               // matched: the clipped peaks come down by the drive
    CHECK_NEAR(db(peakFor(12, 3, false, 0.5f)), -3.0, 0.01);
}

TEST(curb_oversampling_rejects_aliases) {
    // A tone whose period fits the analysis block exactly: harmonics land on their own bins and everything else is an alias.
    const int n = 16384, k = 427;                   // 1251 Hz at 48 kHz
    const double hz = (double)k * kSr / n;
    auto aliasDb = [&](const std::vector<float>& x) {
        std::vector<float> re(x.end() - n, x.end()), im((size_t)n, 0.0f);
        dsp::fft(re.data(), im.data(), n);
        auto pw = [&](int b) { return (double)re[(size_t)b] * re[(size_t)b] + (double)im[(size_t)b] * im[(size_t)b]; };
        double total = 0.0, harmonic = 0.0;
        for (int b = 1; b < n / 2; ++b) total += pw(b);
        for (int h = 1; h * k < n / 2; ++h) harmonic += pw(h * k);
        return 10.0 * std::log10(std::max(total - harmonic, 1e-30) / pw(k));
    };
    struct Case { float knee, drive; double least; } cases[] = {{1.0f, 12.0f, 22.0}, {0.5f, 12.0f, 20.0}, {0.2f, 12.0f, 16.0}, {0.0f, 12.0f, 15.0}};
    for (auto& cs : cases) {
        Curb fine, plain;
        fine.prepare(kSr); plain.prepare(kSr);
        for (Curb* c : {&fine, &plain}) { c->pThresh->set(-6.0f); c->pKnee->set(cs.knee); c->pDrive->set(cs.drive); }
        plain.pOver->set(0.0f);
        const auto in = tone(hz, 1.0, 0.5f);
        const double a = aliasDb(run(fine, in).l), b = aliasDb(run(plain, in).l);
        FIG("Curb alias energy, 1251 Hz driven 12 dB over, knee %3.0f %%: %.1f dB oversampled, %.1f dB without", cs.knee * 100.0, a, b);
        CHECK_MSG(a < b - cs.least, "knee %.1f: aliases %.1f dB with oversampling, %.1f dB without", cs.knee, a, b);
    }
    // A 9 kHz tone: its fifth harmonic (45 kHz) folds back to 3 kHz.
    for (float knee : {1.0f, 0.3f}) {
        Curb fine, plain;
        fine.prepare(kSr); plain.prepare(kSr);
        for (Curb* c : {&fine, &plain}) { c->pThresh->set(-6.0f); c->pKnee->set(knee); c->pDrive->set(6.0f); }
        plain.pOver->set(0.0f);
        const auto in = tone(9000, 0.5, 0.5f);
        const auto a = run(fine, in), b = run(plain, in);
        const double ours = levelDbAt(a.l, 8000, 8192, 3000.0) - levelDbAt(a.l, 8000, 8192, 9000.0);
        const double naive = levelDbAt(b.l, 8000, 8192, 3000.0) - levelDbAt(b.l, 8000, 8192, 9000.0);
        FIG("Curb 9 kHz driven 6 dB over, knee %3.0f %%: fold-back at 3 kHz %.1f dB oversampled, %.1f dB without", knee * 100.0, ours, naive);
        CHECK_MSG(ours < naive - 30.0 && ours < -60.0, "knee %.1f: %.1f dB against %.1f dB", knee, ours, naive);
    }
}
