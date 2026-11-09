// Milestone 14: the compressor stage of Lid, the limiter.
#include "Util.h"
#include "pulp/Effects.h"

using namespace pulp;
using namespace tu;

namespace {
// Runs a stereo tone of the given peak level through the plugin and returns the settled output peak in dB.
double settledDb(Lid& lid, double levelDb, double seconds = 1.0) {
    const double sr = 48000.0;
    const int n = (int)(seconds * sr) / kQuantum * kQuantum;
    std::vector<float> l((size_t)n), r((size_t)n);
    const double amp = std::pow(10.0, levelDb / 20.0);
    for (int i = 0; i < n; ++i) l[(size_t)i] = r[(size_t)i] = (float)(amp * std::sin(2.0 * 3.14159265358979 * 1000.0 * i / sr));
    ProcessContext c;
    for (int at = 0; at + kQuantum <= n; at += kQuantum) { c.io[0] = l.data() + at; c.io[1] = r.data() + at; lid.process(c); }
    return 20.0 * std::log10(peakOf(l, (size_t)n - 9600, (size_t)n) + 1.0e-12);
}
}

TEST(lid_compressor_follows_its_ratio_and_is_out_of_the_way_when_off) {
    Lid lid;
    lid.prepare(48000.0);
    // off (as every project made before has it): a tone under the ceiling passes at its own level
    CHECK_NEAR(settledDb(lid, -6.0), -6.0, 0.01);
    CHECK(lid.meter(2) == 0.0f);
    // on, hard knee, threshold -18, ratio 4: what is over the threshold comes out a quarter as far over
    lid.pComp->set(1.0f);
    lid.pCompThresh->set(-18.0f);
    lid.pCompRatio->set(4.0f);
    lid.pCompKnee->set(0.0f);
    for (double in : {-30.0, -18.0, -12.0, -6.0, 0.0}) {
        lid.reset();
        const double want = in <= -18.0 ? in : -18.0 + (in + 18.0) / 4.0;
        CHECK_NEAR(settledDb(lid, in), want, 0.25);
    }
    CHECK_NEAR(lid.meter(2), 13.5f, 0.3f);          // the last tone was 18 dB over: 13.5 dB taken off
    CHECK(lid.meter(0) >= lid.meter(2));
    // the limiter still holds the ceiling after the compressor's make-up
    lid.pGain->set(24.0f);
    lid.pCeiling->set(-1.0f);
    lid.reset();
    CHECK(settledDb(lid, 0.0) <= -0.99);
    // switched off again, the reduction it held dies away and the tone is back at its level
    lid.pGain->set(0.0f);
    lid.pCeiling->set(-0.3f);
    lid.pComp->set(0.0f);
    CHECK_NEAR(settledDb(lid, -6.0, 3.0), -6.0, 0.02);
    // the compressor presets load and say so
    int withComp = 0;
    for (auto& pr : lid.factoryPresets()) { lid.loadState(pr.state); withComp += lid.pComp->getBool() ? 1 : 0; }
    CHECK(withComp >= 3);
    CHECK(lid.paramGroups().size() == 3);
}

TEST(lid_compressor_attack_and_release_take_their_time) {
    Lid lid;
    lid.prepare(48000.0);
    lid.pComp->set(1.0f);
    lid.pCompThresh->set(-20.0f);
    lid.pCompRatio->set(30.0f);
    lid.pCompKnee->set(0.0f);
    lid.pCompAttack->set(50.0f);
    lid.pCompRelease->set(200.0f);
    const double sr = 48000.0;
    std::vector<float> l(48000), r(48000);
    for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = (float)(0.5 * std::sin(2.0 * 3.14159265358979 * 1000.0 * (double)i / sr));
    ProcessContext c;
    const int64 allocs = Rt::allocationsInRt.load();
    float at50 = 0.0f, settled = 0.0f;
    {
        Rt::Scope rt;
        for (int at = 0; at + kQuantum <= (int)l.size(); at += kQuantum) {
            c.io[0] = l.data() + at; c.io[1] = r.data() + at;
            lid.process(c);
            if (at <= 2400 && at + kQuantum > 2400) at50 = lid.meter(2);
            settled = lid.meter(2);
        }
    }
    CHECK(Rt::allocationsInRt.load() == allocs);
    // after one attack time the reduction is about 63 % of where it ends
    CHECK_MSG(at50 > settled * 0.55f && at50 < settled * 0.72f, "after 50 ms %.2f dB of %.2f", at50, settled);
    // silence: after one release time about 37 % is left
    std::vector<float> zl(9600 + kQuantum, 0.0f), zr = zl;
    float left = 0.0f;
    for (int at = 0; at + kQuantum <= 9600; at += kQuantum) { c.io[0] = zl.data() + at; c.io[1] = zr.data() + at; lid.process(c); left = lid.meter(2); }
    CHECK_MSG(left > settled * 0.30f && left < settled * 0.44f, "after 200 ms %.2f dB of %.2f", left, settled);
}
