// Second Voice: intervals, scale steps, delay, pan, formants, doubling.
#include "VocalTest.h"
using namespace pulp;
using namespace vt;

namespace {
const double kSr = 48000.0;
double hzOfKey(double key) { return 440.0 * std::pow(2.0, (key - 69.0) / 12.0); }
Stereo afterSilence(const std::vector<float>& v, double sr = kSr) {
    Stereo s = silence(0.15, sr);
    append(s, mono(v));
    return s;
}
// Only voice 1, no dry: what one voice does on its own.
void soloVoice(SecondVoice& h, int interval, float detune = 0.0f) {
    h.pDry->set(-60.0f);
    for (int i = 1; i < SecondVoice::kVoices; ++i) h.pOn[i]->set(0.0f);
    h.pOn[0]->set(1.0f);
    h.pInterval[0]->set((float)interval);
    h.pDetune[0]->set(detune);
    h.pLevel[0]->set(0.0f);
    h.pPan[0]->set(0.0f);
    h.pDelay[0]->set(0.0f);
}
}

TEST(harmony_each_interval_is_in_tune) {
    const double f0 = hzOfKey(57);
    const auto in = afterSilence(steadyVoice(f0, 1.0));
    const double inRms = rmsOf(in.l, 24000, 52000);
    for (int formant = 0; formant < 2; ++formant) {
        for (int interval : {-24, -12, -7, -5, -3, 3, 4, 7, 12, 19, 24}) {
            SecondVoice h;
            h.prepare(kSr);
            soloVoice(h, interval);
            h.pFormant->set((float)formant);
            auto out = run(h, in);
            const double want = f0 * std::pow(2.0, interval / 12.0);
            // the period, by correlation: two octaves down with the formants kept there is next to nothing at the fundamental itself
            double sum = 0.0;
            const auto track = correlationTrack(out.l, 26000, 50000, want, kSr, 20.0);
            for (double hz : track) sum += centsBetween(hz, want);
            const double cents = sum / (double)track.size();
            const double level = db(rmsOf(out.l, 24000, 52000) / inRms);
            CHECK_MSG(std::fabs(cents) < 5.0, "interval %+d (formants %s): %.2f cents off", interval, formant ? "kept" : "free", cents);
            // With the formants kept the level depends on where the new harmonics fall on the envelope; within 2.5 dB
            // up to a fifth either way, and within 7 dB at one and two octaves.
            CHECK_MSG(std::fabs(level) < (std::abs(interval) <= 7 ? 2.5 : 7.0), "interval %+d (formants %s): level %+.2f dB", interval, formant ? "kept" : "free", level);
            VT_FIG("interval %+3d st, formants %s: %+.2f cents, level %+.2f dB", interval, formant ? "kept" : "free", cents, level);
            CHECK(allFinite(out));
        }
    }
    // fine detune in cents, alone and on top of an interval
    for (float det : {-50.0f, -12.0f, 8.0f, 35.0f}) {
        SecondVoice h;
        h.prepare(kSr);
        soloVoice(h, 0, det);
        auto out = run(h, in);
        CHECK_NEAR(centsBetween(fundamentalHz(out.l, 24000, 52000, f0, kSr), f0), det, 1.0);
        h.pInterval[0]->set(7.0f);
        h.reset();
        out = run(h, in);
        const double want = f0 * std::pow(2.0, 7.0 / 12.0);
        CHECK_NEAR(centsBetween(fundamentalHz(out.l, 24000, 52000, want, kSr), want), det, 1.0);
    }
    // other rates and a higher voice
    for (double sr : {44100.0, 96000.0}) {
        SecondVoice h;
        h.prepare(sr);
        soloVoice(h, 4);
        h.pRange->set(1.0f);
        const double f = hzOfKey(64), want = hzOfKey(68);
        auto out = run(h, afterSilence(steadyVoice(f, 1.0, sr), sr), sr);
        CHECK_NEAR(centsBetween(fundamentalHz(out.l, (size_t)(0.5 * sr), (size_t)(1.05 * sr), want, sr), want), 0.0, 5.0);
    }
    // the sung pitch is not corrected: a voice 20 cents sharp gives a harmony 20 cents sharp
    {
        SecondVoice h;
        h.prepare(kSr);
        soloVoice(h, 7);
        auto out = run(h, afterSilence(steadyVoice(f0 * std::pow(2.0, 20.0 / 1200.0), 1.0)));
        const double want = f0 * std::pow(2.0, 7.0 / 12.0);
        CHECK_NEAR(centsBetween(fundamentalHz(out.l, 24000, 52000, want, kSr), want), 20.0, 1.0);
    }
    // vibrato is carried by the harmony, at the interval, at every moment
    {
        auto f0v = [f0](double t) { return f0 * std::pow(2.0, 0.4 * std::sin(kTwoPi * 5.5 * t) / 12.0); };
        const auto vib = afterSilence(voice(1.4, kSr, f0v, vowelAh()));
        SecondVoice h;
        h.prepare(kSr);
        soloVoice(h, 4);
        auto out = run(h, vib);
        const size_t lat = (size_t)h.latencySamples();
        const double up = std::pow(2.0, 4.0 / 12.0);
        const auto ti = pitchTrack(vib.l, 28800, 67200, f0, kSr, 25.0, 5.0), to = pitchTrack(out.l, 28800 + lat, 67200 + lat, f0 * up, kSr, 25.0, 5.0);
        double worst = 0.0;
        for (size_t i = 0; i < std::min(ti.size(), to.size()); ++i) worst = std::max(worst, std::fabs(centsBetween(to[i], ti[i] * up)));
        VT_FIG("a third above a voice with +-40 cents of vibrato: never more than %.1f cents from the interval", worst);
        CHECK_MSG(worst < 10.0, "the harmony strays %.1f cents from the interval", worst);      // it follows one period behind
    }
}

TEST(harmony_scale_steps_stay_in_the_key) {
    struct Case { int scale, key; double sung; int steps; double want; const char* what; } cases[] = {
        {vocal::Major, 0, 60, 2, 64, "C major: a third above C is E (4 semitones)"},
        {vocal::Major, 0, 62, 2, 65, "C major: a third above D is F (3 semitones)"},
        {vocal::Major, 0, 64, 2, 67, "C major: a third above E is G (3)"},
        {vocal::Major, 0, 65, 2, 69, "C major: a third above F is A (4)"},
        {vocal::Major, 0, 59, 2, 62, "C major: a third above B is D (3)"},
        {vocal::Major, 0, 64, -2, 60, "C major: a third below E is C (4 down)"},
        {vocal::Major, 0, 62, -2, 59, "C major: a third below D is B (3 down)"},
        {vocal::Major, 0, 60, 4, 67, "C major: a fifth above C is G"},
        {vocal::Major, 0, 59, 4, 65, "C major: a fifth above B is F (a diminished fifth, 6 semitones)"},
        {vocal::Major, 0, 60, 7, 72, "C major: seven steps is an octave"},
        {vocal::Minor, 9, 57, 2, 60, "A minor: a third above A is C (3)"},
        {vocal::Minor, 9, 60, 2, 64, "A minor: a third above C is E (4)"},
        {vocal::Major, 7, 66, 2, 69, "G major: a third above F# is A (3)"},
        {vocal::PentaMinor, 9, 57, 1, 60, "A minor pentatonic: one step above A is C"},
        {vocal::Chromatic, 0, 60, 2, 62, "chromatic: two steps are two semitones"},
    };
    for (auto& cs : cases) {
        SecondVoice h;
        h.prepare(kSr);
        soloVoice(h, cs.steps);
        h.pScale->set((float)cs.scale);
        h.pKey->set((float)cs.key);
        h.pRange->set(1.0f);
        auto out = run(h, afterSilence(steadyVoice(hzOfKey(cs.sung), 0.8)));
        const double cents = centsBetween(fundamentalHz(out.l, 24000, 43000, hzOfKey(cs.want), kSr), hzOfKey(cs.want));
        CHECK_MSG(std::fabs(cents) < 5.0, "%s: %.1f cents from it", cs.what, cents);
        VT_FIG("%s: %+.2f cents", cs.what, cents);
    }
    // a note sung a little off, or not in the scale at all, takes its interval from the nearest scale note
    SecondVoice h;
    h.prepare(kSr);
    soloVoice(h, 2);
    h.pScale->set((float)vocal::Major);
    h.pRange->set(1.0f);
    auto out = run(h, afterSilence(steadyVoice(hzOfKey(64.25), 0.8)));           // E, 25 cents sharp: G, 25 cents sharp
    CHECK_NEAR(centsBetween(fundamentalHz(out.l, 24000, 43000, hzOfKey(67), kSr), hzOfKey(67)), 25.0, 1.5);
    h.reset();
    out = run(h, afterSilence(steadyVoice(hzOfKey(61.3), 0.8)));                 // C# + 30: nearest is D, a third above D is F: 3 semitones up
    CHECK_NEAR(centsBetween(fundamentalHz(out.l, 24000, 43000, hzOfKey(64.3), kSr), hzOfKey(64.3)), 0.0, 1.5);
}

TEST(harmony_delay_and_pan) {
    // a click has no pitch: every voice passes it on unshifted, at its own delay and place
    Stereo in = silence(0.3);
    in.l[1000] = in.r[1000] = 1.0f;
    {
        SecondVoice h;
        h.prepare(kSr);
        h.pDry->set(-60.0f);
        const float dly[4] = {0.0f, 12.5f, 30.0f, 80.0f}, pan[4] = {-1.0f, -0.5f, 0.0f, 1.0f}, lvl[4] = {0.0f, -6.0f, 3.0f, -12.0f};
        for (int i = 0; i < 4; ++i) { h.pOn[i]->set(1.0f); h.pDelay[i]->set(dly[i]); h.pPan[i]->set(pan[i]); h.pLevel[i]->set(lvl[i]); h.pInterval[i]->set(7.0f); }
        auto out = run(h, in);
        const size_t lat = (size_t)h.latencySamples();
        for (int i = 0; i < 4; ++i) {
            const size_t at = 1000 + lat + (size_t)std::lround(dly[i] * 48.0);
            const double th = (pan[i] + 1.0) * kPi / 4.0, g = dbToGain(lvl[i]);
            CHECK_MSG(std::fabs(out.l[at] - g * 1.41421356 * std::cos(th)) < 1e-5 && std::fabs(out.r[at] - g * 1.41421356 * std::sin(th)) < 1e-5,
                      "voice %d: left %.5f right %.5f at its place", i + 1, out.l[at], out.r[at]);
            VT_FIG("voice %d: delay %.1f ms -> click %d samples after the dry position; pan %+.1f -> left %.4f right %.4f", i + 1, dly[i], (int)(at - 1000 - lat), pan[i], out.l[at], out.r[at]);
        }
        // and nothing anywhere else
        size_t nonzero = 0;
        for (size_t i = 0; i < out.l.size(); ++i) if (out.l[i] != 0.0f || out.r[i] != 0.0f) ++nonzero;
        CHECK_MSG(nonzero == 4, "%d samples are not silent", (int)nonzero);
    }
    {   // a sung note: the voice's swell arrives later by the delay, on the side it is panned to
        std::vector<float> v = steadyVoice(220.0, 0.8);
        for (size_t i = 0; i < v.size(); ++i) v[i] *= (float)std::exp(-0.5 * std::pow(((double)i - 19200.0) / 2400.0, 2.0));
        auto centroid = [](const std::vector<float>& x) { double s = 0.0, w = 0.0; for (size_t i = 0; i < x.size(); ++i) { s += (double)i * x[i] * x[i]; w += (double)x[i] * x[i]; } return s / w; };
        for (float ms : {0.0f, 25.0f, 60.0f}) {
            SecondVoice h;
            h.prepare(kSr);
            soloVoice(h, 5);
            h.pDelay[0]->set(ms);
            h.pPan[0]->set(0.6f);
            auto out = run(h, mono(v));
            const double shift = centroid(out.r) - centroid(v) - h.latencySamples();
            CHECK_MSG(std::fabs(shift - ms * 48.0) < 60.0, "delay %.0f ms: the swell is %.1f samples late", ms, shift);
            const double th = 1.6 * kPi / 4.0;
            CHECK_NEAR(rmsOf(out.l) / rmsOf(out.r), std::cos(th) / std::sin(th), 0.01);
            VT_FIG("sung note, delay %.0f ms: energy centre %.1f samples after the dry position (%.1f asked)", ms, shift, ms * 48.0);
        }
    }
    {   // moving the delay while it sounds crossfades to the new place: no clicks, and it gets there
        SecondVoice h;
        h.prepare(kSr);
        soloVoice(h, 0);
        h.pDelay[0]->set(10.0f);
        const auto t = tone(300.0, 1.0, 0.3f);
        auto out = run(h, t, kSr, nullptr, [&](size_t i) { if (i == 24000) h.pDelay[0]->set(55.0f); });
        float step = 0.0f;
        for (size_t i = 12000; i < out.l.size(); ++i) step = std::max(step, std::fabs(out.l[i] - out.l[i - 1]));
        CHECK_MSG(step < 1.2f * 0.3f * (float)(kTwoPi * 300.0 / kSr) * 1.2f, "step %.4f", step);
        const size_t lat = (size_t)h.latencySamples();
        CHECK(nullAgainstDelayed(out.l, t.l, lat + 2640, 40000) < 1e-6);            // settled at 55 ms
        std::vector<float> head(out.l.begin(), out.l.begin() + 24000);
        CHECK(nullAgainstDelayed(head, t.l, lat + 480, 6000) < 1e-6);                // was at 10 ms
    }
}

TEST(harmony_keeps_formants_when_asked) {
    const double f0 = hzOfKey(40);
    const std::vector<Formant> ah = {{600, 5, 1.0}, {1400, 6, 1.0}, {2800, 7, 0.8}};
    const auto in = afterSilence(voice(2.0, kSr, [f0](double) { return f0; }, ah, 0.4f, 8.0, 0.0));
    const size_t a = (size_t)(0.5 * kSr), b = (size_t)(2.0 * kSr);
    const auto inSpec = powerSpectrum(in.l, a, b);
    const double edges[3][2] = {{420, 820}, {1100, 1750}, {2300, 3400}};
    // the resonances the vowel was made with; the measurement itself finds them within 4 % on the input
    const double inPeak[3] = {600.0, 1400.0, 2800.0};
    for (int i = 0; i < 3; ++i) {
        const double pk = envelopePeakHz(inSpec, kSr, 2.0 * f0, edges[i][0], edges[i][1]);
        CHECK_MSG(std::fabs(pk / inPeak[i] - 1.0) < 0.05, "input peak %d measured at %.0f Hz", i + 1, pk);
        VT_FIG("the vowel as sung: envelope peak %d (made at %.0f Hz) measured at %.0f Hz", i + 1, inPeak[i], pk);
    }
    for (int interval : {-5, 4, 7}) {
        const double k = std::pow(2.0, interval / 12.0);
        for (int keep = 1; keep >= 0; --keep) {
            SecondVoice h;
            h.prepare(kSr);
            soloVoice(h, interval);
            h.pFormant->set((float)keep);
            auto out = run(h, in);
            const auto spec = powerSpectrum(out.l, a, b);
            for (int i = 0; i < 3; ++i) {
                const double m = keep ? 1.0 : k;
                const double pk = envelopePeakHz(spec, kSr, 2.0 * f0 * k, edges[i][0] * m, edges[i][1] * m);
                CHECK_MSG(std::fabs(pk / (inPeak[i] * m) - 1.0) < 0.05, "interval %+d, formants %s: peak %d %.0f Hz became %.0f Hz", interval, keep ? "kept" : "free", i + 1, inPeak[i], pk);
                VT_FIG("interval %+d st, formants %s: envelope peak %d %.0f -> %.0f Hz (%+.1f %%)", interval, keep ? "kept" : "free", i + 1, inPeak[i], pk, 100.0 * (pk / inPeak[i] - 1.0));
            }
        }
    }
}

TEST(harmony_doubler_uses_detune_and_delay_only) {
    const double f0 = hzOfKey(57);
    const auto in = afterSilence(steadyVoice(f0, 1.0));
    SecondVoice h;
    h.prepare(kSr);
    soloVoice(h, 7, 9.0f);
    h.pMode->set(1.0f);
    h.pDelay[0]->set(20.0f);
    auto out = run(h, in);
    // the interval is ignored: the voice is 9 cents from the lead, not a fifth away
    CHECK_NEAR(centsBetween(fundamentalHz(out.l, 28800, 52000, f0, kSr), f0), 9.0, 1.0);
    // every doubling preset: voices within 20 cents of the lead and 10 to 50 ms behind it
    int doubles = 0;
    for (auto& pr : h.factoryPresets()) {
        SecondVoice d;
        d.prepare(kSr);
        d.loadState(pr.state);
        if (d.pMode->getInt() != 1) continue;
        ++doubles;
        for (int i = 0; i < SecondVoice::kVoices; ++i) {
            if (!d.pOn[i]->getBool()) continue;
            CHECK_MSG(std::fabs(d.pDetune[i]->get()) <= 20.0f && d.pDelay[i]->get() >= 10.0f && d.pDelay[i]->get() <= 50.0f, "%s voice %d", pr.name.c_str(), i + 1);
        }
        d.pDry->set(-60.0f);
        auto o = run(d, in);
        std::vector<float> sum(o.l.size());
        for (size_t i = 0; i < sum.size(); ++i) sum[i] = o.l[i] + o.r[i];
        const double cents = centsBetween(correlationHz(sum, 40000, f0, kSr), f0);
        CHECK_MSG(std::fabs(cents) < 20.0, "%s: %.1f cents from the lead", pr.name.c_str(), cents);
        CHECK(rmsOf(o.l, 28800) > 0.01);
    }
    CHECK(doubles >= 3);
    // "Wide double": the two sides carry different voices
    SecondVoice w;
    w.prepare(kSr);
    for (auto& pr : w.factoryPresets()) if (pr.name == "Wide double") w.loadState(pr.state);
    CHECK(w.pMode->getInt() == 1);
    w.pDry->set(-60.0f);
    auto o = run(w, in);
    CHECK(maxAbsDiff(o.l, o.r) > 0.05);
    CHECK_NEAR(db(rmsOf(o.l, 28800) / rmsOf(o.r, 28800)), 0.0, 1.0);
}

TEST(harmony_dry_path_unvoiced_sound_and_latency) {
    const struct { double sr; int want; } rates[] = {{44100.0, 794}, {48000.0, 864}, {96000.0, 1728}};
    for (auto& r : rates) {
        SecondVoice h;
        h.prepare(r.sr);
        CHECK_MSG(h.latencySamples() == r.want, "%.0f Hz: latency %d", r.sr, h.latencySamples());
    }
    Stereo in = afterSilence(steadyVoice(196.0, 0.6));
    append(in, noise(0.3, 0.2f));
    for (size_t i = 0; i < in.r.size(); ++i) in.r[i] *= 0.7f;
    {   // no voices: the dry signal, delayed by the latency, bit for bit
        SecondVoice h;
        h.prepare(kSr);
        for (int i = 0; i < 4; ++i) h.pOn[i]->set(0.0f);
        auto out = run(h, in);
        const size_t lat = (size_t)h.latencySamples();
        CHECK(nullAgainstDelayed(out.l, in.l, lat) == 0.0 && nullAgainstDelayed(out.r, in.r, lat) == 0.0);
        CHECK(peakOf(out.l, 0, lat) == 0.0f);
        // dry level in dB
        h.pDry->set(-6.0f);
        h.reset();
        auto quiet = run(h, in);
        CHECK_NEAR(db(rmsOf(quiet.l, 9600) / rmsOf(out.l, 9600)), -6.0, 0.01);
        h.pDry->set(-60.0f);
        h.reset();
        CHECK(peak(run(h, in)) == 0.0f);
    }
    {   // noise is not shifted: a voice set a fifth up passes it on as it is (centre pan, 0 dB, no delay: equal to the mono sum)
        SecondVoice h;
        h.prepare(kSr);
        soloVoice(h, 7);
        const Stereo nz = noise(0.5, 0.2f, kSr, 5u, true);
        auto out = run(h, nz);
        CHECK(nullAgainstDelayed(out.l, nz.l, (size_t)h.latencySamples()) < 1e-7);
        CHECK(h.meter(1) == 0.0f);
    }
    {   // a voice at interval 0, no detune, no delay is the lead itself: with the dry at 0 dB the sum is 6 dB up
        SecondVoice h;
        h.prepare(kSr);
        soloVoice(h, 0);
        h.pDry->set(0.0f);
        const auto v = afterSilence(steadyVoice(196.0, 0.6));
        auto out = run(h, v);
        double worst = 0.0;
        const size_t lat = (size_t)h.latencySamples();
        for (size_t i = lat; i < out.l.size(); ++i) worst = std::max(worst, (double)std::fabs(out.l[i] - 2.0f * v.l[i - lat]));
        CHECK_MSG(worst < 1e-6, "difference %g", worst);
    }
    // any setting: the dry click comes out exactly `latency` samples later, and the latency never changes
    dsp::Noise rnd(99u);
    for (int trial = 0; trial < 16; ++trial) {
        SecondVoice h;
        h.prepare(kSr);
        for (auto& p : h.params()) p->setNorm(rnd.uni());
        h.pDry->set(0.0f);
        for (int i = 0; i < 4; ++i) h.pDelay[i]->set(std::max(1.0f, h.pDelay[i]->get()));     // keep the voices' copies off the dry click
        Stereo click = silence(0.2);
        click.l[500] = 1.0f; click.r[500] = 1.0f;
        auto out = run(h, click);
        CHECK(h.latencySamples() == 864);
        CHECK_MSG(std::fabs(out.l[500 + 864] - 1.0f) < 1e-6 && peakOf(out.l, 0, 500 + 864) == 0.0f, "trial %d: %.4f at the latency", trial, out.l[500 + 864]);
    }
}

TEST(harmony_switching_a_voice_on_and_off_is_clean) {
    SecondVoice h;
    h.prepare(kSr);
    soloVoice(h, 0);
    h.pOn[0]->set(0.0f);
    const auto t = mono(steadyVoice(220.0, 2.0, kSr, 0.4f, false));
    auto out = run(h, t, kSr, nullptr, [&](size_t i) {
        if (i == 24000) h.pOn[0]->set(1.0f);
        if (i == 48000) h.pOn[0]->set(0.0f);
        if (i == 72000) h.pOn[0]->set(1.0f);
    });
    const size_t lat = (size_t)h.latencySamples();
    CHECK(peakOf(out.l, 0, 24000) == 0.0f);
    CHECK(peakOf(out.l, 48200, 72000) == 0.0f);                 // off means off, also after the 80 ms delay line
    CHECK(nullAgainstDelayed(std::vector<float>(out.l.begin(), out.l.begin() + 47000), t.l, lat, 26000) < 1e-6);
    CHECK(nullAgainstDelayed(out.l, t.l, lat, 74000) < 1e-6);
    CHECK(h.meter(2) == 1.0f);
}

TEST(harmony_on_the_recorded_factory_voice) {
    // real sung notes (when the factory library is there): the voice a third above follows the singer's own
    // wavering pitch at the interval
    const struct { const char* note; double approxKey; } notes[] = {{"C4", 48}, {"G#3", 44}, {"G#4", 56}};
    for (auto& nt : notes) {
        Stereo in;
        double sr = 0.0;
        if (!factoryVoice(nt.note, in, sr)) continue;
        for (int keep = 1; keep >= 0; --keep) {
            SecondVoice h;
            h.prepare(sr);
            soloVoice(h, 4);
            h.pFormant->set((float)keep);
            auto out = run(h, in, sr);
            CHECK(allFinite(out));
            const size_t lat = (size_t)h.latencySamples(), a = (size_t)(0.5 * sr), b = (size_t)(1.5 * sr);
            const double f = hzOfKey(nt.approxKey), up = std::pow(2.0, 4.0 / 12.0);
            std::vector<float> lead(in.l.size());
            for (size_t i = 0; i < lead.size(); ++i) lead[i] = 0.5f * (in.l[i] + in.r[i]);
            std::vector<double> err;
            for (size_t c = a; c < b; c += (size_t)(0.01 * sr)) {
                const double hzIn = correlationHz(lead, c, f, sr), hzOut = correlationHz(out.l, c + lat, f * up, sr);
                if (hzIn > 0.0 && hzOut > 0.0) err.push_back(centsBetween(hzOut, hzIn * up));
            }
            std::sort(err.begin(), err.end());
            const double median = err[err.size() / 2];
            int within = 0;
            for (double e : err) if (std::fabs(e) < 10.0) ++within;
            const double level = db(rmsOf(out.l, a + lat, b + lat) / rmsOf(lead, a, b));
            printf("      %s, a third above, formants %s: median %+.2f cents from the interval, %d %% of the time within 10 cents, level %+.2f dB\n",
                   nt.note, keep ? "kept" : "free", median, (int)(100 * within / (int)err.size()), level);
            CHECK_MSG(std::fabs(median) < 5.0, "%s: the harmony is %.1f cents off", nt.note, median);
            CHECK_MSG(within * 10 >= (int)err.size() * 9, "%s: within 10 cents only %d of %d times", nt.note, within, (int)err.size());
            CHECK_MSG(std::fabs(level) < 3.0, "%s: level %.2f dB", nt.note, level);
        }
    }
}
