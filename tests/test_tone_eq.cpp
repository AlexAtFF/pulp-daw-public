// Ten Band: the graphic equaliser against its sliders and against its own curve.
#include "ToneTest.h"
using namespace pulp;
using namespace tu;
using namespace tt;

namespace {

// Gain in dB at several frequencies from one pass: a sum of sines goes in, each is measured on the way out.
std::vector<double> multiGain(Plugin& p, const std::vector<double>& freqs, double sr) {
    Stereo in = silence(1.6, sr);
    const double amp = 0.5 / (double)freqs.size();
    for (size_t i = 0; i < in.l.size(); ++i) {
        double v = 0.0;
        for (double f : freqs) v += amp * std::sin(kTwoPi * f * (double)i / sr);
        in.l[i] = in.r[i] = (float)v;
    }
    p.reset();
    const auto out = runAt(p, in, sr);
    const size_t n = 32768, from = out.l.size() - n;
    std::vector<double> g;
    for (double f : freqs) g.push_back(levelDbAt(out.l, from, n, f, sr) - levelDbAt(in.l, from, n, f, sr));
    return g;
}
std::vector<double> centres(double sr) {
    std::vector<double> f;
    for (int b = 0; b < TenBand::kBands; ++b) f.push_back(std::min(TenBand::centreHz(b), 0.45 * sr));
    return f;
}
void setAll(TenBand& eq, const std::vector<float>& db) { for (int b = 0; b < TenBand::kBands; ++b) eq.pGain[b]->set(db[(size_t)b]); }

}

TEST(ten_band_centres_follow_the_sliders) {
    const std::vector<std::vector<float>> patterns = {
        {3, -2, 4, 0, -5, 6, -3, 2, 5, -4},             // moderate, uneven
        {0, 0, 0, 0, 0, 12, 0, 0, 0, 0},                // one band on its own
        {6, 4, 2, 0, -2, -3, -2, 0, 3, 6},              // a smile
        {-6, -6, -6, -6, -6, -6, -6, -6, -6, -6},
        {9, 9, 9, -9, -9, -9, 9, 9, 9, -9},
    };
    double worstCurve = 0.0, worstMeasured = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        for (auto& pat : patterns) {
            TenBand eq;
            eq.prepare(sr);
            setAll(eq, pat);
            const auto f = centres(sr);
            const auto measured = multiGain(eq, f, sr);
            for (int b = 0; b < TenBand::kBands; ++b) {
                const double curve = eq.magnitudeDb(f[(size_t)b]);
                // the curve passes through the slider settings, and the sound follows the curve
                CHECK_MSG(std::fabs(curve - pat[(size_t)b]) < 0.05, "%.0f Hz band %d: curve %.3f dB, slider %.1f dB", sr, b, curve, pat[(size_t)b]);
                CHECK_MSG(std::fabs(measured[(size_t)b] - pat[(size_t)b]) < 0.25, "%.0f Hz band %d: measured %.3f dB, slider %.1f dB", sr, b, measured[(size_t)b], pat[(size_t)b]);
                worstCurve = std::max(worstCurve, std::fabs(curve - pat[(size_t)b]));
                worstMeasured = std::max(worstMeasured, std::fabs(measured[(size_t)b] - pat[(size_t)b]));
            }
        }
    }
    FIG("ten band: at the band centres the curve is within %.3f dB of the sliders and the measured gain within %.3f dB (five patterns, three rates)", worstCurve, worstMeasured);
    // the output gain is on top of that
    TenBand eq;
    eq.prepare(48000.0);
    eq.pOut->set(-6.0f);
    eq.pGain[5]->set(4.0f);
    CHECK_NEAR(gainDbAt(eq, 1000.0), -2.0, 0.05);
    CHECK_NEAR(gainDbAt(eq, 125.0), -6.0, 0.05);
}

TEST(ten_band_measured_response_matches_its_curve_between_the_bands) {
    // at and between the centres, and outside the outermost bands
    TenBand eq;
    eq.prepare(48000.0);
    setAll(eq, {5, -3, 0, 6, -6, 2, 8, -4, 3, -7});
    std::vector<double> f;
    for (double hz = 22.0; hz < 21000.0; hz *= std::sqrt(2.0)) f.push_back(hz);
    const auto a = multiGain(eq, std::vector<double>(f.begin(), f.begin() + 10), 48000.0);
    const auto b = multiGain(eq, std::vector<double>(f.begin() + 10, f.end()), 48000.0);
    double worst = 0.0;
    for (size_t i = 0; i < f.size(); ++i) {
        const double measured = i < 10 ? a[i] : b[i - 10], curve = eq.magnitudeDb(f[i]);
        CHECK_MSG(std::fabs(measured - curve) < 0.15, "%.0f Hz: measured %.3f dB, curve %.3f dB", f[i], measured, curve);
        worst = std::max(worst, std::fabs(measured - curve));
    }
    FIG("ten band: measured response within %.3f dB of magnitudeDb() from 22 Hz to 20 kHz", worst);
}

TEST(ten_band_neighbours_join_up_and_boost_mirrors_cut) {
    double ripple6 = 0.0, ripple12 = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        for (float level : {6.0f, 12.0f, -12.0f}) {
            TenBand eq;
            eq.prepare(sr);
            setAll(eq, std::vector<float>(10, level));
            // every slider alike: the curve stays near that level all the way from the first centre to the last
            double worst = 0.0;
            for (double hz = 31.25; hz <= 16000.0; hz *= std::pow(2.0, 1.0 / 12.0)) worst = std::max(worst, std::fabs(eq.magnitudeDb(hz) - level));
            CHECK_MSG(worst < (std::fabs(level) > 6.5f ? 0.9 : 0.45), "%.0f Hz, all at %.0f dB: ripple %.2f dB", sr, level, worst);
            (std::fabs(level) > 6.5f ? ripple12 : ripple6) = std::max(std::fabs(level) > 6.5f ? ripple12 : ripple6, worst);
        }
        // constant Q: a cut is the mirror image of the same boost, at every frequency
        TenBand up, down;
        up.prepare(sr); down.prepare(sr);
        const std::vector<float> pat = {4, -6, 9, 0, -3, 7, -8, 2, 5, -5};
        std::vector<float> neg;
        for (float v : pat) neg.push_back(-v);
        setAll(up, pat);
        setAll(down, neg);
        for (double hz = 20.0; hz < 0.45 * sr; hz *= 1.3) CHECK_NEAR(up.magnitudeDb(hz), -down.magnitudeDb(hz), 1.0e-6);
        // and the width of a band does not depend on how far it is moved: the half-height points of one raised
        // band are the same distance apart for 4 dB as for 12 dB
        double widths[2];
        int k = 0;
        for (float gain : {4.0f, 12.0f}) {
            TenBand one;
            one.prepare(sr);
            one.pGain[5]->set(gain);
            double lo = 1000.0, hi = 1000.0;
            while (one.magnitudeDb(lo) > gain * 0.5) lo /= 1.002;
            while (one.magnitudeDb(hi) > gain * 0.5) hi *= 1.002;
            widths[k++] = std::log2(hi / lo);
        }
        CHECK_MSG(std::fabs(widths[0] / widths[1] - 1.0) < 0.12, "%.0f Hz: a band is %.3f octaves wide at 4 dB and %.3f at 12 dB", sr, widths[0], widths[1]);
        if (sr == 48000.0) FIG("ten band: one raised band is %.2f octaves wide between its half-height points at 4 dB and %.2f at 12 dB", widths[0], widths[1]);
    }
    FIG("ten band: all sliders alike: ripple between 31 Hz and 16 kHz is %.2f dB at 6 dB and %.2f dB at 12 dB (worst of three rates)", ripple6, ripple12);
}

TEST(ten_band_extreme_settings_stay_accurate_and_bounded) {
    double worst = 0.0, most = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        const std::vector<std::vector<float>> patterns = {
            {15, -15, 15, -15, 15, -15, 15, -15, 15, -15}, {15, 15, 15, 15, 15, 15, 15, 15, 15, 15},
            {-15, -15, -15, -15, -15, 15, 15, 15, 15, 15}, {0, 0, 0, 15, -15, 0, 0, 0, 0, 0},
        };
        for (auto& pat : patterns) {
            TenBand eq;
            eq.prepare(sr);
            setAll(eq, pat);
            const auto f = centres(sr);
            for (int b = 0; b < TenBand::kBands; ++b) {
                const double err = std::fabs(eq.magnitudeDb(f[(size_t)b]) - pat[(size_t)b]);
                CHECK_MSG(err < 0.5, "%.0f Hz band %d: off by %.2f dB at an extreme setting", sr, b, err);
                worst = std::max(worst, err);
            }
            double g[TenBand::kBands];
            eq.filterGains(g);
            for (double v : g) { CHECK(std::fabs(v) <= TenBand::kMaxFilterDb); most = std::max(most, std::fabs(v)); }
            const auto out = runAt(eq, noise(0.3, 0.5f, sr), sr);
            CHECK(allFinite(out));
            CHECK(peakOf(out.l) < 0.5f * 80.0f);
        }
    }
    FIG("ten band: at the most extreme settings the centres are within %.2f dB; the largest filter gain behind them is %.1f dB", worst, most);
}

TEST(ten_band_flat_is_bit_transparent) {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        TenBand eq;
        eq.prepare(sr);
        const auto in = noise(0.3, 0.7f, sr);
        const auto out = runAt(eq, in, sr);
        CHECK(out.l == in.l && out.r == in.r);
    }
    // sliders moved away and back to zero: once the filters have stopped ringing it is bit-transparent again
    TenBand eq;
    eq.prepare(48000.0);
    const auto in = noise(3.0, 0.7f);
    const auto out = run(eq, in, {}, [&](size_t i) {
        if (i == 9600) { eq.pGain[0]->set(12.0f); eq.pGain[4]->set(-9.0f); eq.pGain[9]->set(15.0f); }
        if (i == 28800) for (auto* p : eq.pGain) p->set(0.0f);
    });
    CHECK(maxAbsDiff(out.l, in.l) > 0.1);
    size_t lastDifferent = 0;
    for (size_t i = 0; i < out.l.size(); ++i) if (out.l[i] != in.l[i] || out.r[i] != in.r[i]) lastDifferent = i;
    CHECK_MSG(lastDifferent < 28800 + 48000, "still not transparent %.2f s after the sliders went back to zero", (double)(lastDifferent - 28800) / 48000.0);
    FIG("ten band: bit-transparent again %.0f ms after every slider is back at zero", (double)(lastDifferent - 28800) / 48.0);
}

TEST(ten_band_slider_jumps_do_not_click) {
    struct Move { double hz; int band; float from, to; } moves[] = {{100.0, 2, 0, 15}, {100.0, 2, 15, -15}, {4000.0, 7, -15, 15}, {1000.0, 5, 12, 0}, {60.0, 0, 0, -15}};
    for (auto& m : moves) {
        TenBand eq;
        eq.prepare(48000.0);
        eq.pGain[m.band]->set(m.from);
        const auto in = sineWave(m.hz, 0.6, 0.2f);
        const auto out = run(eq, in, {}, [&](size_t i) { if (i == 9600) eq.pGain[m.band]->set(m.to); });
        // no sample-to-sample step is bigger than the tone's own steepest step at the larger of the two gains
        const float allowed = maxStep(in.l) * dbToGain(std::max(m.from, m.to)) * 1.15f;
        CHECK_MSG(maxStep(out.l, 9000) <= allowed, "%.0f Hz, band %d %g -> %g dB: step %.5f, allowed %.5f", m.hz, m.band, m.from, m.to, maxStep(out.l, 9000), allowed);
        // and it gets where it is going
        CHECK_NEAR(db(peakOf(out.l, out.l.size() - 4800) / 0.2), eq.magnitudeDb(m.hz), 0.1);
    }
    // the scope tap holds the latest output
    TenBand eq;
    eq.prepare(48000.0);
    const auto out = run(eq, noise(0.2, 0.5f));
    float tap[256];
    CHECK(eq.readScope(tap, 256) == 256);
    CHECK_NEAR(tap[255], 0.5f * (out.l.back() + out.r.back()), 1e-6);
}
