// Spaces (convolution reverb) against known responses.
#include "SpaceTest.h"
#include <thread>
using namespace pulp;
using namespace st;

namespace {
const double kSr = 48000.0;

// y = x * h, the slow and obviously right way, in double precision
std::vector<double> direct(const std::vector<float>& x, const std::vector<float>& h, size_t outLen) {
    std::vector<double> y(outLen, 0.0);
    for (size_t i = 0; i < x.size() && i < outLen; ++i) {
        if (x[i] == 0.0f) continue;
        const double xv = x[i];
        const size_t m = std::min(h.size(), outLen - i);
        for (size_t k = 0; k < m; ++k) y[i + k] += xv * h[k];
    }
    return y;
}
double errorDb(const std::vector<float>& got, const std::vector<double>& want) {
    double worst = 0.0, top = 0.0;
    for (size_t i = 0; i < want.size() && i < got.size(); ++i) { worst = std::max(worst, std::fabs(got[i] - want[i])); top = std::max(top, std::fabs(want[i])); }
    return db(worst / top);
}
std::shared_ptr<SampleData> decayingNoise(size_t frames, int channels, double sr, double rt60, uint32 seed) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = channels;
    for (int c = 0; c < channels; ++c) {
        dsp::Noise nz(seed + 77u * (uint32)c);
        s->ch[c].resize(frames);
        for (size_t i = 0; i < frames; ++i) s->ch[c][i] = nz.next() * (float)std::pow(10.0, -3.0 * (double)i / (rt60 * sr));
    }
    return s;
}
// decay time from the backward-integrated energy, -5 to -35 dB, doubled
double rt60Of(const std::vector<float>& h, double sr) {
    std::vector<double> e(h.size() + 1, 0.0);
    for (size_t i = h.size(); i-- > 0;) e[i] = e[i + 1] + (double)h[i] * h[i];
    size_t t5 = 0, t35 = 0;
    for (size_t i = 0; i < h.size(); ++i) {
        const double d = 10.0 * std::log10(e[i] / e[0] + 1e-30);
        if (t5 == 0 && d <= -5.0) t5 = i;
        if (d <= -35.0) { t35 = i; break; }
    }
    return t35 > t5 ? 2.0 * (double)(t35 - t5) / sr : 0.0;
}
double energy(const std::vector<float>& h, size_t a = 0, size_t b = (size_t)-1) {
    double e = 0.0;
    for (size_t i = a; i < std::min(b, h.size()); ++i) e += (double)h[i] * h[i];
    return e;
}
// energy of a response in a band, by a coarse scan
double bandEnergy(const std::vector<float>& h, double sr, double lo, double hi) {
    double e = 0.0;
    const size_t n = std::min<size_t>(h.size(), 16384);
    for (double hz = lo; hz <= hi; hz *= 1.06) { const double m = magAt(h, hz, sr, n); e += m * m; }
    return e;
}
std::shared_ptr<Spaces> wetOnly(int impulse, double sr = kSr) {
    auto s = std::make_shared<Spaces>();
    s->prepare(sr);
    s->pImpulse->set((float)impulse);
    s->pMix->set(1.0f);
    return s;
}
}

TEST(spaces_convolving_an_impulse_gives_the_impulse_response) {
    for (int idx : {0, 5, 8, 11}) {
        auto s = wetOnly(idx);
        auto ir = s->impulseData();
        CHECK(ir != nullptr);
        if (!ir) continue;
        const size_t len = (size_t)ir->frames();
        CHECK(ir->channels == 2);
        for (size_t at : {(size_t)0, (size_t)37}) {
            s->reset();
            auto out = run(*s, impulse((double)len / kSr + 0.4, kSr, at));
            double worst = 0.0, after = 0.0;
            const double top = std::max(tu::peakOf(ir->ch[0]), tu::peakOf(ir->ch[1]));
            for (size_t i = 0; i < len; ++i) {
                worst = std::max(worst, (double)std::fabs(out.l[i + at] - ir->ch[0][i]));
                worst = std::max(worst, (double)std::fabs(out.r[i + at] - ir->ch[1][i]));
            }
            for (size_t i = len + at; i < out.l.size(); ++i) after = std::max(after, (double)std::max(std::fabs(out.l[i]), std::fabs(out.r[i])));
            CHECK_MSG(db(worst / top) < -100.0, "%s, impulse at %d: error %.1f dB", Spaces::impulseName(idx), (int)at, db(worst / top));
            CHECK_MSG(db(after / top) < -100.0, "%s: %.1f dB after the end", Spaces::impulseName(idx), db(after / top));
            CHECK(peak(out, 0, at) == 0.0f);        // nothing before the impulse: no latency, no pre-ring
            if (at == 0) printf("      %-13s impulse in, impulse response out: error %.1f dB\n", Spaces::impulseName(idx), db(worst / top));
        }
        CHECK(s->latencySamples() == 0);
    }
}

TEST(spaces_matches_direct_convolution) {
    {   // a short kernel that lives entirely in the 64-sample partitions
        auto k = decayingNoise(300, 1, kSr, 0.02, 5u);
        auto s = wetOnly(Spaces::kBuiltIn);
        CHECK(s->setImpulse(k, false));
        auto in = noise(0.25, 0.5f);
        auto out = run(*s, in);
        const double eL = errorDb(out.l, direct(in.l, k->ch[0], out.l.size())), eR = errorDb(out.r, direct(in.r, k->ch[0], out.r.size()));
        CHECK_MSG(eL < -100.0 && eR < -100.0, "error %.1f / %.1f dB", eL, eR);
        printf("      300-sample kernel against direct convolution: error %.1f dB\n", std::max(eL, eR));
    }
    {   // a stereo kernel long enough to run through all three partition sizes
        auto k = decayingNoise(30000, 2, kSr, 0.5, 9u);
        auto s = wetOnly(Spaces::kBuiltIn);
        CHECK(s->setImpulse(k, false));
        Stereo in = silence(1.0);
        dsp::Noise a(3u), b(4u);
        for (size_t i = 0; i < 3000; ++i) { in.l[i] = 0.5f * a.next(); in.r[i] = 0.5f * b.next(); }
        for (size_t i = 9000; i < 9700; ++i) { in.l[i] = 0.5f * a.next(); in.r[i] = -0.5f * b.next(); }
        auto out = run(*s, in);
        const double eL = errorDb(out.l, direct(in.l, k->ch[0], out.l.size())), eR = errorDb(out.r, direct(in.r, k->ch[1], out.r.size()));
        CHECK_MSG(eL < -100.0 && eR < -100.0, "error %.1f / %.1f dB", eL, eR);
        printf("      30000-sample stereo kernel against direct convolution: error %.1f dB\n", std::max(eL, eR));
    }
    {   // normalising scales the kernel to a fixed energy and nothing else
        auto k = decayingNoise(2000, 1, kSr, 0.1, 6u);
        auto s = wetOnly(Spaces::kBuiltIn);
        CHECK(s->setImpulse(k, true));
        auto used = s->impulseData();
        CHECK(used && used->frames() == 2000);
        if (used) {
            CHECK_NEAR(energy(used->ch[0]), 0.25, 1e-4);
            const double g = used->ch[0][0] / k->ch[0][0];
            double worst = 0.0;
            for (size_t i = 0; i < 2000; ++i) worst = std::max(worst, std::fabs(used->ch[0][i] - g * k->ch[0][i]));
            CHECK(worst < 1e-6);
        }
    }
}

TEST(spaces_is_linear) {
    auto s = wetOnly(2);
    auto a = noise(0.5, 0.4f, kSr, 21u);
    auto b = tone(330.0, 0.5, 0.5f);
    for (size_t i = 6000; i < b.l.size(); ++i) b.l[i] = b.r[i] = 0.0f;
    Stereo sum = a;
    for (size_t i = 0; i < sum.l.size(); ++i) { sum.l[i] = 2.0f * a.l[i] - 0.5f * b.l[i]; sum.r[i] = 2.0f * a.r[i] - 0.5f * b.r[i]; }
    auto ya = run(*s, a);
    s->reset();
    auto yb = run(*s, b);
    s->reset();
    auto ys = run(*s, sum);
    double worst = 0.0;
    for (size_t i = 0; i < ys.l.size(); ++i) worst = std::max(worst, (double)std::fabs(ys.l[i] - (2.0f * ya.l[i] - 0.5f * yb.l[i])));
    CHECK_MSG(db(worst / tu::peakOf(ys.l)) < -100.0, "superposition error %.1f dB", db(worst / tu::peakOf(ys.l)));
    // and it does not care when the sound arrives
    s->reset();
    Stereo late = silence(0.5);
    late.l.insert(late.l.begin(), 1000, 0.0f);          // 1000 samples later (not a whole number of blocks)
    late.r.insert(late.r.begin(), 1000, 0.0f);
    for (size_t i = 0; i < a.l.size(); ++i) { late.l[i + 1000] = a.l[i]; late.r[i + 1000] = a.r[i]; }
    late.l.resize(late.l.size() / kQuantum * kQuantum);
    late.r.resize(late.l.size());
    auto yl = run(*s, late);
    worst = 0.0;
    for (size_t i = 0; i + 1000 < yl.l.size() && i < ya.l.size(); ++i) worst = std::max(worst, (double)std::fabs(yl.l[i + 1000] - ya.l[i]));
    CHECK_MSG(db(worst / tu::peakOf(ya.l)) < -100.0, "time shift error %.1f dB", db(worst / tu::peakOf(ya.l)));
}

TEST(spaces_runs_a_three_second_impulse_many_times_faster_than_real_time) {
    auto s = std::make_shared<Spaces>();
    s->prepare(kSr);
    s->pImpulse->set(5.0f);
    s->pMix->set(0.3f);
    auto ir = s->impulseData();
    CHECK(ir && ir->frames() == 144000);        // three seconds, stereo
    const double seconds = 20.0;
    auto in = noise(seconds, 0.3f);
    double best = 1e9, worstQuantum = 0.0;
    for (int rep = 0; rep < 3; ++rep) {
        s->reset();
        Stereo work = in;
        double slowest = 0.0;
        const double t0 = nowSeconds();
        for (size_t i = 0; i < work.l.size(); i += kQuantum) {
            ProcessContext c;
            c.sampleRate = kSr;
            c.io[0] = work.l.data() + i;
            c.io[1] = work.r.data() + i;
            const double q0 = nowSeconds();
            s->process(c);
            slowest = std::max(slowest, nowSeconds() - q0);
        }
        const double t = nowSeconds() - t0;
        if (t < best) { best = t; worstQuantum = slowest; }
        CHECK(allFinite(work));
    }
    const double factor = seconds / best;
#ifdef _DEBUG
    const double need = 2.0;        // unoptimised build
#else
    const double need = 25.0;       // under 4 % of one core
#endif
    CHECK_MSG(factor > need, "only %.1f times real time", factor);
    printf("      3 s stereo impulse, 20 s of noise: %.3f s, %.0f times real time, %.2f %% of one core; slowest quantum %.0f us of %.0f us\n",
           best, factor, 100.0 / factor, worstQuantum * 1e6, kQuantum / kSr * 1e6);
    // the decay trim (two passes per long partition) and a moving trim (old and new gains) cost more; still small
    s->pDecay->set(0.6f);
    s->reset();
    Stereo work = in;
    const double t0 = nowSeconds();
    for (size_t i = 0; i < work.l.size(); i += kQuantum) {
        ProcessContext c;
        c.sampleRate = kSr;
        c.io[0] = work.l.data() + i;
        c.io[1] = work.r.data() + i;
        s->pDecay->set(0.5f + 0.4f * (float)((i / kQuantum) % 200) / 200.0f);
        s->process(c);
    }
    const double moving = seconds / (nowSeconds() - t0);
    CHECK(moving > need * 0.4);
    printf("      the same with the decay trim moving every quantum: %.0f times real time\n", moving);
    // the longest impulse a user can load: ten seconds, stereo
    Spaces big;
    big.prepare(kSr);
    big.pMix->set(0.3f);
    CHECK(big.setImpulse(decayingNoise((size_t)(10.0 * kSr), 2, kSr, 8.0, 71u)));
    big.pImpulse->set((float)Spaces::kBuiltIn);
    Stereo ten = noise(10.0, 0.3f);
    double bestTen = 1e9;
    for (int rep = 0; rep < 2; ++rep) {
        big.reset();
        Stereo w = ten;
        const double t1 = nowSeconds();
        for (size_t i = 0; i < w.l.size(); i += kQuantum) {
            ProcessContext c;
            c.sampleRate = kSr;
            c.io[0] = w.l.data() + i;
            c.io[1] = w.r.data() + i;
            big.process(c);
        }
        bestTen = std::min(bestTen, nowSeconds() - t1);
    }
    CHECK(10.0 / bestTen > need * 0.4);
    printf("      10 s stereo impulse, 10 s of noise: %.0f times real time, %.2f %% of one core\n", 10.0 / bestTen, 10.0 * bestTen);
}

TEST(spaces_has_twelve_different_impulses_that_are_the_same_every_time) {
    static const char* names[Spaces::kBuiltIn] = {"Small room", "Medium room", "Large room", "Tiled room", "Chamber hall", "Concert hall",
                                                  "Cathedral", "Plate", "Spring", "Ambience", "Reversed", "Gated"};
    static const double seconds[Spaces::kBuiltIn] = {0.5, 0.85, 1.5, 1.3, 2.2, 3.0, 6.0, 2.4, 2.6, 0.28, 1.1, 0.34};
    Spaces s;
    s.prepare(kSr);
    CHECK(s.pImpulse->info.choices.size() == (size_t)Spaces::kBuiltIn + 1);
    std::vector<SamplePtr> irs;
    std::vector<double> rt;
    for (int i = 0; i < Spaces::kBuiltIn; ++i) {
        CHECK(std::string(Spaces::impulseName(i)) == names[i]);
        CHECK(s.pImpulse->info.choices[(size_t)i] == names[i]);
        auto ir = s.impulseData(i);
        CHECK(ir != nullptr);
        if (!ir) return;
        irs.push_back(ir);
        CHECK(ir->channels == 2);
        CHECK_NEAR((double)ir->frames() / kSr, seconds[i], 0.001);
        bool finite = true;
        for (int c = 0; c < 2; ++c) for (float v : ir->ch[c]) finite = finite && std::isfinite(v);
        CHECK(finite);
        // every one carries the same energy, so switching between them does not jump in level
        CHECK_NEAR(0.5 * (energy(ir->ch[0]) + energy(ir->ch[1])), 0.25, 1e-3);
        // no direct sound: the dry signal is the direct sound
        const double eAll = energy(ir->ch[0]);
        // no DC
        double mean = 0.0;
        for (float v : ir->ch[0]) mean += v;
        CHECK(std::fabs(mean) < 0.05);
        // the two sides are different
        double cross = 0.0;
        for (size_t k = 0; k < ir->ch[0].size(); ++k) cross += (double)ir->ch[0][k] * ir->ch[1][k];
        const double corr = cross / std::sqrt(eAll * energy(ir->ch[1]));
        CHECK_MSG(std::fabs(corr) < 0.6, "%s: left/right correlation %.2f", names[i], corr);
        rt.push_back(rt60Of(ir->ch[0], kSr));
        double centre = 0.0;
        for (size_t k = 0; k < ir->ch[0].size(); ++k) centre += (double)k * ir->ch[0][k] * ir->ch[0][k];
        printf("      %-13s %.2f s  decay %.2f s  peak %.3f  L/R correlation %+.2f  energy centre at %.0f %%  above 4 kHz %.1f dB\n", names[i],
               (double)ir->frames() / kSr, rt.back(), tu::peakOf(ir->ch[0]), corr, 100.0 * centre / eAll / (double)ir->frames(),
               10.0 * std::log10(bandEnergy(ir->ch[0], kSr, 4000, 16000) / bandEnergy(ir->ch[0], kSr, 200, 4000) + 1e-30));
    }
    // decay times: rooms from small to cathedral, each close to what it was designed for
    static const double want[7] = {0.36, 0.62, 1.05, 1.0, 1.6, 2.3, 5.0};
    for (int i = 0; i < 7; ++i) CHECK_MSG(std::fabs(rt[(size_t)i] / want[i] - 1.0) < 0.3, "%s decays in %.2f s, designed for %.2f s", names[i], rt[(size_t)i], want[i]);
    CHECK(rt[0] < rt[1] && rt[1] < rt[2] && rt[2] < rt[4] && rt[4] < rt[5] && rt[5] < rt[6]);
    CHECK_MSG(std::fabs(rt[7] / 1.9 - 1.0) < 0.3, "plate decays in %.2f s", rt[7]);
    CHECK_MSG(rt[8] > 1.0 && rt[8] < 3.5, "spring decays in %.2f s", rt[8]);
    // the reversed one swells: most of its energy is in its last third. The gated one stops dead.
    {
        const auto& h = irs[10]->ch[0];
        CHECK(energy(h, h.size() * 2 / 3) > 0.7 * energy(h));
        const auto& g = irs[11]->ch[0];
        const size_t n = g.size(), ms20 = (size_t)(0.02 * kSr);
        CHECK(energy(g, n - 3 * ms20, n - 2 * ms20) > 0.25 * energy(g, n / 2, n / 2 + ms20));       // still loud 40 ms before the end
    }
    // the spring is dark, the tiled room bright
    CHECK(bandEnergy(irs[8]->ch[0], kSr, 6000, 16000) < 0.01 * bandEnergy(irs[8]->ch[0], kSr, 300, 4000));
    auto treble = [&](size_t i) { return bandEnergy(irs[i]->ch[0], kSr, 4000, 16000) / bandEnergy(irs[i]->ch[0], kSr, 200, 4000); };
    CHECK(treble(3) > 4.0 * treble(5) && treble(3) > 1.5 * treble(0));
    // no two are alike
    for (size_t a = 0; a < irs.size(); ++a)
        for (size_t b = a + 1; b < irs.size(); ++b) {
            const size_t n = (size_t)std::min(irs[a]->frames(), irs[b]->frames());
            double cross = 0.0;
            for (size_t k = 0; k < n; ++k) cross += (double)irs[a]->ch[0][k] * irs[b]->ch[0][k];
            const double corr = cross / std::sqrt(energy(irs[a]->ch[0], 0, n) * energy(irs[b]->ch[0], 0, n));
            CHECK_MSG(std::fabs(corr) < 0.3, "%s and %s correlate %.2f", names[a], names[b], corr);
        }
    // generated again from scratch (other rates push 48 kHz out of the cache), they come out bit for bit the same
    std::vector<float> keep0 = irs[0]->ch[0], keep8 = irs[8]->ch[1], keep7 = irs[7]->ch[0];
    for (double other : {22050.0, 16000.0, 11025.0}) { Spaces t; t.prepare(other); CHECK(t.impulseData(0) != nullptr); }
    Spaces again;
    again.prepare(kSr);
    CHECK(again.impulseData(0)->ch[0] == keep0);
    CHECK(again.impulseData(8)->ch[1] == keep8);
    CHECK(again.impulseData(7)->ch[0] == keep7);
}

TEST(spaces_pre_delay_trims_tone_and_width_do_what_they_say) {
    auto ref = wetOnly(4);
    auto ir = ref->impulseData();
    const size_t len = (size_t)ir->frames();
    const double seconds = (double)len / kSr + 0.5;
    auto plain = run(*ref, impulse(seconds));
    {   // pre-delay: the same response, 20 ms (960 samples) later, to the sample
        auto s = wetOnly(4);
        s->pPre->set(20.0f);
        auto out = run(*s, impulse(seconds));
        CHECK(peak(out, 0, 960) == 0.0f);
        double worst = 0.0;
        for (size_t i = 0; i + 960 < out.l.size(); ++i) worst = std::max(worst, (double)std::fabs(out.l[i + 960] - plain.l[i]));
        CHECK_MSG(worst < 1e-7, "difference %g", worst);
        CHECK_NEAR(s->tailSeconds() - ref->tailSeconds(), 0.02, 1e-6);
    }
    {   // decay trim: half the decay time, same start
        const double full = rt60Of(plain.l, kSr);
        for (float trim : {0.5f, 0.25f}) {
            auto s = wetOnly(4);
            s->pDecay->set(trim);
            auto out = run(*s, impulse(seconds));
            const double got = rt60Of(out.l, kSr);
            CHECK_MSG(std::fabs(got / (full * trim) - 1.0) < 0.2, "trim %.2f: decay %.2f s, untrimmed %.2f s", trim, got, full);
            CHECK(std::fabs(energy(out.l, 0, 480) / energy(plain.l, 0, 480) - 1.0) < 0.15);     // the first 10 ms are hardly touched
            // the envelope is smooth: no steps where the 4096-sample partitions meet (compare short-time energy either side)
            double worstStep = 0.0;
            for (size_t b = 8192 + 4096; b + 2048 < len / 2; b += 4096) {
                const double before = energy(out.l, b - 2048, b) / energy(plain.l, b - 2048, b), after = energy(out.l, b, b + 2048) / energy(plain.l, b, b + 2048);
                const double expect = std::pow(10.0, -6.0 * (1.0 / trim - 1.0) * (2048.0 / kSr) / full);       // what the trim alone does over 2048 samples
                worstStep = std::max(worstStep, std::fabs(10.0 * std::log10(after / before / expect)));
            }
            // (between the boundaries the envelope is a straight line where it should be an exponential: at the
            // hardest trim that is a bow of about 1 dB in the middle of a late partition, which this also sees)
            CHECK_MSG(worstStep < (trim < 0.4f ? 1.0 : 0.4), "trim %.2f: %.2f dB step at a partition boundary", trim, worstStep);
        }
    }
    {   // length: nothing after the cut, untouched well before it
        auto s = wetOnly(4);
        s->pLength->set(0.3f);
        auto out = run(*s, impulse(seconds));
        const size_t cut = (size_t)(0.3 * (double)len);
        CHECK(peak(out, cut + 4096, out.l.size()) < 1e-6f);        // the fade ends with the partition the cut falls in
        CHECK(tu::maxAbsDiff(std::vector<float>(out.l.begin(), out.l.begin() + (std::ptrdiff_t)(cut * 3 / 4)),
                             std::vector<float>(plain.l.begin(), plain.l.begin() + (std::ptrdiff_t)(cut * 3 / 4))) < 1e-6);
        CHECK(energy(out.l, cut * 3 / 4, cut) > 0.0);
        CHECK_NEAR(s->tailSeconds(), 0.3 * (double)len / kSr + 0.15, 1e-3);
    }
    {   // low cut and high cut on the reverb only
        auto s = wetOnly(4);
        s->pLowCut->set(500.0f);
        auto lowCut = run(*s, impulse(seconds));
        CHECK(bandEnergy(lowCut.l, kSr, 60, 200) < 0.1 * bandEnergy(plain.l, kSr, 60, 200));
        CHECK_NEAR(bandEnergy(lowCut.l, kSr, 2000, 8000) / bandEnergy(plain.l, kSr, 2000, 8000), 1.0, 0.05);
        auto t = wetOnly(4);
        t->pHighCut->set(2000.0f);
        auto highCut = run(*t, impulse(seconds));
        CHECK(bandEnergy(highCut.l, kSr, 6000, 12000) < 0.02 * bandEnergy(plain.l, kSr, 6000, 12000));
        CHECK_NEAR(bandEnergy(highCut.l, kSr, 100, 600) / bandEnergy(plain.l, kSr, 100, 600), 1.0, 0.05);
    }
    {   // width: 0 is mono, 2 doubles the difference between the sides and keeps the middle
        auto s = wetOnly(4);
        s->pWidth->set(0.0f);
        auto mono = run(*s, impulse(seconds));
        CHECK(tu::maxAbsDiff(mono.l, mono.r) < 1e-7);
        auto t = wetOnly(4);
        t->pWidth->set(2.0f);
        auto wide = run(*t, impulse(seconds));
        double worstSide = 0.0, worstMid = 0.0;
        for (size_t i = 0; i < wide.l.size(); ++i) {
            worstSide = std::max(worstSide, (double)std::fabs((wide.l[i] - wide.r[i]) - 2.0f * (plain.l[i] - plain.r[i])));
            worstMid = std::max(worstMid, (double)std::fabs((wide.l[i] + wide.r[i]) - (plain.l[i] + plain.r[i])));
        }
        CHECK(worstSide < 1e-6 && worstMid < 1e-6);
    }
    {   // dry / wet: dry only at 0 (bit for bit), both at full level in the middle, wet only at 1
        auto in = noise(0.3, 0.4f);
        auto s = std::make_shared<Spaces>();
        s->prepare(kSr);
        s->pMix->set(0.0f);
        auto dry = run(*s, in);
        CHECK(dry.l == in.l && dry.r == in.r);
        auto wet = run(*wetOnly(5), in);
        auto m = std::make_shared<Spaces>();
        m->prepare(kSr);
        m->pMix->set(0.5f);
        auto both = run(*m, in);
        double worst = 0.0;
        for (size_t i = 0; i < both.l.size(); ++i) worst = std::max(worst, (double)std::fabs(both.l[i] - (in.l[i] + wet.l[i])));
        CHECK(worst < 1e-6);
    }
}

TEST(spaces_loads_a_user_impulse_and_keeps_it_in_the_project) {
    const std::string dir = t::tmpDir();
    const std::string stereoPath = dir + "/space_ir_stereo_44k.wav", monoPath = dir + "/space_ir_mono.wav", longPath = dir + "/space_ir_long_8k.wav";
    auto stereo = decayingNoise(22050, 2, 44100.0, 0.3, 31u);      // half a second at another rate
    auto mono = decayingNoise(4800, 1, 48000.0, 0.05, 32u);
    auto longOne = decayingNoise(8000 * 11, 1, 8000.0, 8.0, 33u);  // eleven seconds
    CHECK(writeWavFile(stereoPath, *stereo, WavFormat::Float32));
    CHECK(writeWavFile(monoPath, *mono, WavFormat::Float32));
    CHECK(writeWavFile(longPath, *longOne, WavFormat::Pcm16));

    Spaces s;
    s.prepare(kSr);
    s.pMix->set(1.0f);
    std::string err;
    CHECK(!s.loadImpulse(dir + "/space_no_such_file.wav", &err));
    CHECK(!err.empty());
    CHECK(s.pImpulse->getInt() == 5 && s.impulsePath().empty());        // a failed load changes nothing
    CHECK(s.saveState()["extra"].isNull());

    CHECK(s.loadImpulse(stereoPath, &err));
    CHECK(s.pImpulse->getInt() == Spaces::kBuiltIn);                    // loading selects it
    CHECK(s.impulsePath() == stereoPath);
    auto used = s.impulseData();
    CHECK(used && used->channels == 2);
    if (!used) return;
    CHECK_NEAR((double)used->frames(), 24000.0, 2.0);                   // resampled to the engine rate: still half a second
    CHECK_NEAR(0.5 * (energy(used->ch[0]) + energy(used->ch[1])), 0.25, 1e-3);
    CHECK_NEAR(rt60Of(used->ch[0], kSr), 0.3, 0.05);                     // and it still decays as the file does
    auto out = run(s, impulse(1.0));
    double worst = 0.0;
    for (size_t i = 0; i < (size_t)used->frames(); ++i) worst = std::max(worst, (double)std::fabs(out.l[i] - used->ch[0][i]));
    CHECK(db(worst / tu::peakOf(used->ch[0])) < -100.0);
    CHECK_NEAR(s.tailSeconds(), 0.5 + 0.15, 0.01);

    // saved under "ir", and a fresh instance given the state sounds the same
    const Json state = s.saveState();
    CHECK(state["extra"]["ir"].str("") == stereoPath);
    Spaces r;
    r.prepare(kSr);
    r.loadState(state);
    CHECK(r.saveState() == state);
    CHECK(r.pImpulse->getInt() == Spaces::kBuiltIn);
    auto again = run(r, impulse(1.0));
    CHECK(again.l == out.l && again.r == out.r);
    // loading the state before prepare() works too (that is the order a project load uses)
    Spaces early;
    early.loadState(state);
    early.prepare(kSr);
    CHECK(run(early, impulse(1.0)).l == out.l);
    // a relative path resolves against the project folder
    Json rel = state;
    Json ex = Json::object();
    ex.set("ir", "space_ir_stereo_44k.wav");
    rel.set("extra", ex);
    Spaces viaDir;
    viaDir.prepare(kSr);
    viaDir.loadState(rel, dir);
    CHECK(run(viaDir, impulse(1.0)).l == out.l);
    CHECK(viaDir.impulsePath() == "space_ir_stereo_44k.wav");
    // a state without a file drops the user impulse; a state naming a missing file keeps the name and plays nothing
    Spaces plain;
    plain.prepare(kSr);
    r.loadState(plain.saveState());
    CHECK(r.impulsePath().empty() && r.impulseData(Spaces::kBuiltIn) == nullptr);
    Json gone = state;
    Json ex2 = Json::object();
    ex2.set("ir", dir + "/space_no_such_file.wav");
    gone.set("extra", ex2);
    r.loadState(gone);
    CHECK(r.saveState() == gone);
    r.pMix->set(1.0f);
    r.reset();
    CHECK(peak(run(r, impulse(0.2))) == 0.0f);

    // mono: the same response on both sides
    CHECK(s.loadImpulse(monoPath, &err));
    s.reset();
    auto m = run(s, impulse(0.5));
    CHECK(s.impulseData()->channels == 1 && m.l == m.r && peak(m) > 0.01f);
    // longer than ten seconds: the first ten are used, faded off at the end
    CHECK(s.loadImpulse(longPath, &err));
    auto ten = s.impulseData();
    CHECK(ten && ten->frames() == (int64)(10.0 * kSr));
    CHECK(ten && ten->ch[0].back() == 0.0f && tu::peakOf(ten->ch[0], 400000, 440000) > 1e-4f * tu::peakOf(ten->ch[0]));
    CHECK_NEAR(s.tailSeconds(), 10.15, 0.01);
    s.reset();
    auto longOut = run(s, noise(0.5, 0.3f));
    CHECK(allFinite(longOut) && peak(longOut) > 0.01f);
    // from memory, and clearing
    CHECK(s.setImpulse(nullptr));
    CHECK(s.impulseData() == nullptr && s.saveState()["extra"].isNull());
    std::string why;
    CHECK(!s.setImpulse(std::make_shared<SampleData>(), true, &why) && !why.empty());
}

TEST(spaces_changes_impulse_while_audio_runs) {
    // Wet only, a steady sine in: any click shows up as a jump between neighbouring samples.
    auto maxStep = [](const Stereo& o, size_t from = 1) {
        float m = 0.0f;
        for (size_t i = std::max<size_t>(from, 1); i < o.l.size(); ++i) m = std::max(m, std::max(std::fabs(o.l[i] - o.l[i - 1]), std::fabs(o.r[i] - o.r[i - 1])));
        return m;
    };
    {   // the impulse choice is a parameter: every built-in is ready, the switch is a short fade out and in
        auto s = wetOnly(1);
        const size_t q = 4800 / kQuantum * kQuantum;
        Stereo all;
        const int64 before = Rt::allocationsInRt.load();
        for (int k = 0; k < 14; ++k) {
            Stereo part = silence((double)q / kSr);
            for (size_t i = 0; i < q; ++i) part.l[i] = part.r[i] = 0.5f * (float)std::sin(kTwoPi * 220.0 * (double)((size_t)k * q + i) / kSr);
            s->pImpulse->set((float)((k * 5 + 1) % Spaces::kBuiltIn));
            {
                Rt::Scope rt;
                for (size_t i = 0; i < q; i += kQuantum) {
                    ProcessContext c;
                    c.io[0] = part.l.data() + i;
                    c.io[1] = part.r.data() + i;
                    s->process(c);
                }
            }
            append(all, part);
        }
        CHECK(Rt::allocationsInRt.load() == before);
        CHECK(allFinite(all) && peak(all) > 0.05f);
        // a 220 Hz sine of amplitude a moves at most a * 0.029 per sample; the reverb's gain at one frequency can be a few times one
        CHECK_MSG(maxStep(all) < 0.12f, "largest step %.4f", maxStep(all));
        // after a switch the reverb is the new impulse's: steady state equals a run that only ever had that impulse
        auto fresh = wetOnly((13 * 5 + 1) % Spaces::kBuiltIn);
        Stereo tail = silence(1.0);
        auto a = run(*s, tail);
        (void)a;
        s->reset();
        CHECK(run(*s, impulse(0.5)).l == run(*fresh, impulse(0.5)).l);
    }
    {   // user impulses handed over from another thread while the audio thread keeps going
        auto s = wetOnly(Spaces::kBuiltIn);
        std::vector<std::shared_ptr<SampleData>> pool;
        for (int k = 0; k < 6; ++k) pool.push_back(decayingNoise((size_t)(6000 + 9000 * k), 1 + k % 2, 44100.0, 0.2 + 0.1 * k, 50u + (uint32)k));
        CHECK(s->setImpulse(pool[0]));
        std::atomic<bool> done{false};
        std::atomic<int> swaps{0};
        std::thread control([&] {
            for (int k = 1; k <= 18; ++k) {
                s->setImpulse(pool[(size_t)k % pool.size()]);
                swaps.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
            done.store(true);
        });
        float l[kQuantum], r[kQuantum], last[2] = {0, 0}, step = 0.0f, top = 0.0f;
        bool finite = true;
        size_t n = 0;
        const int64 before = Rt::allocationsInRt.load();
        {
            Rt::Scope rt;
            while (!done.load() || n % 4096 != 0) {
                for (int i = 0; i < kQuantum; ++i, ++n) l[i] = r[i] = 0.5f * (float)std::sin(kTwoPi * 220.0 * (double)n / kSr);
                ProcessContext c;
                c.io[0] = l;
                c.io[1] = r;
                s->process(c);
                for (int i = 0; i < kQuantum; ++i) {
                    finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
                    step = std::max(step, std::max(std::fabs(l[i] - last[0]), std::fabs(r[i] - last[1])));
                    top = std::max(top, std::fabs(l[i]));
                    last[0] = l[i]; last[1] = r[i];
                }
                if (n > (size_t)48000 * 600) break;
            }
        }
        control.join();
        CHECK(Rt::allocationsInRt.load() == before);
        CHECK(finite && top > 0.05f && swaps.load() == 18);
        CHECK_MSG(step < 0.12f, "largest step %.4f over %d samples", step, (int)n);
        // what it ends up with is the last one handed over
        Stereo quiet = silence(3.0);
        run(*s, quiet);
        s->reset();
        auto ref = wetOnly(Spaces::kBuiltIn);
        ref->setImpulse(pool[18 % pool.size()]);
        CHECK(run(*s, impulse(0.6)).l == run(*ref, impulse(0.6)).l);
    }
    {   // moving the trims, pre-delay and tone while it runs: no clicks either
        auto s = wetOnly(4);
        Stereo in = tone(220.0, 3.0, 0.5f);
        Stereo out = in;
        for (size_t i = 0; i < in.l.size(); i += kQuantum) {
            const double t = (double)i / (double)in.l.size();
            s->pDecay->set(0.2f + 0.8f * (float)std::fabs(std::sin(t * 9.0)));
            s->pLength->set(0.3f + 0.7f * (float)std::fabs(std::cos(t * 7.0)));
            s->pWidth->set((float)(1.0 + std::sin(t * 30.0)));
            s->pMix->set(0.6f + 0.4f * (float)std::sin(t * 40.0));
            if (i % 9600 == 0) s->pPre->set((float)((i / 9600) % 4) * 15.0f);
            ProcessContext c;
            c.io[0] = out.l.data() + i;
            c.io[1] = out.r.data() + i;
            s->process(c);
        }
        CHECK(allFinite(out));
        CHECK_MSG(maxStep(out) < 0.12f, "largest step %.4f", maxStep(out));
    }
}

TEST(spaces_follows_the_sample_rate) {
    const std::string path = t::tmpDir() + "/space_ir_rate_test.wav";
    CHECK(writeWavFile(path, *decayingNoise(22050, 2, 44100.0, 0.3, 31u), WavFormat::Float32));
    Spaces s;
    const bool haveUser = s.loadImpulse(path);
    for (double sr : {44100.0, 96000.0, 48000.0}) {
        s.prepare(sr);
        s.pMix->set(1.0f);
        s.pImpulse->set(4.0f);
        auto ir = s.impulseData();
        CHECK(ir != nullptr);
        if (!ir) continue;
        CHECK_NEAR((double)ir->frames() / sr, 2.2, 0.001);
        CHECK_NEAR(s.tailSeconds(), 2.35, 0.002);
        CHECK_NEAR(rt60Of(ir->ch[0], sr), 1.6, 0.4);                // the same room at any rate
        Transport t;
        t.sr = sr;
        auto out = run(s, impulse(0.5, sr), t);
        double worst = 0.0;
        for (size_t i = 0; i < out.l.size(); ++i) worst = std::max(worst, (double)std::fabs(out.l[i] - ir->ch[0][i]));
        CHECK_MSG(db(worst / tu::peakOf(ir->ch[0])) < -100.0, "%.0f Hz: error %.1f dB", sr, db(worst / tu::peakOf(ir->ch[0])));
        // a 500 Hz low cut is at 500 Hz whatever the rate
        s.pLowCut->set(500.0f);
        s.reset();
        auto cut = run(s, impulse(0.5, sr), t);
        CHECK(bandEnergy(cut.l, sr, 60, 150) < 0.05 * bandEnergy(out.l, sr, 60, 150));
        s.pLowCut->set(20.0f);
        if (haveUser) {     // the user's file is resampled again for the new rate
            auto u = s.impulseData(Spaces::kBuiltIn);
            CHECK(u != nullptr);
            if (u) CHECK_NEAR((double)u->frames() / sr, 0.5, 0.001);
        }
    }
    CHECK(haveUser);
}

TEST(spaces_goes_quiet_and_wakes_up_the_same) {
    auto s = wetOnly(0);
    auto first = run(*s, impulse(0.7));
    // long after the tail, the output is exactly zero and stays so
    auto quiet = run(*s, silence(1.0));
    CHECK(peak(quiet, 24000) == 0.0f);
    // and the next sound gets the same reverb, bit for bit, as the first did
    auto second = run(*s, impulse(0.7));
    CHECK(second.l == first.l && second.r == first.r);
    // an empty user slot is silent rather than stale
    auto u = wetOnly(Spaces::kBuiltIn);
    CHECK(peak(run(*u, noise(0.2, 0.5f))) == 0.0f);
}
