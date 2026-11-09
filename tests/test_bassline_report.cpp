// Prints measured figures for the notes (set BASSLINE_REPORT=1). Asserts nothing; the checks are in
// the other test_bassline_* files.
#include "BasslineTestUtil.h"
#include "pulp/Clatter.h"
#include "pulp/LowFour.h"
#include "pulp/Squelch.h"
#include "pulp/Thud.h"
#include "pulp/Twine.h"
#include <chrono>
#include <cstdlib>
using namespace pulp;
using namespace bt;

namespace {
template <class T>
void presetLevels(const char* tag) {
    T proto;
    double lo = 1.0e9, hi = -1.0e9;
    for (auto& pr : proto.factoryPresets()) {
        T t;
        t.loadState(pr.state);
        const auto out = oneNote(t, 60, 24000, 96000);
        const double pk = db(peakOf(out));
        lo = std::min(lo, pk);
        hi = std::max(hi, pk);
        printf("    [%s] %-20s peak %6.2f dBFS  rms(0.5 s) %6.2f  length(-40 dB) %.3f s\n", tag, pr.name.c_str(), pk, db(rmsOf(out, 0, 24000)), lengthSeconds(out, 48000.0));
    }
    printf("    [%s] %d presets, peaks from %.2f to %.2f dBFS\n", tag, (int)proto.factoryPresets().size(), lo, hi);
}
}

TEST(bassline_report_preset_levels) {
    if (!std::getenv("BASSLINE_REPORT")) return;
    presetLevels<Squelch>("squelch");
    presetLevels<Twine>("twine");
    presetLevels<LowFour>("lowfour");
    presetLevels<Thud>("thud");
    presetLevels<Clatter>("clatter");
}

TEST(bassline_report_parameters) {
    if (!std::getenv("BASSLINE_REPORT")) return;
    const std::shared_ptr<Plugin> all[] = {std::make_shared<Squelch>(), std::make_shared<Twine>(), std::make_shared<LowFour>(), std::make_shared<Thud>(), std::make_shared<Clatter>()};
    for (auto& p : all) {
        printf("    [params] %s (%s): %d parameters, %d presets\n", p->displayName(), p->typeId(), (int)p->params().size(), (int)p->factoryPresets().size());
        for (auto& g : p->paramGroups())
            for (auto& id : g.ids) {
                const ParamInfo& i = p->param(id)->info;
                std::string range;
                if (!i.choices.empty()) { for (auto& c : i.choices) range += (range.empty() ? "" : " / ") + c; }
                else { char b[96]; snprintf(b, sizeof b, "%g to %g %s%s", i.min, i.max, i.unit.c_str(), i.steps > 0 ? " (whole steps)" : ""); range = b; }
                printf("    [params] | %s | `%s` | %s | %s | %s |\n", g.title.c_str(), i.id.c_str(), i.name.c_str(), range.c_str(), p->param(id)->displayFor(i.def).c_str());
            }
        std::string names;
        for (auto& pr : p->factoryPresets()) names += (names.empty() ? "" : ", ") + pr.name;
        printf("    [params] presets: %s\n", names.c_str());
    }
}

TEST(bassline_report_processing_cost) {
    if (!std::getenv("BASSLINE_REPORT")) return;
    // Wall-clock cost of 10 s of sound at 48 kHz with every voice in use (one core, this build, this machine).
    struct Load { const char* name; std::shared_ptr<Plugin> p; int notes; int spacing; };
    auto twine = std::make_shared<Twine>();
    twine->pDecay->set(20.0f);
    twine->pRing->set(1.0f);
    auto bass = std::make_shared<LowFour>();
    bass->pSustain->set(1.0f);
    auto thud = std::make_shared<Thud>();
    thud->pDecay->set(8000.0f);
    thud->pChoke->set(0.0f);
    auto kit = std::make_shared<Clatter>();
    kit->pModel->set((float)Clatter::Cymbal);
    kit->pDecay->set(1.0f);
    const Load loads[] = {{"squelch (1 voice)", std::make_shared<Squelch>(), 1, 0}, {"twine (32 strings ringing)", twine, 32, 64}, {"lowfour (8 strings ringing)", bass, 8, 64},
                          {"thud (6 hits ringing)", thud, 6, 64}, {"clatter (16 cymbals, re-hit every 0.5 s)", kit, 16, 64}};
    for (auto& l : loads) {
        Drive d(*l.p, 48000.0);
        uint32 id = 1;
        for (int rep = 0; rep < 20; ++rep)
            for (int i = 0; i < l.notes; ++i) d.on((int64)rep * 24000 + (int64)i * l.spacing, 40 + i, id++);
        const auto t0 = std::chrono::steady_clock::now();
        d.run(480000);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("    [cost] %-42s %.2f %% of one core (%.1f ms per second of sound)\n", l.name, 10.0 * s, 100.0 * s);
    }
}
