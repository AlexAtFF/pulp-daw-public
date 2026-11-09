// Talker: the channel vocoder against signals with energy in known places.
#include "VocalTest.h"
using namespace pulp;
using namespace vt;

namespace {
const double kSr = 48000.0;

double centreOf(const Talker& t, int band) {
    double lo, hi;
    t.bandEdges(band, lo, hi);
    return std::sqrt(lo * hi);
}
// Power of a signal between two frequencies as a share of all of it.
double share(const std::vector<double>& spec, double f1, double f2) { return bandPower(spec, f1, f2, kSr) / std::max(1e-30, bandPower(spec, 0.0, kSr / 2, kSr)); }

// A band-limited saw by adding harmonics.
Stereo sawTone(double hz, double seconds, float amp) {
    Stereo s = silence(seconds);
    const int H = (int)(0.45 * kSr / hz);
    for (size_t i = 0; i < s.l.size(); ++i) {
        double v = 0.0;
        for (int h = 1; h <= H; ++h) v += std::sin(kTwoPi * hz * h * (double)i / kSr) / h;
        s.l[i] = s.r[i] = amp * (float)(v * 2.0 / kPi);
    }
    return s;
}
// Envelope: the largest absolute value in each run of `win` samples.
std::vector<float> peaksOver(const std::vector<float>& x, size_t win) {
    std::vector<float> e;
    for (size_t a = 0; a + win <= x.size(); a += win) e.push_back(peakOf(x, a, a + win));
    return e;
}
}

TEST(vocoder_energy_in_one_band_comes_out_in_that_band_only) {
    const Stereo carrier = noise(3.0, 0.3f, kSr, 5u);           // flat noise on the sidechain
    for (int band : {3, 9, 15}) {
        Talker t;
        t.prepare(kSr);
        t.pSibil->set(0.0f);
        const int bands = t.pBands->getInt();
        CHECK(bands == 20);
        double lo, hi;
        t.bandEdges(band, lo, hi);
        const double fc = std::sqrt(lo * hi), ratio = hi / lo;      // one band is `ratio` wide
        auto out = run(t, tone(fc, 3.0, 0.25f), kSr, &carrier);
        CHECK(t.meter(0) == 1.0f);
        const auto spec = powerSpectrum(out.l, 24000, out.l.size(), 8192);
        const double inBand = share(spec, lo, hi);
        const double within1 = share(spec, lo / ratio, hi * ratio);
        const double beyond2 = 1.0 - share(spec, lo / (ratio * ratio), hi * ratio * ratio);
        const double beyond5 = 1.0 - share(spec, lo / std::pow(ratio, 5.0), hi * std::pow(ratio, 5.0));
        VT_FIG("tone at band %d (%.0f Hz, %.0f to %.0f): %.1f %% of the output in the band, %.1f %% within one band either side, %.1f dB beyond two, %.1f dB beyond five; output %.1f dBFS",
               band, fc, lo, hi, 100.0 * inBand, 100.0 * within1, 10.0 * std::log10(beyond2 + 1e-12), 10.0 * std::log10(beyond5 + 1e-12), db(rmsOf(out.l, 24000)));
        CHECK_MSG(inBand > 0.6, "band %d: only %.1f %% in the band", band, 100.0 * inBand);
        CHECK_MSG(within1 > 0.93, "band %d: only %.1f %% within one band either side", band, 100.0 * within1);
        CHECK_MSG(beyond2 < 0.02, "band %d: %.2f %% beyond two bands", band, 100.0 * beyond2);
        CHECK_MSG(beyond5 < 0.002, "band %d: %.3f %% beyond five bands", band, 100.0 * beyond5);
        CHECK(rmsOf(out.l, 24000) > 0.003);
        // the noise in the band is as loud as the tone says: twice the tone, 6 dB more
        Talker t2;
        t2.prepare(kSr);
        t2.pSibil->set(0.0f);
        auto louder = run(t2, tone(fc, 3.0, 0.5f), kSr, &carrier);
        CHECK_NEAR(db(rmsOf(louder.l, 24000) / rmsOf(out.l, 24000)), 6.0, 0.2);
    }
    // no modulator: no output, whatever the carrier does
    Talker t;
    t.prepare(kSr);
    auto out = run(t, silence(1.0), kSr, &carrier);
    CHECK(peak(out) == 0.0f);
    // narrower bands (width 0.5): the band's output is narrower
    auto spread = [&](float width) {
        Talker w;
        w.prepare(kSr);
        w.pSibil->set(0.0f);
        w.pWidth->set(width);
        const double fc = centreOf(w, 9);
        auto o = run(w, tone(fc, 3.0, 0.25f), kSr, &carrier);
        const auto spec = powerSpectrum(o.l, 24000, o.l.size(), 8192);
        double m = 0.0, s2 = 0.0, tot = 0.0;
        for (size_t k = 1; k < spec.size(); ++k) { const double oct = std::log2((double)k * kSr / 8192.0 / fc); m += oct * spec[k]; tot += spec[k]; }
        m /= tot;
        for (size_t k = 1; k < spec.size(); ++k) { const double oct = std::log2((double)k * kSr / 8192.0 / fc) - m; s2 += oct * oct * spec[k]; }
        return std::sqrt(s2 / tot);
    };
    const double wide = spread(1.0), narrow = spread(0.5), wider = spread(2.0);
    VT_FIG("spread of one band's output: %.3f octaves at width 0.5, %.3f at 1, %.3f at 2", narrow, wide, wider);
    CHECK(narrow < 0.75 * wide && wider > 1.3 * wide);
}

TEST(vocoder_band_layout_follows_its_controls) {
    Talker t;
    t.prepare(kSr);
    for (int bands : {8, 20, 40}) {
        t.pBands->set((float)bands);
        t.pLow->set(100.0f); t.pHigh->set(8000.0f); t.pWidth->set(1.0f);
        double lo, hi, prevHi = 100.0;
        for (int b = 0; b < bands; ++b) {
            t.bandEdges(b, lo, hi);
            CHECK_NEAR(lo, prevHi, 0.01 * prevHi);          // log spaced and touching
            prevHi = hi;
        }
        CHECK_NEAR(prevHi, 8000.0, 1.0);
    }
    // each band of the bank passes its own centre at full level: a tone at every centre in turn gives the same output
    // level from a noise carrier whose bands have the same share of an octave
    Talker u;
    u.prepare(kSr);
    u.pSibil->set(0.0f);
    u.pBands->set(12.0f);
    const Stereo carrier = noise(1.5, 0.3f, kSr, 5u);
    double lo = 1e9, hi = -1e9;
    for (int b = 0; b < 12; ++b) {
        u.reset();
        double f1, f2;
        u.bandEdges(b, f1, f2);
        auto out = run(u, tone(std::sqrt(f1 * f2), 1.5, 0.25f), kSr, &carrier);
        // white noise has power in proportion to the width in Hz; take that out
        const double level = db(rmsOf(out.l, 24000)) - 10.0 * std::log10(f2 - f1);
        lo = std::min(lo, level); hi = std::max(hi, level);
    }
    VT_FIG("12 bands, a tone at each centre: band levels (per Hz of band) within %.2f dB of each other", hi - lo);
    CHECK_MSG(hi - lo < 2.5, "band levels differ by %.2f dB", hi - lo);
}

TEST(vocoder_envelope_attack_and_release_times) {
    struct Case { float attack, release; double tol; } cases[] = {{50, 200, 0.08}, {10, 40, 0.08}, {150, 600, 0.08}, {2, 20, 0.08}};
    for (auto& cs : cases) {
        Talker t;
        t.prepare(kSr);
        t.pSibil->set(0.0f);
        t.pAttack->set(cs.attack); t.pRelease->set(cs.release);
        const double fc = centreOf(t, 10);
        // modulator: the tone switched on at 0.5 s and off at 2.5 s; carrier: the same tone, steady
        Stereo mod = tone(fc, 5.0, 0.25f);
        const size_t on = 24000, off = 120000;
        for (size_t i = 0; i < mod.l.size(); ++i) if (i < on || i >= off) mod.l[i] = mod.r[i] = 0.0f;
        const Stereo carrier = tone(fc, 5.0, 0.5f);
        auto out = run(t, mod, kSr, &carrier);
        const size_t win = 24;                                      // half a millisecond
        const auto env = peaksOver(out.l, win);
        const float full = env[(off - 2400) / win];
        size_t up = on / win, down = off / win;
        while (up < env.size() && env[up] < 0.632f * full) ++up;
        while (down < env.size() && env[down] > 0.368f * full) ++down;
        const double attackMs = (double)(up * win - on) / 48.0, releaseMs = (double)(down * win - off) / 48.0;
        VT_FIG("attack %.0f ms set: 63 %% after %.1f ms; release %.0f ms set: 37 %% after %.1f ms", cs.attack, attackMs, cs.release, releaseMs);
        // the band filter itself takes a few milliseconds to respond (this band is 176 Hz wide), on top of the time set
        CHECK_MSG(std::fabs(attackMs - cs.attack) < cs.tol * cs.attack + 3.5, "attack %.0f ms measured %.1f ms", cs.attack, attackMs);
        CHECK_MSG(std::fabs(releaseMs - cs.release) < cs.tol * cs.release + 3.5, "release %.0f ms measured %.1f ms", cs.release, releaseMs);
        CHECK(full > 0.01f);
        // steady part is steady: the envelope has no ripple to speak of
        float lo = 1e9f, hi = 0.0f;
        for (size_t k = (on + 48000) / win; k < (off - 2400) / win; ++k) { lo = std::min(lo, env[k]); hi = std::max(hi, env[k]); }
        CHECK_MSG(hi / lo < 1.03f, "steady envelope varies by %.2f dB", db(hi / lo));
    }
}

TEST(vocoder_makes_sound_with_its_own_carrier_when_nothing_is_routed) {
    const auto v = mono(steadyVoice(131.0, 2.0));
    const double inRms = rmsOf(v.l, 24000);
    for (int type = 0; type < 3; ++type) {
        Talker t;
        t.prepare(kSr);
        t.pCarrier->set((float)type);
        t.pChord->set(0.0f);                // one note
        t.pPitch->set(45.0f);               // 110 Hz
        auto out = run(t, v);
        CHECK(t.meter(0) == 0.0f);
        CHECK(allFinite(out));
        const double level = db(rmsOf(out.l, 24000) / inRms);
        const double at110 = levelDbAt(out.l, 24000, 48000, 660.0, kSr), between = levelDbAt(out.l, 24000, 48000, 605.0, kSr);
        const double at131 = levelDbAt(out.l, 24000, 48000, 655.0, kSr);
        VT_FIG("own carrier %s: output %+.1f dB against the voice; 660 Hz (carrier harmonic) %.1f dBFS, 605 Hz (between) %.1f, 655 Hz (voice harmonic) %.1f",
               type == 0 ? "saw" : type == 1 ? "pulse" : "noise", level, at110, between, at131);
        CHECK_MSG(level > -9.0 && level < 9.0, "carrier type %d: output %.1f dB against the voice", type, level);
        if (type < 2) {
            // the output is at the carrier's pitch (110 Hz and its multiples), not at the voice's
            CHECK_MSG(at110 > between + 25.0 && at110 > at131 + 12.0, "carrier type %d: 660 Hz %.1f, 605 Hz %.1f, 655 Hz %.1f", type, at110, between, at131);
            CHECK_NEAR(fundamentalHz(out.l, 24000, 90000, 110.0, kSr), 110.0, 0.4);
        } else {
            CHECK(std::fabs(at110 - between) < 12.0);       // noise: no lines
        }
        // stereo carrier: the two sides are not the same signal, but equally loud
        if (type != 1 || true) {
            CHECK(maxAbsDiff(out.l, out.r) > 1e-4);
            CHECK_NEAR(db(rmsOf(out.l, 24000) / rmsOf(out.r, 24000)), 0.0, 1.0);
        }
    }
    // the chord control: a fifth adds 165 Hz and 220 Hz to 110 Hz
    Talker t;
    t.prepare(kSr);
    t.pChord->set(2.0f);
    t.pPitch->set(45.0f);
    auto out = run(t, noise(2.0, 0.2f, kSr, 3u, true));
    const double fifth = levelDbAt(out.l, 24000, 48000, 164.8 * 3, kSr), off = levelDbAt(out.l, 24000, 48000, 164.8 * 3 + 30.0, kSr);
    CHECK_MSG(fifth > off + 15.0, "fifth's harmonic %.1f dBFS, beside it %.1f", fifth, off);
    // the pitch control moves it
    Talker p;
    p.prepare(kSr);
    p.pChord->set(0.0f);
    p.pPitch->set(57.0f);
    out = run(p, v);
    CHECK_NEAR(fundamentalHz(out.l, 24000, 90000, 220.0, kSr), 220.0, 0.8);
}

TEST(vocoder_uses_the_sidechain_when_it_is_routed) {
    const auto v = mono(steadyVoice(110.0, 2.0));
    const Stereo carrier = sawTone(150.0, 2.0, 0.5f);
    Talker t;
    t.prepare(kSr);
    auto out = run(t, v, kSr, &carrier);
    CHECK(t.meter(0) == 1.0f);
    const double at600 = levelDbAt(out.l, 24000, 48000, 600.0, kSr), at550 = levelDbAt(out.l, 24000, 48000, 550.0, kSr), at660 = levelDbAt(out.l, 24000, 48000, 660.0, kSr);
    VT_FIG("sidechain saw at 150 Hz, voice at 110 Hz: 600 Hz %.1f dBFS, 550 Hz %.1f, 660 Hz %.1f", at600, at550, at660);
    CHECK(at600 > at550 + 25.0 && at600 > at660 + 25.0);
    CHECK(maxAbsDiff(out.l, out.r) == 0.0);         // a mono carrier and a mono voice give a mono result
    // the switch off: back to the built-in carrier (110 Hz saw chord), although the sidechain is there
    Talker u;
    u.prepare(kSr);
    u.pSide->set(0.0f);
    u.pChord->set(0.0f);
    u.pPitch->set(45.0f);
    out = run(u, v, kSr, &carrier);
    CHECK(u.meter(0) == 0.0f);
    CHECK(levelDbAt(out.l, 24000, 48000, 660.0, kSr) > levelDbAt(out.l, 24000, 48000, 600.0, kSr) + 20.0);

    // and through the mixer: a voice on insert 2 with Talker, a saw on insert 1 sent only to its sidechain
    for (int routed = 0; routed < 2; ++routed) {
        Project p;
        p.global->tempo.set(120.0f);
        auto saw = std::make_shared<SampleData>();
        saw->sampleRate = kSr; saw->channels = 1; saw->ch[0] = carrier.l;
        auto car = addSampleChannel(p, saw, "Saw");
        p.channels.back().mixerTrack = 1;
        Project::setStep(p.pattern().notesFor(car.id), 0, true);
        auto vs = std::make_shared<SampleData>();
        vs->sampleRate = kSr; vs->channels = 1; vs->ch[0] = v.l;
        auto vc = addSampleChannel(p, vs, "Voice");
        p.channels.back().mixerTrack = 2;
        Project::setStep(p.pattern().notesFor(vc.id), 0, true);
        p.setRoute(1, 0, false);                        // the saw is not heard on its own
        if (routed) CHECK(p.setRoute(1, 2, true, true));
        auto talker = std::make_shared<Talker>();
        talker->pChord->set(0.0f);
        talker->pPitch->set(45.0f);
        p.setSlot(2, 0, talker);
        RenderOptions o;
        o.lengthSamples = 96000;
        auto r = renderOffline(p, o);
        const double c600 = levelDbAt(r->ch[0], 24000, 48000, 600.0, kSr), c660 = levelDbAt(r->ch[0], 24000, 48000, 660.0, kSr);
        VT_FIG("in a project, sidechain %s: 600 Hz %.1f dBFS, 660 Hz %.1f", routed ? "routed" : "not routed", c600, c660);
        if (routed) CHECK(c600 > c660 + 20.0 && talker->meter(0) == 1.0f);
        else CHECK(c660 > c600 + 20.0 && talker->meter(0) == 0.0f);
    }
}

TEST(vocoder_formant_shift_moves_the_carrier_bank) {
    const Stereo carrier = noise(3.0, 0.3f, kSr, 5u);
    for (float shift : {-7.0f, 0.0f, 5.0f, 12.0f}) {
        Talker t;
        t.prepare(kSr);
        t.pSibil->set(0.0f);
        t.pFormant->set(shift);
        double lo, hi;
        t.bandEdges(9, lo, hi);
        const double fc = std::sqrt(lo * hi), k = std::pow(2.0, shift / 12.0);
        auto out = run(t, tone(fc, 3.0, 0.25f), kSr, &carrier);
        const auto spec = powerSpectrum(out.l, 24000, out.l.size(), 8192);
        const double moved = share(spec, lo * k, hi * k), stayed = share(spec, lo, hi);
        VT_FIG("formant shift %+.0f st: %.1f %% of the output in the band moved by that much, %.1f %% where the tone is", shift, 100.0 * moved, 100.0 * stayed);
        CHECK_MSG(moved > 0.6, "shift %.0f: %.1f %% in the moved band", shift, 100.0 * moved);
        if (shift != 0.0f) CHECK(stayed < 0.2);
    }
    // bands pushed past the top of the spectrum are dropped, not folded: +12 st at 96 % of the range stays finite and quiet
    Talker t;
    t.prepare(kSr);
    t.pFormant->set(12.0f);
    t.pHigh->set(16000.0f);
    auto out = run(t, noise(1.0, 0.3f, kSr, 9u), kSr, &carrier);
    CHECK(allFinite(out) && peak(out) < 4.0f);
}

TEST(vocoder_noise_for_sibilants_and_air) {
    // an "s": noise above 5 kHz. The saw carrier has little up there; with unvoiced noise switched in, the band comes back.
    Stereo ess = noise(1.5, 0.2f, kSr, 21u, true);
    dsp::Biquad hp[2];
    hp[0].highpass(5000.0, 0.54, kSr); hp[1].highpass(5000.0, 1.31, kSr);
    for (auto& x : ess.l) x = hp[1].process(hp[0].process(x));
    ess.r = ess.l;
    auto level = [&](float sibil, float& unvoiced) {
        Talker t;
        t.prepare(kSr);
        t.pSibil->set(sibil);
        t.pChord->set(0.0f);
        t.pPitch->set(36.0f);
        auto out = run(t, ess);
        unvoiced = t.meter(1);
        return bandPower(powerSpectrum(out.l, 24000, out.l.size()), 5000, 8000, kSr);
    };
    float u0 = 0.0f, u1 = 0.0f;
    const double without = level(0.0f, u0), with = level(1.0f, u1);
    VT_FIG("an 's' (noise above 5 kHz) on a low saw: 5 to 8 kHz is %.1f dB louder with unvoiced noise; judged %.2f unvoiced", 10.0 * std::log10(with / without), u1);
    CHECK_MSG(with > 4.0 * without, "unvoiced noise adds only %.1f dB", 10.0 * std::log10(with / without));
    CHECK(u1 > 0.9f && u0 > 0.9f);
    // a vowel is not judged unvoiced, and its vocoded sound is the same with the control up or down
    Talker a, b;
    a.prepare(kSr); b.prepare(kSr);
    a.pSibil->set(0.0f); b.pSibil->set(1.0f);
    const auto v = mono(steadyVoice(131.0, 1.0));
    auto oa = run(a, v), ob = run(b, v);
    CHECK_MSG(b.meter(1) < 0.02f, "a vowel is judged %.2f unvoiced", b.meter(1));
    CHECK(maxAbsDiff(oa.l, ob.l) < 1e-4);
    VT_FIG("a vowel: judged %.3f unvoiced", b.meter(1));

    // air: the dry signal above the air frequency, added to the vocoded sound
    Talker t;
    t.prepare(kSr);
    t.pAir->set(1.0f); t.pAirFreq->set(4000.0f); t.pHigh->set(2000.0f); t.pSibil->set(0.0f);
    auto out = run(t, tone(10000.0, 1.0, 0.1f));
    CHECK_NEAR(levelDbAt(out.l, 24000, 16384, 10000.0, kSr), -20.0, 0.5);        // well above 4 kHz: all of it
    t.reset();
    out = run(t, tone(1000.0, 1.0, 0.1f));
    CHECK(levelDbAt(out.l, 24000, 16384, 1000.0, kSr) < -40.0);                  // two octaves below: 24 dB down, and the top band (2 kHz) hardly hears it
    t.pAir->set(0.0f);
    t.reset();
    out = run(t, tone(10000.0, 1.0, 0.1f));
    CHECK(levelDbAt(out.l, 24000, 16384, 10000.0, kSr) < -60.0);
    t.pAir->set(0.5f);
    t.reset();
    out = run(t, tone(10000.0, 1.0, 0.1f));
    CHECK_NEAR(levelDbAt(out.l, 24000, 16384, 10000.0, kSr), -26.0, 0.5);
}

TEST(vocoder_mix_output_level_and_no_latency) {
    Talker t;
    t.prepare(kSr);
    CHECK(t.latencySamples() == 0);
    t.pMix->set(0.0f);
    Stereo in = mono(steadyVoice(180.0, 0.5));
    for (auto& x : in.r) x *= 0.5f;
    auto out = run(t, in);
    CHECK(out.l == in.l && out.r == in.r);          // dry, not delayed, bit for bit
    // output level in dB is what it says
    Talker a, b;
    a.prepare(kSr); b.prepare(kSr);
    b.pOut->set(-12.0f);
    auto oa = run(a, in), ob = run(b, in);
    CHECK_NEAR(db(rmsOf(ob.l, 12000) / rmsOf(oa.l, 12000)), -12.0, 0.01);
    // half mix: half the dry plus half the wet
    Talker h;
    h.prepare(kSr);
    h.pMix->set(0.5f);
    auto oh = run(h, in);
    double worst = 0.0;
    for (size_t i = 0; i < oh.l.size(); ++i) worst = std::max(worst, (double)std::fabs(oh.l[i] - 0.5f * (in.l[i] + oa.l[i])));
    CHECK(worst < 1e-5);
    // latency stays 0 whatever is set
    dsp::Noise rnd(5u);
    for (int i = 0; i < 20; ++i) { for (auto& p : t.params()) p->setNorm(rnd.uni()); CHECK(t.latencySamples() == 0); }
}

TEST(vocoder_changing_the_number_of_bands_does_not_click) {
    // a steady vocoded tone while the band count jumps about: around each change the output never steps
    // further from one sample to the next than it does in the steady stretches either side
    Talker t;
    t.prepare(kSr);
    t.pSibil->set(0.0f);
    const auto v = mono(steadyVoice(131.0, 3.0));
    const Stereo carrier = tone(440.0, 3.0, 0.4f);
    dsp::Noise rnd(17u);
    std::vector<size_t> changes;
    auto out = run(t, v, kSr, &carrier, [&](size_t i) {
        if (i > 24000 && (i / 64) % 150 == 0) { t.pBands->set(8.0f + std::floor(rnd.uni() * 32.9f)); changes.push_back(i); }
    });
    auto maxStep = [&](size_t a, size_t b) { float m = 0.0f; for (size_t i = a + 1; i < std::min(b, out.l.size()); ++i) m = std::max(m, std::fabs(out.l[i] - out.l[i - 1])); return m; };
    double worst = 0.0;
    int counted = 0;
    for (size_t c : changes) {
        if (c + 4000 > out.l.size()) continue;
        const float during = maxStep(c, c + 700), around = std::max(maxStep(c - 1500, c), maxStep(c + 700, c + 2200));
        worst = std::max(worst, (double)during / std::max(around, 1e-9f));
        ++counted;
    }
    VT_FIG("%d band-count changes: the largest step during a change is %.2f times the largest step either side of it", counted, worst);
    CHECK(counted >= 10);
    CHECK_MSG(worst < 1.2, "a change steps %.2f times further than the steady sound", worst);
    CHECK(allFinite(out));
    CHECK(t.meter(2) >= 8.0f && t.meter(2) <= 40.0f);
}

TEST(vocoder_on_the_recorded_factory_voice) {
    Stereo in;
    double sr = 0.0;
    if (!factoryVoice("C4", in, sr)) return;
    for (int type = 0; type < 3; ++type) {
        Talker t;
        t.prepare(sr);
        t.pCarrier->set((float)type);
        t.pChord->set(0.0f);
        t.pPitch->set(45.0f);
        auto out = run(t, in, sr);
        CHECK(allFinite(out));
        const size_t a = (size_t)(0.5 * sr), b = (size_t)(2.0 * sr);
        const double level = db(rmsOf(out.l, a, b) / rmsOf(in.l, a, b));
        printf("      the sung C4 through Talker on its own %s: output %+.1f dB against the voice, peak %.2f\n", type == 0 ? "saw" : type == 1 ? "pulse" : "noise", level, peak(out));
        CHECK_MSG(level > -10.0 && level < 10.0 && peak(out) < 2.0f, "level %.1f dB, peak %.2f", level, peak(out));
        if (type < 2) CHECK_NEAR(centsBetween(correlationHz(out.l, (size_t)(1.0 * sr), 110.0, sr), 110.0), 0.0, 8.0);      // the carrier's note, not the singer's
        CHECK(t.meter(1) < 0.2f);       // a sung vowel is not taken for a sibilant
    }
}
