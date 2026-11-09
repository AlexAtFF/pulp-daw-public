// Bend (waveshaper with a drawable curve) and Middle and Sides.
#include "ToneTest.h"
#include <set>
using namespace pulp;
using namespace tu;
using namespace tt;

namespace {
const double kSr = 48000.0;

size_t peakIndex(const std::vector<float>& x) {
    size_t at = 0;
    for (size_t i = 0; i < x.size(); ++i) if (std::fabs(x[i]) > std::fabs(x[at])) at = i;
    return at;
}
std::shared_ptr<Bend> bend(const Bend::Points& curve, int symmetry, int over) {
    auto b = std::make_shared<Bend>();
    b->prepare(kSr);
    b->setCurve(curve);
    b->pSymmetry->set((float)symmetry); b->pOver->set((float)over);
    return b;
}
const Bend::Points kSoft = {{0.0f, 0.0f}, {0.3f, 0.3f}, {0.6f, 0.55f}, {0.8f, 0.68f}, {1.0f, 0.75f}};
const Bend::Points kLopsided = {{-1.0f, -0.6f}, {-0.5f, -0.42f}, {0.0f, 0.1f}, {0.4f, 0.45f}, {0.75f, 0.8f}, {1.0f, 0.9f}};
// A stereo signal with an equal part and an opposite part.
Stereo wide(double seconds, float amp, double sr = kSr) {
    Stereo s = noise(seconds, amp, sr, 21u);
    return s;
}
}

// ------------------------------------------------------------------ Bend

TEST(bend_curve_goes_through_its_points_smoothly_without_overshoot) {
    dsp::Noise rnd(404u);
    for (int trial = 0; trial < 60; ++trial) {
        Bend::Points pts;
        const int n = 2 + (int)(rnd.uni() * 14.99f);
        for (int i = 0; i < n; ++i) pts.push_back({-1.0f + 2.0f * ((float)i + 0.1f + 0.8f * rnd.uni()) / (float)n, rnd.next()});
        Bend b;
        b.setCurve(pts);
        const auto got = b.curve();
        CHECK((int)got.size() == n);
        const double zero = b.transfer(0.0);
        (void)zero;
        b.pSymmetry->set(Bend::Mirror);     // mirrored: what comes out for a positive input is the curve itself
        for (size_t i = 0; i + 1 < got.size(); ++i) {
            const double x0 = got[i].first, x1 = got[i + 1].first, y0 = got[i].second, y1 = got[i + 1].second;
            if (x0 > 0.0) CHECK_NEAR(b.transfer(x0), y0, 1.0e-9);
            // between two points the curve stays between their levels
            for (int k = 1; k < 20; ++k) {
                const double x = x0 + (x1 - x0) * k / 20.0;
                if (x <= 0.0) continue;
                const double y = b.transfer(x);
                CHECK_MSG(y >= std::min(y0, y1) - 1.0e-9 && y <= std::max(y0, y1) + 1.0e-9, "trial %d: %.6f at %.4f is outside %.6f..%.6f", trial, y, x, y0, y1);
            }
            // and it has no corner at a point: the slope just before is the slope just after
            if (i > 0 && x0 > 0.01) {
                const double e = 1.0e-8;
                const double before = (b.transfer(x0) - b.transfer(x0 - e)) / e, after = (b.transfer(x0 + e) - b.transfer(x0)) / e;
                CHECK_MSG(std::fabs(before - after) < 1.0e-3 * (1.0 + std::fabs(before)), "trial %d: slope %.5f before and %.5f after the point at %.4f", trial, before, after, x0);
            }
        }
        // flat beyond the first and the last point
        CHECK_NEAR(b.transfer(1.0), got.back().first < 1.0f ? got.back().second : b.transfer(1.0), 1.0e-9);
    }
    // the straight line is the input itself, to the last bit of a float
    Bend line;
    for (int sym = 0; sym < 2; ++sym) {
        line.pSymmetry->set((float)sym);
        for (int i = 0; i < 2000; ++i) { const float x = rnd.next(); CHECK((float)line.transfer(x) == x); }
    }
    // tidying: sorted, clamped, at most 16, never fewer than two
    Bend t;
    t.setCurve({{0.5f, 0.2f}, {-0.5f, 3.0f}, {-2.0f, -0.4f}, {0.5f, 0.9f}, {std::nanf(""), 0.0f}});
    const auto tidy = t.curve();
    CHECK(tidy.size() == 3);
    CHECK(tidy[0].first == -1.0f && tidy[0].second == -0.4f && tidy[1].first == -0.5f && tidy[1].second == 1.0f && tidy[2].first == 0.5f && tidy[2].second == 0.9f);
    Bend::Points many;
    for (int i = 0; i < 40; ++i) many.push_back({-1.0f + (float)i / 20.0f, 0.0f});
    t.setCurve(many);
    CHECK((int)t.curve().size() == Bend::kMaxPoints);
    t.setCurve({{0.3f, 0.3f}});
    CHECK(t.curve() == Bend::straightLine());
    t.setCurve({});
    CHECK(t.curve() == Bend::straightLine());
}

TEST(bend_applies_its_curve_exactly_at_dc) {
    // a steady level in, a steady level out: gain out * curve(gain in * level), for every oversampling setting
    double worst[4] = {};
    const auto presets = Bend().factoryPresets();
    for (size_t pi = 0; pi < presets.size(); ++pi) {
        for (int over = 0; over < 4; ++over) {
            Bend b;
            b.prepare(kSr);
            b.loadState(presets[pi].state);
            b.pOver->set((float)over);
            const double pre = dbToGain(b.pPre->get()), post = dbToGain(b.pPost->get());
            for (float v : {-0.83f, -0.31f, -0.05f, 0.0f, 0.02f, 0.27f, 0.5f, 0.77f}) {
                b.reset();
                const auto out = run(b, level(0.02, v));
                const double want = post * b.transfer(pre * v);
                const double err = std::fabs(out.l.back() - want);
                worst[over] = std::max(worst[over], err);
                CHECK_MSG(err < (over == 0 ? 2.0e-7 : 1.0e-5), "%s, oversampling %d, level %.2f: %.8f, the curve says %.8f", presets[pi].name.c_str(), over, v, out.l.back(), want);
            }
        }
    }
    FIG("bend at DC, ten preset curves, eight levels: worst error %.1e with oversampling off, %.1e at 2x, %.1e at 4x, %.1e at 8x", worst[0], worst[1], worst[2], worst[3]);
    // against the points themselves: a curve drawn through (0.25, 0.6) gives 0.6 for a level of 0.25
    auto b = bend({{-1.0f, -1.0f}, {-0.5f, -0.1f}, {0.25f, 0.6f}, {0.75f, 0.3f}, {1.0f, 1.0f}}, Bend::Mirror, 0);
    for (auto pt : {std::pair<float, float>{0.25f, 0.6f}, {0.75f, 0.3f}, {1.0f, 1.0f}}) {
        b->reset();
        CHECK_NEAR(run(*b, level(0.01, pt.first)).l.back(), pt.second, 1.0e-6);
        b->reset();
        CHECK_NEAR(run(*b, level(0.01, -pt.first)).l.back(), -pt.second, 1.0e-6);      // mirrored for the other polarity
    }
    // drawn for both polarities: the negative half is its own, and the whole curve is moved so that nothing in gives nothing out
    auto full = bend(kLopsided, Bend::Full, 0);
    CHECK(full->transfer(0.0) == 0.0);
    full->reset();
    CHECK_NEAR(run(*full, level(0.01, -0.5f)).l.back(), -0.42 - 0.1, 1.0e-6);
    full->reset();
    CHECK_NEAR(run(*full, level(0.01, 0.4f)).l.back(), 0.45 - 0.1, 1.0e-6);
    // the two gains
    auto g = bend(kSoft, Bend::Mirror, 0);
    g->pPre->set(6.0f); g->pPost->set(-6.0f);
    CHECK_NEAR(run(*g, level(0.01, 0.15f)).l.back(), dbToGain(-6.0f) * g->transfer(0.15 * dbToGain(6.0f)), 1.0e-6);
}

TEST(bend_curve_survives_save_and_load) {
    Bend a;
    const Bend::Points drawn = {{-1.0f, -0.25f}, {-0.3333f, 0.7f}, {0.0f, 0.0f}, {0.123456f, -0.5f}, {0.9f, 0.31f}};
    a.setCurve(drawn);
    a.pPre->set(7.5f); a.pSymmetry->set(Bend::Mirror); a.pOver->set(3);
    CHECK(a.curve() == drawn);
    // the curve is in the state as {"curve": [[x, y], ...]}
    const Json state = a.saveState();
    const Json& arr = state["extra"]["curve"];
    CHECK(arr.isArray() && arr.size() == 5);
    CHECK(arr.at(1).at(0).f() == -0.3333f && arr.at(1).at(1).f() == 0.7f);
    // into another instance
    Bend b;
    b.loadState(state);
    CHECK(b.curve() == drawn);
    CHECK(b.saveState() == state);
    // through text, as a project file does it
    Bend c;
    std::string err;
    c.loadState(Json::parse(state.dump(), &err));
    CHECK(err.empty());
    CHECK(c.curve() == drawn);
    CHECK(c.pPre->get() == 7.5f && c.pSymmetry->getInt() == Bend::Mirror && c.pOver->getInt() == 3);
    for (double x = -1.0; x <= 1.0; x += 0.01) CHECK(c.transfer(x) == a.transfer(x));
    // and it sounds the same
    a.prepare(kSr); c.prepare(kSr);
    const auto in = groove(0.3, 0.8f);
    CHECK(run(a, in).l == run(c, in).l);
    // a state with no curve in it (an older file) gives the straight line
    Json bare = Json::object();
    bare.set("type", "shaper");
    bare.set("params", Json::object());
    c.loadState(bare);
    CHECK(c.curve() == Bend::straightLine());
    // every factory preset carries a curve of its own
    const auto presets = Bend().factoryPresets();
    CHECK(presets.size() >= 8);
    std::set<std::string> curves;
    for (auto& p : presets) {
        CHECK(p.state["extra"]["curve"].isArray() && p.state["extra"]["curve"].size() >= 2);
        CHECK_MSG(curves.insert(p.state["extra"]["curve"].dump()).second, "%s has the same curve as another preset", p.name.c_str());
    }
}

TEST(bend_symmetry_harmonics_and_oversampling) {
    // mirrored: the same curve for both polarities, so odd harmonics only; drawn for both: what was drawn, even harmonics and all
    {
        auto m = bend(kSoft, Bend::Mirror, 2);
        m->pPre->set(9.0f);
        for (double x = 0.01; x < 1.0; x += 0.07) CHECK(m->transfer(-x) == -m->transfer(x));
        const auto out = run(*m, sineWave(1000, 0.7, 0.3f));
        const double h2 = harmonicDb(out.l, 1000, 2), h3 = harmonicDb(out.l, 1000, 3);
        CHECK_MSG(h2 < -90.0 && h3 > -40.0, "mirrored: 2nd %.1f dB, 3rd %.1f dB", h2, h3);
        auto f = bend(kLopsided, Bend::Full, 2);
        f->pPre->set(6.0f);
        const auto lop = run(*f, sineWave(1000, 0.7, 0.3f));
        const double l2 = harmonicDb(lop.l, 1000, 2);
        CHECK_MSG(l2 > -50.0, "drawn for both polarities, lopsided: 2nd %.1f dB", l2);
        FIG("bend, 1 kHz: mirrored soft knee 2nd %.1f dB, 3rd %.1f dB; lopsided full curve 2nd %.1f dB", h2, h3, l2);
    }
    // oversampling: 9 kHz into the soft knee, pushed. What folds back, for each setting.
    {
        double alias[4];
        for (int over = 0; over < 4; ++over) {
            auto b = bend(kSoft, Bend::Mirror, over);
            b->pPre->set(12.0f);
            alias[over] = aliasDb(run(*b, sineWave(9000, 0.7, 0.2f)).l);
        }
        CHECK_MSG(alias[2] < alias[0] - 30.0 && alias[3] < alias[0] - 30.0, "aliases %.1f dB off, %.1f at 2x, %.1f at 4x, %.1f at 8x", alias[0], alias[1], alias[2], alias[3]);
        CHECK(alias[1] < alias[0] - 6.0 && alias[2] < alias[1] && alias[3] <= alias[2] + 1.0);
        FIG("bend, soft knee, 9 kHz at 0.2, 12 dB in: aliases %.1f dB with oversampling off, %.1f at 2x, %.1f at 4x, %.1f at 8x", alias[0], alias[1], alias[2], alias[3]);
        // a curve with a corner in it (the flat beyond its last point) is harder: the figures, for the record
        double hard[4];
        for (int over = 0; over < 4; ++over) {
            auto b = bend({{0.0f, 0.0f}, {0.5f, 0.5f}}, Bend::Mirror, over);
            b->pPre->set(18.0f);
            hard[over] = aliasDb(run(*b, sineWave(9000, 0.7, 0.3f)).l);
        }
        CHECK(hard[3] < hard[0] - 12.0);
        FIG("bend, a curve that clips hard, 9 kHz: aliases %.1f dB off, %.1f at 2x, %.1f at 4x, %.1f at 8x (no curve-specific anti-aliasing here, only the oversampling)", hard[0], hard[1], hard[2], hard[3]);
    }
}

TEST(bend_latency_is_the_same_for_every_setting) {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        for (int over = 0; over < 4; ++over) {
            Bend b;
            b.prepare(sr);
            b.pOver->set((float)over);
            CHECK(b.latencySamples() == 32);
            const auto in = noise(0.2, 0.7f, sr);
            // the straight line with oversampling off: bit for bit the input, 32 samples late
            if (over == 0) {
                const auto out = runAt(b, in, sr);
                CHECK(maxDiffDelayed(out.l, in.l, 32, 0) == 0.0 && maxDiffDelayed(out.r, in.r, 32, 0) == 0.0 && peakOf(out.l, 0, 32) == 0.0f);
            } else {
                // oversampled: a click comes out 32 samples later and evenly about that sample, and a tone nulls
                const auto click = runAt(b, impulse(0.05, sr, 100, 0.5f), sr);
                CHECK_MSG(peakIndex(click.l) == 132, "%.0f Hz, setting %d: click comes out %d samples late", sr, over, (int)peakIndex(click.l) - 100);
                CHECK(std::fabs(click.l[131] - click.l[133]) < 1.0e-4f);
                b.reset();
                const auto t = sineWave(1000, 0.2, 0.5f, sr);
                const auto out = runAt(b, t, sr);
                CHECK_MSG(maxDiffDelayed(out.l, t.l, 32, 2000) < 0.5 * 6.0e-5, "%.0f Hz, setting %d: a 1 kHz tone nulls to %.2g", sr, over, maxDiffDelayed(out.l, t.l, 32, 2000));
            }
            // dry
            Bend d;
            d.prepare(sr);
            d.setCurve(kSoft);
            d.pOver->set((float)over); d.pMix->set(0.0f); d.pPre->set(20.0f);
            const auto dry = runAt(d, in, sr);
            CHECK(maxDiffDelayed(dry.l, in.l, 32, 0) == 0.0);
        }
    }
    // changing the oversampling, the symmetry or the curve under a tone
    auto b = bend(kSoft, Bend::Mirror, 2);
    b->pPre->set(6.0f);
    const auto in = sineWave(220, 1.0, 0.4f);
    const auto out = run(*b, in, {}, [&](size_t i) {
        const size_t q = i / kQuantum;
        if (q % 30 == 15) b->pOver->set((float)((q / 30) % 4));
        if (q % 90 == 60) b->pSymmetry->set(b->pSymmetry->getInt() ? 0.0f : 1.0f);
    });
    CHECK(allFinite(out));
    // A switch fades the shaped signal out and in over a quantum each, so the most the output may move in one sample
    // is the shaped tone's own steepest step plus its peak spread over 64 samples.
    auto still = bend(kSoft, Bend::Mirror, 2);
    still->pPre->set(6.0f);
    const auto steady = run(*still, in);
    const float allowed = maxStep(steady.l, 4800) + peakOf(steady.l) / 64.0f * 1.05f;
    CHECK_MSG(maxStep(out.l) <= allowed, "largest step %.4f, allowed %.4f", maxStep(out.l), allowed);
    FIG("bend: oversampling and symmetry switched under a 220 Hz tone: largest step %.4f (the shaped tone's own steepest is %.4f, a click would be up to %.2f)", maxStep(out.l), maxStep(steady.l, 4800), peakOf(steady.l));
}

// ------------------------------------------------------------------ Middle and Sides
// (No mono-maker and no polarity switches here: Stereo Kit already has both, so they were left out as instructed.)

TEST(middle_sides_maths_is_exact) {
    const auto in = wide(0.3, 0.6f);
    // at rest: bit for bit what went in
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        MiddleSides ms;
        ms.prepare(sr);
        const auto src = noise(0.2, 0.7f, sr);
        const auto out = runAt(ms, src, sr);
        CHECK(out.l == src.l && out.r == src.r);
    }
    // mono in gives no side, whatever the side controls are doing
    {
        MiddleSides ms;
        ms.prepare(kSr);
        ms.pSideLevel->set(12.0f); ms.pSideLow->set(12.0f); ms.pSideHigh->set(-9.0f); ms.pWidth->set(2.0f);
        Stereo mono = in;
        mono.r = mono.l;
        const auto out = run(ms, mono);
        CHECK(out.l == out.r);
        CHECK(out.l == mono.l);                 // and with the middle at rest it is the input itself
    }
    // sides only in (left = -right) stays sides only, whatever the middle controls are doing
    {
        MiddleSides ms;
        ms.prepare(kSr);
        ms.pMidLevel->set(9.0f); ms.pMidLow->set(-12.0f); ms.pMidHigh->set(12.0f);
        Stereo side = in;
        for (size_t i = 0; i < side.r.size(); ++i) side.r[i] = -side.l[i];
        const auto out = run(ms, side);
        bool opposite = true;
        for (size_t i = 0; i < out.l.size(); ++i) opposite = opposite && out.l[i] == -out.r[i];
        CHECK(opposite);
        CHECK(out.l == side.l);
    }
    // the middle off leaves exactly the sides; the sides off (or width at nothing) leaves exactly the middle
    {
        MiddleSides noMid, noSide, narrow;
        noMid.prepare(kSr); noSide.prepare(kSr); narrow.prepare(kSr);
        noMid.pMidLevel->set(MiddleSides::kLevelOff);
        noSide.pSideLevel->set(MiddleSides::kLevelOff);
        narrow.pWidth->set(0.0f);
        const auto a = run(noMid, in), b = run(noSide, in), c = run(narrow, in);
        double worstA = 0.0, worstB = 0.0, worstC = 0.0;
        for (size_t i = 0; i < a.l.size(); ++i) {
            const double m = 0.5 * ((double)in.l[i] + in.r[i]), s = 0.5 * ((double)in.l[i] - in.r[i]);
            worstA = std::max({worstA, std::fabs(a.l[i] - s), std::fabs(a.r[i] + s)});
            worstB = std::max({worstB, std::fabs(b.l[i] - m), std::fabs(b.r[i] - m)});
            worstC = std::max({worstC, std::fabs(c.l[i] - m), std::fabs(c.r[i] - m)});
        }
        CHECK_MSG(worstA < 1.0e-7 && worstB < 1.0e-7 && worstC < 1.0e-7, "%.2g %.2g %.2g", worstA, worstB, worstC);
        FIG("middle and sides: middle off leaves the sides to within %.1e, sides off leaves the middle to within %.1e", worstA, worstB);
    }
    // mono compatibility: left + right is the middle alone, so nothing done to the sides changes the mono sum
    {
        MiddleSides plain, busy;
        plain.prepare(kSr); busy.prepare(kSr);
        for (auto* ms : {&plain, &busy}) { ms->pMidLevel->set(-3.0f); ms->pMidLow->set(4.0f); ms->pMidHigh->set(-2.0f); }
        busy.pSideLevel->set(9.0f); busy.pSideLow->set(-12.0f); busy.pSideHigh->set(8.0f); busy.pWidth->set(1.7f);
        const auto a = run(plain, in), b = run(busy, in);
        double worst = 0.0;
        for (size_t i = 0; i < a.l.size(); ++i) worst = std::max(worst, std::fabs(((double)a.l[i] + a.r[i]) - ((double)b.l[i] + b.r[i])));
        CHECK_MSG(worst < 1.0e-6, "the mono sum moves by %.2g when only the sides are changed", worst);
        CHECK(maxAbsDiff(a.l, b.l) > 0.05);
        FIG("middle and sides: the mono sum changes by %.1e when the sides are raised 9 dB, widened and equalised", worst);
    }
}

TEST(middle_sides_levels_shelves_width_and_balance) {
    const auto mono = [](double hz) { return sineWave(hz, 0.7, 0.2f); };
    const auto sides = [](double hz) { auto s = sineWave(hz, 0.7, 0.2f); for (auto& v : s.r) v = -v; return s; };
    const auto gain = [](MiddleSides& ms, const Stereo& in, double hz) {
        ms.reset();
        const auto out = run(ms, in);
        return levelDbAt(out.l, out.l.size() - 16384, 16384, hz) - db(0.2);
    };
    MiddleSides ms;
    ms.prepare(kSr);
    ms.pMidLevel->set(4.0f); ms.pMidLow->set(6.0f); ms.pMidLowHz->set(150.0f); ms.pMidHigh->set(-5.0f); ms.pMidHighHz->set(4000.0f);
    ms.pSideLevel->set(-3.0f); ms.pSideLow->set(-9.0f); ms.pSideLowHz->set(300.0f); ms.pSideHigh->set(7.0f); ms.pSideHighHz->set(6000.0f);
    double worst = 0.0;
    for (double hz : {40.0, 150.0, 300.0, 1000.0, 4000.0, 6000.0, 15000.0}) {
        const double m = gain(ms, mono(hz), hz), s = gain(ms, sides(hz), hz);
        CHECK_MSG(std::fabs(m - ms.midDb(hz)) < 0.05, "middle at %.0f Hz: measured %.3f dB, curve %.3f dB", hz, m, ms.midDb(hz));
        CHECK_MSG(std::fabs(s - ms.sideDb(hz)) < 0.05, "sides at %.0f Hz: measured %.3f dB, curve %.3f dB", hz, s, ms.sideDb(hz));
        worst = std::max({worst, std::fabs(m - ms.midDb(hz)), std::fabs(s - ms.sideDb(hz))});
    }
    // the shelves are the stated size at their far ends and half of it (in dB) at their corners
    CHECK_NEAR(ms.midDb(20.0), 4.0 + 6.0, 0.2);
    CHECK_NEAR(ms.midDb(150.0), 4.0 + 3.0, 0.2);
    CHECK_NEAR(ms.midDb(20000.0), 4.0 - 5.0, 0.3);
    CHECK_NEAR(ms.midDb(900.0), 4.0, 0.4);
    CHECK_NEAR(ms.sideDb(20.0), -3.0 - 9.0, 0.2);
    CHECK_NEAR(ms.sideDb(6000.0), -3.0 + 3.5, 0.2);
    CHECK_NEAR(ms.sideDb(20000.0), -3.0 + 7.0, 0.5);
    FIG("middle and sides: measured middle and side responses within %.3f dB of midDb() and sideDb() from 40 Hz to 15 kHz", worst);
    // width scales the sides; balance and output gain work on left and right
    MiddleSides w;
    w.prepare(kSr);
    w.pWidth->set(2.0f);
    CHECK_NEAR(gain(w, sides(1000), 1000), 6.02, 0.01);
    CHECK_NEAR(gain(w, mono(1000), 1000), 0.0, 0.001);
    MiddleSides bal;
    bal.prepare(kSr);
    bal.pBalance->set(1.0f); bal.pOut->set(-6.0f);
    const auto out = run(bal, mono(500));
    CHECK(peakOf(out.l) == 0.0f);
    CHECK_NEAR(db(peakOf(out.r, 9600) / 0.2), -6.0, 0.02);
    bal.pBalance->set(-0.5f);
    bal.reset();
    const auto half = run(bal, mono(500));
    CHECK_NEAR(peakOf(half.r, 9600) / peakOf(half.l, 9600), 0.5, 0.002);
    // a shelf turned up and back under a tone glides, and afterwards the effect is bit-transparent again
    MiddleSides g;
    g.prepare(kSr);
    const auto in = noise(3.0, 0.5f);
    const auto moved = run(g, in, {}, [&](size_t i) {
        if (i == 9600) { g.pMidLow->set(12.0f); g.pSideHigh->set(-12.0f); }
        if (i == 28800) { g.pMidLow->set(0.0f); g.pSideHigh->set(0.0f); }
    });
    size_t lastDifferent = 0;
    for (size_t i = 0; i < moved.l.size(); ++i) if (moved.l[i] != in.l[i] || moved.r[i] != in.r[i]) lastDifferent = i;
    CHECK(maxAbsDiff(moved.l, in.l) > 0.05);
    CHECK_MSG(lastDifferent > 28800 && lastDifferent < 28800 + 24000, "last difference at %d", (int)lastDifferent);
    MiddleSides step;
    step.prepare(kSr);
    const auto t = sineWave(90, 0.6, 0.3f);
    const auto stepped = run(step, t, {}, [&](size_t i) { if (i == 9600) step.pMidLow->set(12.0f); if (i == 19200) step.pMidLevel->set(-12.0f); });
    // (a level is moved over one quantum: the tone's own step at its largest gain, plus the change of level spread over 64 samples)
    const float allowed = maxStep(t.l) * dbToGain(12.0f) * 1.05f + 0.3f * dbToGain(12.0f) * (1.0f - dbToGain(-12.0f)) / 64.0f * 1.05f;
    CHECK_MSG(maxStep(stepped.l) <= allowed, "largest step %.5f, allowed %.5f", maxStep(stepped.l), allowed);
}
