// Speed: sixteen-note chords on one core, for both synths. These run first (the file sorts before
// the other two) so the figures are taken on a fresh process.
#include "FmwtTestUtil.h"
#include "pulp/Ripple.h"
#include "pulp/Sextet.h"
using namespace pulp;
using namespace ft;

namespace {

template <class Synth> void loadPreset(Synth& s, const std::string& name) {
    for (auto& pr : s.factoryPresets()) if (pr.name == name) { s.loadState(pr.state); return; }
    CHECK_MSG(false, "no preset %s", name.c_str());
}

int tableNamed(const char* name) {
    for (int i = 0; i < Ripple::numBuiltinTables(); ++i) if (Ripple::builtinTable(i).name == name) return i;
    CHECK_MSG(false, "no table called %s", name);
    return 0;
}

} // namespace

TEST(sextet_sixteen_note_chords_run_far_faster_than_real_time) {
    struct Case { const char* name; const char* preset; };
    const Case cases[] = {{"Warm Strings (6 carriers, 6 feedbacks)", "Warm Strings"}, {"Aurora Pad (3 pairs, 2 LFOs)", "Aurora Pad"},
                          {"Brass Section (3 pairs, feedback)", "Brass Section"}, {"every operator, all 36 routing cells", nullptr}};
    for (auto& c : cases) {
        Sextet s;
        if (c.preset) loadPreset(s, c.preset);
        else {
            s.pAlgo->set(0.0f);
            for (int i = 0; i < Sextet::kOps; ++i) {
                s.op[i].level->set(0.5f);
                s.op[i].sustain->set(1.0f);
                s.op[i].out->set(1.0f);
                s.op[i].coarse->set((float)(1 + i));
                for (int j = 0; j < Sextet::kOps; ++j) s.matrix[i][j]->set(0.1f);
            }
        }
        Drive d(s);
        for (int i = 0; i < 16; ++i) d.on(i * 10, 36 + i * 3, (uint32)(i + 1));
        d.run(4800);
        CHECK_MSG(s.activeVoices() == 16, "%d voices", s.activeVoices());
        // the best of three two-second stretches, so that something else using the machine does not spoil the figure
        const double seconds = 2.0;
        double took = 1.0e9;
        for (int rep = 0; rep < 3; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            d.run((int64)(seconds * 48000));
            took = std::min(took, secondsSince(t0));
        }
        CHECK(s.activeVoices() == 16);
        printf("    16 notes, %-42s %5.1f x real time (%.1f%% of one core)\n", c.name, seconds / took, 100.0 * took / seconds);
#ifdef NDEBUG
        CHECK_MSG(seconds / took > 3.0, "%s: only %.1f x real time", c.name, seconds / took);
#endif
    }
}

TEST(ripple_sixteen_note_chords_with_unison_run_far_faster_than_real_time) {
    struct Case { const char* name; int which; };
    const Case cases[] = {{"2 oscillators x 7 unison, sub, noise, 24 dB filter, effects", 0}, {"Tidal Pad (5 + 3 unison, chorus)", 1},
                          {"Choir of Wires (5 unison + 1)", 2}, {"Default (one oscillator)", 3}};
    for (auto& c : cases) {
        Ripple r;
        if (c.which == 0) {
            for (int o = 0; o < 2; ++o) {
                r.osc[o].table->set((float)tableNamed(o ? "Vowels" : "Harmonic Sweep"));
                r.osc[o].level->set(0.7f);
                r.osc[o].uni->set(7.0f);
                r.osc[o].detune->set(25.0f);
                r.osc[o].phase->set(1.0f);
                r.osc[o].posLfo[o]->set(0.4f);
            }
            r.pSubLevel->set(0.4f);
            r.pNoise->set(0.1f);
            r.pFType->set(2.0f);
            r.pCutoff->set(3000.0f);
            r.pFEnv->set(2.0f);
            r.pChorus->set(0.5f);
            r.pDelay->set(0.3f);
        }
        if (c.which == 1) loadPreset(r, "Tidal Pad");
        if (c.which == 2) loadPreset(r, "Choir of Wires");
        r.pSustain->set(1.0f);
        Drive d(r);
        for (int i = 0; i < 16; ++i) d.on(i * 10, 36 + i * 3, (uint32)(i + 1));
        d.run(4800);
        CHECK_MSG(r.activeVoices() == 16, "%d voices", r.activeVoices());
        // the best of three two-second stretches, so that something else using the machine does not spoil the figure
        const double seconds = 2.0;
        double took = 1.0e9;
        for (int rep = 0; rep < 3; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            d.run((int64)(seconds * 48000));
            took = std::min(took, secondsSince(t0));
        }
        CHECK(r.activeVoices() == 16);
        printf("    16 notes, %-60s %5.1f x real time (%.1f%% of one core)\n", c.name, seconds / took, 100.0 * took / seconds);
#ifdef NDEBUG
        CHECK_MSG(seconds / took > 3.0, "%s: only %.1f x real time", c.name, seconds / took);
#endif
    }
}
