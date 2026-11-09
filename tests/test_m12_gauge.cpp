// Milestone 12: Gauge, the analyser. It must change nothing and measure correctly.
#include "Util.h"
#include "pulp/Analysis.h"

using namespace pulp;
using namespace tu;

namespace {
// Feeds whole quanta of a stereo signal through the plugin, in place.
void run(Gauge& g, std::vector<float>& l, std::vector<float>& r, bool playing = true) {
    ProcessContext c;
    c.playing = playing;
    for (size_t at = 0; at + kQuantum <= l.size(); at += kQuantum) {
        c.io[0] = l.data() + at;
        c.io[1] = r.data() + at;
        g.process(c);
    }
}
std::vector<float> sineWave(double hz, double seconds, double sr, double amp) {
    std::vector<float> out((size_t)(seconds * sr) / kQuantum * kQuantum);
    for (size_t i = 0; i < out.size(); ++i) out[i] = (float)(amp * std::sin(2.0 * 3.14159265358979 * hz * (double)i / sr));
    return out;
}
double lufsOf(double hz, double ampL, double ampR, double sr = 48000.0, double seconds = 5.0, int which = Gauge::ShortTerm) {
    Gauge g;
    g.prepare(sr);
    auto l = sineWave(hz, seconds, sr, ampL), r = sineWave(hz, seconds, sr, ampR);
    run(g, l, r);
    return which < 0 ? g.integratedLufs() : (double)g.meter(which);
}
}

TEST(gauge_changes_nothing_and_reports_no_latency) {
    Gauge g;
    g.prepare(48000.0);
    CHECK(g.latencySamples() == 0);
    dsp::Noise n(5);
    std::vector<float> l(6400), r(6400);
    for (size_t i = 0; i < l.size(); ++i) { l[i] = n.next() * 0.7f; r[i] = n.next() * 0.3f; }
    const auto l0 = l, r0 = r;
    const int64 allocs = Rt::allocationsInRt.load();
    {
        Rt::Scope rt;
        run(g, l, r);
    }
    CHECK(Rt::allocationsInRt.load() == allocs);
    CHECK(l == l0 && r == r0);
    // what it remembers is what went through
    std::vector<float> a(256), b(256);
    CHECK(g.readStereo(a.data(), b.data(), 256) == 256);
    CHECK(std::equal(a.begin(), a.end(), l0.end() - 256) && std::equal(b.begin(), b.end(), r0.end() - 256));
    CHECK(g.factoryPresets().size() >= 5);
}

TEST(gauge_loudness_follows_the_standard) {
    const double minus23 = std::pow(10.0, -23.0 / 20.0);
    // a 997 Hz sine on both sides at -23 dBFS reads -23 LUFS; on one side at full scale, -3.01
    CHECK_NEAR(lufsOf(997.0, minus23, minus23), -23.0, 0.1);
    CHECK_NEAR(lufsOf(997.0, minus23, minus23, 48000.0, 5.0, Gauge::Momentary), -23.0, 0.1);
    CHECK_NEAR(lufsOf(997.0, minus23, minus23, 48000.0, 5.0, -1), -23.0, 0.1);
    CHECK_NEAR(lufsOf(997.0, 1.0, 0.0), -3.01, 0.1);
    CHECK_NEAR(lufsOf(997.0, minus23, minus23, 44100.0), -23.0, 0.1);
    CHECK_NEAR(lufsOf(997.0, minus23, minus23, 96000.0), -23.0, 0.1);
    // the weighting: bass counts for less, treble for a little more
    const double low = lufsOf(40.0, minus23, minus23), high = lufsOf(8000.0, minus23, minus23);
    CHECK_MSG(low < -27.0 && low > -40.0, "40 Hz reads %.2f", low);
    CHECK_MSG(high > -20.5 && high < -18.0, "8 kHz reads %.2f", high);
    // integrated loudness leaves silence out: five seconds of tone and twenty of nothing still read the tone
    Gauge g;
    g.prepare(48000.0);
    auto l = sineWave(997.0, 5.0, 48000.0, 0.1), r = l;
    run(g, l, r);
    std::vector<float> zl(48000 * 20 / kQuantum * kQuantum, 0.0f), zr = zl;
    run(g, zl, zr);
    CHECK_NEAR(g.integratedLufs(), -20.0, 0.2);
    CHECK(g.meter(Gauge::Momentary) <= -100.0f);
    // a quiet passage more than 10 LU down is left out too
    auto ql = sineWave(997.0, 10.0, 48000.0, 0.01), qr = ql;
    run(g, ql, qr);
    CHECK_NEAR(g.integratedLufs(), -20.0, 0.3);
    // it starts again when playback starts
    run(g, zl, zr, false);
    auto l2 = sineWave(997.0, 3.0, 48000.0, 0.5), r2 = l2;
    run(g, l2, r2, true);
    CHECK_NEAR(g.integratedLufs(), -6.0, 0.2);
    Gauge fresh;
    fresh.prepare(48000.0);
    CHECK(fresh.integratedLufs() <= -119.0);
}

TEST(gauge_peak_rms_and_correlation) {
    Gauge g;
    g.prepare(48000.0);
    auto l = sineWave(440.0, 2.0, 48000.0, 0.5), r = sineWave(440.0, 2.0, 48000.0, 0.25);
    run(g, l, r);
    CHECK_NEAR(g.meter(Gauge::PeakL), -6.02, 0.05);
    CHECK_NEAR(g.meter(Gauge::PeakR), -12.04, 0.05);
    CHECK_NEAR(g.meter(Gauge::Rms), 10.0 * std::log10(0.5 * (0.125 + 0.03125)), 0.1);
    CHECK_NEAR(g.meter(Gauge::Correlation), 1.0, 0.01);
    // one side upside down: -1
    Gauge inv;
    inv.prepare(48000.0);
    auto il = sineWave(440.0, 2.0, 48000.0, 0.5), ir = il;
    for (auto& v : ir) v = -v;
    run(inv, il, ir);
    CHECK_NEAR(inv.meter(Gauge::Correlation), -1.0, 0.01);
    // unrelated noise on each side: about 0
    Gauge un;
    un.prepare(48000.0);
    dsp::Noise a(1), b(2);
    std::vector<float> nl(96000), nr(96000);
    for (size_t i = 0; i < nl.size(); ++i) { nl[i] = a.next() * 0.3f; nr[i] = b.next() * 0.3f; }
    run(un, nl, nr);
    CHECK(std::abs(un.meter(Gauge::Correlation)) < 0.1f);
    // peaks fall after their hold, unless told to stay
    std::vector<float> zl(48000 * 4 / kQuantum * kQuantum, 0.0f), zr = zl;
    run(g, zl, zr);
    CHECK(g.meter(Gauge::PeakL) < -40.0f);
    Gauge held;
    held.prepare(48000.0);
    held.pHold->set(1.0f);
    auto hl = sineWave(440.0, 1.0, 48000.0, 0.5), hr = hl;
    run(held, hl, hr);
    run(held, zl, zr);
    CHECK_NEAR(held.meter(Gauge::PeakL), -6.02, 0.05);
    CHECK(held.meter(Gauge::Rms) < -50.0f);        // the level itself has died away
}
