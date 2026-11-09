// Platter: a record to grab and scratch.
#include "CreativeTest.h"
using namespace pulp;
using namespace ct;

namespace {
const double kSr = 48000.0;
size_t quantumAt(double seconds) { return (size_t)std::llround(seconds * kSr / kQuantum); }
size_t sampleAt(double seconds) { return quantumAt(seconds) * kQuantum; }
}

TEST(platter_not_held_is_the_input_bit_for_bit) {
    const auto in = busy(2.0);
    Platter p;
    p.prepare(kSr);
    CHECK(p.latencySamples() == 0);
    dsp::Noise rnd(3u);
    // every control but Hold thrown about: nothing may change
    const auto out = run(p, in, {}, [&](size_t q) {
        if (q % 5 == 0)
            for (auto& prm : p.params()) if (prm.get() != p.pHold) prm->setNorm(rnd.uni());
    });
    CHECK(same(out, in));
    CHECK(p.meter(2) == 0.0f && p.meter(0) == 0.0f);
}

TEST(platter_held_with_the_record_turning_carries_on_seamlessly) {
    const auto in = busy(3.0);
    Platter p;
    p.prepare(kSr);
    // speed 1, position still: grabbed at 0.5 s, let go at 2 s. The sound never leaves the input, to the bit,
    // through the grab, the hold and the release
    const auto out = run(p, in, {}, [&](size_t q) {
        if (q == quantumAt(0.5)) p.pHold->set(1.0f);
        if (q == quantumAt(2.0)) p.pHold->set(0.0f);
        if (q == quantumAt(1.0)) { CHECK(p.meter(2) == 1.0f); CHECK(p.meter(0) == 0.0f); CHECK(p.meter(1) == 1.0f); }
    });
    CHECK(same(out, in));
    // the same when it starts out held
    Platter h;
    h.prepare(kSr);
    h.pHold->set(1.0f);
    CHECK(same(run(h, in), in));
}

TEST(platter_speed_zero_brakes_to_silence_and_starts_again) {
    const double f = 600.0, brake = 0.4, start = 0.15;
    const auto in = tone(f, 4.0, 0.5f);
    Platter p;
    p.prepare(kSr);
    p.pSpeed->set(0.0f); p.pBrake->set((float)(brake * 1000.0)); p.pStart->set((float)(start * 1000.0));
    float behindWhenStopped = 0.0f, speedWhenStopped = 9.0f;
    const auto out = run(p, in, {}, [&](size_t q) {
        if (q == quantumAt(0.5)) p.pHold->set(1.0f);
        if (q == quantumAt(2.0)) { behindWhenStopped = p.meter(0); speedWhenStopped = p.meter(1); p.pSpeed->set(1.0f); }
    });
    const size_t grab = sampleAt(0.5), go = sampleAt(2.0);
    bool exact = true;
    for (size_t i = 0; i < grab; ++i) exact = exact && out.l[i] == in.l[i];
    CHECK(exact);
    // braking: the speed falls in a straight line, so the pitch does: three quarters, a half, a quarter of the way down
    for (double part : {0.25, 0.5, 0.75}) {
        const size_t mid = grab + (size_t)(part * brake * kSr);
        const double hz = tu::measureHz(out.l, mid - 700, mid + 700, kSr);
        CHECK_MSG(std::fabs(hz / (f * (1.0 - part)) - 1.0) < 0.02, "%.0f %% through the brake: %.1f Hz", part * 100.0, hz);
    }
    // sample by sample: the motor loses 1 / (brake time) of its speed every sample from the grab's own sample on, so
    // after i + 1 samples the record is (i + 1)(i + 2) / 2 of those steps behind the input
    double worst = 0.0, worstLater = 0.0;
    for (size_t i = 0; i < (size_t)(0.9 * brake * kSr); ++i) {
        const double behind = 0.5 * (double)(i + 1) * (double)(i + 2) / (brake * kSr);
        const double d = std::fabs((double)out.l[grab + i] - 0.5 * std::sin(kTwoPi * f * ((double)(grab + i) - behind) / kSr));
        worst = std::max(worst, d);
        if (behind > 8.0) worstLater = std::max(worstLater, d);
    }
    // (less than a sample behind, it is read with a straight line between two samples: 4e-4 at 600 Hz)
    CHECK_MSG(worst < 5.0e-4 && worstLater < 2.0e-5, "braking is off its curve by %g (%g once it is eight samples behind)", worst, worstLater);
    // after the brake time: silence, exactly, for as long as it stands
    const size_t stopped = grab + (size_t)(brake * kSr) + 2;
    CHECK_MSG(peak(out, stopped, go) == 0.0f, "stopped, it still gives %g", peak(out, stopped, go));
    CHECK_MSG(peak(out, stopped - 100, stopped) < 0.01f, "just before the stop: %g", peak(out, stopped - 100, stopped));
    CHECK_NEAR(speedWhenStopped, 0.0, 1e-6);
    CHECK_NEAR(behindWhenStopped, 1.5 - 0.5 * brake, 0.002);       // it fell behind by all the time it stood, and half the braking
    // starting again: up to speed after the start time, at the old pitch, and behind the input by what it lost
    const size_t up = go + (size_t)(start * kSr) + 200;
    CHECK_NEAR(tu::measureHz(out.l, up, up + 20000, kSr), f, 0.2);
    CHECK_NEAR(tu::rmsOf(out.l, up, up + 20000), 0.5 / std::sqrt(2.0), 0.002);
    CHECK_NEAR(p.meter(0), 1.5 - 0.5 * brake + 0.5 * start, 0.002);
    CHECK_NEAR(p.meter(1), 1.0, 1e-6);
    const double half = tu::measureHz(out.l, go + (size_t)(0.5 * start * kSr) - 500, go + (size_t)(0.5 * start * kSr) + 500, kSr);
    CHECK_MSG(std::fabs(half / (0.5 * f) - 1.0) < 0.05, "half way through the start: %.1f Hz", half);
    // no clicks anywhere: no step beyond the sine's own
    const float own = 0.5f * (float)(kTwoPi * f / kSr);
    CHECK_MSG(maxStep(out.l) <= own * 1.02f, "largest step %.4f, the sine's own %.4f", maxStep(out.l), own);
    printf("    platter brake: off its curve by %.2e (%.2e once it is eight samples behind); largest step %.4f (the sine alone %.4f)\n", worst, worstLater, maxStep(out.l), own);
}

TEST(platter_position_forward_at_twice_real_time_is_an_octave_up_and_backward_is_reversed) {
    // The input is a sweep rising 100 Hz every second from 300 Hz, so where the needle is can be heard.
    const double f0 = 300.0, slope = 100.0;
    const auto in = chirp(f0, slope, 6.0, 0.5f);
    Platter p;
    p.prepare(kSr);
    p.pSpeed->set(0.0f); p.pBrake->set(5.0f); p.pRange->set(1000.0f); p.pInertia->set(4.0f);
    // grabbed at 1 s with the motor off. Position: still until 2 s; back half the range by 2.5 s (backwards at normal speed);
    // from 3 s forward the whole range in half a second (twice real time); from 4 s back the whole range in a second.
    auto positionAt = [](double t) {
        if (t < 2.0) return 0.0;
        if (t < 2.5) return -(t - 2.0);
        if (t < 3.0) return -0.5;
        if (t < 3.5) return -0.5 + 2.0 * (t - 3.0);
        if (t < 4.0) return 0.5;
        if (t < 5.0) return 0.5 - (t - 4.0);
        return -0.5;
    };
    float speedForward = 0.0f, speedBack = 0.0f, speedStill = 9.0f;
    const auto out = run(p, in, {}, [&](size_t q) {
        const double t = (double)(q * kQuantum) / kSr;
        if (q == quantumAt(1.0)) p.pHold->set(1.0f);
        p.pPosition->set((float)positionAt(t));
        if (q == quantumAt(3.25)) speedForward = p.meter(1);
        if (q == quantumAt(4.5)) speedBack = p.meter(1);
        if (q == quantumAt(3.75)) speedStill = p.meter(1);
    });
    // where the record stopped: the grab, plus the little it turned while braking (half of 5 ms)
    const double G = (double)sampleAt(1.0) / kSr + 0.0025;
    auto sweepHz = [&](double place) { return f0 + slope * place; };
    CHECK_NEAR(speedForward, 2.0, 0.01);
    CHECK_NEAR(speedBack, -1.0, 0.01);
    CHECK_NEAR(speedStill, 0.0, 1e-4);
    // forward at twice real time: at 3.25 s the needle passes the grab point; the pitch is twice the sweep's there
    const double fwd = tu::measureHz(out.l, sampleAt(3.2), sampleAt(3.3), kSr);
    CHECK_MSG(std::fabs(1200.0 * std::log2(fwd / (2.0 * sweepHz(G)))) < 12.0, "forward at 2x: %.1f Hz, the record there is at %.1f Hz", fwd, sweepHz(G));
    // and it rises four times as fast as the sweep does (twice the speed, twice the pitch)
    const double fwdEarly = tu::measureHz(out.l, sampleAt(3.1), sampleAt(3.15), kSr), fwdLate = tu::measureHz(out.l, sampleAt(3.35), sampleAt(3.4), kSr);
    CHECK_NEAR((fwdLate - fwdEarly) / 0.25, 4.0 * slope, 20.0);
    // backward at normal speed: the rising sweep comes out falling, at the sweep's own pitch for the place
    const double backEarly = tu::measureHz(out.l, sampleAt(4.2), sampleAt(4.3), kSr), backLate = tu::measureHz(out.l, sampleAt(4.7), sampleAt(4.8), kSr);
    CHECK_NEAR(backEarly, sweepHz(G + 0.25), 3.0);
    CHECK_NEAR(backLate, sweepHz(G - 0.25), 3.0);
    CHECK(backLate < backEarly - 40.0);
    // the first stroke too: back from the grab point at normal speed
    CHECK_NEAR(tu::measureHz(out.l, sampleAt(2.2), sampleAt(2.3), kSr), sweepHz(G - 0.25), 3.0);
    // full level while it moves, silence while it stands
    CHECK_NEAR(tu::rmsOf(out.l, sampleAt(3.2), sampleAt(3.3)), 0.5 / std::sqrt(2.0), 0.01);
    CHECK_NEAR(tu::rmsOf(out.l, sampleAt(4.2), sampleAt(4.8)), 0.5 / std::sqrt(2.0), 0.01);
    CHECK(peak(out, sampleAt(1.2), sampleAt(1.95)) == 0.0f);
    CHECK(peak(out, sampleAt(3.7), sampleAt(3.95)) < 1.0e-4f);
    // no clicks at the turns: no step beyond that of the fastest thing played (the sweep's top at double speed)
    const float fastest = 0.5f * (float)(kTwoPi * 2.0 * sweepHz(G + 0.5) / kSr);
    CHECK_MSG(maxStep(out.l) <= fastest * 1.05f, "largest step %.4f, bound %.4f", maxStep(out.l), fastest * 1.05f);
    printf("    platter: forward at 2x %.1f Hz where the record is at %.1f Hz; backwards %.1f then %.1f Hz; largest step %.4f\n", fwd, sweepHz(G), backEarly, backLate, maxStep(out.l));

    // read fast, the record is band-limited: 15 kHz played at double speed would be 30 kHz, which a plain read
    // folds back to 18 kHz at nearly full level
    Platter b;
    b.prepare(kSr);
    b.pSpeed->set(0.0f); b.pBrake->set(5.0f); b.pRange->set(1000.0f); b.pInertia->set(4.0f);
    const auto hi = run(b, tone(15000.0, 3.0, 0.5f), {}, [&](size_t q) {
        const double t = (double)(q * kQuantum) / kSr;
        if (q == quantumAt(1.5)) b.pHold->set(1.0f);
        b.pPosition->set((float)(t < 2.0 ? 0.0 : std::min(0.5, 2.0 * (t - 2.0))));
    });
    const double alias = levelDb(hi.l, sampleAt(2.05), 8192, 18000.0);
    printf("    platter: 15 kHz at 0.5 read at double speed leaves %.1f dB at 18 kHz (a plain read: about -6 dB)\n", alias);
    CHECK_MSG(alias < -40.0, "alias at %.1f dB", alias);
    // and at double speed a 5 kHz tone is a clean 10 kHz one
    Platter c;
    c.prepare(kSr);
    c.pSpeed->set(0.0f); c.pBrake->set(5.0f); c.pRange->set(1000.0f); c.pInertia->set(4.0f);
    const auto mid = run(c, tone(5000.0, 3.0, 0.5f), {}, [&](size_t q) {
        const double t = (double)(q * kQuantum) / kSr;
        if (q == quantumAt(1.5)) c.pHold->set(1.0f);
        c.pPosition->set((float)(t < 2.0 ? 0.0 : std::min(0.5, 2.0 * (t - 2.0))));
    });
    CHECK_NEAR(levelDb(mid.l, sampleAt(2.05), 8192, 10000.0), db(0.5), 0.3);
}

TEST(platter_grab_release_and_cut_make_no_clicks) {
    const double f = 500.0;
    const float amp = 0.5f, own = amp * (float)(kTwoPi * f / kSr);
    const auto in = tone(f, 4.0, amp);
    {   // grabbed with the hand already off centre and the motor slowing; let go far from the input
        Platter p;
        p.prepare(kSr);
        p.pSpeed->set(0.3f); p.pBrake->set(200.0f); p.pPosition->set(-0.8f); p.pRange->set(200.0f); p.pInertia->set(100.0f);
        const auto out = run(p, in, {}, [&](size_t q) {
            if (q == quantumAt(0.5)) p.pHold->set(1.0f);
            if (q == quantumAt(1.0)) p.pPosition->set(0.4f);
            if (q == quantumAt(1.5)) p.pHold->set(0.0f);
            if (q == quantumAt(2.0)) p.pHold->set(1.0f);
            if (q == quantumAt(2.004)) p.pHold->set(0.0f);      // let go again before the fade has finished, and grab again in it
            if (q == quantumAt(2.008)) p.pHold->set(1.0f);
            if (q == quantumAt(2.5)) p.pHold->set(0.0f);
        });
        // the needle never moves faster than about three times real time here (the hand's 240 ms throw takes its 100 ms
        // of inertia); a release is a 10 ms crossfade of two signals
        const float bound = 3.2f * own + 2.0f * amp / 480.0f;
        CHECK_MSG(maxStep(out.l) <= bound, "largest step %.4f, bound %.4f", maxStep(out.l), bound);
        // 10 ms after a release it is the input again, exactly
        bool exact = true;
        for (size_t i = sampleAt(1.5) + 481; i < sampleAt(2.0); ++i) exact = exact && out.l[i] == in.l[i];
        for (size_t i = sampleAt(2.5) + 481; i < out.l.size(); ++i) exact = exact && out.l[i] == in.l[i];
        CHECK(exact);
        CHECK(out.l[sampleAt(1.5) + 100] != in.l[sampleAt(1.5) + 100]);
        printf("    platter grab and release: largest step %.4f (the sine alone %.4f, bound %.4f)\n", maxStep(out.l), own, bound);
    }
    {   // the cut: closed it is silent, from a quarter up it is fully open, and it moves in 1.5 ms
        Platter p;
        p.prepare(kSr);
        p.pHold->set(1.0f);
        const auto out = run(p, in, {}, [&](size_t q) {
            if (q == quantumAt(0.5)) p.pCut->set(0.0f);
            if (q == quantumAt(1.0)) p.pCut->set(0.25f);
            if (q == quantumAt(1.5)) p.pCut->set(0.125f);
        });
        bool open = true;
        for (size_t i = 0; i < sampleAt(0.5); ++i) open = open && out.l[i] == in.l[i];
        for (size_t i = sampleAt(1.0) + 73; i < sampleAt(1.5); ++i) open = open && out.l[i] == in.l[i];
        CHECK(open);
        CHECK(peak(out, sampleAt(0.5) + 73, sampleAt(1.0)) == 0.0f);
        CHECK(peak(out, sampleAt(0.5) + 60, sampleAt(0.5) + 73) > 0.0f);
        CHECK_NEAR(tu::rmsOf(out.l, sampleAt(1.6), sampleAt(2.0)), 0.5 * amp / std::sqrt(2.0), 0.002);
        CHECK_MSG(maxStep(out.l) <= own + amp / 72.0f + 1e-5f, "largest step %.4f", maxStep(out.l));
        // grabbing with the cut closed: the cut closes from the grab in its 1.5 ms, it does not chop
        Platter c;
        c.prepare(kSr);
        c.pCut->set(0.0f);
        const auto shut = run(c, in, {}, [&](size_t q) { if (q == quantumAt(0.5)) c.pHold->set(1.0f); });
        CHECK(peak(shut, sampleAt(0.5) + 73) == 0.0f);
        CHECK(shut.l[sampleAt(0.5) + 20] != 0.0f);
        CHECK(maxStep(shut.l) <= own + amp / 72.0f + 1e-5f);
    }
}

TEST(platter_reverse_motor_and_the_ends_of_the_record) {
    const auto in = chirp(300.0, 100.0, 4.0, 0.5f);
    {   // a motor running backwards plays backwards: the sweep falls, at the sweep's own rate
        Platter p;
        p.prepare(kSr);
        p.pSpeed->set(-1.0f); p.pBrake->set(10.0f); p.pStart->set(10.0f);
        const auto out = run(p, in, {}, [&](size_t q) { if (q == quantumAt(2.0)) p.pHold->set(1.0f); });
        const double early = tu::measureHz(out.l, sampleAt(2.2), sampleAt(2.3), kSr), late = tu::measureHz(out.l, sampleAt(2.7), sampleAt(2.8), kSr);
        CHECK_NEAR(early, 300.0 + 100.0 * (2.0 - 0.25 + 0.01), 3.0);
        CHECK_NEAR(late - early, -50.0, 2.0);
        CHECK_NEAR(p.meter(1), -1.0, 1e-6);
    }
    {   // stopped for longer than the record is: the needle is carried along at the far end, and nothing breaks
        Platter p;
        p.prepare(kSr);
        p.pSpeed->set(0.0f); p.pBrake->set(5.0f);
        p.pHold->set(1.0f);
        Stereo longIn = tone(300.0, 14.0, 0.5f);
        const auto out = run(p, longIn);
        CHECK(allFinite(out));
        CHECK(p.meter(0) <= (float)(Platter::kSeconds + 1.0) && p.meter(0) >= (float)Platter::kSeconds - 0.1f);
        // carried along at the end of the record it plays again, at normal speed, about ten seconds late
        CHECK_NEAR(tu::measureHz(out.l, sampleAt(12.5), sampleAt(13.5), kSr), 300.0, 0.5);
        CHECK(peak(out, sampleAt(2.0), sampleAt(9.0)) == 0.0f);
    }
}
