// Sheaf: speed. Sixteen notes on one core, for the plain case the brief names (128 partials each), for the
// heaviest factory preset, and for everything switched on at once.
// This file sorts before the other Sheaf tests so that the figures are taken at the start of the run: the
// same code measures about half as fast when it runs after ten seconds of other tests in the same process
// (seen here, and by the Sextet / Ripple tests before). The limits below hold in both places.
#include "SheafTestUtil.h"
using namespace pulp;
using namespace sheaft;

namespace {

// Sixteen held notes; returns how many times faster than real time they render (best of three stretches).
double sixteenNotes(Sheaf& s, double seconds, int lowKey = 36, int step = 3) {
    Drive d(s);
    for (int i = 0; i < 16; ++i) d.on(i * 10, lowKey + i * step, (uint32)(i + 1));
    d.run(4800);
    CHECK_MSG(s.activeVoices() == 16, "%d voices", s.activeVoices());
    double took = 1.0e9;
    for (int rep = 0; rep < 3; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        d.run((int64)(seconds * 48000));
        took = std::min(took, secondsSince(t0));
    }
    CHECK(s.activeVoices() == 16);
    CHECK(allFinite(d.L));
    return seconds / took;
}

} // namespace

TEST(sheaf_sixteen_notes_run_far_faster_than_real_time) {
#ifdef NDEBUG
    const double seconds = 2.0;
#else
    const double seconds = 0.25;
#endif
    {   // 16 voices of 128 partials: low notes, so that all 128 fit below half the sample rate (key 24 to 39: 33 to 78 Hz)
        auto s = plainSheaf(Sheaf::Saw, 128);
        s->pVoices->set(16.0f);
        const double x = sixteenNotes(*s, seconds, 24, 1);
        printf("    16 notes x 128 partials (2048 sines), steady:              %6.1f x real time (%.1f%% of one core)\n", x, 100.0 / x);
#ifdef NDEBUG
        CHECK_MSG(x > 8.0, "16 x 128 partials: only %.1f x real time", x);
#endif
        s->forceScalar = true;
        const double plain = sixteenNotes(*s, seconds, 24, 1);
        printf("    the same without SSE (plain C++ oscillator loop):          %6.1f x real time\n", plain);
    }
    {   // the same with vibrato (every sine retuned every 64 samples), the filter, formants and tilt all moving
        auto s = plainSheaf(Sheaf::Saw, 128);
        s->pVoices->set(16.0f);
        s->lfo[0].pitch->set(20.0f);
        s->lfo[1].tilt->set(3.0f);
        s->lfo[1].filter->set(1.0f);
        s->pFilterType->set((float)Sheaf::LowPass);
        s->pCutoff->set(6000.0f);
        s->pFormGain[0]->set(9.0f);
        s->pFormGain[1]->set(6.0f);
        s->spec[1].shape->set((float)Sheaf::Vowel);
        s->pMorph->set(0.5f);
        const double x = sixteenNotes(*s, seconds, 24, 1);
        printf("    16 x 128 partials, everything moving (vibrato, filter ...): %6.1f x real time (%.1f%% of one core)\n", x, 100.0 / x);
#ifdef NDEBUG
        CHECK_MSG(x > 5.0, "16 x 128 moving partials: only %.1f x real time", x);
#endif
    }
    {   // the most there is: 16 voices of 256 partials, everything moving
        auto s = plainSheaf(Sheaf::Saw, 256);
        s->pVoices->set(16.0f);
        s->lfo[0].pitch->set(20.0f);
        s->lfo[1].tilt->set(3.0f);
        s->pFilterType->set((float)Sheaf::LowPass);
        s->pCutoff->set(6000.0f);
        s->pFormGain[0]->set(9.0f);
        const double x = sixteenNotes(*s, seconds, 24, 1);
        printf("    16 x 256 partials (4096 sines), everything moving:         %6.1f x real time (%.1f%% of one core)\n", x, 100.0 / x);
#ifdef NDEBUG
        CHECK_MSG(x > 2.5, "16 x 256 moving partials: only %.1f x real time", x);
#endif
    }
    {   // The tables behind the spectra are rebuilt on the audio thread when their parameters move. The worst case:
        // both spectra (as vowels, the costliest shape), the formants and the filter curve, all moved every quantum.
        Sheaf s;
        s.spec[0].shape->set((float)Sheaf::Vowel);
        s.spec[1].shape->set((float)Sheaf::Vowel);
        s.pMorph->set(0.5f);
        s.pFormGain[0]->set(6.0f);
        Drive d(s);
        d.on(0, 48, 1);
        d.run(4800);
        const int quanta = (int)(seconds * 48000) / kQuantum;
        double took[2] = {1.0e9, 1.0e9};
        for (int rep = 0; rep < 3; ++rep)
            for (int moving = 0; moving < 2; ++moving) {
                const auto t0 = std::chrono::steady_clock::now();
                for (int q = 0; q < quanta; ++q) {
                    if (moving) {
                        const float u = (float)(q % 100) / 99.0f;
                        s.spec[0].colour->set(u);
                        s.spec[1].tilt->set(-6.0f + 12.0f * u);
                        s.pFormFreq[0]->set(400.0f + 2000.0f * u);
                        s.pSlope->set(12.0f + 24.0f * u);
                    }
                    d.run(kQuantum);
                }
                took[moving] = std::min(took[moving], secondsSince(t0));
            }
        const double cost = 100.0 * (took[1] - took[0]) / (quanta * kQuantum / 48000.0);
        printf("    rebuilding all four tables every quantum (parameters automated) costs %.1f%% of one core\n", cost);
        CHECK(allFinite(d.L));
#ifdef NDEBUG
        CHECK_MSG(cost < 20.0, "table rebuilds cost %.1f%% of a core", cost);
#endif
    }
    // every factory preset, sixteen notes each: find the heaviest, then time that one properly
    Sheaf proto;
    std::string heaviest;
    double slowest = 1.0e9;
    for (auto& pr : proto.factoryPresets()) {
        Sheaf s;
        s.loadState(pr.state);
        s.pVoices->set(16.0f);
        s.pCoarse->set(0.0f);
        const double x = sixteenNotes(s, seconds * 0.25);
        if (x < slowest) { slowest = x; heaviest = pr.name; }
    }
    Sheaf s;
    loadPreset(s, heaviest);
    s.pVoices->set(16.0f);
    s.pCoarse->set(0.0f);
    const double x = sixteenNotes(s, seconds);
    printf("    16 notes of the heaviest preset (%s):%*s%6.1f x real time (%.1f%% of one core)\n", heaviest.c_str(), std::max(1, 22 - (int)heaviest.size()), "", x, 100.0 / x);
#ifdef NDEBUG
    CHECK_MSG(x > 3.0, "%s: only %.1f x real time", heaviest.c_str(), x);
#endif
}
