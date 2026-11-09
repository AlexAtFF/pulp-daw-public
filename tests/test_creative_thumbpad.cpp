// Thumb Pad: each effect against what its corner of the pad claims, and no jumps when the pad is played hard.
#include "CreativeTest.h"
using namespace pulp;
using namespace ct;

namespace {
const double kSr = 48000.0;

std::shared_ptr<ThumbPad> pad(int mode, float x, float y) {
    auto p = std::make_shared<ThumbPad>();
    p->prepare(kSr);
    p->pMode->set((float)mode); p->pX->set(x); p->pY->set(y);
    return p;
}
double gainAt(int mode, float x, float y, double hz, double seconds = 0.5) {
    auto p = pad(mode, x, y);
    return gainDbAt(*p, hz, 0.02f, seconds);
}
std::vector<float> impulseResponse(int mode, float x, float y, double seconds) {
    auto p = pad(mode, x, y);
    return run(*p, impulse(seconds)).l;
}
}

TEST(thumb_pad_filter_sweeps_at_the_corners) {
    CHECK_NEAR(ThumbPad::lowPassHz(0.0), 80.0, 1e-9);
    CHECK_NEAR(ThumbPad::lowPassHz(1.0), 18000.0, 1e-6);
    CHECK_NEAR(ThumbPad::highPassHz(0.0), 20.0, 1e-9);
    CHECK_NEAR(ThumbPad::highPassHz(1.0), 8000.0, 1e-6);
    // low-pass: X right = open, X left = 80 Hz, 12 dB an octave; Y bottom = flat corner, Y top = a peak of Q 12.7 (22.1 dB)
    CHECK_NEAR(gainAt(ThumbPad::LowPass, 1.0f, 0.0f, 1000.0), 0.0, 0.05);
    CHECK(gainAt(ThumbPad::LowPass, 1.0f, 0.0f, 10000.0) > -1.0);
    CHECK_NEAR(gainAt(ThumbPad::LowPass, 0.0f, 0.0f, 80.0), -3.01, 0.1);
    CHECK_NEAR(gainAt(ThumbPad::LowPass, 0.0f, 0.0f, 1000.0), -43.9, 0.5);
    CHECK_NEAR(gainAt(ThumbPad::LowPass, 0.0f, 0.0f, 2000.0) - gainAt(ThumbPad::LowPass, 0.0f, 0.0f, 1000.0), -12.0, 0.3);
    CHECK_NEAR(gainAt(ThumbPad::LowPass, 0.5f, 0.0f, ThumbPad::lowPassHz(0.5)), -3.01, 0.1);
    CHECK_NEAR(gainAt(ThumbPad::LowPass, 0.5f, 1.0f, ThumbPad::lowPassHz(0.5)), 20.0 * std::log10(ThumbPad::filterQ(1.0)), 0.3);
    CHECK_NEAR(gainAt(ThumbPad::LowPass, 0.5f, 1.0f, 100.0), 0.0, 0.2);
    // high-pass: X left = open, X right = 8 kHz
    CHECK_NEAR(gainAt(ThumbPad::HighPass, 0.0f, 0.0f, 1000.0), 0.0, 0.05);
    CHECK_NEAR(gainAt(ThumbPad::HighPass, 1.0f, 0.0f, 8000.0), -3.01, 0.1);
    // (12 dB an octave on the filter's own warped frequency scale: -37.8 dB three octaves down, where a plain ratio gives -36.1)
    CHECK_NEAR(gainAt(ThumbPad::HighPass, 1.0f, 0.0f, 1000.0), 40.0 * std::log10(std::tan(kPi * 1000.0 / kSr) / std::tan(kPi * 8000.0 / kSr)), 0.3);
    CHECK_NEAR(gainAt(ThumbPad::HighPass, 0.5f, 1.0f, ThumbPad::highPassHz(0.5)), 20.0 * std::log10(ThumbPad::filterQ(1.0)), 0.3);
    CHECK_NEAR(gainAt(ThumbPad::HighPass, 0.5f, 1.0f, 8000.0), 0.0, 0.2);
}

TEST(thumb_pad_flanger_phaser_and_ring_at_the_corners) {
    {   // flanger, X left: a copy 0.25 ms (12 samples) later. Teeth every 4 kHz, the gaps half way between.
        CHECK_NEAR(ThumbPad::flangerMs(0.0), 0.25, 1e-12);
        CHECK_NEAR(ThumbPad::flangerMs(1.0), 10.0, 1e-9);
        CHECK_NEAR(gainAt(ThumbPad::Flanger, 0.0f, 0.0f, 4000.0), 3.01, 0.05);
        CHECK(gainAt(ThumbPad::Flanger, 0.0f, 0.0f, 2000.0) < -60.0);
        CHECK(gainAt(ThumbPad::Flanger, 0.0f, 0.0f, 6000.0) < -60.0);
        // X right: 10 ms (480 samples): teeth every 100 Hz
        CHECK_NEAR(gainAt(ThumbPad::Flanger, 1.0f, 0.0f, 1000.0), 3.01, 0.05);
        CHECK(gainAt(ThumbPad::Flanger, 1.0f, 0.0f, 1050.0) < -60.0);
        // Y top: feedback 0.9. A tooth is 0.707 * (1 + 1 / (1 - 0.9)) = +17.8 dB, a gap 0.707 * (1 - 1 / 1.9) = -9.5 dB
        CHECK_NEAR(gainAt(ThumbPad::Flanger, 0.0f, 1.0f, 4000.0), 20.0 * std::log10(0.7071 * 11.0), 0.1);
        CHECK_NEAR(gainAt(ThumbPad::Flanger, 0.0f, 1.0f, 2000.0), 20.0 * std::log10(0.7071 * (1.0 - 1.0 / 1.9)), 0.1);
    }
    {   // phaser, no feedback: six stages give three notches, the middle one at the frequency X sets
        for (float x : {0.0f, 0.5f, 1.0f}) {
            const double fc = ThumbPad::phaserHz(x);
            const auto h = impulseResponse(ThumbPad::Phaser, x, 0.0f, 0.5);
            // a notch is a point of the response at least 40 dB down and lower than its neighbours
            std::vector<double> found;
            double prev2 = 1.0, prev1 = 1.0, f1 = 0.0;
            for (double hz = 20.0; hz < 20000.0; hz *= 1.002) {
                const double m = magAt(h, hz, kSr, 16384);
                if (prev1 < prev2 && prev1 <= m && db(prev1) < -40.0) found.push_back(f1);
                prev2 = prev1; prev1 = m; f1 = hz;
            }
            CHECK_MSG(found.size() == 3, "X %.1f: %d notches", x, (int)found.size());
            if (found.size() == 3) {
                CHECK_MSG(std::fabs(found[1] / fc - 1.0) < 0.003, "X %.1f: middle notch at %.1f Hz, expected %.1f", x, found[1], fc);
                const double t = std::tan(kPi * fc / kSr);
                CHECK_NEAR(found[0], kSr / kPi * std::atan(t * std::tan(kPi / 12.0)), 0.004 * found[0]);
                CHECK_NEAR(found[2], kSr / kPi * std::atan(t * std::tan(5.0 * kPi / 12.0)), 0.004 * found[2]);
            }
            CHECK_NEAR(db(magAt(h, 5.0, kSr, 16384)), 0.0, 0.1);      // far below the notches nothing is lost
        }
        CHECK_NEAR(ThumbPad::phaserHz(0.0), 200.0, 1e-9);
        CHECK_NEAR(ThumbPad::phaserHz(1.0), 4000.0, 1e-6);
        // Y top: feedback 0.85 lifts what lies between the notches: at the bottom 0.5 * (1 + 1 / 0.15) = +11.7 dB
        const auto h = impulseResponse(ThumbPad::Phaser, 1.0f, 1.0f, 0.5);
        CHECK_NEAR(db(magAt(h, 5.0, kSr)), 20.0 * std::log10(0.5 * (1.0 + 1.0 / 0.15)), 0.2);
    }
    {   // ring: the input times a sine. Y top: only the two side frequencies are left, each at half
        const double f = 1000.0;
        for (float x : {0.5f, 1.0f}) {
            const double fc = ThumbPad::ringHz(x);
            auto p = pad(ThumbPad::Ring, x, 1.0f);
            const auto out = run(*p, tone(f, 1.0, 0.5f));
            const size_t from = 8192, n = 32768;
            CHECK_NEAR(levelDb(out.l, from, n, std::fabs(f - fc)), db(0.25), 0.1);
            CHECK_NEAR(levelDb(out.l, from, n, f + fc), db(0.25), 0.1);
            CHECK(levelDb(out.l, from, n, f) < -80.0);
        }
        CHECK_NEAR(ThumbPad::ringHz(0.0), 20.0, 1e-9);
        CHECK_NEAR(ThumbPad::ringHz(1.0), 4000.0, 1e-6);
        // Y bottom: untouched. Half way: the input at half, the sides at a quarter each
        auto dry = pad(ThumbPad::Ring, 0.7f, 0.0f);
        const auto in = busy(0.5);
        CHECK(same(run(*dry, in), in));
        auto half = pad(ThumbPad::Ring, 1.0f, 0.5f);
        const auto out = run(*half, tone(f, 1.0, 0.5f));
        CHECK_NEAR(levelDb(out.l, 8192, 32768, f), db(0.25), 0.1);
        CHECK_NEAR(levelDb(out.l, 8192, 32768, 5000.0), db(0.125), 0.1);
    }
}

TEST(thumb_pad_echo_and_wash_at_the_corners) {
    {   // echo: X is the time (30 ms to 900 ms), Y the feedback (0 to 0.92). The wet signal is the input plus its echoes.
        struct Case { float x, y; } cases[] = {{0.0f, 0.5f}, {1.0f, 0.5f}, {0.0f, 1.0f}, {0.5f, 0.0f}};
        for (auto& cs : cases) {
            const double ms = ThumbPad::echoMs(cs.x), fb = ThumbPad::echoFeedback(cs.y);
            const size_t d = (size_t)std::llround(ms * 0.001 * kSr);
            const auto h = impulseResponse(ThumbPad::Echo, cs.x, cs.y, 3.0);
            CHECK(h[0] == 1.0f);
            // (a time that is not a whole number of samples spreads the echo over the sixteen samples around it)
            float before = 0.0f;
            for (size_t i = 1; i + 10 < d; ++i) before = std::max(before, std::fabs(h[i]));
            CHECK_MSG(before < 1e-6f, "X %.1f: %.6f before the echo", cs.x, before);
            double first = 0.0, second = 0.0;
            for (size_t i = d - 9; i <= d + 9; ++i) first += h[i];
            for (size_t i = 2 * d - 18; i <= 2 * d + 18; ++i) second += h[i];
            CHECK_MSG(std::fabs(first - 1.0) < 0.01 && std::fabs(h[d]) > 0.5f, "X %.1f: first echo %.4f at sample %d (%.3f)", cs.x, first, (int)d, h[d]);
            CHECK_MSG(std::fabs(second - fb) < 0.01, "X %.1f Y %.1f: second echo %.4f, feedback %.4f", cs.x, cs.y, second, fb);
        }
        CHECK(ThumbPad::echoMs(0.0) == 30.0);
        CHECK_NEAR(ThumbPad::echoMs(1.0), 900.0, 1e-9);
        const auto h = impulseResponse(ThumbPad::Echo, 0.0f, 0.5f, 0.5);
        CHECK(h[1440] == 1.0f && h[2880] == 0.46f);           // 30 ms is 1440 samples: to the sample, to the bit
        CHECK_NEAR(h[4320], 0.46 * 0.46, 1e-6);
    }
    {   // wash: X is the decay time (0.4 s to 8 s), measured the usual way: the energy curve's fall from -5 to -35 dB, doubled
        for (float x : {0.0f, 0.5f, 1.0f}) {
            const double want = ThumbPad::washSeconds(x);
            auto h = impulseResponse(ThumbPad::Wash, x, 1.0f, want * 1.5 + 0.5);
            h[0] = 0.0f;        // the input itself
            std::vector<double> e(h.size() + 1, 0.0);
            for (size_t i = h.size(); i-- > 0;) e[i] = e[i + 1] + (double)h[i] * h[i];
            size_t t5 = 0, t35 = 0;
            for (size_t i = 0; i < h.size(); ++i) {
                const double d = 10.0 * std::log10(e[i] / e[0] + 1e-30);
                if (t5 == 0 && d <= -5.0) t5 = i;
                if (t35 == 0 && d <= -35.0) { t35 = i; break; }
            }
            const double rt60 = 2.0 * (double)(t35 - t5) / kSr;
            printf("    thumb pad wash: X %.1f claims %.2f s, measured %.2f s\n", x, want, rt60);
            CHECK_MSG(t35 > t5 && std::fabs(rt60 / want - 1.0) < 0.12, "X %.1f: decay %.2f s, expected %.2f s", x, rt60, want);
        }
        CHECK_NEAR(ThumbPad::washSeconds(0.0), 0.4, 1e-9);
        CHECK_NEAR(ThumbPad::washSeconds(1.0), 8.0, 1e-9);
        // Y is the brightness: at the bottom the tail has lost its top
        auto bright = pad(ThumbPad::Wash, 0.6f, 1.0f), dark = pad(ThumbPad::Wash, 0.6f, 0.0f);
        const auto in = noise(0.2, 0.5f);
        Stereo padded = in;
        append(padded, silence(1.0));
        const auto b = run(*bright, padded), d = run(*dark, padded);
        const size_t from = in.l.size() + 9600, n = 16384;
        const double bHigh = levelDb(b.l, from, n, 7000.0) - levelDb(b.l, from, n, 300.0), dHigh = levelDb(d.l, from, n, 7000.0) - levelDb(d.l, from, n, 300.0);
        // (single frequencies of a noise tail are ragged, so several are averaged)
        double bSum = 0.0, dSum = 0.0;
        for (double hz = 6000.0; hz < 9000.0; hz += 97.0) { bSum += std::pow(10.0, levelDb(b.l, from, n, hz) / 10.0); dSum += std::pow(10.0, levelDb(d.l, from, n, hz) / 10.0); }
        (void)bHigh; (void)dHigh;
        CHECK_MSG(10.0 * std::log10(dSum / bSum) < -15.0, "the dark tail is only %.1f dB down at 6 to 9 kHz", 10.0 * std::log10(dSum / bSum));
        // the two sides differ: it is a stereo tail from a mono input
        Stereo mono = in;
        mono.r = mono.l;
        append(mono, silence(0.5));
        auto w = pad(ThumbPad::Wash, 0.6f, 0.8f);
        const auto st = run(*w, mono);
        CHECK(tu::maxAbsDiff(st.l, st.r) > 0.01);
    }
}

TEST(thumb_pad_drive_crusher_and_vowel_at_the_corners) {
    {   // drive, X left: 0 dB of drive, as good as clean at a low level
        auto p = pad(ThumbPad::Drive, 0.0f, 1.0f);
        const auto out = run(*p, tone(1000.0, 0.5, 0.05f));
        const double f1 = levelDb(out.l, 8192, 8192, 1000.0), f3 = levelDb(out.l, 8192, 8192, 3000.0);
        CHECK_NEAR(f1, db(0.05), 0.05);
        CHECK(f3 - f1 < -65.0);
        // X right: 36 dB of drive turns a sine nearly square (third harmonic at a third: -9.5 dB), level kept in hand
        auto hot = pad(ThumbPad::Drive, 1.0f, 1.0f);
        const auto sq = run(*hot, tone(500.0, 0.5, 0.5f));
        const double h1 = levelDb(sq.l, 8192, 8192, 500.0), h3 = levelDb(sq.l, 8192, 8192, 1500.0), h2 = levelDb(sq.l, 8192, 8192, 1000.0);
        CHECK_NEAR(h3 - h1, -9.9, 0.5);
        CHECK(h2 - h1 < -70.0);                 // symmetric: no even harmonics
        CHECK(peak(sq, 4800) < 0.6f && peak(sq, 4800) > 0.4f);
        // Y bottom: the tone control is a low-pass at 800 Hz (6 dB an octave): the 9th harmonic of 500 Hz is 15 dB lower
        auto dull = pad(ThumbPad::Drive, 1.0f, 0.0f);
        const auto du = run(*dull, tone(500.0, 0.5, 0.5f));
        const double cut = (levelDb(du.l, 8192, 8192, 4500.0) - levelDb(du.l, 8192, 8192, 500.0)) - (levelDb(sq.l, 8192, 8192, 4500.0) - h1);
        CHECK_NEAR(cut, -13.7, 1.0);            // 4500 Hz against 500 Hz through one pole at 800 Hz
        // aliasing: a 5 kHz tone driven hard. Its 7th harmonic (35 kHz) folds to 13 kHz, its 9th (45 kHz) to 3 kHz.
        auto al = pad(ThumbPad::Drive, 0.75f, 1.0f);
        const auto a = run(*al, tone(5000.0, 0.5, 0.5f));
        std::vector<float> naive(a.l.size());
        const double g = std::pow(10.0, ThumbPad::driveDb(0.75) / 20.0), norm = std::tanh(0.5) / std::tanh(0.5 * g);
        for (size_t i = 0; i < naive.size(); ++i) naive[i] = (float)(norm * std::tanh(g * 0.5 * std::sin(kTwoPi * 5000.0 * (double)i / kSr)));
        const double ours3 = levelDb(a.l, 8192, 8192, 3000.0) - levelDb(a.l, 8192, 8192, 5000.0), plain3 = levelDb(naive, 8192, 8192, 3000.0) - levelDb(naive, 8192, 8192, 5000.0);
        const double ours13 = levelDb(a.l, 8192, 8192, 13000.0) - levelDb(a.l, 8192, 8192, 5000.0), plain13 = levelDb(naive, 8192, 8192, 13000.0) - levelDb(naive, 8192, 8192, 5000.0);
        printf("    thumb pad drive, 5 kHz at 27 dB of drive: alias at 3 kHz %.1f dB (plain tanh %.1f dB), at 13 kHz %.1f dB (plain %.1f dB)\n", ours3, plain3, ours13, plain13);
        CHECK(ours3 < plain3 - 6.0 && ours13 < plain13 - 3.0);
    }
    {   // crusher, top right: every sample, 16 bits: the input to within half a 16-bit step
        const auto in = busy(0.5);
        auto clean = pad(ThumbPad::Crush, 1.0f, 1.0f);
        const auto out = run(*clean, in);
        CHECK(tu::maxAbsDiff(out.l, in.l) <= 0.5 / 32768.0 + 1e-9);
        // X left: a new sample 500 times a second: held for 96 samples
        CHECK_NEAR(ThumbPad::crushHz(0.0, kSr), 500.0, 1e-9);
        CHECK_NEAR(ThumbPad::crushHz(1.0, kSr), kSr, 1e-6);
        auto slow = pad(ThumbPad::Crush, 0.0f, 1.0f);
        const auto held = run(*slow, noise(0.5, 0.5f));
        size_t changes = 0, shortest = 1000000, last = 0;
        for (size_t i = 1; i < held.l.size(); ++i)
            if (held.l[i] != held.l[i - 1]) { if (changes > 0) shortest = std::min(shortest, i - last); last = i; ++changes; }
        CHECK_MSG(shortest == 96 && changes == held.l.size() / 96 - (held.l.size() % 96 == 0 ? 1 : 0), "%d changes, shortest hold %d", (int)changes, (int)shortest);
        // Y bottom: two bits: steps of half of full scale
        CHECK(ThumbPad::crushBits(0.0) == 2.0 && ThumbPad::crushBits(1.0) == 16.0);
        auto coarse = pad(ThumbPad::Crush, 1.0f, 0.0f);
        const auto two = run(*coarse, noise(0.5, 0.99f));
        bool onGrid = true;
        int seen[5] = {};
        for (float v : two.l) { const float k = v * 2.0f; onGrid = onGrid && k == std::round(k) && std::fabs(k) <= 2.0f; if (onGrid) ++seen[(int)k + 2]; }
        CHECK(onGrid && seen[0] > 0 && seen[1] > 0 && seen[2] > 0 && seen[3] > 0 && seen[4] > 0);
    }
    {   // vowel: the three formants of the vowel X picks, at their levels. Y top = sharp (Q 18)
        for (float x : {0.0f, 0.5f, 1.0f}) {
            double hz[3], gain[3];
            ThumbPad::vowelFormants(x, hz, gain);
            for (int f = 0; f < 3; ++f) {
                const double g = gainAt(ThumbPad::Vowel, x, 1.0f, hz[f]);
                CHECK_MSG(std::fabs(g - db(gain[f])) < 2.0, "X %.1f formant %d (%.0f Hz): %.1f dB, expected %.1f dB", x, f + 1, hz[f], g, db(gain[f]));
            }
            // between the first two formants the level falls well below the first
            const double valley = gainAt(ThumbPad::Vowel, x, 1.0f, std::sqrt(hz[0] * hz[1]));
            CHECK_MSG(valley < -12.0, "X %.1f: valley at %.1f dB", x, valley);
        }
        double hz[3], gain[3];
        ThumbPad::vowelFormants(0.0, hz, gain);
        CHECK(hz[0] == 650.0 && hz[1] == 1080.0 && hz[2] == 2650.0 && gain[0] == 1.0);
        ThumbPad::vowelFormants(1.0, hz, gain);
        CHECK_NEAR(hz[0], 350.0, 1e-9);
        CHECK_NEAR(hz[1], 600.0, 1e-9);
        // Y bottom = broad (Q 4): the first formant is still at its level, the valley is shallower
        ThumbPad::vowelFormants(0.0, hz, gain);
        CHECK_NEAR(gainAt(ThumbPad::Vowel, 0.0f, 0.0f, 650.0), 0.0, 3.5);
        CHECK(gainAt(ThumbPad::Vowel, 0.0f, 0.0f, 1700.0) > gainAt(ThumbPad::Vowel, 0.0f, 1.0f, 1700.0) + 5.0);
    }
}

TEST(thumb_pad_tape_speed_stops_and_catches_up_an_octave_high) {
    const double f = 440.0;
    const auto in = tone(f, 3.0, 0.5f);
    {   // centre: normal speed, the input to the bit
        auto p = pad(ThumbPad::TapeSpeed, 0.5f, 0.3f);
        CHECK(same(run(*p, in), in));
        CHECK(ThumbPad::tapeSpeed(0.0) == 0.0 && ThumbPad::tapeSpeed(0.5) == 1.0 && ThumbPad::tapeSpeed(1.0) == 2.0);
    }
    {   // X left with little inertia: stopped and silent in a few milliseconds. Then X right: the half second that went
        // by plays at double speed, an octave up, and after that it is the input again, to the bit
        auto p = pad(ThumbPad::TapeSpeed, 0.5f, 0.0f);
        const size_t stopAt = 24000 / kQuantum, goAt = 48000 / kQuantum;
        const auto out = run(*p, in, {}, [&](size_t q) {
            if (q == stopAt) p->pX->set(0.0f);
            if (q == goAt) p->pX->set(1.0f);
        });
        bool exact = true;
        for (size_t i = 0; i < 24000; ++i) exact = exact && out.l[i] == in.l[i];
        CHECK(exact);
        CHECK_MSG(peak(out, 24000 + 4800, 48000) < 1.0e-6f, "stopped, it still gives %.2e", peak(out, 24000 + 4800, 48000));
        const double hz = tu::measureHz(out.l, 48000 + 6000, 48000 + 20000, kSr);
        CHECK_MSG(std::fabs(1200.0 * std::log2(hz / (2.0 * f))) < 3.0, "catching up at %.2f Hz", hz);
        CHECK_NEAR(tu::rmsOf(out.l, 48000 + 6000, 48000 + 20000), 0.5 / std::sqrt(2.0), 0.005);
        exact = true;
        for (size_t i = 48000 + 28000; i < out.l.size(); ++i) exact = exact && out.l[i] == in.l[i];
        CHECK(exact);
        // nowhere a jump: the stop, the start and the landing are all smooth (the octave-up sine's own step is 0.058)
        CHECK_MSG(maxStep(out.l) < 0.5f * (float)(kTwoPi * 2.0 * f / kSr) * 1.25f, "largest step %.4f", maxStep(out.l));
        printf("    thumb pad tape: catching up at %.2f Hz for 880; largest step %.4f\n", hz, maxStep(out.l));
    }
    {   // Y top: a second of inertia: half a second after X goes left the tape is still turning, well below pitch
        auto p = pad(ThumbPad::TapeSpeed, 0.5f, 1.0f);
        const auto out = run(*p, in, {}, [&](size_t q) { if (q == 24000 / kQuantum) p->pX->set(0.0f); });
        const double hz = tu::measureHz(out.l, 24000 + 22000, 24000 + 26000, kSr);
        CHECK_NEAR(hz, f * std::exp(-0.5), 12.0);      // speed after half a second of a one-second time constant
        CHECK_NEAR(ThumbPad::tapeInertiaSeconds(1.0), 1.0, 1e-9);
        CHECK_NEAR(ThumbPad::tapeInertiaSeconds(0.0), 0.005, 1e-12);
    }
}

TEST(thumb_pad_switching_and_fast_pad_movement_make_no_jumps) {
    // A 220 Hz sine at 0.5 goes in. First, what each effect does at rest in its four corners: the largest change
    // from one sample to the next. Then the pad is played as hard as it can be: X and Y thrown to a new place
    // every quantum (750 times a second), the effect switched in and out every 50 ms. The hard playing may not
    // make a step larger than the bound stated beside each effect.
    const float amp = 0.5f;
    const auto in = tone(220.0, 3.0, amp);
    const float own = amp * (float)(kTwoPi * 220.0 / kSr);        // 0.0144
    // (Where the bound is far above the sine's own step, that is the effect and not the playing: a resonant filter
    // thrown across its range rings at up to 8 kHz, the drive at 36 dB makes square edges, the crusher makes
    // steps by design, the ring at 4 kHz and the flanger's teeth are fast signals.)
    struct Bound { int mode; float moving; } bounds[] = {
        {ThumbPad::LowPass, 0.22f}, {ThumbPad::HighPass, 0.20f}, {ThumbPad::Flanger, 0.22f}, {ThumbPad::Phaser, 0.05f},
        {ThumbPad::Echo, 0.18f}, {ThumbPad::Wash, 0.08f}, {ThumbPad::Drive, 0.45f}, {ThumbPad::Crush, 1.1f},
        {ThumbPad::Ring, 0.30f}, {ThumbPad::Vowel, 0.03f}, {ThumbPad::TapeSpeed, 0.03f}};
    for (auto& b : bounds) {
        float rest = 0.0f;
        for (float x : {0.0f, 1.0f})
            for (float y : {0.0f, 1.0f}) {
                auto p = pad(b.mode, x, y);
                const auto out = run(*p, in);
                rest = std::max(rest, maxStep(out.l, 48000));
                CHECK(allFinite(out));
            }
        // twice: with the pad thrown about, and with the effect switched in and out as well
        auto p = pad(b.mode, 0.5f, 0.5f);
        dsp::Noise rnd(99u + (uint32)b.mode);
        Stereo out = run(*p, in, {}, [&](size_t q) {
            const float u = rnd.uni(), v = rnd.uni();
            p->pX->set(u < 0.3f ? 0.0f : (u > 0.7f ? 1.0f : u));
            p->pY->set(v < 0.3f ? 0.0f : (v > 0.7f ? 1.0f : v));
        });
        append(out, run(*p, in, {}, [&](size_t q) {
            const float u = rnd.uni(), v = rnd.uni();
            p->pX->set(u < 0.3f ? 0.0f : (u > 0.7f ? 1.0f : u));
            p->pY->set(v < 0.3f ? 0.0f : (v > 0.7f ? 1.0f : v));
            if (q % 37 == 0) p->pActive->set(p->pActive->getBool() ? 0.0f : 1.0f);
        }));
        const float moving = std::max(maxStep(out.l), maxStep(out.r));
        // zipper noise would show as steps at the moments the controls arrive (the first sample of each quantum):
        // those steps are no larger than the ones in between
        float arriving = 0.0f, between = 0.0f;
        for (size_t i = 1; i < out.l.size(); ++i) {
            const float d = std::fabs(out.l[i] - out.l[i - 1]);
            if (i % kQuantum == 0) arriving = std::max(arriving, d); else between = std::max(between, d);
        }
        CHECK_MSG(arriving <= between * 1.02f + 1e-5f, "%s: a step of %.4f where the controls arrive, %.4f elsewhere", p->pMode->display().c_str(), arriving, between);
        printf("    thumb pad %-16s largest step at rest %.4f, played hard %.4f (bound %.2f; the sine alone %.4f)\n", p->pMode->display().c_str(), rest, moving, b.moving, own);
        CHECK(allFinite(out));
        CHECK_MSG(moving <= b.moving, "%s: step %.4f when played hard, bound %.2f", p->pMode->display().c_str(), moving, b.moving);
        CHECK_MSG(peak(out) < 8.0f, "%s peaks at %.2f", p->pMode->display().c_str(), peak(out));
    }
    {   // the Active switch alone, on an effect that changes the sound a lot: a 10 ms fade each way
        auto p = pad(ThumbPad::HighPass, 1.0f, 0.0f);
        const auto out = run(*p, in, {}, [&](size_t q) { if (q % 75 == 0 && q > 0) p->pActive->set(p->pActive->getBool() ? 0.0f : 1.0f); });
        // the fade moves the gain 1/480 a sample between two signals at most 0.5 apart... and the sine's own step
        CHECK_MSG(maxStep(out.l) <= own + 2.0f * amp / 480.0f + 0.001f, "largest step %.4f", maxStep(out.l));
        // switched out it is the input exactly (after the 10 ms), switched in it is the effect
        bool exact = true;
        for (size_t i = 75 * 64 + 481; i < 150 * 64; ++i) exact = exact && out.l[i] == in.l[i];
        CHECK(exact);
        CHECK(peak(out, 150 * 64 + 2000, 225 * 64) < 0.02f);
        CHECK(p->meter(0) == 0.0f || p->meter(0) == 1.0f);
    }
    {   // choosing another effect while it sounds: out with a fade, in with a fade
        auto p = pad(ThumbPad::Ring, 1.0f, 1.0f);
        dsp::Noise rnd(5u);
        const auto out = run(*p, in, {}, [&](size_t q) { if (q % 60 == 59) p->pMode->set((float)(int)(rnd.uni() * 10.99f)); });
        CHECK(allFinite(out));
        CHECK_MSG(maxStep(out.l) < 0.55f, "largest step %.4f", maxStep(out.l));
    }
}

TEST(thumb_pad_mix_zero_and_switched_out_are_the_input) {
    const auto in = busy(1.0);
    for (int mode = 0; mode < ThumbPad::NumModes; ++mode) {
        auto p = pad(mode, 0.8f, 0.9f);
        p->pMix->set(0.0f);
        CHECK_MSG(same(run(*p, in), in), "%s with the mix at 0", p->pMode->display().c_str());
        auto q = pad(mode, 0.8f, 0.9f);
        q->pActive->set(0.0f);
        CHECK_MSG(same(run(*q, in), in), "%s switched out", q->pMode->display().c_str());
        CHECK(q->meter(0) == 0.0f);
        // and with the mix up and the effect in, it does something (the tape at its centre is the one that does not)
        auto w = pad(mode, 0.8f, 0.9f);
        if (mode != ThumbPad::TapeSpeed) CHECK_MSG(!same(run(*w, in), in), "%s does nothing", w->pMode->display().c_str());
        CHECK(std::string(ThumbPad::axisName(mode, 0)).size() > 2 && std::string(ThumbPad::axisName(mode, 1)).size() > 2);
    }
    // half mix: half way between the input and the effect
    auto full = pad(ThumbPad::Drive, 0.6f, 0.6f), half = pad(ThumbPad::Drive, 0.6f, 0.6f);
    half->pMix->set(0.5f);
    const auto a = run(*full, in), b = run(*half, in);
    double worst = 0.0;
    for (size_t i = 0; i < a.l.size(); ++i) worst = std::max(worst, std::fabs(0.5 * ((double)a.l[i] + in.l[i]) - b.l[i]));
    CHECK(worst < 1e-6);
}
