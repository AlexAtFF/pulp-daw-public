// Stack: tone stack, cabinets, voicings, aliasing and timing.
#include "ToneTest.h"
using namespace pulp;
using namespace tu;
using namespace tt;

namespace {
const double kSr = 48000.0;
const float kQuiet = 2.0e-5f;       // too quiet for any stage to bend it, even with the lead voicing's gain

std::shared_ptr<Stack> amp(int voicing, float gain, int cab) {
    auto a = std::make_shared<Stack>();
    a->prepare(kSr);
    a->pVoicing->set((float)voicing); a->pGain->set(gain); a->pCab->set((float)cab); a->pMaster->set(0.0f);
    return a;
}
size_t peakIndex(const std::vector<float>& x) {
    size_t at = 0;
    for (size_t i = 0; i < x.size(); ++i) if (std::fabs(x[i]) > std::fabs(x[at])) at = i;
    return at;
}
double thdDb(const std::vector<float>& x, double hz) {
    double sum = 0.0;
    for (int h = 2; h <= 9; ++h) sum += std::pow(10.0, harmonicDb(x, hz, h) / 10.0);
    return 10.0 * std::log10(sum + 1e-30);
}
// The amplifier as Stack runs it, but at the project rate with the plain curves: no oversampling at all.
// (Tone stack flat and presence off, as the test sets them, so there is nothing between the preamp and the power stage.)
std::vector<float> stackNaive(const std::vector<float>& in, int voicing, float gainKnob) {
    const Stack::Voice& v = Stack::voice(voicing);
    dsp::Biquad hp, pre;
    hp.highpass(v.hpHz, 0.7071, kSr);
    pre.peak(v.preHz, v.preQ, v.preDb, kSr);
    tonefx::Pole inter[3], couple[3], dc;
    for (auto& p : inter) p.setHz(v.interLp, kSr);
    for (auto& p : couple) p.setHz(20.0, kSr);
    dc.setHz(8.0, kSr);
    const double gs = std::pow(10.0, (v.gainLo + (v.gainHi - v.gainLo) * gainKnob * 0.1) / 20.0 / v.stages);
    const double b = v.bias, rest = tonefx::Sig::f(b, Stack::kKneeNeg), inv = 1.0 / tonefx::Sig::slope(b, Stack::kKneeNeg);
    const double pg = std::pow(10.0, v.powerDb / 20.0), prest = tonefx::Sig::f(Stack::kPowerBias, Stack::kPowerKneeNeg), pinv = 1.0 / tonefx::Sig::slope(Stack::kPowerBias, Stack::kPowerKneeNeg);
    std::vector<float> out(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        double y = pre.process(hp.process(in[i]));
        for (int s = 0; s < v.stages; ++s) {
            y = -(tonefx::Sig::f(b + gs * y, Stack::kKneeNeg) - rest) * inv;
            y = couple[s].hp(inter[s].lp(y));
        }
        if (v.stages & 1) y = -y;
        y = (tonefx::Sig::f(Stack::kPowerBias + pg * y, Stack::kPowerKneeNeg) - prest) * pinv;
        out[i] = (float)dc.hp(y * std::pow(10.0, v.levelDb / 20.0));
    }
    return out;
}
// Sets the middle knob so that the tone stack is flat (each voicing has its own middle setting at noon).
void flatTone(Stack& a) {
    a.pBass->set(5.0f); a.pTreble->set(5.0f); a.pPresence->set(0.0f);
    a.pMid->set(5.0f - Stack::voice(a.pVoicing->getInt()).midNoonDb * 0.5f);
}
}

TEST(stack_quiet_signal_response_matches_its_curve) {
    // every voicing, a few settings of everything, cabinet in and out: the measured gain of a signal too quiet to
    // distort is what responseDb() says
    double worst = 0.0;
    dsp::Noise rnd(31u);
    for (int voicing = 0; voicing < Stack::NumVoicings; ++voicing) {
        for (int trial = 0; trial < 3; ++trial) {
            auto a = amp(voicing, trial == 0 ? 0.0f : 2.0f * trial, trial == 0 ? Stack::CabOff : 1 + (voicing + trial) % 4);
            if (trial > 0) { a->pBass->set(rnd.uni() * 10.0f); a->pMid->set(rnd.uni() * 10.0f); a->pTreble->set(rnd.uni() * 10.0f); a->pPresence->set(rnd.uni() * 10.0f); a->pMic->set(rnd.uni()); }
            for (double hz : {60.0, 150.0, 400.0, 1000.0, 2500.0, 5000.0}) {
                const double want = a->responseDb(hz), got = gainDbAt(*a, hz, kQuiet);
                CHECK_MSG(std::fabs(got - want) < 0.3, "voicing %d trial %d, %.0f Hz: measured %.2f dB, curve %.2f dB", voicing, trial, hz, got, want);
                worst = std::max(worst, std::fabs(got - want));
            }
        }
    }
    FIG("stack: quiet-signal response within %.2f dB of responseDb() (four voicings, three settings each, 60 Hz to 5 kHz)", worst);
}

TEST(stack_tone_controls_do_what_they_say) {
    for (int voicing = 0; voicing < Stack::NumVoicings; ++voicing) {
        const Stack::Voice& v = Stack::voice(voicing);
        // one knob from 0 to 10 with the others at noon: the measured change is the change in toneStackDb(), it is
        // the stated size where the control works, and next to nothing at the other end of the spectrum
        struct Knob { const char* id; double at, far, size; } knobs[] = {
            {"bass", v.bassHz * 0.3, 6000.0, 24.0}, {"mid", v.midHz, 30.0, 20.0}, {"treble", v.trebleHz * 3.0, 60.0, 24.0}, {"presence", 11000.0, 150.0, 9.0}};
        for (auto& k : knobs) {
            auto a = amp(voicing, 0.0f, Stack::CabOff);
            a->pPresence->set(0.0f);
            set(*a, k.id, 0.0f);
            const double lowAt = gainDbAt(*a, k.at, kQuiet), lowFar = gainDbAt(*a, k.far, kQuiet), curveLow = a->toneStackDb(k.at);
            set(*a, k.id, 10.0f);
            const double highAt = gainDbAt(*a, k.at, kQuiet), highFar = gainDbAt(*a, k.far, kQuiet), curveHigh = a->toneStackDb(k.at);
            CHECK_MSG(std::fabs((highAt - lowAt) - (curveHigh - curveLow)) < 0.15, "voicing %d %s at %.0f Hz: measured change %.2f dB, curve %.2f dB", voicing, k.id, k.at, highAt - lowAt, curveHigh - curveLow);
            CHECK_MSG(highAt - lowAt > k.size * 0.85 && highAt - lowAt < k.size * 1.02, "voicing %d %s: %.1f dB of travel at %.0f Hz, %.0f stated", voicing, k.id, highAt - lowAt, k.at, k.size);
            CHECK_MSG(std::fabs(highFar - lowFar) < 1.0, "voicing %d %s: changes %.0f Hz by %.2f dB", voicing, k.id, k.far, highFar - lowFar);
            if (voicing == Stack::Crunch) FIG("stack, crunch: %s from 0 to 10 moves %.0f Hz by %.1f dB and %.0f Hz by %.2f dB", k.id, k.at, highAt - lowAt, k.far, highFar - lowFar);
        }
        // at noon the stack is flat but for the voicing's own middle setting
        auto a = amp(voicing, 0.0f, Stack::CabOff);
        a->pPresence->set(0.0f);
        CHECK_NEAR(a->toneStackDb(v.midHz), v.midNoonDb, 0.05);
        CHECK_NEAR(a->toneStackDb(20.0), 0.0, 0.3);
        CHECK_NEAR(a->toneStackDb(18000.0), 0.0, 0.3);
    }
    // a knob turned under a tone glides
    auto a = amp(Stack::Clean, 2.0f, Stack::Cab212);
    const auto in = sineWave(110, 1.0, 0.05f);
    const auto out = run(*a, in, {}, [&](size_t i) { if (i == 9600) a->pBass->set(10.0f); if (i == 24000) { a->pBass->set(0.0f); a->pMic->set(1.0f); } });
    CHECK_MSG(maxStep(out.l) < maxStep(in.l) * 12.0f, "largest step %.5f, the tone's own %.5f", maxStep(out.l), maxStep(in.l));
}

TEST(stack_cabinets_differ_and_are_band_limited) {
    const double probes[] = {40.0, 80.0, 150.0, 400.0, 1000.0, 2500.0, 4000.0, 6000.0, 9000.0};
    double curves[Stack::NumCabs][9] = {};
    double worst = 0.0;
    auto bare = amp(Stack::Clean, 0.0f, Stack::CabOff);
    CHECK(bare->cabinetDb(1000.0) == 0.0);
    for (int cab = 1; cab < Stack::NumCabs; ++cab) {
        auto a = amp(Stack::Clean, 0.0f, cab);
        for (int k = 0; k < 9; ++k) {
            // what the cabinet adds is the difference between it and no cabinet, and that is cabinetDb()
            const double measured = gainDbAt(*a, probes[k], kQuiet * 10.0f) - gainDbAt(*bare, probes[k], kQuiet * 10.0f);
            curves[cab][k] = a->cabinetDb(probes[k]);
            CHECK_MSG(std::fabs(measured - curves[cab][k]) < 0.25, "cabinet %d at %.0f Hz: measured %.2f dB, curve %.2f dB", cab, probes[k], measured, curves[cab][k]);
            worst = std::max(worst, std::fabs(measured - curves[cab][k]));
        }
        // band-limited at both ends: a speaker has no deep bass and no top
        const bool bass = cab == Stack::CabBass15;
        CHECK_MSG(a->cabinetDb(12000.0) < -25.0 && a->cabinetDb(18000.0) < -40.0, "cabinet %d: %.1f dB at 12 kHz, %.1f dB at 18 kHz", cab, a->cabinetDb(12000.0), a->cabinetDb(18000.0));
        CHECK_MSG(a->cabinetDb(bass ? 15.0 : 30.0) < -12.0, "cabinet %d: %.1f dB at the bottom", cab, a->cabinetDb(bass ? 15.0 : 30.0));
        CHECK(a->cabinetDb(1000.0) > -6.0 && a->cabinetDb(1000.0) < 3.0);
        // the last octave falls at 24 dB or more
        CHECK(a->cabinetDb(16000.0) - a->cabinetDb(8000.0) < -20.0);
        // microphone: at the edge of the cone the top goes and the box comes up
        const double at12k = a->cabinetDb(12000.0);
        a->pMic->set(0.0f);
        const double brightTop = a->cabinetDb(4500.0), brightLow = a->cabinetDb(bass ? 80.0 : 150.0);
        a->pMic->set(1.0f);
        const double edgeTop = a->cabinetDb(4500.0);
        CHECK_MSG(a->cabinetDb(4500.0) < brightTop - 8.0, "cabinet %d: 4.5 kHz is %.1f dB at the centre and %.1f dB at the edge", cab, brightTop, a->cabinetDb(4500.0));
        CHECK(a->cabinetDb(bass ? 80.0 : 150.0) > brightLow + 1.0);
        CHECK_NEAR(gainDbAt(*a, 3000.0, kQuiet * 10.0f) - gainDbAt(*bare, 3000.0, kQuiet * 10.0f), a->cabinetDb(3000.0), 0.25);
        FIG("stack cabinet %d: %.1f dB at 80 Hz, %.1f at 400 Hz, %.1f at 2.5 kHz, %.1f at 6 kHz, %.1f at 12 kHz; microphone centre to edge takes 4.5 kHz from %.1f to %.1f dB",
            cab, curves[cab][1], curves[cab][3], curves[cab][5], curves[cab][7], at12k, brightTop, edgeTop);
    }
    // every pair of cabinets differs by a few dB somewhere
    for (int a = 1; a < Stack::NumCabs; ++a) {
        for (int b = a + 1; b < Stack::NumCabs; ++b) {
            double most = 0.0;
            for (int k = 0; k < 9; ++k) most = std::max(most, std::fabs(curves[a][k] - curves[b][k]));
            CHECK_MSG(most > 2.5, "cabinets %d and %d differ by at most %.2f dB", a, b, most);
        }
    }
    FIG("stack: measured cabinet responses within %.2f dB of cabinetDb()", worst);
}

TEST(stack_voicings_distort_as_they_should) {
    // a guitar-sized tone, the same knob settings: clean stays clean, crunch distorts, lead distorts more
    double thd[Stack::NumVoicings], h2[Stack::NumVoicings], level[Stack::NumVoicings];
    for (int voicing = 0; voicing < Stack::NumVoicings; ++voicing) {
        auto a = amp(voicing, 5.0f, Stack::CabOff);
        const auto out = run(*a, sineWave(220, 0.7, 0.1f));
        thd[voicing] = thdDb(out.l, 220);
        h2[voicing] = harmonicDb(out.l, 220, 2);
        level[voicing] = db(peakOf(out.l, 12000));
    }
    CHECK_MSG(thd[Stack::Clean] < thd[Stack::Crunch] - 6.0 && thd[Stack::Crunch] < thd[Stack::Lead], "distortion %.1f / %.1f / %.1f dB for clean / crunch / lead", thd[0], thd[1], thd[2]);
    CHECK(thd[Stack::Lead] > -12.0);
    for (double v : h2) CHECK(v > -45.0);               // the stages are asymmetric: there is a second harmonic in every voicing
    for (double v : level) CHECK(v < 6.0 && v > -20.0);
    FIG("stack, 220 Hz at 0.1, gain at 5: harmonics 2 to 9 together %.1f / %.1f / %.1f / %.1f dB and the 2nd alone %.1f / %.1f / %.1f / %.1f dB for clean / crunch / lead / bass",
        thd[0], thd[1], thd[2], thd[3], h2[0], h2[1], h2[2], h2[3]);
    // the gain knob: more gain, more distortion; at nothing the clean voicing really is clean
    auto clean = amp(Stack::Clean, 0.0f, Stack::CabOff);
    const double cleanest = thdDb(run(*clean, sineWave(220, 0.7, 0.02f)).l, 220);
    CHECK_MSG(cleanest < -40.0, "clean at gain 0, 220 Hz at 0.02: %.1f dB", cleanest);
    double last = -200.0;
    for (float gain : {0.0f, 3.0f, 6.0f, 9.0f}) {
        auto a = amp(Stack::Crunch, gain, Stack::CabOff);
        const double d = thdDb(run(*a, sineWave(220, 0.7, 0.05f)).l, 220);
        CHECK(d > last);
        last = d;
    }
    // the output stays in phase with the input whatever the number of stages (so the dry / wet control mixes, not cancels)
    for (int voicing = 0; voicing < Stack::NumVoicings; ++voicing) {
        auto a = amp(voicing, 0.0f, Stack::CabOff);
        flatTone(*a);
        const auto in = sineWave(500, 0.4, kQuiet);
        const auto out = run(*a, in);
        double dot = 0.0, aa = 0.0, bb = 0.0;
        for (size_t i = 9600; i < out.l.size(); ++i) { dot += (double)out.l[i] * in.l[i - 32]; aa += (double)out.l[i] * out.l[i]; bb += (double)in.l[i - 32] * in.l[i - 32]; }
        // (not 1: the voicing filters turn the phase by up to 35 degrees at 500 Hz; upside down would be -1)
        CHECK_MSG(dot / std::sqrt(aa * bb) > 0.75, "voicing %d: output against input %.3f", voicing, dot / std::sqrt(aa * bb));
    }
    // master
    auto a = amp(Stack::Clean, 0.0f, Stack::CabOff), b = amp(Stack::Clean, 0.0f, Stack::CabOff);
    b->pMaster->set(-12.0f);
    CHECK_NEAR(gainDbAt(*a, 1000, kQuiet) - gainDbAt(*b, 1000, kQuiet), 12.0, 0.01);
}

TEST(stack_folds_back_far_less_than_the_plain_curves) {
    struct Case { int voicing; float gain; } cases[] = {{Stack::Clean, 10.0f}, {Stack::Crunch, 7.0f}, {Stack::Lead, 6.0f}, {Stack::Bass, 9.0f}};
    for (auto& cs : cases) {
        auto a = amp(cs.voicing, cs.gain, Stack::CabOff);
        flatTone(*a);
        const auto in = sineWave(9000, 0.7, 0.5f);
        const auto out = run(*a, in);
        const double ours = aliasDb(out.l), plain = aliasDb(stackNaive(in.l, cs.voicing, cs.gain));
        CHECK_MSG(ours < plain - 30.0, "voicing %d gain %.0f: aliases %.1f dB, without oversampling %.1f dB", cs.voicing, cs.gain, ours, plain);
        FIG("stack voicing %d at gain %.0f, 9 kHz at 0.5: aliases %.1f dB against %.1f dB without oversampling (%.1f dB better)", cs.voicing, cs.gain, ours, plain, plain - ours);
        // and the stand-in really is the same amplifier: at 1 kHz the two agree on level and on the harmonics
        a->reset();
        const auto tone1k = sineWave(1000, 0.7, 0.2f);
        const auto mine = run(*a, tone1k).l, theirs = stackNaive(tone1k.l, cs.voicing, cs.gain);
        CHECK_NEAR(levelDbAt(mine, mine.size() - 16384, 16384, 1000.0), levelDbAt(theirs, theirs.size() - 16384, 16384, 1000.0), 0.6);
        CHECK_NEAR(harmonicDb(mine, 1000, 3), harmonicDb(theirs, 1000, 3), 3.0);
    }
}

TEST(stack_latency_and_switching) {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        Stack dry;
        dry.prepare(sr);
        CHECK(dry.latencySamples() == 32);
        dry.pMix->set(0.0f);
        const auto in = noise(0.3, 0.7f, sr);
        const auto out = runAt(dry, in, sr);
        CHECK(maxDiffDelayed(out.l, in.l, 32, 0) == 0.0 && peakOf(out.l, 0, 32) == 0.0f);
        // the wet path: a quiet click comes out 32 samples later plus what the roll-offs between the stages delay its
        // peak by (one time constant for every stage after the first), whatever the number of stages
        for (int voicing = 0; voicing < Stack::NumVoicings; ++voicing) {
            Stack a;
            a.prepare(sr);
            a.pVoicing->set((float)voicing); a.pGain->set(0.0f); a.pCab->set(0.0f); a.pMaster->set(0.0f);
            flatTone(a);
            const Stack::Voice& v = Stack::voice(voicing);
            const auto click = runAt(a, impulse(0.05, sr, 100, kQuiet * 10.0f), sr);
            const double expected = 32.0 + (v.stages - 1) * sr / (kTwoPi * v.interLp);
            CHECK_MSG(std::fabs((double)peakIndex(click.l) - 100.0 - expected) <= 1.0, "%.0f Hz, voicing %d: click comes out %d samples late, expected %.1f", sr, voicing, (int)peakIndex(click.l) - 100, expected);
        }
    }
    // voicing and cabinet changed under a tone: the sound dips and comes back, it does not click
    auto a = amp(Stack::Crunch, 5.0f, Stack::Cab212);
    a->pMaster->set(-9.0f);
    const auto in = sineWave(196, 1.5, 0.2f);
    float steady = 0.0f;
    for (int voicing = 0; voicing < Stack::NumVoicings; ++voicing) {
        for (int cab : {(int)Stack::CabOff, (int)Stack::Cab112}) {
            auto s = amp(voicing, 5.0f, cab);
            s->pMaster->set(-9.0f);
            steady = std::max(steady, maxStep(run(*s, sineWave(196, 0.4, 0.2f)).l, 9600));
        }
    }
    const auto out = run(*a, in, {}, [&](size_t i) {
        const size_t q = i / kQuantum;
        if (q % 40 == 20) a->pVoicing->set((float)((q / 40) % 4));
        if (q % 40 == 0) a->pCab->set((float)((q / 40) % 5));
    });
    CHECK(allFinite(out));
    CHECK_MSG(maxStep(out.l) < steady * 1.1f, "largest step while switching %.4f, the steepest any voicing makes by itself %.4f", maxStep(out.l), steady);
    FIG("stack: voicing and cabinet switched every 53 ms under a 196 Hz tone: largest step %.4f (the steepest steady waveform steps by %.4f)", maxStep(out.l), steady);
}
