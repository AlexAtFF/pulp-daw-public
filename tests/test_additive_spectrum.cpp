// Sheaf, the additive synthesizer: where the partials are, how loud each one is, and that nothing else is there.
#include "SheafTestUtil.h"
using namespace pulp;
using namespace sheaft;

TEST(sheaf_every_partial_is_on_its_frequency_at_every_sample_rate) {
    double worstAll = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (float inharm : {0.0f, 0.3f})
            for (int key : {33, 60, 87}) {
                auto s = plainSheaf();
                flatten(*s);
                s->pInharm->set(inharm);
                const double f0 = keyToHz(key), stiff = Sheaf::stiffness(inharm);
                const auto want = harmonicsOf(f0, sr, 256, stiff);
                const auto out = hold(*s, key, sr, 48000);
                const auto got = lineFrequencies(out, 6000, sr, want);
                double worst = 0.0;
                int worstK = 0;
                for (size_t i = 0; i < want.size(); ++i) {
                    if (want[i] > 0.9 * 0.5 * sr) break;       // above this they are fading out; still checked for level elsewhere
                    const double c = got[i] > 0.0 ? std::fabs(centsOff(got[i], want[i])) : 1.0e9;
                    if (c > worst) { worst = c; worstK = (int)i + 1; }
                }
                CHECK_MSG(worst < 1.0, "%.0f Hz, key %d, inharmonicity %.1f: partial %d is %.3f cents off", sr, key, inharm, worstK, worst);
                CHECK(want.size() >= (key == 33 && inharm == 0.0f ? 256u : 10u));
                worstAll = std::max(worstAll, worst);
            }
    printf("    every partial (up to 256 per note, three keys, three rates, with and without stretch): worst %.4f cents\n", worstAll);
    // the stretch is the stiff string's: partial k at k * sqrt((1 + B k^2) / (1 + B)), the fundamental staying put
    CHECK_NEAR(Sheaf::stretched(1.0, 0.003), 1.0, 1e-12);
    CHECK_NEAR(Sheaf::stretched(10.0, 0.003), 10.0 * std::sqrt(1.3 / 1.003), 1e-12);
    CHECK_NEAR(Sheaf::stiffness(1.0f), 0.01, 1e-9);
}

TEST(sheaf_makes_nothing_that_is_not_a_partial) {
    // The brightest sound there is: 256 partials, all at the same level. Everything in the output must sit on a
    // partial frequency below half the sample rate. An alias would land somewhere else.
    double worstAll = -300.0;
    int notes = 0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int key = 21; key <= 127; key += 5) {
            auto s = plainSheaf();
            flatten(*s);
            Drive d(*s, sr);
            NoteEvent e;
            e.type = NoteEvent::On; e.key = key; e.id = 1;
            e.fineCents = (float)((key * 37) % 100) - 50.0f;        // off the keys too, so partials come near the limit at all distances
            d.ev(0, e);
            d.run(40000);
            const double f0 = keyToHz(key + e.fineCents * 0.01);
            const auto want = harmonicsOf(f0, sr, 256);
            double where = 0.0;
            const double stray = strayDb(d.L, 6000, 32768, sr, want, 12, &where);
            CHECK_MSG(stray < -100.0, "%.0f Hz, key %d: something at %.1f Hz, %.1f dB below the strongest partial", sr, key, where, stray);
            worstAll = std::max(worstAll, stray);
            // and the partials that should be there are: the last one below 90% of half the rate is at full level
            int last = 0;
            for (size_t i = 0; i < want.size(); ++i) if (want[i] <= 0.9 * 0.5 * sr) last = (int)i + 1;
            if (last >= 2) CHECK_MSG(std::fabs(harmonicDb(d.L, 6000, 16384, f0, last, sr)) < 0.05, "%.0f Hz, key %d: partial %d is at %.2f dB", sr, key, last, harmonicDb(d.L, 6000, 16384, f0, last, sr));
            // nothing at or past 98%: the first partial that would be there is not
            const double beyond = f0 * (double)(want.size() + 1);
            if (want.size() < 256 && beyond < 0.5 * sr - 20.0)
                CHECK_MSG(db(ampAt(d.L, 6000, 16384, beyond, sr) / ampAt(d.L, 6000, 16384, f0, sr)) < -100.0, "%.0f Hz, key %d: a partial at %.0f Hz", sr, key, beyond);
            ++notes;
        }
    printf("    %d notes from key 21 to 126 at three rates: strongest non-partial component %.1f dB below the strongest partial\n", notes, worstAll);
}

TEST(sheaf_partials_leave_at_the_top_instead_of_folding_back_when_the_pitch_rises) {
    // The brightest sound, bent up two octaves in three seconds: partial after partial reaches the limit (98% of
    // half the sample rate) and is faded out before it gets there. Two places must stay empty all the way:
    // the top 1.5% of the spectrum, where a partial let past the limit (or folded back from just beyond half the
    // sample rate) would be, and the stretch below the fundamental, which an alias on its way down would cross.
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        auto s = plainSheaf();
        flatten(*s);
        Drive d(*s, sr);
        d.on(0, sr > 50000.0 ? 72 : 60, 1);
        const int key = sr > 50000.0 ? 72 : 60;
        const int quanta = (int)(3.0 * sr) / kQuantum;
        for (int q = 0; q < quanta; ++q) { d.bend = 24.0f * (float)q / (float)quanta; d.run(kQuantum); }
        const int n = 8192;
        double worstBelow = -300.0, worstTop = -300.0;
        int frames = 0;
        for (size_t at = 4096; at + (size_t)n < d.L.size(); at += 2048, ++frames) {
            const double f0 = keyToHz(key + 24.0 * (double)at / (double)(quanta * kQuantum));      // the fundamental at the start of the frame; it only rises
            const auto m = spectrumFine(d.L, at, n);
            double below = 0.0, top = 0.0, all = 0.0;
            for (int b = 4; b < n / 2; ++b) {
                const double e = m[(size_t)b] * m[(size_t)b];
                all += e;
                if (b * sr / n < 0.6 * f0) below += e;
                if (b * sr / n > 0.985 * 0.5 * sr) top += e;
            }
            worstBelow = std::max(worstBelow, 10.0 * std::log10(std::max(below, 1.0e-30) / all));
            worstTop = std::max(worstTop, 10.0 * std::log10(std::max(top, 1.0e-30) / all));
        }
        CHECK_MSG(worstBelow < -80.0, "%.0f Hz: %.1f dB of the sound is below the fundamental during the sweep", sr, worstBelow);
        CHECK_MSG(worstTop < -65.0, "%.0f Hz: %.1f dB of the sound is in the top 1.5%% of the spectrum during the sweep", sr, worstTop);
        printf("    %.0f Hz, 256 flat partials swept up two octaves (%d frames): below the fundamental %.1f dB, above 98.5%% of half the rate %.1f dB, relative to the sound\n",
               sr, frames, worstBelow, worstTop);
        CHECK(allFinite(d.L));
        // the partials fade out, they are not switched off: no step bigger than the densest moment's own
        CHECK_MSG(maxStep(d.L, (size_t)(1.0 * sr), d.L.size()) <= maxStep(d.L, 0, (size_t)(1.0 * sr)) * 1.2f, "a step of %.4f late in the sweep (%.4f early)", maxStep(d.L, (size_t)(1.0 * sr), d.L.size()), maxStep(d.L, 0, (size_t)(1.0 * sr)));
    }
}

TEST(sheaf_a_long_note_neither_drifts_nor_fades) {
    // The oscillators are points turned round a circle sixty-four samples at a time. After a minute (2.9 million
    // turns each) every partial is still at its level and on its frequency.
    const double sr = 48000.0, f0 = keyToHz(57);
    auto s = plainSheaf(Sheaf::Saw, 16);
    Drive d(*s, sr);
    d.on(0, 57, 1);
    d.run((int64)(61.0 * sr));
    std::vector<double> want;
    for (int k = 1; k <= 16; ++k) want.push_back(f0 * k);
    const size_t early = (size_t)(1.0 * sr), late = (size_t)(59.0 * sr);
    const auto f1 = lineFrequencies(d.L, early, sr, want), f2 = lineFrequencies(d.L, late, sr, want);
    double worstCents = 0.0, worstDb = 0.0;
    for (int k = 1; k <= 16; ++k) {
        worstCents = std::max({worstCents, std::fabs(centsOff(f1[(size_t)k - 1], want[(size_t)k - 1])), std::fabs(centsOff(f2[(size_t)k - 1], want[(size_t)k - 1]))});
        worstDb = std::max(worstDb, std::fabs(db(ampFine(d.L, late, 16384, f0 * k, sr) / ampFine(d.L, early, 16384, f0 * k, sr))));
    }
    CHECK_MSG(worstCents < 0.01, "after a minute a partial is %.4f cents off", worstCents);
    CHECK_MSG(worstDb < 0.001, "after a minute a partial's level has moved %.5f dB", worstDb);
    CHECK_MSG(strayDb(d.L, late, 32768, sr, want) < -110.0, "after a minute: %.1f dB of something else", strayDb(d.L, late, 32768, sr, want));
    printf("    a note held for a minute: partials within %.5f cents and %.5f dB of where they started\n", worstCents, worstDb);
}
