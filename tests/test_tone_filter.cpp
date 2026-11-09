// Motion Filter: modes and slopes, resonance, the envelope follower, the LFO and its lock to the song, drive.
#include "ToneTest.h"
using namespace pulp;
using namespace tu;
using namespace tt;

namespace {
const double kSr = 48000.0;

std::shared_ptr<MotionFilter> filter(int mode, float cutoff, int slope, float reso = 0.0f) {
    auto f = std::make_shared<MotionFilter>();
    f->prepare(kSr);
    f->pMode->set((float)mode); f->pCutoff->set(cutoff); f->pSlope->set((float)slope); f->pReso->set(reso);
    return f;
}
// What a two-pole low-pass of this Q gives at a frequency, for a filter that is exact at its cutoff (dB).
double lowpassDb(double hz, double cutoff, double q, double sr = kSr) {
    const double r = std::tan(kPi * hz / sr) / std::tan(kPi * cutoff / sr);
    return -10.0 * std::log10((1.0 - r * r) * (1.0 - r * r) + (r / q) * (r / q));
}
// The cutoff after every quantum of a run (left and right), read from the plugin's own read-out.
struct Trace { std::vector<float> left, right, env; };
Trace traceRun(MotionFilter& f, const Stereo& in, const Transport& t = {}) {
    Trace tr;
    run(f, in, t, [&](size_t i) { if (i > 0) { tr.env.push_back(f.meter(0)); tr.left.push_back(f.meter(1)); tr.right.push_back(f.meter(2)); } });
    tr.env.push_back(f.meter(0)); tr.left.push_back(f.meter(1)); tr.right.push_back(f.meter(2));
    return tr;
}
// Frequency of the strongest component in a stretch of signal.
double strongestHz(const std::vector<float>& x, size_t from, int n, double sr = kSr) {
    std::vector<float> re((size_t)n), im((size_t)n, 0.0f);
    for (int i = 0; i < n; ++i) re[(size_t)i] = x[from + (size_t)i] * (float)(0.5 - 0.5 * std::cos(kTwoPi * i / (double)(n - 1)));
    dsp::fft(re.data(), im.data(), n);
    int best = 1;
    double bestP = 0.0;
    for (int k = 1; k < n / 2; ++k) { const double p = (double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k]; if (p > bestP) { bestP = p; best = k; } }
    return best * sr / n;
}
}

TEST(motion_filter_modes_and_slopes) {
    using M = MotionFilter;
    // low-pass: flat below, 3 dB down at the cutoff, 12 dB per octave above; the 24 dB slope is two of those
    auto lp = filter(M::Low, 1000, 0);
    CHECK_NEAR(gainDbAt(*lp, 100), 0.0, 0.1);
    CHECK_NEAR(gainDbAt(*lp, 1000), -3.01, 0.1);
    CHECK_NEAR(gainDbAt(*lp, 4000), lowpassDb(4000, 1000, 0.7071), 0.2);
    const double lpSlope = gainDbAt(*lp, 8000) - gainDbAt(*lp, 4000);
    CHECK_MSG(lpSlope < -11.5 && lpSlope > -15.0, "12 dB low-pass falls %.1f dB from 4 to 8 kHz", lpSlope);
    auto lp24 = filter(M::Low, 1000, 1);
    CHECK_NEAR(gainDbAt(*lp24, 1000), -6.02, 0.15);
    CHECK_NEAR(gainDbAt(*lp24, 4000), 2.0 * lowpassDb(4000, 1000, 0.7071), 0.4);
    const double lp24Slope = gainDbAt(*lp24, 4000) - gainDbAt(*lp24, 2000);
    CHECK_MSG(lp24Slope < -22.0 && lp24Slope > -27.0, "24 dB low-pass falls %.1f dB from 2 to 4 kHz", lp24Slope);
    // high-pass
    auto hp = filter(M::High, 1000, 0);
    CHECK_NEAR(gainDbAt(*hp, 8000), 0.0, 0.2);
    CHECK_NEAR(gainDbAt(*hp, 1000), -3.01, 0.1);
    const double hpSlope = gainDbAt(*hp, 125) - gainDbAt(*hp, 250);
    CHECK_MSG(hpSlope < -11.5 && hpSlope > -12.5, "12 dB high-pass falls %.1f dB from 250 to 125 Hz", hpSlope);
    auto hp24 = filter(M::High, 1000, 1);
    const double hp24Slope = gainDbAt(*hp24, 125) - gainDbAt(*hp24, 250);
    CHECK_MSG(hp24Slope < -23.0 && hp24Slope > -25.0, "24 dB high-pass falls %.1f dB from 250 to 125 Hz", hp24Slope);
    // band-pass: unity at the centre, falling away either side, twice as fast in dB with the second stage
    auto bp = filter(M::Band, 1000, 0, 0.5f), bp24 = filter(M::Band, 1000, 1, 0.5f);
    CHECK_NEAR(gainDbAt(*bp, 1000), 0.0, 0.1);
    CHECK_NEAR(gainDbAt(*bp24, 1000), 0.0, 0.15);
    const double skirt = gainDbAt(*bp, 2000), skirt24 = gainDbAt(*bp24, 2000);
    CHECK(skirt < -18.0 && gainDbAt(*bp, 500) < -18.0);
    CHECK_NEAR(skirt24, 2.0 * skirt, 0.3);
    // notch: nothing at the centre, everything far from it
    auto notch = filter(M::Notch, 1000, 0, 0.3f), notch24 = filter(M::Notch, 1000, 1, 0.3f);
    CHECK(gainDbAt(*notch, 1000) < -50.0);
    CHECK_NEAR(gainDbAt(*notch, 100), 0.0, 0.2);
    CHECK_NEAR(gainDbAt(*notch, 9000), 0.0, 0.2);
    CHECK_NEAR(gainDbAt(*notch24, 1200), 2.0 * gainDbAt(*notch, 1200), 0.2);
    // peak: flat with no resonance; with resonance a bump of Q / 0.707 at the centre, narrower with the second stage
    auto flat = filter(M::Peak, 1000, 0, 0.0f);
    for (double hz : {100.0, 1000.0, 9000.0}) CHECK_NEAR(gainDbAt(*flat, hz), 0.0, 1.0e-4);
    const double q = M::resonanceQ(0.5f);
    auto pk = filter(M::Peak, 1000, 0, 0.5f), pk24 = filter(M::Peak, 1000, 1, 0.5f);
    CHECK_NEAR(gainDbAt(*pk, 1000), db(q / 0.7071), 0.15);
    CHECK_NEAR(gainDbAt(*pk24, 1000), db(q / 0.7071), 0.25);
    CHECK_NEAR(gainDbAt(*pk, 100), 0.0, 0.3);
    CHECK(gainDbAt(*pk24, 1150) < gainDbAt(*pk, 1150) - 1.0);
    FIG("motion filter at 1 kHz: low-pass -3.01 / -6.02 dB at the cutoff measured %.2f / %.2f; slopes %.1f and %.1f dB per octave (low), %.1f and %.1f (high)",
        gainDbAt(*lp, 1000), gainDbAt(*lp24, 1000), lpSlope, lp24Slope, hpSlope, hp24Slope);
    // the output gain and the mix
    auto out = filter(M::Peak, 1000, 0, 0.0f);
    out->pOut->set(-6.0f);
    CHECK_NEAR(gainDbAt(*out, 500), -6.0, 0.01);
    out->pMix->set(0.5f);
    CHECK_NEAR(gainDbAt(*out, 500), db(0.5 + 0.5 * dbToGain(-6.0f)), 0.01);
}

TEST(motion_filter_resonance_peaks_and_stays_stable) {
    using M = MotionFilter;
    for (float reso : {0.3f, 0.6f, 1.0f}) {
        auto f = filter(M::Low, 1000, 0, reso);
        const double q = M::resonanceQ(reso);
        CHECK_NEAR(gainDbAt(*f, 1000, 0.01f), db(q), 0.3);
    }
    FIG("motion filter: full resonance is Q %.1f, a peak of %.1f dB at the cutoff", M::resonanceQ(1.0f), db(M::resonanceQ(1.0f)));
    // Full resonance, every mode and slope, with the cutoff thrown about as fast as the LFO and the envelope can:
    // the output stays within what the resonance can give, and dies away when the input stops.
    double worst = 0.0;
    for (double sr : {44100.0, 96000.0}) {
        for (int mode = 0; mode < M::NumModes; ++mode) {
            for (int slope = 0; slope < 2; ++slope) {
                M f;
                f.prepare(sr);
                f.pMode->set((float)mode); f.pSlope->set((float)slope); f.pReso->set(1.0f); f.pCutoff->set(700.0f);
                f.pLfoSync->set(0); f.pLfoRate->set(20.0f); f.pLfoShape->set(2); f.pLfoDepth->set(5.0f); f.pLfoPhase->set(180.0f);
                f.pEnvAmt->set(5.0f); f.pEnvSens->set(20.0f); f.pAttack->set(0.1f); f.pRelease->set(5.0f);
                Stereo in = noise(1.0, 0.5f, sr);
                in.l[100] = in.r[100] = 1.0f;
                append(in, silence(2.5, sr));
                const auto out = runAt(f, in, sr);
                CHECK(allFinite(out));
                const float peak = std::max(peakOf(out.l), peakOf(out.r));
                // Q of 24.7 on both stages of a band-pass or notch at most; nothing here can give more than that squared
                CHECK_MSG(peak < 0.5f * 60.0f, "mode %d slope %d at %.0f Hz: peak %.1f", mode, slope, sr, peak);
                worst = std::max(worst, (double)peak);
                const size_t tail = out.l.size() - (size_t)(0.5 * sr);
                CHECK_MSG(peakOf(out.l, tail) == 0.0f && peakOf(out.r, tail) == 0.0f, "mode %d slope %d: still ringing (%g) two seconds after the input stopped", mode, slope, peakOf(out.l, tail));
            }
        }
    }
    FIG("motion filter: full resonance, 20 Hz square LFO over 5 octaves plus the envelope, noise at 0.5: worst output peak %.2f", worst);
}

TEST(motion_filter_envelope_moves_the_cutoff_as_stated) {
    using M = MotionFilter;
    // a steady level: the envelope settles at level * sensitivity, and the cutoff at base * 2^(amount * envelope)
    struct Case { float level, sensDb, amount; } cases[] = {{0.25f, 6.0f, 3.0f}, {0.1f, 12.0f, -2.0f}, {0.5f, 0.0f, 4.0f}, {0.5f, 12.0f, 2.0f}};
    for (auto& cs : cases) {
        M f;
        f.prepare(kSr);
        f.pCutoff->set(800.0f); f.pEnvAmt->set(cs.amount); f.pEnvSens->set(cs.sensDb); f.pAttack->set(5.0f); f.pRelease->set(100.0f);
        run(f, level(0.5, cs.level));
        const double env = std::min(1.0, (double)cs.level * dbToGain(cs.sensDb));
        CHECK_NEAR(f.meter(0), env, 0.002);
        CHECK_NEAR(f.meter(1) / (800.0 * std::pow(2.0, cs.amount * env)), 1.0, 0.004);
        CHECK(f.meter(2) == f.meter(1));
    }
    // attack and release are time constants: 63 % of the way after one of them
    {
        M f;
        f.prepare(kSr);
        f.pEnvSens->set(0.0f); f.pAttack->set(20.0f); f.pRelease->set(200.0f); f.pEnvAmt->set(2.0f);
        const size_t attack = (size_t)(0.020 * kSr), release = (size_t)(0.200 * kSr);
        Stereo in = level(0.5, 0.8f);
        append(in, silence(0.5));
        const auto tr = traceRun(f, in);
        // the follower sees the signal as late as the filter does (the latency), so count from there
        const auto envAt = [&](size_t sample) { return tr.env[(sample + tonefx::kOsLatency) / kQuantum - 1]; };
        CHECK_NEAR(envAt(attack) / 0.8, 1.0 - std::exp(-1.0), 0.04);
        CHECK_NEAR(envAt(24000 - 64) / 0.8, 1.0, 0.001);
        CHECK_NEAR(envAt(24000 + release) / 0.8, std::exp(-1.0), 0.02);
        FIG("motion filter envelope: %.1f %% after one attack time (63.2 expected), %.1f %% left after one release time (36.8 expected)",
            100.0 * envAt(attack) / 0.8, 100.0 * envAt(24000 + release) / 0.8);
    }
    // and in the sound: a 3 kHz tone through a low-pass that the tone's own level opens
    {
        M f;
        f.prepare(kSr);
        f.pCutoff->set(300.0f); f.pReso->set(0.0f); f.pEnvAmt->set(4.0f); f.pEnvSens->set(0.0f); f.pAttack->set(1.0f); f.pRelease->set(1500.0f);
        const auto out = run(f, sineWave(3000, 1.0, 0.5f));
        const double cutoff = f.meter(1);
        CHECK_MSG(cutoff > 300.0 * 3.5 && cutoff < 300.0 * 4.0, "cutoff %.0f Hz", cutoff);      // 2^(4 * 0.5) = 4, less the follower's ripple
        const double measured = levelDbAt(out.l, out.l.size() - 8192, 8192, 3000.0) - db(0.5);
        CHECK_NEAR(measured, lowpassDb(3000, cutoff, 0.7071), 0.5);
        // without the envelope the same tone is far further down
        f.pEnvAmt->set(0.0f);
        CHECK_NEAR(gainDbAt(f, 3000, 0.5f), lowpassDb(3000, 300, 0.7071), 0.3);
        FIG("motion filter envelope: 3 kHz at 0.5 opens a 300 Hz low-pass to %.0f Hz; the tone comes out at %.1f dB instead of %.1f dB", cutoff, measured, lowpassDb(3000, 300, 0.7071));
    }
}

TEST(motion_filter_lfo_rate_depth_and_shapes) {
    using M = MotionFilter;
    const double perQuantum = kQuantum / kSr;
    // free running sine: 2 Hz, one octave either way
    {
        M f;
        f.prepare(kSr);
        f.pCutoff->set(1000.0f); f.pLfoSync->set(0); f.pLfoRate->set(2.0f); f.pLfoDepth->set(1.0f);
        const auto tr = traceRun(f, silence(4.0));
        float lo = 1e9f, hi = 0.0f;
        int rising = 0;
        double firstUp = -1, lastUp = -1;
        for (size_t i = 1; i < tr.left.size(); ++i) {
            lo = std::min(lo, tr.left[i]); hi = std::max(hi, tr.left[i]);
            if (tr.left[i - 1] < 1000.0f && tr.left[i] >= 1000.0f) {
                const double at = ((double)i - 1.0 + (1000.0 - tr.left[i - 1]) / (tr.left[i] - tr.left[i - 1])) * perQuantum;
                if (firstUp < 0) firstUp = at;
                lastUp = at;
                ++rising;
            }
        }
        const double rate = (rising - 1) / (lastUp - firstUp);
        CHECK_NEAR(rate, 2.0, 0.002);
        CHECK_NEAR(hi, 2000.0, 12.0);
        CHECK_NEAR(lo, 500.0, 3.0);
        FIG("motion filter LFO: set to 2 Hz and one octave: measured %.4f Hz, cutoff from %.1f to %.1f Hz around 1 kHz", rate, lo, hi);
    }
    // the shapes: where the cutoff is a quarter, a half and three quarters of the way through a cycle (1 Hz, 2 octaves)
    struct Shape { int shape; double at25, at49, at75; } shapes[] = {
        {0, 2.0, 2.0 * std::sin(kTwoPi * 0.49), -2.0}, {1, 2.0, 0.08, -2.0}, {2, 2.0, 2.0, -2.0}, {3, -1.0, -0.04, 1.0}, {4, 1.0, 0.04, -1.0}};
    for (auto& s : shapes) {
        M f;
        f.prepare(kSr);
        f.pCutoff->set(1000.0f); f.pLfoSync->set(0); f.pLfoRate->set(1.0f); f.pLfoDepth->set(2.0f); f.pLfoShape->set((float)s.shape);
        const auto tr = traceRun(f, silence(1.0));
        const auto octAt = [&](double seconds) { return std::log2(tr.left[(size_t)(seconds / perQuantum) - 1] / 1000.0); };
        // a little before each point, so the square's edge and the smoothing are not in the way
        CHECK_NEAR(octAt(0.25), s.at25, 0.06);
        CHECK_NEAR(octAt(0.49), s.at49, 0.06);
        CHECK_NEAR(octAt(0.75), s.at75, 0.06);
    }
    // random steps: one level for the whole cycle, another for the next, the same every time
    {
        M f;
        f.prepare(kSr);
        f.pCutoff->set(1000.0f); f.pLfoSync->set(0); f.pLfoRate->set(4.0f); f.pLfoDepth->set(2.0f); f.pLfoShape->set(5);
        const auto tr = traceRun(f, silence(1.0));
        const auto at = [&](double seconds) { return tr.left[(size_t)(seconds / perQuantum) - 1]; };
        CHECK_NEAR(at(0.10), at(0.24), 0.5);
        CHECK(std::fabs(at(0.24) - at(0.49)) > 20.0f);
        CHECK(std::fabs(at(0.49) - at(0.74)) > 20.0f);
        M g;
        g.prepare(kSr);
        g.pCutoff->set(1000.0f); g.pLfoSync->set(0); g.pLfoRate->set(4.0f); g.pLfoDepth->set(2.0f); g.pLfoShape->set(5);
        CHECK(traceRun(g, silence(1.0)).left == tr.left);
        CHECK(M::lfoWave(5, 3.2) == M::lfoWave(5, 3.9));
        for (int cycle = 0; cycle < 200; ++cycle) CHECK(std::fabs(M::lfoWave(5, cycle + 0.5)) <= 1.0f);
    }
    // the right channel can run ahead: 90 degrees is a quarter of a cycle, 180 the mirror image
    for (float degrees : {90.0f, 180.0f}) {
        M f;
        f.prepare(kSr);
        f.pCutoff->set(1000.0f); f.pLfoSync->set(0); f.pLfoRate->set(1.0f); f.pLfoDepth->set(1.5f); f.pLfoPhase->set(degrees);
        const auto tr = traceRun(f, silence(2.0));
        const size_t shift = (size_t)(degrees / 360.0 * kSr / kQuantum);
        double worst = 0.0;
        for (size_t i = 10; i + shift < tr.left.size(); ++i) worst = std::max(worst, std::fabs(std::log2((double)tr.right[i] / (double)tr.left[i + shift])));
        CHECK_MSG(worst < 0.01, "%.0f degrees: right is not left %.0f degrees later (off by %.4f octaves)", degrees, degrees, worst);
    }
    // and in the sound: a resonant band-pass on noise, held high and low in turn by a slow square
    {
        M f;
        f.prepare(kSr);
        f.pMode->set(M::Band); f.pCutoff->set(1000.0f); f.pReso->set(0.9f); f.pLfoSync->set(0); f.pLfoRate->set(1.0f); f.pLfoShape->set(2); f.pLfoDepth->set(1.0f);
        const auto out = run(f, noise(1.0, 0.3f));
        const double high = strongestHz(out.l, 4000, 16384), low = strongestHz(out.l, 28000, 16384);
        CHECK_NEAR(high / 2000.0, 1.0, 0.03);
        CHECK_NEAR(low / 500.0, 1.0, 0.03);
        FIG("motion filter LFO in the sound: band-pass on noise rings at %.0f Hz in the high half of a square and %.0f Hz in the low half (2000 and 500 expected)", high, low);
    }
}

TEST(motion_filter_lfo_is_locked_to_the_song_position) {
    using M = MotionFilter;
    const auto setUp = [](M& f, int division) {
        f.prepare(kSr);
        f.pCutoff->set(1000.0f); f.pLfoSync->set(1); f.pLfoDiv->set((float)division); f.pLfoDepth->set(2.0f); f.pLfoShape->set(0);
    };
    // every division: one cycle is that many ticks
    static const double ticks[M::kDivisions] = {120, 160, 240, 320, 480, 640, 720, 960, 1440, 1920, 3840, 7680, 15360, 30720};
    for (int d = 0; d < M::kDivisions; ++d) CHECK(M::divisionTicks(d) == ticks[d]);
    for (double tempo : {120.0, 90.0, 174.0}) {
        for (int division : {4, 7, 10}) {
            M f;
            setUp(f, division);
            Transport t;
            t.playing = true; t.tempo = tempo;
            const auto tr = traceRun(f, silence(1.5), t);
            const double ticksPerSample = tempo * kPPQ / (60.0 * kSr);
            double worst = 0.0;
            for (size_t q = 20; q < tr.left.size(); ++q) {
                // the position of the last sample of quantum q, less the latency: the sound that comes out there went in that much earlier
                const double tick = ((double)((q + 1) * kQuantum - 1) - tonefx::kOsLatency) * ticksPerSample;
                const double want = 2.0 * std::sin(kTwoPi * tick / ticks[division]);
                worst = std::max(worst, std::fabs(std::log2(tr.left[q] / 1000.0) - want));
            }
            // what is left is the 2 ms of smoothing the cutoff goes through
            const double lag = 2.0 * kTwoPi * (tempo * kPPQ / (60.0 * ticks[division])) * 0.001 * M::kSmoothMs;
            CHECK_MSG(worst < lag * 1.2 + 0.002, "tempo %.0f division %d: cutoff is %.4f octaves from where the song position puts it", tempo, division, worst);
        }
    }
    // the same position gives the same phase after a seek: a run from the start and a run that starts in the middle
    {
        M a, b;
        setUp(a, 7);
        setUp(b, 7);
        Transport ta, tb;
        ta.playing = tb.playing = true;
        const double ticksPerSample = 120.0 * kPPQ / (60.0 * kSr);
        const size_t seekQuantum = 520;                                    // an odd place, not on a beat
        tb.startTick = (double)(seekQuantum * kQuantum) * ticksPerSample;
        const auto whole = traceRun(a, silence(2.0), ta);
        const auto part = traceRun(b, silence(0.6), tb);
        double worst = 0.0;
        for (size_t q = 20; q < part.left.size(); ++q) worst = std::max(worst, std::fabs(std::log2((double)part.left[q] / (double)whole.left[q + seekQuantum])));
        CHECK_MSG(worst < 1.0e-5, "after a seek the cutoff is %.2g octaves from where it was at that position", worst);
        // and the sound is the same too once the filter has forgotten where it started
        const auto in = noise(2.0, 0.3f);
        M c, d;
        setUp(c, 7);
        setUp(d, 7);
        const auto full = run(c, in, ta);
        Stereo tailIn;
        tailIn.l.assign(in.l.begin() + (long long)(seekQuantum * kQuantum), in.l.end());
        tailIn.r.assign(in.r.begin() + (long long)(seekQuantum * kQuantum), in.r.end());
        const auto tail = run(d, tailIn, tb);
        double diff = 0.0;
        for (size_t i = 9600; i < tail.l.size(); ++i) diff = std::max(diff, (double)std::fabs(tail.l[i] - full.l[i + seekQuantum * kQuantum]));
        CHECK_MSG(diff < 1.0e-4, "0.2 s after a seek the sound differs by %g from a run that played through", diff);
        FIG("motion filter: after a seek the cutoff is within %.1e octaves of a run from the start, the sound within %.1e", worst, diff);
    }
    // stopped, a synced LFO still runs at the tempo's rate (it has no position to follow)
    {
        M f;
        setUp(f, 7);
        Transport t;
        t.tempo = 150.0;
        const auto tr = traceRun(f, silence(2.0), t);
        int rising = 0;
        for (size_t i = 1; i < tr.left.size(); ++i) if (tr.left[i - 1] < 1000.0f && tr.left[i] >= 1000.0f) ++rising;
        CHECK_MSG(rising == 4 || rising == 5, "%d cycles in two seconds", rising);      // 2.5 Hz for two seconds, starting on a crossing
    }
}

TEST(motion_filter_fast_cutoff_moves_make_no_zipper_noise) {
    using M = MotionFilter;
    // An 80 Hz tone through a low-pass whose cutoff a 20 Hz saw throws over four octaves. If the cutoff moved in
    // steps of a quantum there would be lines every 750 Hz (48000 / 64) all the way up; there are none.
    M f;
    f.prepare(kSr);
    f.pCutoff->set(900.0f); f.pReso->set(0.3f); f.pLfoSync->set(0); f.pLfoRate->set(20.0f); f.pLfoShape->set(4); f.pLfoDepth->set(2.0f);
    const auto out = run(f, sineWave(80, 2.0, 0.5f));
    const size_t n = 65536, from = out.l.size() - n;
    const double toneDb = levelDbAt(out.l, from, n, 80.0);
    double worstLine = -400.0;
    for (int k = 4; k <= 30; ++k) worstLine = std::max(worstLine, levelDbAt(out.l, from, n, 750.0 * k) - toneDb);
    const double high = bandDb(out.l, from, (int)n, 5000.0, 24000.0) - toneDb;
    CHECK_MSG(worstLine < -100.0, "line at a multiple of 750 Hz at %.1f dB", worstLine);
    CHECK_MSG(high < -90.0, "everything above 5 kHz at %.1f dB", high);
    FIG("motion filter, 80 Hz tone, 20 Hz saw LFO over 4 octaves: strongest line at a multiple of 750 Hz %.1f dB, all above 5 kHz %.1f dB (re the tone)", worstLine, high);
    // The cutoff knob itself: a jump of five octaves in the middle of a tone. The cutoff gets there through 2 ms of
    // smoothing, so the output moves to its new shape over a few milliseconds: fast, but not a step. The largest
    // difference between two samples stays under an eighth of the tone's peak (a click would be most of it).
    M g;
    g.prepare(kSr);
    g.pCutoff->set(200.0f);
    const auto in = sineWave(150, 0.5, 0.4f);
    const auto jump = run(g, in, {}, [&](size_t i) { if (i == 9600) g.pCutoff->set(6400.0f); if (i == 16000) g.pCutoff->set(200.0f); });
    CHECK_MSG(maxStep(jump.l, 9000) <= 0.4f * 0.125f, "step %.5f, the tone's own %.5f", maxStep(jump.l, 9000), maxStep(in.l));
    FIG("motion filter: cutoff knob jumped from 200 Hz to 6.4 kHz and back under a 150 Hz tone at 0.4: largest step %.4f (the tone's own is %.4f)", maxStep(jump.l, 9000), maxStep(in.l));
}

TEST(motion_filter_drive_latency_and_switching) {
    using M = MotionFilter;
    // nothing switched in: the input comes back untouched, 32 samples late, at any rate; so does the dry path
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        M f;
        f.prepare(sr);
        CHECK(f.latencySamples() == 32);
        f.pMode->set(M::Peak); f.pReso->set(0.0f);
        const auto in = noise(0.3, 0.7f, sr);
        const auto out = runAt(f, in, sr);
        CHECK(maxDiffDelayed(out.l, in.l, 32, 0) == 0.0 && maxDiffDelayed(out.r, in.r, 32, 0) == 0.0);
        CHECK(peakOf(out.l, 0, 32) == 0.0f);
        M dry;
        dry.prepare(sr);
        dry.pMix->set(0.0f); dry.pDrive->set(18.0f); dry.pReso->set(0.8f);
        const auto d = runAt(dry, in, sr);
        CHECK(maxDiffDelayed(d.l, in.l, 32, 0) == 0.0);
    }
    // with drive the wet path is just as late: a quiet tone through the peak mode nulls against the delayed input
    {
        M f;
        f.prepare(kSr);
        f.pMode->set(M::Peak); f.pReso->set(0.0f); f.pDrive->set(6.0f);
        const auto in = sineWave(1000, 0.3, 0.001f);
        const auto out = run(f, in);
        // drive of 6 dB lifts a quiet signal by 3 dB (the stage turns down by the square root of what it turns up)
        const double g = dbToGain(3.0f);
        double worst = 0.0;
        for (size_t i = 4800; i < out.l.size(); ++i) worst = std::max(worst, (double)std::fabs(out.l[i] - (float)g * in.l[i - 32]));
        CHECK_MSG(worst < 0.001 * 0.003, "difference %g", worst);
        double early = 0.0, late = 0.0;
        for (size_t i = 4800; i < out.l.size(); ++i) {
            early = std::max(early, (double)std::fabs(out.l[i] - (float)g * in.l[i - 31]));
            late = std::max(late, (double)std::fabs(out.l[i] - (float)g * in.l[i - 33]));
        }
        CHECK(early > 30.0 * worst && late > 30.0 * worst);
    }
    // the drive stage is symmetrical: odd harmonics only
    {
        M f;
        f.prepare(kSr);
        f.pMode->set(M::Peak); f.pReso->set(0.0f); f.pDrive->set(18.0f);
        const auto out = run(f, sineWave(1000, 0.6, 0.4f));
        const double h2 = harmonicDb(out.l, 1000, 2), h3 = harmonicDb(out.l, 1000, 3), h5 = harmonicDb(out.l, 1000, 5);
        CHECK(h3 > -25.0 && h5 > -40.0);
        CHECK(h2 < -90.0);
        // 9 kHz driven hard: what folds back, against the same curve with no oversampling
        f.reset();
        const auto hard = run(f, sineWave(9000, 0.6, 0.5f));
        std::vector<float> naive(hard.l.size());
        const double gain = dbToGain(18.0f);
        for (size_t i = 0; i < naive.size(); ++i) naive[i] = (float)(tonefx::Sig::f(gain * 0.5 * std::sin(kTwoPi * 9000.0 * (double)i / kSr), 1.0) / std::sqrt(gain));
        const double ours = aliasDb(hard.l), plain = aliasDb(naive);
        CHECK_MSG(ours < plain - 30.0, "drive aliases %.1f dB, without oversampling %.1f dB", ours, plain);
        FIG("motion filter drive, 1 kHz at 18 dB: 2nd %.1f dB, 3rd %.1f dB, 5th %.1f dB; 9 kHz aliases %.1f dB against %.1f dB without oversampling (%.1f dB better)",
            h2, h3, h5, ours, plain, plain - ours);
    }
    // turning the drive on and off, and changing mode and slope, in the middle of a tone: no click
    {
        M f;
        f.prepare(kSr);
        f.pCutoff->set(4000.0f);
        const auto in = sineWave(220, 1.0, 0.3f);
        dsp::Noise rnd(5u);
        const auto out = run(f, in, {}, [&](size_t i) {
            const size_t q = i / kQuantum;
            if (q % 40 == 10) f.pDrive->set(f.pDrive->get() > 0.0f ? 0.0f : 12.0f);
            if (q % 40 == 30) { f.pMode->set((float)(int)(rnd.uni() * 4.99f)); f.pSlope->set(rnd.uni() < 0.5f ? 0.0f : 1.0f); }
        });
        CHECK(allFinite(out));
        // a 220 Hz tone at 0.3 steps by 0.0086 a sample; drive adds 6 dB to a signal this quiet and resonance a little more
        CHECK_MSG(maxStep(out.l) < maxStep(in.l) * 4.0f, "largest step %.4f, the tone's own %.4f", maxStep(out.l), maxStep(in.l));
        FIG("motion filter: drive switched on and off and the mode changed every 50 ms under a 220 Hz tone: largest step %.4f (the tone's own is %.4f)", maxStep(out.l), maxStep(in.l));
    }
}
