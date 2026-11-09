// Sibilant (de-esser) and Doorman (gate / expander) against known responses.
#include "DynTestUtil.h"
using namespace pulp;
using namespace tu;
using namespace dt;

namespace {
const double kSr = 48000.0;

// Noise that lives only between 6 and 8 kHz: 80 sines with fixed pseudo-random phases, faded in and out over 5 ms.
Stereo essBurst(double seconds, double from, double to, float rms) {
    Stereo s = silence(seconds);
    dsp::Noise nz(321u);
    const int count = 80;
    double phase[80];
    for (double& p : phase) p = kPi * (1.0 + nz.next());
    const float each = rms * (float)std::sqrt(2.0 / count);
    for (size_t i = (size_t)(from * kSr); i < (size_t)(to * kSr); ++i) {
        const double t = (double)i / kSr;
        const double edge = std::min(t - from, to - t) / 0.005;
        const double fade = edge >= 1.0 ? 1.0 : 0.5 - 0.5 * std::cos(kPi * edge);
        double v = 0.0;
        for (int k = 0; k < count; ++k) v += std::sin(kTwoPi * (6000.0 + 25.0 * k + 12.5) * t + phase[k]);
        s.l[i] = s.r[i] = (float)(each * fade * v);
    }
    return s;
}
}

TEST(sibilant_takes_the_range_off_a_burst_and_leaves_a_tone_alone) {
    // A 500 Hz tone all the way through, with a burst of 6 to 8 kHz noise from 0.5 to 0.9 s.
    Stereo in = fadeIn(tone(500, 2.4, 0.25f), 20.0);
    const Stereo burst = essBurst(2.4, 0.5, 0.9, 0.1f);
    addTo(in, burst);
    const size_t before = 4800, during = (size_t)(0.6 * kSr), after = (size_t)(2.2 * kSr);
    for (int mode = 0; mode < 2; ++mode) {
        Sibilant ds;
        ds.prepare(kSr);
        ds.pFreq->set(6928.0f); ds.pWidth->set(2.5f); ds.pThresh->set(-38.0f); ds.pRange->set(10.0f); ds.pMode->set((float)mode);
        CHECK(ds.latencySamples() == 0);
        const auto out = run(ds, in);
        // the tone alone, before the burst and well after it: not one bit different
        CHECK(std::equal(out.l.begin() + (long)before, out.l.begin() + (long)(0.49 * kSr), in.l.begin() + (long)before));
        CHECK(std::equal(out.l.begin() + (long)after, out.l.end(), in.l.begin() + (long)after));
        // the burst comes down by about the range
        const double cut = 10.0 * std::log10(bandPower(in.l, during, 8192, 5900, 8100) / bandPower(out.l, during, 8192, 5900, 8100));
        const double toneChange = levelDbAt(out.l, during, 8192, 500.0) - levelDbAt(in.l, during, 8192, 500.0);
        FIG("Sibilant %s, range 10 dB: burst down %.2f dB, 500 Hz tone %+.2f dB during the burst", mode == 0 ? "band only" : "whole signal", cut, toneChange);
        if (mode == 0) {
            CHECK_MSG(cut > 8.5 && cut < 10.3, "band only: burst reduced by %.2f dB", cut);
            CHECK_MSG(std::fabs(toneChange) < 0.3, "band only: tone moved %.2f dB", toneChange);      // only the band is turned down
        } else {
            CHECK_MSG(std::fabs(cut - 10.0) < 0.5, "whole signal: burst reduced by %.2f dB", cut);
            CHECK_MSG(std::fabs(toneChange + 10.0) < 0.5, "whole signal: tone moved %.2f dB", toneChange);      // everything is
        }
    }
    // at the centre of the band the split mode takes off the range exactly
    for (float range : {4.0f, 12.0f, 24.0f}) {
        Sibilant ds;
        ds.prepare(kSr);
        ds.pFreq->set(7000.0f); ds.pWidth->set(1.0f); ds.pThresh->set(-50.0f); ds.pRange->set(range);
        const auto out = run(ds, tone(7000, 0.5, 0.25f));
        CHECK_NEAR(db(rmsOf(out.l, 12000) / (0.25 / std::sqrt(2.0))), -range, 0.2);
        CHECK_NEAR(ds.meter(0), range, 0.05);
    }
    // range 0 does nothing at all
    Sibilant off;
    off.prepare(kSr);
    off.pRange->set(0.0f); off.pThresh->set(-60.0f);
    CHECK(run(off, in).l == in.l);
}

TEST(sibilant_threshold_band_and_listen) {
    // whole-signal mode on a steady 7 kHz tone: what is over the threshold comes off, up to the range
    auto cutFor = [](float inDb, float thresh, float range) {
        Sibilant ds;
        ds.prepare(kSr);
        ds.pFreq->set(7000.0f); ds.pWidth->set(1.5f); ds.pThresh->set(thresh); ds.pRange->set(range); ds.pMode->set(1.0f);
        const float amp = dbToGain(inDb);
        auto out = run(ds, tone(7000, 0.5, amp));
        return -db(rmsOf(out.l, 12000) / (amp / std::sqrt(2.0)));
    };
    CHECK_NEAR(cutFor(-20, -26, 12), 6.0, 0.5);         // 6 dB over
    CHECK_NEAR(cutFor(-10, -26, 12), 12.0, 0.3);        // 16 dB over: stops at the range
    CHECK_NEAR(cutFor(-40, -26, 12), 0.0, 1e-5);        // under: untouched
    CHECK_NEAR(cutFor(-26, -26, 12), 0.75, 0.4);        // on the threshold, half way into the 6 dB knee
    // only the band triggers it: equally loud tones outside the band pass
    for (double hz : {200.0, 1000.0, 1500.0}) {
        Sibilant ds;
        ds.prepare(kSr);
        ds.pFreq->set(7000.0f); ds.pWidth->set(1.0f); ds.pThresh->set(-40.0f); ds.pRange->set(12.0f); ds.pMode->set(1.0f);
        const auto in = fadeIn(tone(hz, 0.4, 0.5f), 20.0);
        const auto out = run(ds, in);
        CHECK_MSG(std::equal(out.l.begin() + 4800, out.l.end(), in.l.begin() + 4800), "%.0f Hz is turned down", hz);
        CHECK(ds.meter(0) == 0.0f);
    }
    // moving the band moves what it hears
    Sibilant lowBand;
    lowBand.prepare(kSr);
    lowBand.pFreq->set(3000.0f); lowBand.pWidth->set(1.0f); lowBand.pThresh->set(-20.0f); lowBand.pRange->set(12.0f); lowBand.pMode->set(1.0f);
    CHECK(rmsOf(run(lowBand, fadeIn(tone(3000, 0.4, 0.5f), 20.0)).l, 9600) < 0.3 * 0.5 / std::sqrt(2.0));
    lowBand.reset();
    CHECK(rmsOf(run(lowBand, fadeIn(tone(9000, 0.4, 0.5f), 20.0)).l, 9600) > 0.999 * 0.5 / std::sqrt(2.0));

    // listen: the detection band comes out, 24 dB per octave down either side
    auto heard = [](double hz) {
        Sibilant ds;
        ds.prepare(kSr);
        ds.pFreq->set(6000.0f); ds.pWidth->set(1.0f); ds.pListen->set(1.0f); ds.pThresh->set(0.0f);
        auto out = run(ds, tone(hz, 0.4, 0.25f));
        return db(rmsOf(out.l, 9600) / (0.25 / std::sqrt(2.0)));
    };
    CHECK_NEAR(heard(6000.0), -0.5, 0.3);                       // both edges are half an octave away
    CHECK_NEAR(heard(6000.0 / std::sqrt(2.0)), -3.0, 0.3);      // the lower edge of a one-octave band
    CHECK_NEAR(heard(6000.0 * std::sqrt(2.0)), -3.0, 0.5);
    CHECK(heard(1000.0) < -45.0 && heard(500.0) < -70.0);
    CHECK(heard(18000.0) < -20.0);
    FIG("Sibilant detector, 6 kHz one octave wide: %.2f dB at 6 kHz, %.1f dB at 1 kHz, %.1f dB at 500 Hz, %.1f dB at 18 kHz", heard(6000), heard(1000), heard(500), heard(18000));
}

TEST(sibilant_release_time_constant) {
    // a 7 kHz tone that stops: the reduction decays with the release time (whole-signal mode, read from a quiet pilot tone)
    Sibilant ds;
    ds.prepare(kSr);
    ds.pFreq->set(7000.0f); ds.pWidth->set(1.0f); ds.pThresh->set(-40.0f); ds.pRange->set(12.0f); ds.pMode->set(1.0f); ds.pRelease->set(100.0f);
    Stereo in = level(1.0, 0.1f);                // DC is far outside the band: it only carries the gain
    const Stereo ess = tone(7000, 0.3, 0.3f);
    for (size_t i = 0; i < ess.l.size(); ++i) { in.l[i] += ess.l[i]; in.r[i] += ess.r[i]; }
    const auto out = run(ds, in);
    const size_t stop = ess.l.size();
    auto cutAt = [&](size_t n) { return -db(out.l[n] / 0.1); };
    // the band's own filter rings out for a moment after the tone stops, so measure from 20 ms on
    const size_t t0 = stop + 960;
    const double c0 = cutAt(t0);
    CHECK(c0 > 8.0);
    CHECK_NEAR(cutAt(t0 + 4800) / c0, std::exp(-1.0), 0.03);
    CHECK_NEAR(cutAt(t0 + 9600) / c0, std::exp(-2.0), 0.03);
    FIG("Sibilant release 100 ms: %.3f of the reduction left after 100 ms (ideal 0.368)", cutAt(t0 + 4800) / c0);
}

// ------------------------------------------------------------------ Doorman

namespace {
void setGate(Doorman& g, float thresh, float hyst, float range, float attack, float hold, float release, float look) {
    g.pThresh->set(thresh); g.pHyst->set(hyst); g.pRange->set(range); g.pAttack->set(attack); g.pHold->set(hold); g.pRelease->set(release); g.pLook->set(look);
}
}

TEST(doorman_closes_by_its_range_below_and_passes_above) {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        Doorman g;
        g.prepare(sr);
        CHECK(g.latencySamples() == (int)std::lround(0.005 * sr));
    }
    for (float range : {12.0f, 30.0f, 80.0f}) {
        Doorman g;
        g.prepare(kSr);
        setGate(g, -40, 3, range, 1, 50, 100, 2);
        const int lat = g.latencySamples();
        CHECK(lat == 240);
        // below the threshold: down by the range
        const auto low = tone(1000, 0.5, dbToGain(-50.0f));
        auto out = run(g, low);
        CHECK_NEAR(db(peakOf(out.l, 12000) / dbToGain(-50.0f)), -range, 0.01);
        CHECK_NEAR(g.meter(0), range, 0.01);
        CHECK(g.meter(1) == 0.0f);
        // above it: the input itself, late by the latency and nothing else
        g.reset();
        const auto loud = tone(1000, 0.5, dbToGain(-20.0f));
        out = run(g, loud);
        CHECK_MSG(maxDiffDelayed(out.l, loud.l, lat, 2400) == 0.0, "open gate differs by %g", maxDiffDelayed(out.l, loud.l, lat, 2400));
        CHECK(g.meter(0) == 0.0f && g.meter(1) == 1.0f);
    }
    // range 0: nothing happens whatever the level
    Doorman g;
    setGate(g, -20, 3, 0, 1, 50, 100, 2);
    g.prepare(kSr);
    const auto in = noise(0.3, 0.01f);
    CHECK(maxDiffDelayed(run(g, in).l, in.l, 240, 0) == 0.0);
}

TEST(doorman_attack_hold_and_release_from_steps) {
    // DC steps with the detector filters off: the gain is the output divided by the input.
    const size_t n1 = 4800, n2 = n1 + 14400;
    Stereo in = level(0.1, 0.001f);          // -60 dB: closed
    append(in, level(0.3, 0.1f));            // -20 dB: opens
    append(in, level(1.2, 0.001f));          // and closes again
    size_t closeAt[2] = {0, 0};
    int which = 0;
    for (float hold : {20.0f, 200.0f}) {
        Doorman g;
        g.prepare(kSr);
        setGate(g, -40, 6, 40, 5, hold, 100, 0);
        const size_t lat = (size_t)g.latencySamples();
        const auto out = run(g, in);
        auto gain = [&](size_t n) { return (double)out.l[n + lat] / (double)in.l[n]; };
        CHECK_NEAR(db(gain(n1 - 1)), -40.0, 0.01);
        // attack 5 ms: the amplitude covers 63 % of the way in 5 ms and 95 % in 15 ms
        CHECK_NEAR((gain(n1 + 240) - 0.01) / 0.99, 1.0 - std::exp(-1.0), 0.01);
        CHECK_NEAR((gain(n1 + 720) - 0.01) / 0.99, 1.0 - std::exp(-3.0), 0.01);
        CHECK(gain(n1 + 4800) == 1.0);
        // after the level drops the detector takes 3 ms * ln(0.1 / 0.005) = 9 ms to fall under the closing threshold;
        // then the hold time passes with the gate fully open, and only then does it start to close
        const size_t detector = (size_t)std::ceil(0.003 * kSr * std::log(0.1 / dbToGain(-46.0f)));
        const size_t holdN = (size_t)std::lround(0.001 * hold * kSr);
        size_t first = 0;
        for (size_t n = n2; n < in.l.size() - lat - 64; ++n) if (gain(n) < 1.0) { first = n; break; }
        closeAt[which++] = first;
        CHECK_MSG(std::fabs((double)first - (double)(n2 + detector + holdN)) <= 3.0, "hold %.0f ms: starts closing %d samples after the drop, expected %d", hold, (int)(first - n2), (int)(detector + holdN));
        // release 100 ms: the level in dB covers 63 % of the range in 100 ms, 86 % in 200 ms
        CHECK_NEAR(db(gain(first + 4800)), -40.0 * (1.0 - std::exp(-1.0)), 0.2);
        CHECK_NEAR(db(gain(first + 9600)), -40.0 * (1.0 - std::exp(-2.0)), 0.2);
        FIG("Doorman hold %.0f ms: closing starts %.2f ms after the drop (detector %.2f ms + hold); attack 5 ms: %.3f at 5 ms; release 100 ms: %.2f dB at 100 ms",
            hold, (double)(first - n2) * 1000.0 / kSr, (double)detector * 1000.0 / kSr, (gain(n1 + 240) - 0.01) / 0.99, db(gain(first + 4800)));
    }
    CHECK(closeAt[1] - closeAt[0] == 8640);          // 180 ms more hold is exactly 180 ms later
}

TEST(doorman_hysteresis) {
    Doorman g;
    g.prepare(kSr);
    setGate(g, -40, 6, 60, 1, 10, 20, 0);
    const size_t lat = (size_t)g.latencySamples();
    Stereo in = level(0.2, dbToGain(-60.0f));
    append(in, level(0.2, dbToGain(-42.0f)));    // under the opening threshold: stays closed
    append(in, level(0.2, dbToGain(-38.0f)));    // over it: opens
    append(in, level(0.4, dbToGain(-43.0f)));    // between the two thresholds: stays open
    append(in, level(0.4, dbToGain(-48.0f)));    // under the closing threshold (-46): closes
    const auto out = run(g, in);
    auto gainDb = [&](double t) { const size_t n = (size_t)(t * kSr); return db(out.l[n + lat] / in.l[n]); };
    CHECK_NEAR(gainDb(0.19), -60.0, 0.01);
    CHECK_NEAR(gainDb(0.39), -60.0, 0.01);
    CHECK_NEAR(gainDb(0.59), 0.0, 1e-9);
    CHECK_NEAR(gainDb(0.99), 0.0, 1e-9);
    CHECK_NEAR(gainDb(1.39), -60.0, 0.01);
    // without hysteresis the level between the thresholds would have closed it
    g.reset();
    g.pHyst->set(0.0f);
    const auto out0 = run(g, in);
    CHECK(db(out0.l[(size_t)(0.99 * kSr) + lat] / in.l[(size_t)(0.99 * kSr)]) < -59.0);
}

TEST(doorman_look_ahead_opens_before_the_sound) {
    // A tone that starts out of silence. Without look-ahead the attack eats its first cycle; with it the gate is open in time.
    Stereo in = silence(0.1);
    append(in, tone(1000, 0.2, 0.25f));
    const size_t n1 = 4800;
    double kept[3] = {0, 0, 0};
    int k = 0;
    for (float look : {0.0f, 2.0f, 5.0f}) {
        Doorman g;
        g.prepare(kSr);
        setGate(g, -40, 3, 80, 1, 50, 100, look);
        const size_t lat = (size_t)g.latencySamples();
        CHECK(lat == 240);                              // the latency does not move with the control
        const auto out = run(g, in);
        kept[k++] = rmsOf(out.l, n1 + lat, n1 + lat + 48) / rmsOf(in.l, n1, n1 + 48);       // the first millisecond
        // once open, the output is the input 240 samples late whatever the look-ahead
        CHECK(maxDiffDelayed(out.l, in.l, (int)lat, n1 + lat + 2400) == 0.0);
        CHECK(peakOf(out.l, 0, n1 + lat - 1) == 0.0f);
    }
    FIG("Doorman, attack 1 ms: first millisecond of a tone kept %.3f with no look-ahead, %.3f with 2 ms, %.3f with 5 ms", kept[0], kept[1], kept[2]);
    CHECK(kept[0] < 0.6);
    CHECK(kept[1] > 0.85 && kept[1] > kept[0] + 0.3);
    CHECK(kept[2] > 0.99);
}

TEST(doorman_sidechain_and_detector_filters) {
    CHECK(Doorman().wantsSidechain());
    // a quiet signal, under the threshold on its own; the sidechain is loud for the first half
    const auto quiet = tone(500, 1.0, dbToGain(-50.0f));
    Stereo side = tone(100, 1.0, 0.5f);
    for (size_t i = side.l.size() / 2; i < side.l.size(); ++i) side.l[i] = side.r[i] = 0.0f;
    Doorman g;
    g.prepare(kSr);
    setGate(g, -30, 3, 60, 1, 20, 50, 2);
    g.pSide->set(1.0f);
    auto out = run(g, quiet, kSr, &side);
    CHECK_NEAR(db(rmsOf(out.l, 4800, 20000) / rmsOf(quiet.l, 4800, 20000)), 0.0, 0.01);        // keyed open
    CHECK_NEAR(db(rmsOf(out.l, 40000, 47000) / rmsOf(quiet.l, 40000, 47000)), -60.0, 0.1);     // and shut when the key stops
    // the switch off, or nothing routed to the sidechain: it listens to its own input
    g.reset();
    g.pSide->set(0.0f);
    out = run(g, quiet, kSr, &side);
    CHECK_NEAR(db(rmsOf(out.l, 4800, 20000) / rmsOf(quiet.l, 4800, 20000)), -60.0, 0.1);
    g.reset();
    g.pSide->set(1.0f);
    out = run(g, quiet);
    CHECK_NEAR(db(rmsOf(out.l, 4800, 20000) / rmsOf(quiet.l, 4800, 20000)), -60.0, 0.1);

    // detector low-pass at 150 Hz: a loud 8 kHz tone no longer opens the gate, a 60 Hz tone still does
    auto opens = [](double hz, float hp, float lp) {
        Doorman f;
        f.prepare(kSr);
        setGate(f, -30, 3, 60, 1, 20, 50, 2);
        f.pScHp->set(hp); f.pScLp->set(lp);
        const auto in = tone(hz, 0.5, 0.5f);
        const auto o = run(f, in);
        return rmsOf(o.l, 12000) > 0.5 * rmsOf(in.l, 12000);
    };
    CHECK(opens(8000, 20, 20000) && opens(60, 20, 20000));
    CHECK(!opens(8000, 20, 150) && opens(60, 20, 150));
    CHECK(opens(8000, 2000, 20000) && !opens(60, 2000, 20000));         // and the high-pass the other way round
    // the filters are 12 dB per octave: listen to the detector and measure
    auto heard = [](double hz, float hp, float lp) {
        Doorman f;
        f.prepare(kSr);
        f.pScHp->set(hp); f.pScLp->set(lp); f.pListen->set(1.0f);
        const auto o = run(f, tone(hz, 0.5, 0.25f));
        return db(rmsOf(o.l, 12000) / (0.25 / std::sqrt(2.0)));
    };
    CHECK_NEAR(heard(1000, 1000, 20000), -3.0, 0.1);
    CHECK_NEAR(heard(250, 1000, 20000), -24.1, 0.5);
    CHECK_NEAR(heard(1000, 20, 1000), -3.0, 0.1);
    CHECK_NEAR(heard(4000, 20, 1000), -24.6, 0.7);
    CHECK_NEAR(heard(1000, 20, 20000), 0.0, 0.01);
}

TEST(doorman_opens_and_closes_without_clicks) {
    // A 1 kHz tone whose level swells and fades across the threshold, smoothly: the gate must not add an edge.
    const double total = 2.0;
    Stereo in = silence(total);
    for (size_t i = 0; i < in.l.size(); ++i) {
        const double t = (double)i / kSr;
        const double swell = 0.5 - 0.5 * std::cos(kTwoPi * 2.0 * t);                       // 0..1 twice a second
        const double amp = dbToGain(-60.0f) + (dbToGain(-12.0f) - dbToGain(-60.0f)) * swell * swell;
        in.l[i] = in.r[i] = (float)(amp * std::sin(kTwoPi * 1000.0 * t));
    }
    const float inStep = maxStep(in.l);
    for (float look : {0.0f, 3.0f})
        for (float attack : {0.2f, 2.0f, 20.0f}) {
            Doorman g;
            g.prepare(kSr);
            setGate(g, -30, 4, 80, attack, 10, 60, look);
            const auto out = run(g, in);
            // no sample-to-sample step larger than the steepest slope of the tone itself
            CHECK_MSG(maxStep(out.l) <= inStep, "look %.0f attack %.1f: step %.5f, the input's steepest %.5f", look, attack, maxStep(out.l), inStep);
            // and the gate really did open and close four times
            CHECK(peakOf(out.l) > 0.9f * peakOf(in.l));
            int closings = 0;
            bool wasOpen = false;
            for (size_t i = 240; i + 480 < out.l.size(); i += 480) {
                const bool open = rmsOf(out.l, i, i + 480) > 0.5 * rmsOf(in.l, i - 240, i + 240);
                if (wasOpen && !open) ++closings;
                wasOpen = open;
            }
            CHECK_MSG(closings == 4, "closed %d times", closings);
        }
    // the gain itself never jumps: on DC the largest step is the attack's first one
    Doorman g;
    g.prepare(kSr);
    setGate(g, -40, 6, 80, 1, 5, 30, 0);
    Stereo steps = level(0.05, 0.001f);
    for (int r = 0; r < 5; ++r) { append(steps, level(0.05, 0.5f)); append(steps, level(0.15, 0.001f)); }
    const auto o = run(g, steps);
    const size_t lat = (size_t)g.latencySamples();
    double worst = 0.0;
    for (size_t n = 1; n + lat < o.l.size(); ++n) worst = std::max(worst, std::fabs((double)o.l[n + lat] / steps.l[n] - (double)o.l[n + lat - 1] / steps.l[n - 1]));
    const double firstStep = 1.0 - std::exp(-1.0 / (0.001 * kSr));
    CHECK_MSG(worst <= firstStep * 1.001, "largest gain step %.5f, expected at most %.5f", worst, firstStep);
    FIG("Doorman attack 1 ms: largest gain step per sample %.5f (the attack's first step is %.5f)", worst, firstStep);
}
