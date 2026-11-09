// Lint: learned-profile noise reduction against noise of known level, tones, and silence.
#include "VocalTest.h"
#include <thread>
using namespace pulp;
using namespace vt;

namespace {
const double kSr = 48000.0;

// Lets the plugin hear `what` with learn on, then switches learn off. Returns what came out meanwhile.
Stereo learn(Lint& d, const Stereo& what, double sr = kSr) {
    d.pLearn->set(1.0f);
    auto out = run(d, what, sr);
    d.pLearn->set(0.0f);
    Stereo one = silence(0.0, sr);
    one.l.assign(kQuantum, 0.0f); one.r = one.l;
    run(d, one, sr);                    // the switch is read at the start of a quantum: one goes by, and the profile is made
    return out;
}
double bandDb(const std::vector<float>& x, size_t a, size_t b, double f1, double f2, double sr = kSr) {
    return 10.0 * std::log10(bandPower(powerSpectrum(x, a, b), f1, f2, sr) + 1e-30);
}
}

TEST(denoise_learns_the_noise_then_removes_it_and_keeps_a_tone) {
    Lint d;
    d.prepare(kSr);
    CHECK(!d.hasProfile());
    const Stereo hiss = noise(1.5, 0.02f, kSr, 11u);                // -37 dBFS white noise, different on the two sides
    const auto passed = learn(d, hiss);
    const size_t lat = (size_t)d.latencySamples();
    // while learning, the sound goes through untouched (delayed by the latency)
    CHECK(nullAgainstDelayed(passed.l, hiss.l, lat) == 0.0 && nullAgainstDelayed(passed.r, hiss.r, lat) == 0.0);
    CHECK(d.meter(3) > 120.0f);         // frames averaged

    const Stereo in = add(noise(3.0, 0.02f, kSr, 33u), tone(1000.0, 3.0, 0.2f));
    auto out = run(d, in);
    CHECK(d.hasProfile() && d.meter(1) == 0.0f && d.meter(2) == 1.0f);
    const size_t a = 48000, b = 140000;
    const double toneIn = levelDbAt(in.l, a, 65536, 1000.0, kSr), toneOut = levelDbAt(out.l, a + lat, 65536, 1000.0, kSr);
    const double hi = bandDb(out.l, a + lat, b + lat, 3000, 9000) - bandDb(in.l, a, b, 3000, 9000);
    const double lo = bandDb(out.l, a + lat, b + lat, 100, 700) - bandDb(in.l, a, b, 100, 700);
    const double top = bandDb(out.l, a + lat, b + lat, 12000, 20000) - bandDb(in.l, a, b, 12000, 20000);
    const double near = bandDb(out.l, a + lat, b + lat, 1150, 1400) - bandDb(in.l, a, b, 1150, 1400);
    const double right = bandDb(out.r, a + lat, b + lat, 3000, 9000) - bandDb(in.r, a, b, 3000, 9000);
    VT_FIG("default settings (floor 24 dB): noise %.1f dB at 100-700 Hz, %.1f dB at 3-9 kHz, %.1f dB at 12-20 kHz (right side %.1f dB), %.1f dB just above the tone; the tone changes by %+.3f dB; meter %.1f dB",
           lo, hi, top, right, near, toneOut - toneIn, d.meter(0));
    CHECK_MSG(hi < -12.0 && lo < -12.0 && top < -12.0 && right < -12.0, "noise reduced by %.1f / %.1f / %.1f / %.1f dB", -lo, -hi, -top, -right);
    CHECK_MSG(std::fabs(toneOut - toneIn) < 1.0, "the tone changed by %.2f dB", toneOut - toneIn);
    CHECK_NEAR(hi, -24.0, 1.0);          // it is the floor that sets how far the noise goes down
    CHECK(d.meter(0) > 20.0f);

    // the floor control is the reduction; strength 0 or floor 0 leaves everything alone
    const Stereo nz = noise(2.0, 0.02f, kSr, 44u);
    for (float fl : {6.0f, 12.0f, 40.0f, 60.0f}) {
        // a deep floor needs more strength: at 2x a few bins still poke over the threshold now and then, and at
        // -40 dB those few are most of what is left
        const float strength = fl > 30.0f ? 3.5f : 2.0f;
        d.pFloor->set(fl);
        d.pAmount->set(strength);
        d.reset();
        auto o = run(d, nz);
        const double red = bandDb(o.l, 48000 + lat, 90000 + lat, 200, 20000) - bandDb(nz.l, 48000, 90000, 200, 20000);
        CHECK_MSG(std::fabs(red + fl) < 1.5, "floor %.0f dB: reduced by %.2f dB", fl, -red);
        VT_FIG("floor %.0f dB, strength %.1f: noise alone goes down %.2f dB", fl, strength, -red);
        if (fl == 40.0f) {
            d.pAmount->set(2.0f);
            d.reset();
            o = run(d, nz);
            VT_FIG("floor 40 dB, strength 2.0: noise alone goes down %.2f dB", bandDb(nz.l, 48000, 90000, 200, 20000) - bandDb(o.l, 48000 + lat, 90000 + lat, 200, 20000));
        }
    }
    d.pAmount->set(2.0f);
    d.pFloor->set(0.0f);
    d.reset();
    auto o = run(d, nz);
    CHECK(nullAgainstDelayed(o.l, nz.l, lat, 20000) < 1e-5);
    d.pFloor->set(24.0f); d.pAmount->set(0.0f);
    d.reset();
    o = run(d, nz);
    CHECK(nullAgainstDelayed(o.l, nz.l, lat, 20000) < 1e-5);
    // a quiet tone only 10 dB above the noise in its bins is still kept within 1 dB by the default settings... measured, not assumed
    d.pAmount->set(2.0f);
    d.reset();
    const float quietAmp = 0.004f;
    const Stereo q = add(noise(3.0, 0.02f, kSr, 55u), tone(2500.0, 3.0, quietAmp));
    o = run(d, q);
    const double qIn = levelDbAt(q.l, a, 65536, 2500.0, kSr), qOut = levelDbAt(o.l, a + lat, 65536, 2500.0, kSr);
    VT_FIG("a tone at -48 dBFS in -37 dBFS noise (about 19 dB above the noise in its own bin): changes by %+.2f dB", qOut - qIn);
    CHECK(qOut - qIn > -6.0);
}

TEST(denoise_without_a_profile_it_only_delays) {
    const struct { double sr; int want, fft; } rates[] = {{44100.0, 1984, 2048}, {48000.0, 1984, 2048}, {96000.0, 4032, 4096}, {192000.0, 8128, 8192}};
    for (auto& r : rates) {
        Lint d;
        d.prepare(r.sr);
        CHECK_MSG(d.latencySamples() == r.want && d.fftSize() == r.fft, "%.0f Hz: latency %d, frame %d", r.sr, d.latencySamples(), d.fftSize());
    }
    Stereo in = add(noise(1.0, 0.1f, kSr, 3u), tone(440.0, 1.0, 0.3f));
    dsp::Noise rnd(8u);
    for (int trial = 0; trial < 10; ++trial) {
        Lint d;
        d.prepare(kSr);
        if (trial > 0) for (auto& p : d.params()) p->setNorm(rnd.uni());
        d.pLearn->set(trial == 9 ? 1.0f : 0.0f);            // learning is also a pure delay
        auto out = run(d, in);
        const size_t lat = (size_t)d.latencySamples();
        CHECK(lat == 1984);
        CHECK_MSG(nullAgainstDelayed(out.l, in.l, lat) == 0.0 && nullAgainstDelayed(out.r, in.r, lat) == 0.0, "trial %d: difference %g", trial, nullAgainstDelayed(out.l, in.l, lat));
        CHECK(peakOf(out.l, 0, lat) == 0.0f);
        CHECK(d.meter(0) == 0.0f);
    }
    // with a profile the latency is the same: a short burst of tone comes out where it went in, plus the latency
    Lint d;
    d.prepare(kSr);
    learn(d, noise(1.0, 0.01f, kSr, 5u));
    std::vector<float> v(48000, 0.0f);
    for (size_t i = 0; i < v.size(); ++i) v[i] = 0.5f * (float)(std::sin(kTwoPi * 800.0 * (double)i / kSr) * std::exp(-0.5 * std::pow(((double)i - 24000.0) / 1500.0, 2.0)));
    dsp::Noise more(77u);
    for (auto& x : v) x += 0.01f * more.next();
    auto out = run(d, mono(v));
    auto centroid = [](const std::vector<float>& x) { double s = 0.0, w = 0.0; for (size_t i = 0; i < x.size(); ++i) { s += (double)i * x[i] * x[i]; w += (double)x[i] * x[i]; } return s / w; };
    // (the input's own centre, taken over the burst only: the noise around it would pull it towards the middle)
    std::vector<float> burst(v.begin() + 16000, v.begin() + 32000), outBurst(out.l.begin() + 16000 + 1984, out.l.begin() + 32000 + 1984);
    const double shift = centroid(outBurst) - centroid(burst);
    VT_FIG("with a profile: a burst of tone comes out %.1f samples from the reported latency (1984)", shift);
    CHECK_MSG(std::fabs(shift) < 8.0, "the burst is %.1f samples off the latency", shift);
    dsp::Noise rnd2(9u);
    for (int i = 0; i < 20; ++i) { for (auto& p : d.params()) p->setNorm(rnd2.uni()); CHECK(d.latencySamples() == 1984); }
}

TEST(denoise_profile_survives_save_and_load) {
    Lint a;
    a.prepare(kSr);
    // a fresh one says so in its state
    CHECK(a.saveExtra()["learned"].b(true) == false);
    Stereo coloured = noise(1.5, 0.03f, kSr, 21u);
    dsp::OnePole lpL, lpR;
    lpL.setHz(800.0, kSr); lpR.setHz(2500.0, kSr);
    for (size_t i = 0; i < coloured.l.size(); ++i) { coloured.l[i] = lpL.lp(coloured.l[i]) * 2.0f; coloured.r[i] = lpR.lp(coloured.r[i]); }      // the two sides differ
    learn(a, coloured);
    a.pFloor->set(30.0f);
    const Json state = a.saveState();
    CHECK(state["extra"]["learned"].b(false));
    CHECK(state["extra"]["bins"].i() == 1025 && state["extra"]["noise"].size() == 1025 && state["extra"]["noiseR"].size() == 1025);
    const std::string text = state.dump();
    VT_FIG("saved state with a stereo profile: %d bytes of JSON (%d numbers, tenths of a dB)", (int)text.size(), 2 * 1025);
    CHECK(text.size() < 16000);

    // through text and back into a new one
    Lint b;
    b.loadState(Json::parse(text));
    CHECK(b.hasProfile());
    b.prepare(kSr);
    CHECK(b.saveState() == state);
    CHECK(b.pFloor->get() == 30.0f);
    Stereo in = add(noise(2.0, 0.03f, kSr, 99u), tone(700.0, 2.0, 0.2f));
    for (size_t i = 0; i < in.l.size(); ++i) { in.l[i] = lpL.lp(in.l[i]) * 2.0f; in.r[i] = lpR.lp(in.r[i]); }
    a.reset();
    const auto oa = run(a, in), ob = run(b, in);
    CHECK_MSG(oa.l == ob.l && oa.r == ob.r, "the reloaded profile gives a different result (%g apart)", maxAbsDiff(oa.l, ob.l));
    const size_t lat = (size_t)a.latencySamples();
    const double red = bandDb(oa.l, 48000 + lat, 90000 + lat, 2000, 12000) - bandDb(in.l, 48000, 90000, 2000, 12000);
    CHECK_MSG(red < -12.0, "reduced by %.1f dB", -red);

    // loaded after prepare, as when a project state is applied to a live plugin: picked up on the next quantum
    Lint c;
    c.prepare(kSr);
    c.loadState(state);
    const auto oc = run(c, in);
    CHECK(oc.l == oa.l && oc.r == oa.r);
    CHECK(c.saveState() == state);

    // a preset (no extra) changes the settings and keeps the profile; a state saved without a profile clears it
    c.loadState(c.factoryPresets()[2].state);
    c.prepare(kSr);
    CHECK(c.hasProfile() && c.saveState()["extra"] == state["extra"]);
    Lint fresh;
    c.loadState(fresh.saveState());
    c.prepare(kSr);
    CHECK(!c.hasProfile());
    const auto od = run(c, in);
    CHECK(nullAgainstDelayed(od.l, in.l, lat) == 0.0);

    // the same profile at another sample rate: it is stored by frequency, so the same noise is still taken out
    Lint hi;
    hi.loadState(state);
    hi.prepare(96000.0);
    CHECK(hi.hasProfile());
    Stereo in96 = noise(2.0, 0.03f * 1.41421356f, 96000.0, 99u);        // the same noise density per Hz at twice the rate
    dsp::OnePole l2, r2;
    l2.setHz(800.0, 96000.0); r2.setHz(2500.0, 96000.0);
    for (size_t i = 0; i < in96.l.size(); ++i) { in96.l[i] = l2.lp(in96.l[i]) * 2.0f; in96.r[i] = r2.lp(in96.r[i]); }
    const auto o96 = run(hi, in96, 96000.0);
    const size_t lat96 = (size_t)hi.latencySamples();
    const double red96 = bandDb(o96.l, 96000 + lat96, 180000 + lat96, 300, 12000, 96000.0) - bandDb(in96.l, 96000, 180000, 300, 12000, 96000.0);
    VT_FIG("profile learned at 48 kHz used at 96 kHz: the same noise goes down %.1f dB (%.1f dB at 48 kHz, floor 30)", -red96, -red);
    CHECK_MSG(red96 < -12.0, "at 96 kHz: reduced by %.1f dB", -red96);
    // garbage in the state does no harm
    Lint g;
    g.loadState(Json::parse("{\"type\":\"denoise\",\"params\":{},\"extra\":{\"learned\":true,\"bins\":99999,\"noise\":[1,2,3]}}"));
    g.prepare(kSr);
    CHECK(!g.hasProfile());
    g.loadState(Json::parse("{\"type\":\"denoise\",\"params\":{},\"extra\":{\"learned\":true,\"bins\":3,\"rate\":48000,\"noise\":[-99999,\"x\",50000]}}"));
    g.prepare(kSr);
    CHECK(allFinite(run(g, in)));
}

TEST(denoise_does_not_pump_or_warble) {
    Lint d;
    d.prepare(kSr);
    learn(d, noise(2.0, 0.02f, kSr, 11u));
    const size_t lat = (size_t)d.latencySamples();
    // noise, digital silence, noise again
    Stereo in = noise(2.0, 0.02f, kSr, 61u);
    append(in, silence(1.0));
    append(in, noise(2.0, 0.02f, kSr, 62u));
    auto out = run(d, in);
    // silence in, silence out: exactly nothing once the last frame holding noise has gone by
    // (a frame that holds the end of one noise or the start of the next spreads a little of it over its whole
    // length, about -126 dBFS before the noise starts: so the check leaves one frame out at either end)
    const size_t gapStart = 96000 + lat + 2048, gapEnd = 144000 + lat - 2048;
    CHECK_MSG(peakOf(out.l, gapStart, gapEnd) == 0.0f && peakOf(out.r, gapStart, gapEnd) == 0.0f, "peak %g in the silence", peakOf(out.l, gapStart, gapEnd));
    // the residue is steady: level in 50 ms windows over the settled part, and right after the noise comes back
    auto windows = [&](size_t a, size_t b, double& lo, double& hi) {
        lo = 1e9; hi = -1e9;
        for (size_t w = a; w + 2400 <= b; w += 2400) { const double v = db(rmsOf(out.l, w, w + 2400)); lo = std::min(lo, v); hi = std::max(hi, v); }
    };
    double lo1, hi1, lo2, hi2, lo3, hi3, inLo, inHi;
    windows(24000 + lat, 96000 + lat - 2048, lo1, hi1);
    windows(144000 + lat + 2048, 144000 + lat + 2048 + 12000, lo2, hi2);        // the first quarter second after the silence
    windows(168000 + lat, 235000 + lat, lo3, hi3);
    {
        inLo = 1e9; inHi = -1e9;
        for (size_t w = 24000; w + 2400 <= 96000; w += 2400) { const double v = db(rmsOf(in.l, w, w + 2400)); inLo = std::min(inLo, v); inHi = std::max(inHi, v); }
    }
    VT_FIG("residual noise in 50 ms windows: %.2f to %.2f dBFS settled (the noise itself varies %.2f dB), %.2f to %.2f dBFS right after a silence, %.2f to %.2f later",
           lo1, hi1, inHi - inLo, lo2, hi2, lo3, hi3);
    CHECK_MSG(hi1 - lo1 < (inHi - inLo) + 1.0, "the residue varies by %.2f dB (the noise itself by %.2f)", hi1 - lo1, inHi - inLo);
    CHECK_MSG(hi2 < hi1 + 1.0, "after the silence the residue is %.2f dB louder than before", hi2 - hi1);
    CHECK(std::fabs(0.5 * (lo3 + hi3) - 0.5 * (lo1 + hi1)) < 0.5);

    // warble ("musical noise"): how much each small patch of the spectrum flickers from frame to frame. For plain
    // noise the power in one bin of one frame scatters with a standard deviation equal to its mean (ratio 1).
    // Isolated tones popping in and out make the residue scatter far more than that.
    auto flicker = [&](const std::vector<float>& x, size_t from, size_t to) {
        const int n = 1024;
        std::vector<float> re((size_t)n), im((size_t)n);
        std::vector<double> s1((size_t)n / 2, 0.0), s2((size_t)n / 2, 0.0);
        int frames = 0;
        for (size_t a = from; a + (size_t)n <= to; a += (size_t)n) {
            for (int i = 0; i < n; ++i) { re[(size_t)i] = x[a + (size_t)i] * (float)(0.5 - 0.5 * std::cos(kTwoPi * i / n)); im[(size_t)i] = 0.0f; }
            dsp::fft(re.data(), im.data(), n);
            for (int k = 4; k < n / 2; ++k) { const double p = (double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k]; s1[(size_t)k] += p; s2[(size_t)k] += p * p; }
            ++frames;
        }
        double ratio = 0.0;
        int count = 0;
        for (int k = 4; k < n / 2; ++k) {
            const double mean = s1[(size_t)k] / frames, var = s2[(size_t)k] / frames - mean * mean;
            if (mean > 0.0) { ratio += std::sqrt(std::max(0.0, var)) / mean; ++count; }
        }
        return ratio / count;
    };
    const double fIn = flicker(in.l, 24000, 94000), fOut = flicker(out.l, 24000 + lat, 94000 + lat);
    // the same through plain subtraction with no smoothing at all, for comparison
    Lint plain;
    plain.prepare(kSr);
    learn(plain, noise(2.0, 0.02f, kSr, 11u));
    plain.pTime->set(0.0f); plain.pFreq->set(0.0f); plain.pAttack->set(0.0f); plain.pRelease->set(10.0f);
    auto raw = run(plain, in);
    const double fRaw = flicker(raw.l, 24000 + lat, 94000 + lat);
    const double rawRed = db(rmsOf(raw.l, 24000 + lat, 94000 + lat) / rmsOf(in.l, 24000, 94000));
    VT_FIG("flicker of the spectrum (1 = plain noise): input %.2f, default settings %.2f, unsmoothed subtraction %.2f (which takes the noise down only %.1f dB)", fIn, fOut, fRaw, -rawRed);
    CHECK_MSG(fOut < 1.15 * fIn, "the residue flickers %.2f times the noise's own %.2f", fOut / fIn, fIn);
    CHECK(fRaw > 1.5 * fIn);            // the measure does see musical noise when it is there
}

TEST(denoise_attack_and_release_do_what_they_say) {
    // hiss, with a loud tone from 1.0 s to 2.0 s
    Stereo in = noise(4.0, 0.02f, kSr, 71u, true);
    for (size_t i = 48000; i < 96000; ++i) { const float t = 0.3f * (float)std::sin(kTwoPi * 3000.0 * (double)i / kSr); in.l[i] += t; in.r[i] += t; }
    auto make = [&](float attack, float release) {
        auto d = std::make_shared<Lint>();
        d->prepare(kSr);
        learn(*d, noise(2.0, 0.02f, kSr, 11u, true));
        d->pTime->set(0.0f); d->pAttack->set(attack); d->pRelease->set(release);
        return d;
    };
    const size_t lat = 1984;
    // release: after the tone stops, the bins it held open let the hiss around 3 kHz through for about that long
    auto after = [&](float release) {
        auto d = make(0.0f, release);
        auto out = run(*d, in);
        return bandDb(out.l, 96000 + lat + 2400, 96000 + lat + 12000, 2950, 3050);       // 50 to 250 ms after the tone
    };
    const double shortRel = after(10.0f), longRel = after(1500.0f);
    VT_FIG("hiss beside the tone, 50 to 250 ms after it stops: %.1f dB louder with a 1500 ms release than with 10 ms", longRel - shortRel);
    CHECK_MSG(longRel > shortRel + 10.0, "release makes %.1f dB of difference", longRel - shortRel);
    // attack: the start of the tone is held back
    auto start = [&](float attack) {
        auto d = make(attack, 100.0f);
        auto out = run(*d, in);
        return levelDbAt(out.l, 48000 + lat + 480, 1920, 3000.0, kSr);       // 10 to 50 ms into the tone
    };
    const double fast = start(0.0f), slow = start(200.0f);
    VT_FIG("the first 50 ms of the tone: %.1f dBFS with no attack time, %.1f dBFS with 200 ms (the tone is -10.5 dBFS)", fast, slow);
    CHECK_NEAR(fast, -10.5, 1.5);
    CHECK_MSG(slow < fast - 6.0, "attack makes %.1f dB of difference", fast - slow);
    // and either way the tone is all there once it has been going for a while
    auto d = make(200.0f, 1500.0f);
    auto out = run(*d, in);
    CHECK_NEAR(levelDbAt(out.l, 48000 + lat + 36000, 8192, 3000.0, kSr), -10.46, 0.5);
}

TEST(denoise_learning_again_replaces_the_profile_and_an_empty_learn_keeps_it) {
    Lint d;
    d.prepare(kSr);
    learn(d, noise(1.0, 0.01f, kSr, 5u));
    const Json first = d.saveExtra();
    CHECK(first["learned"].b(false));
    // learn switched on and off again before a single frame went by: nothing learned, the profile stays
    const Stereo nz = noise(0.5, 0.2f, kSr, 6u);
    auto out = run(d, nz, kSr, nullptr, [&](size_t i) { d.pLearn->set(i == 64 * 9 ? 1.0f : 0.0f); });
    CHECK(d.saveExtra() == first);
    CHECK(d.hasProfile());
    // learning louder noise replaces it
    learn(d, noise(1.0, 0.1f, kSr, 7u));
    const Json second = d.saveExtra();
    CHECK(!(second == first));
    const int was = first["noise"].at(300).i(), is = second["noise"].at(300).i();
    VT_FIG("bin 300 of the profile: %d tenths of a dB for -43 dBFS noise, %d for -23 dBFS noise", was, is);
    CHECK_NEAR(is - was, 200.0, 15.0);          // 20 dB louder noise: 200 tenths of a dB
    // white noise of variance v has a density of v / rate per Hz: the stored number is that, in tenths of a dB
    const double want = 100.0 * std::log10((0.1 * 0.1 / 3.0) / kSr);
    CHECK_NEAR(is, want, 12.0);
    d.reset();
    const Stereo loud = noise(2.0, 0.1f, kSr, 8u);
    out = run(d, loud);
    CHECK(db(rmsOf(out.l, 60000) / rmsOf(loud.l, 60000)) < -20.0);
    // reset does not forget the profile, prepare neither
    d.reset();
    CHECK(d.hasProfile() && d.saveExtra() == second);
    d.prepare(44100.0);
    CHECK(d.hasProfile() && d.saveExtra() == second);
}

TEST(denoise_profile_can_be_saved_and_loaded_while_it_runs) {
    // The audio thread learns and processes while another thread saves and loads the state, as the program does
    // when a project is saved during playback. Nothing may block, crash or come out half-written.
    Lint d;
    d.prepare(kSr);
    learn(d, noise(0.5, 0.02f, kSr, 11u));
    const Json good = d.saveState();
    std::atomic<bool> stop{false};
    std::atomic<int> saves{0}, bad{0};
    std::thread other([&] {
        Lint probe;
        while (!stop.load()) {
            const Json s = d.saveState();
            const Json& ex = s["extra"];
            // whatever is read is a whole profile: all of its numbers there and in range
            if (ex["learned"].b(false)) {
                const int bins = ex["bins"].i();
                if (bins != 1025 || (int)ex["noise"].size() != bins) bad.fetch_add(1);
                else for (int k = 0; k < bins; k += 37) { const int v = ex["noise"].at((size_t)k).i(); if (v < -3000 || v > 300) bad.fetch_add(1); }
            }
            if (saves.fetch_add(1) % 3 == 2) d.loadState(good);
        }
    });
    const Stereo nz = noise(0.25, 0.03f, kSr, 5u);
    const int64 before = Rt::allocationsInRt.load();
    bool finite = true;
    // 40 rounds, and more if the other thread has had little time (a busy machine, a Debug build): the test is
    // about the two running together, however the scheduler shares them out
    for (int round = 0; round < 40 || (saves.load() <= 12 && round < 4000); ++round) {
        Stereo io = nz;
        for (size_t i = 0; i + kQuantum <= io.l.size(); i += kQuantum) {
            d.pLearn->set((round % 4 == 1 && i < 6400) ? 1.0f : 0.0f);
            ProcessContext c;
            c.io[0] = io.l.data() + i;
            c.io[1] = io.r.data() + i;
            Rt::Scope rt;
            d.process(c);
        }
        finite = finite && allFinite(io);
    }
    stop.store(true);
    other.join();
    CHECK(finite);
    CHECK_MSG(bad.load() == 0, "%d half-written profiles were read", bad.load());
    CHECK(saves.load() > 10);
    CHECK(Rt::allocationsInRt.load() == before);
    CHECK(d.hasProfile());
}

TEST(denoise_each_side_has_its_own_profile_and_mono_is_cheaper_not_different) {
    // hiss on the left only: the right side has nothing to learn and is left alone
    Stereo hiss = noise(1.5, 0.02f, kSr, 11u);
    std::fill(hiss.r.begin(), hiss.r.end(), 0.0f);
    Lint d;
    d.prepare(kSr);
    learn(d, hiss);
    Stereo in = noise(2.0, 0.02f, kSr, 31u);
    const size_t lat = (size_t)d.latencySamples();
    auto out = run(d, in);
    const double left = db(rmsOf(out.l, 48000 + lat) / rmsOf(in.l, 48000)), right = db(rmsOf(out.r, 48000 + lat) / rmsOf(in.r, 48000));
    VT_FIG("profile learned from hiss on the left only: left %.1f dB, right %.2f dB", left, right);
    CHECK(left < -20.0 && std::fabs(right) < 0.1);
    // the same sound on both sides takes a shortcut (one transform instead of two); the result is the same as the long way
    Lint m, s;
    m.prepare(kSr); s.prepare(kSr);
    const Stereo same = noise(1.5, 0.02f, kSr, 11u, true);
    learn(m, same); learn(s, same);
    Stereo both = add(noise(2.0, 0.02f, kSr, 41u, true), tone(500.0, 2.0, 0.1f));
    Stereo different = both;
    for (size_t i = 0; i < different.r.size(); ++i) different.r[i] *= 0.999f;         // not bit-identical sides: no shortcut
    auto om = run(m, both), os = run(s, different);
    CHECK(om.l == om.r);
    CHECK(om.l == os.l);
    CHECK(bandDb(om.l, 60000 + lat, 90000 + lat, 3000, 9000) - bandDb(both.l, 60000, 90000, 3000, 9000) < -12.0);
}

TEST(denoise_takes_out_hum_lines_between_the_notes) {
    // mains hum and its harmonics with some hiss; then the same under a tone
    auto hum = [](double seconds, uint32 seed) {
        Stereo s = noise(seconds, 0.003f, kSr, seed, true);
        for (size_t i = 0; i < s.l.size(); ++i) {
            double v = 0.0;
            for (int h = 1; h <= 8; ++h) v += (0.03 / h) * std::sin(kTwoPi * 50.0 * h * (double)i / kSr + h);
            s.l[i] += (float)v; s.r[i] += (float)v;
        }
        return s;
    };
    Lint d;
    d.prepare(kSr);
    for (auto& pr : d.factoryPresets()) if (pr.name == "Hum and buzz lines") d.loadState(pr.state);
    CHECK(d.pFreq->get() == 0.0f);
    learn(d, hum(2.0, 3u));
    const Stereo in = add(hum(3.0, 4u), tone(440.0, 3.0, 0.2f));
    auto out = run(d, in);
    const size_t lat = (size_t)d.latencySamples(), a = 60000;
    for (double hz : {50.0, 100.0, 150.0, 250.0}) {
        const double before = levelDbAt(in.l, a, 65536, hz, kSr), after = levelDbAt(out.l, a + lat, 65536, hz, kSr);
        CHECK_MSG(after < before - 12.0, "%.0f Hz hum: %.1f dB", hz, after - before);
        VT_FIG("hum line at %.0f Hz: %.1f dB", hz, after - before);
    }
    const double toneChange = levelDbAt(out.l, a + lat, 65536, 440.0, kSr) - levelDbAt(in.l, a, 65536, 440.0, kSr);
    VT_FIG("the 440 Hz tone over the hum: %+.2f dB", toneChange);
    CHECK(std::fabs(toneChange) < 1.0);
}

TEST(denoise_on_the_recorded_factory_voice_with_hiss_added) {
    Stereo clean;
    double sr = 0.0;
    if (!factoryVoice("C4", clean, sr, 3.0)) return;
    // hiss at -41 dBFS under the voice; one second of the hiss alone to learn from
    const Stereo hiss = noise((double)clean.l.size() / sr, 0.015f, sr, 91u);
    Stereo in = clean;
    for (size_t i = 0; i < in.l.size(); ++i) { in.l[i] += hiss.l[i]; in.r[i] += hiss.r[i]; }
    Lint d;
    d.prepare(sr);
    learn(d, noise(1.0, 0.015f, sr, 92u), sr);
    auto out = run(d, in, sr);
    const size_t lat = (size_t)d.latencySamples(), a = (size_t)(0.6 * sr), b = (size_t)(2.6 * sr);
    // up where the voice has next to nothing, the hiss goes down by the floor; down where the voice lives, its level stays
    const double top = bandDb(out.l, a + lat, b + lat, 9000, 18000, sr) - bandDb(in.l, a, b, 9000, 18000, sr);
    const double voiceBand = bandDb(out.l, a + lat, b + lat, 100, 3000, sr) - bandDb(clean.l, a, b, 100, 3000, sr);
    // what is wrong with the result, against the clean recording: before it was all the hiss
    double errOut = 0.0, errIn = 0.0, sig = 0.0;
    for (size_t i = a; i < b; ++i) {
        const double eo = out.l[i + lat] - clean.l[i], ei = in.l[i] - clean.l[i];
        errOut += eo * eo; errIn += ei * ei; sig += (double)clean.l[i] * clean.l[i];
    }
    printf("      the sung C4 with hiss at -41 dBFS: hiss at 9-18 kHz %.1f dB; the voice band (100 Hz-3 kHz) %+.2f dB; signal to error %.1f dB before, %.1f dB after\n",
           top, voiceBand, 10.0 * std::log10(sig / errIn), 10.0 * std::log10(sig / errOut));
    CHECK_MSG(top < -12.0, "hiss above 9 kHz reduced by %.1f dB", -top);
    CHECK_MSG(std::fabs(voiceBand) < 1.0, "the voice band changed by %.2f dB", voiceBand);
    CHECK_MSG(errOut < 0.5 * errIn, "the error went from %.1f to %.1f dB below the voice", 10.0 * std::log10(sig / errIn), 10.0 * std::log10(sig / errOut));
}
