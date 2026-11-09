// True Note: pitch correction against synthetic sung vowels of known pitch (and the factory voice recordings when present).
#include "VocalTest.h"
#include <filesystem>
using namespace pulp;
using namespace vt;

namespace {
const double kSr = 48000.0;
double hzOfKey(double key) { return 440.0 * std::pow(2.0, (key - 69.0) / 12.0); }

// 0.15 s of silence, then the voice
Stereo afterSilence(const std::vector<float>& v, double sr = kSr) {
    Stereo s = silence(0.15, sr);
    append(s, mono(v));
    return s;
}
}

TEST(vocal_pitch_detector_finds_the_period) {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        for (int range = 0; range < 2; ++range) {
            for (double hz : {65.0, 98.0, 147.0, 220.0, 331.0, 523.0, 880.0, 1100.0}) {
                if (range == 0 && hz > 590.0) continue;
                if (range == 1 && hz < 115.0) continue;
                vocal::PitchDetector d;
                d.prepare(sr, 0);
                d.setRange(range);
                const auto v = steadyVoice(hz, 0.4, sr);
                for (float x : v) d.push(x);
                const auto* e = d.latest();
                CHECK(e != nullptr);
                if (!e) continue;
                const double cents = centsBetween(sr / e->period, hz);
                CHECK_MSG(e->voiced && std::fabs(cents) < 2.0, "%.0f Hz at %.0f, range %d: voiced %d, %.2f cents off, clarity %.3f", hz, sr, range, (int)e->voiced, cents, e->clarity);
                if (sr == 48000.0) VT_FIG("detector %.0f Hz range %d: %.3f cents off, clarity %.3f", hz, range, cents, e->clarity);
            }
        }
    }
    // noise and silence are not voiced, and a plain sine is
    vocal::PitchDetector d;
    d.prepare(kSr, 0);
    int voicedFrames = 0;
    float worst = 0.0f;
    auto nz = noise(2.0, 0.3f);
    for (size_t i = 0; i < nz.l.size(); ++i) {
        const int64 before = d.estimates();
        d.push(nz.l[i]);
        if (d.estimates() != before) { voicedFrames += d.latest()->voiced ? 1 : 0; worst = std::max(worst, d.latest()->clarity); }
    }
    CHECK_MSG(voicedFrames == 0, "%d frames of white noise were called voiced", voicedFrames);
    VT_FIG("white noise: highest clarity %.3f over %d analyses", worst, (int)d.estimates());
    d.reset();
    for (int i = 0; i < 48000; ++i) d.push(0.0f);
    CHECK(d.latest() && !d.latest()->voiced);
    d.reset();
    for (int i = 0; i < 24000; ++i) d.push(0.2f * (float)std::sin(kTwoPi * 196.0 * i / kSr));
    CHECK(d.latest()->voiced);
    CHECK_NEAR(centsBetween(kSr / d.latest()->period, 196.0), 0.0, 1.0);
}

TEST(tune_flat_voice_lands_on_the_note_at_speed_zero) {
    struct Case { double sr, key; int range; } cases[] = {
        {48000.0, 45, 0}, {48000.0, 57, 0}, {48000.0, 64, 0}, {48000.0, 69, 1}, {48000.0, 76, 1}, {48000.0, 81, 1},
        {44100.0, 57, 0}, {44100.0, 72, 1}, {96000.0, 52, 0}, {96000.0, 69, 1}};
    for (auto& cs : cases) {
        TrueNote t;
        t.prepare(cs.sr);
        t.pSpeed->set(0.0f);
        t.pRange->set((float)cs.range);
        const double want = hzOfKey(cs.key), sung = want * std::pow(2.0, -30.0 / 1200.0);       // 30 cents flat
        auto out = run(t, afterSilence(steadyVoice(sung, 1.0, cs.sr), cs.sr), cs.sr);
        const size_t a = (size_t)(0.5 * cs.sr), b = (size_t)(1.0 * cs.sr);
        const double got = fundamentalHz(out.l, a, b, want, cs.sr);
        const double cents = centsBetween(got, want);
        CHECK_MSG(std::fabs(cents) < 5.0, "key %.0f at %.0f Hz: %.2f cents from the note", cs.key, cs.sr, cents);
        VT_FIG("speed 0, key %.0f (%.1f Hz) at %.0f: output %.2f cents from the note (sung 30 flat)", cs.key, want, cs.sr, cents);
        CHECK(t.meter(2) == 1.0f);
        CHECK_NEAR(t.meter(0), cs.key - 0.30, 0.03);
        CHECK_NEAR(t.meter(1), 30.0, 2.0);
        // the level stays what it was
        const double inRms = 0.4 * 0.0 + rmsOf(afterSilence(steadyVoice(sung, 1.0, cs.sr), cs.sr).l, a, b);
        CHECK_MSG(std::fabs(db(rmsOf(out.l, a, b) / inRms)) < 1.0, "level changed by %.2f dB", db(rmsOf(out.l, a, b) / inRms));
    }
}

TEST(tune_how_soon_after_a_note_starts_it_is_corrected) {
    // The detector needs a couple of periods before it calls a sound a voice; until then the start of the note
    // goes through as it was sung. Measured: the first moment the output (in 10 ms windows) is within 5 cents.
    for (double key : {45.0, 57.0, 69.0}) {
        const double want = hzOfKey(key), sung = want * std::pow(2.0, -30.0 / 1200.0);
        TrueNote t;
        t.prepare(kSr);
        t.pSpeed->set(0.0f);
        t.pRange->set(key > 60 ? 1.0f : 0.0f);
        const auto in = afterSilence(steadyVoice(sung, 0.6, kSr, 0.4f, true));
        auto out = run(t, in);
        const size_t onset = 7168 + (size_t)t.latencySamples();
        double firstMs = -1.0;
        for (size_t c = onset + 240; c + 4800 < out.l.size(); c += 96) {
            const double hz = correlationHz(out.l, c, want, kSr);
            if (hz > 0.0 && std::fabs(centsBetween(hz, want)) < 5.0) { firstMs = (double)(c - onset) / 48.0; break; }
        }
        VT_FIG("a note at key %.0f (%.0f Hz), 30 cents flat, speed 0: on the note from %.0f ms after it starts", key, want, firstMs);
        CHECK_MSG(firstMs >= 0.0 && firstMs < 60.0, "key %.0f: corrected only after %.0f ms", key, firstMs);
    }
}

TEST(tune_slower_speeds_arrive_in_the_stated_time) {
    const double want = hzOfKey(57), sung = want * std::pow(2.0, -30.0 / 1200.0);
    const auto in = afterSilence(steadyVoice(sung, 1.2));
    const size_t onset = (size_t)(0.15 * kSr);
    for (float speed : {50.0f, 100.0f, 200.0f, 400.0f}) {
        TrueNote t;
        t.prepare(kSr);
        t.pSpeed->set(speed);
        auto out = run(t, in);
        const size_t lat = (size_t)t.latencySamples();
        auto centsAt = [&](double ms) {
            const size_t c = onset + lat + (size_t)(0.001 * ms * kSr), half = (size_t)(0.015 * kSr);
            return centsBetween(fundamentalHz(out.l, c - half, c + half, want, kSr), want);
        };
        // nine tenths of the way in the stated time: 30 cents flat becomes 30 * 10^(-t / time)
        const double earlyMs = std::max(20.0, 0.12 * speed), expected = -30.0 * std::pow(10.0, -earlyMs / speed);
        const double early = centsAt(earlyMs), atTime = centsAt(speed + 30.0), late = centsAt(2.2 * speed + 60.0);
        CHECK_MSG(std::fabs(early - expected) < 7.0, "speed %.0f ms: %.1f cents after %.0f ms, expected about %.1f", speed, early, earlyMs, expected);
        CHECK_MSG(std::fabs(atTime) < 5.0, "speed %.0f ms: %.1f cents off when the time is up", speed, atTime);
        CHECK_MSG(std::fabs(late) < 2.0, "speed %.0f ms: %.1f cents off later", speed, late);
        VT_FIG("speed %.0f ms: %.1f cents early, %.1f at the stated time, %.2f later", speed, early, atTime, late);
    }
}

TEST(tune_notes_outside_the_scale_go_to_the_nearest_scale_note) {
    struct Case { int scale, key; double sungKey, wantKey; const char* what; } cases[] = {
        {vocal::Major, 0, 61.2, 62, "C major: C#+20 goes up to D"},
        {vocal::Major, 0, 65.7, 65, "C major: F#-30 goes down to F"},
        {vocal::Major, 0, 58.1, 59, "C major: A#+10 goes up to B"},
        {vocal::PentaMinor, 9, 59.2, 60, "A minor pentatonic: B+20 goes to C"},
        {vocal::Blues, 4, 60.4, 59, "E blues: C+40 goes to B"},
        {vocal::Dorian, 2, 61.3, 62, "D dorian: C#+30 goes to D"},
        {vocal::Mixolydian, 7, 66.3, 67, "G mixolydian: F#+30 goes to G"},
        {vocal::HarmonicMinor, 9, 66.8, 68, "A harmonic minor: G-20 goes to G#"},
        {vocal::Minor, 9, 66.8, 67, "A minor: G-20 stays G"},
        {vocal::PentaMajor, 0, 63.4, 64, "C major pentatonic: D#+40 goes to E"},
        {vocal::Chromatic, 0, 63.4, 63, "chromatic: D#+40 stays D#"},
    };
    for (auto& cs : cases) {
        TrueNote t;
        t.prepare(kSr);
        t.pSpeed->set(0.0f);
        t.pScale->set((float)cs.scale);
        t.pKey->set((float)cs.key);
        auto out = run(t, afterSilence(steadyVoice(hzOfKey(cs.sungKey), 0.8)));
        const double got = fundamentalHz(out.l, 24000, 43000, hzOfKey(cs.wantKey), kSr);
        const double cents = centsBetween(got, hzOfKey(cs.wantKey));
        CHECK_MSG(std::fabs(cents) < 5.0, "%s: %.1f cents from it", cs.what, cents);
        VT_FIG("%s: %.2f cents from it", cs.what, cents);
    }
    // custom scale with only C and G: an E has to travel three semitones up to G
    TrueNote t;
    t.prepare(kSr);
    t.pSpeed->set(0.0f);
    t.pScale->set((float)TrueNote::kCustomScale);
    for (int i = 0; i < 12; ++i) t.pNote[i]->set(i == 0 || i == 7 ? 1.0f : 0.0f);
    auto out = run(t, afterSilence(steadyVoice(hzOfKey(52.0), 0.8)));
    const double cents = centsBetween(fundamentalHz(out.l, 24000, 43000, hzOfKey(55), kSr), hzOfKey(55));
    CHECK_MSG(std::fabs(cents) < 5.0, "custom scale C and G: E3 is %.1f cents from G3", cents);
    // no notes at all: nothing to aim at, so the pitch is left alone
    for (int i = 0; i < 12; ++i) t.pNote[i]->set(0.0f);
    t.reset();
    const auto in = afterSilence(steadyVoice(hzOfKey(52.3), 0.8));
    out = run(t, in);
    CHECK(nullAgainstDelayed(out.l, in.l, (size_t)t.latencySamples()) == 0.0);
}

TEST(tune_vibrato_is_flattened_at_speed_zero_and_kept_when_slow) {
    for (double centre : {57.0, 66.0}) {
        const double base = hzOfKey(centre - 0.10);
        auto f0 = [base](double t) { return base * std::pow(2.0, 0.40 * std::sin(kTwoPi * 5.5 * t) / 12.0); };       // +-40 cents, 5.5 Hz
        const auto in = afterSilence(voice(1.6, kSr, f0, vowelAh()));
        auto swing = [&](const std::vector<float>& x, size_t delay, double& mean) {
            const auto track = pitchTrack(x, (size_t)(0.6 * kSr) + delay, (size_t)(1.6 * kSr) + delay, hzOfKey(centre), kSr, 25.0, 5.0);
            double lo = 1e9, hi = -1e9, sum = 0.0;
            for (double hz : track) { const double c = centsBetween(hz, hzOfKey(centre)); lo = std::min(lo, c); hi = std::max(hi, c); sum += c; }
            mean = sum / (double)track.size();
            return 0.5 * (hi - lo);
        };
        double inMean = 0.0, m0 = 0.0, m1 = 0.0;
        const double inSwing = swing(in.l, 0, inMean);
        TrueNote hard;
        hard.prepare(kSr);
        hard.pSpeed->set(0.0f);
        hard.pRange->set(centre > 60 ? 1.0f : 0.0f);
        const double hardSwing = swing(run(hard, in).l, (size_t)hard.latencySamples(), m0);
        TrueNote slow;
        slow.prepare(kSr);
        slow.pSpeed->set(400.0f);
        slow.pRange->set(centre > 60 ? 1.0f : 0.0f);
        const double slowSwing = swing(run(slow, in).l, (size_t)slow.latencySamples(), m1);
        VT_FIG("vibrato at key %.0f: sung +-%.1f cents (mean %.1f); speed 0 leaves +-%.1f (mean %.1f); 400 ms keeps +-%.1f (mean %.1f)", centre, inSwing, inMean, hardSwing, m0, slowSwing, m1);
        CHECK_MSG(inSwing > 34.0 && inSwing < 44.0, "the test signal swings +-%.1f cents", inSwing);
        CHECK_MSG(hardSwing < 0.2 * inSwing, "speed 0 leaves +-%.1f cents of +-%.1f", hardSwing, inSwing);
        CHECK_MSG(std::fabs(m0) < 4.0, "speed 0 sits %.1f cents from the note", m0);
        CHECK_MSG(slowSwing > 0.75 * inSwing, "400 ms keeps only +-%.1f cents of +-%.1f", slowSwing, inSwing);
        CHECK_MSG(std::fabs(m1) < 5.0, "400 ms sits %.1f cents from the note on average", m1);
    }
}

TEST(tune_keeps_formants_when_asked_and_moves_them_when_not) {
    const double f0 = hzOfKey(40), up = hzOfKey(43);            // a low E (82 Hz) moved three semitones up to G
    const std::vector<Formant> ah = {{600, 5, 1.0}, {1400, 6, 1.0}, {2800, 7, 0.8}};
    const auto in = afterSilence(voice(2.0, kSr, [f0](double) { return f0; }, ah, 0.4f, 8.0, 0.0));      // a flat pulse train through three resonances
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
    {
        TrueNote t;
        t.prepare(kSr);
        t.pSpeed->set(0.0f); t.pTranspose->set(3.0f); t.pFormant->set(1.0f);
        auto out = run(t, in);
        CHECK_NEAR(centsBetween(fundamentalHz(out.l, a, b, up, kSr), up), 0.0, 5.0);
        const auto spec = powerSpectrum(out.l, a, b);
        for (int i = 0; i < 3; ++i) {
            const double pk = envelopePeakHz(spec, kSr, 2.0 * up, edges[i][0], edges[i][1]);
            CHECK_MSG(std::fabs(pk / inPeak[i] - 1.0) < 0.04, "formant %d kept: %.0f Hz became %.0f Hz", i + 1, inPeak[i], pk);
            VT_FIG("keep formants, +3 st: peak %d %.0f -> %.0f Hz (%.1f %%)", i + 1, inPeak[i], pk, 100.0 * (pk / inPeak[i] - 1.0));
        }
    }
    {
        TrueNote t;
        t.prepare(kSr);
        t.pSpeed->set(0.0f); t.pTranspose->set(3.0f); t.pFormant->set(0.0f);
        auto out = run(t, in);
        CHECK_NEAR(centsBetween(fundamentalHz(out.l, a, b, up, kSr), up), 0.0, 5.0);
        const auto spec = powerSpectrum(out.l, a, b);
        const double ratio = std::pow(2.0, 3.0 / 12.0);
        for (int i = 0; i < 3; ++i) {
            const double pk = envelopePeakHz(spec, kSr, 2.0 * up, edges[i][0] * ratio, edges[i][1] * ratio);
            CHECK_MSG(std::fabs(pk / (inPeak[i] * ratio) - 1.0) < 0.05, "formant %d not kept: %.0f Hz became %.0f Hz, expected %.0f", i + 1, inPeak[i], pk, inPeak[i] * ratio);
            VT_FIG("formants free, +3 st: peak %d %.0f -> %.0f Hz (%.1f %%, a plain resample gives 18.9 %%)", i + 1, inPeak[i], pk, 100.0 * (pk / inPeak[i] - 1.0));
        }
    }
    {   // formant shift alone: the pitch stays, the envelope moves by four semitones
        TrueNote t;
        t.prepare(kSr);
        t.pSpeed->set(0.0f); t.pFShift->set(4.0f);
        auto out = run(t, in);
        CHECK_NEAR(centsBetween(fundamentalHz(out.l, a, b, f0, kSr), f0), 0.0, 5.0);
        const auto spec = powerSpectrum(out.l, a, b);
        const double ratio = std::pow(2.0, 4.0 / 12.0);
        for (int i = 0; i < 3; ++i) {
            const double pk = envelopePeakHz(spec, kSr, 2.0 * f0, edges[i][0] * ratio, edges[i][1] * ratio);
            CHECK_MSG(std::fabs(pk / (inPeak[i] * ratio) - 1.0) < 0.06, "formant shift: %.0f Hz became %.0f Hz, expected %.0f", inPeak[i], pk, inPeak[i] * ratio);
            VT_FIG("formant shift +4 st: peak %d %.0f -> %.0f Hz (%.1f %%, asked 26.0 %%)", i + 1, inPeak[i], pk, 100.0 * (pk / inPeak[i] - 1.0));
        }
    }
}

TEST(tune_formant_keeping_does_not_depend_on_where_in_the_cycle_a_note_starts) {
    // The grains are cut around the voice's pulses. If they were not, a note that happened to start half a
    // period later would come out hollow and quieter. Eight starting points across one period, each moved up a fourth.
    const double f0 = hzOfKey(45), up = hzOfKey(50);
    const std::vector<Formant> ah = {{600, 5, 1.0}, {1400, 6, 1.0}, {2800, 7, 0.8}};
    const auto v = voice(1.0, kSr, [f0](double) { return f0; }, ah, 0.4f, 8.0, 0.0);
    const double period = kSr / f0;
    double loDb = 1e9, hiDb = -1e9, worstPeak = 0.0;
    for (int k = 0; k < 8; ++k) {
        Stereo in = silence(0.15);
        std::vector<float> pad((size_t)(k * period / 8.0), 0.0f);
        in.l.insert(in.l.end(), pad.begin(), pad.end());
        in.l.insert(in.l.end(), v.begin(), v.end());
        in.l.resize(in.l.size() / kQuantum * kQuantum);
        in.r = in.l;
        TrueNote t;
        t.prepare(kSr);
        t.pSpeed->set(0.0f); t.pTranspose->set(5.0f);
        auto out = run(t, in);
        const size_t a = (size_t)(0.45 * kSr), b = (size_t)(1.05 * kSr);
        CHECK_NEAR(centsBetween(fundamentalHz(out.l, a, b, up, kSr), up), 0.0, 5.0);
        const double level = db(rmsOf(out.l, a, b) / rmsOf(in.l, a, b));
        loDb = std::min(loDb, level); hiDb = std::max(hiDb, level);
        const auto spec = powerSpectrum(out.l, a, b);
        const double edges[3][2] = {{420, 820}, {1100, 1750}, {2300, 3400}};
        for (int i = 0; i < 3; ++i) {
            const double made[3] = {600.0, 1400.0, 2800.0};
            worstPeak = std::max(worstPeak, std::fabs(envelopePeakHz(spec, kSr, 2.0 * up, edges[i][0], edges[i][1]) / made[i] - 1.0));
        }
    }
    VT_FIG("eight starting points, +5 st with formants kept: level %+.2f to %+.2f dB against the input, envelope peaks within %.1f %%", loDb, hiDb, 100.0 * worstPeak);
    CHECK_MSG(hiDb - loDb < 1.0 && std::fabs(loDb) < 2.0 && std::fabs(hiDb) < 2.0, "level between %.2f and %.2f dB", loDb, hiDb);
    CHECK_MSG(worstPeak < 0.06, "an envelope peak is %.1f %% from where it was made", 100.0 * worstPeak);
}

TEST(tune_unvoiced_sound_and_silence_pass_untouched) {
    // white noise, silence, a click, a noise burst: nothing with a pitch
    Stereo in = noise(1.0, 0.3f);
    append(in, silence(0.3));
    Stereo click = silence(0.2);
    click.l[1000] = 0.9f; click.r[1003] = -0.7f;
    append(in, click);
    append(in, noise(0.25, 0.05f, kSr, 77u));
    append(in, silence(0.3));
    for (int variant = 0; variant < 3; ++variant) {
        TrueNote t;
        t.prepare(kSr);
        if (variant == 1) { t.pSpeed->set(0.0f); t.pTranspose->set(5.0f); t.pFormant->set(0.0f); t.pFShift->set(-7.0f); t.pScale->set((float)vocal::Major); }
        if (variant == 2) { t.pRange->set(1.0f); t.pSpeed->set(300.0f); t.pAmount->set(0.4f); }
        auto out = run(t, in);
        const size_t lat = (size_t)t.latencySamples();
        CHECK_MSG(nullAgainstDelayed(out.l, in.l, lat) == 0.0 && nullAgainstDelayed(out.r, in.r, lat) == 0.0, "variant %d: difference %g", variant, nullAgainstDelayed(out.l, in.l, lat));
        CHECK(peakOf(out.l, 0, lat) == 0.0f);
        CHECK(t.meter(2) == 0.0f && t.meter(0) == 0.0f && t.meter(1) == 0.0f);
    }
}

TEST(tune_amount_zero_and_mix_zero_null_against_the_delayed_input) {
    auto f0 = [](double t) { return 205.0 * std::pow(2.0, 0.3 * std::sin(kTwoPi * 5.0 * t) / 12.0); };
    Stereo in = afterSilence(voice(1.0, kSr, f0, vowelAh()));
    for (size_t i = 0; i < in.r.size(); ++i) in.r[i] *= 0.6f;      // not the same on both sides
    {
        TrueNote t;
        t.prepare(kSr);
        t.pSpeed->set(0.0f); t.pAmount->set(0.0f);
        auto out = run(t, in);
        const size_t lat = (size_t)t.latencySamples();
        CHECK(t.meter(2) == 1.0f);          // it does hear a voice, it just has nothing to do
        CHECK_MSG(nullAgainstDelayed(out.l, in.l, lat) == 0.0 && nullAgainstDelayed(out.r, in.r, lat) == 0.0, "amount 0: difference %g", nullAgainstDelayed(out.l, in.l, lat));
    }
    {
        TrueNote t;
        t.prepare(kSr);
        t.pSpeed->set(0.0f); t.pMix->set(0.0f); t.pTranspose->set(7.0f);
        auto out = run(t, in);
        const size_t lat = (size_t)t.latencySamples();
        CHECK_MSG(nullAgainstDelayed(out.l, in.l, lat) == 0.0 && nullAgainstDelayed(out.r, in.r, lat) == 0.0, "mix 0: difference %g", nullAgainstDelayed(out.l, in.l, lat));
    }
    {   // and with everything on, the same voice is changed
        TrueNote t;
        t.prepare(kSr);
        t.pSpeed->set(0.0f);
        auto out = run(t, in);
        CHECK(nullAgainstDelayed(out.l, in.l, (size_t)t.latencySamples(), 24000) > 0.05);
    }
}

TEST(tune_latency_is_reported_and_never_changes) {
    const struct { double sr; int want; } rates[] = {{44100.0, 794}, {48000.0, 864}, {96000.0, 1728}};
    for (auto& r : rates) {
        TrueNote t;
        t.prepare(r.sr);
        CHECK_MSG(t.latencySamples() == r.want, "%.0f Hz: latency %d", r.sr, t.latencySamples());
    }
    // any setting: a click comes out exactly `latency` samples later
    dsp::Noise rnd(4242u);
    for (int trial = 0; trial < 24; ++trial) {
        TrueNote t;
        t.prepare(kSr);
        if (trial > 0) for (auto& p : t.params()) p->setNorm(rnd.uni());
        if (trial > 0) t.pMix->set(1.0f);
        const int lat = t.latencySamples();
        CHECK(lat == 864);
        Stereo in = silence(0.1);
        in.l[777] = 1.0f; in.r[777] = -0.5f;
        auto out = run(t, in);
        CHECK(t.latencySamples() == lat);
        size_t at = 0;
        for (size_t i = 0; i < out.l.size(); ++i) if (std::fabs(out.l[i]) > std::fabs(out.l[at])) at = i;
        CHECK_MSG(at == 777 + (size_t)lat && out.l[at] == 1.0f && out.r[at] == -0.5f, "trial %d: click at %d (latency %d)", trial, (int)at - 777, lat);
    }
    // a voiced signal too: with the correction at work the waveform still lines up with the delayed input
    // (the envelope of the output peaks where the delayed input's does)
    TrueNote t;
    t.prepare(kSr);
    t.pSpeed->set(0.0f);
    std::vector<float> v = steadyVoice(217.0, 0.6);
    for (size_t i = 0; i < v.size(); ++i) v[i] *= (float)std::exp(-0.5 * std::pow(((double)i - 14400.0) / 2400.0, 2.0));       // a 50 ms swell centred at 0.3 s
    auto out = run(t, mono(v));
    auto centroid = [](const std::vector<float>& x) { double s = 0.0, w = 0.0; for (size_t i = 0; i < x.size(); ++i) { s += (double)i * x[i] * x[i]; w += (double)x[i] * x[i]; } return s / w; };
    const double shift = centroid(out.l) - centroid(v);
    CHECK_MSG(std::fabs(shift - 864.0) < 48.0, "the swell moved by %.1f samples (latency 864)", shift);
    VT_FIG("voiced swell: energy centre moved by %.1f samples; reported latency 864", shift);
}

TEST(tune_follows_a_melody_and_a_scale_change) {
    // three sung notes in a row, each 25 cents sharp; the output steps from note to note
    const double keys[3] = {57.25, 60.25, 55.25};
    auto f0 = [&](double t) { return hzOfKey(keys[std::min(2, (int)(t / 0.5))]); };
    const auto in = afterSilence(voice(1.5, kSr, f0, vowelAh()));
    TrueNote t;
    t.prepare(kSr);
    t.pSpeed->set(0.0f);
    auto out = run(t, in);
    const size_t lat = (size_t)t.latencySamples(), on = (size_t)(0.15 * kSr) + lat;
    for (int i = 0; i < 3; ++i) {
        const double want = hzOfKey(std::floor(keys[i]));
        const size_t a = on + (size_t)((0.5 * i + 0.15) * kSr), b = on + (size_t)((0.5 * i + 0.45) * kSr);
        const double cents = centsBetween(fundamentalHz(out.l, a, b, want, kSr), want);
        CHECK_MSG(std::fabs(cents) < 5.0, "note %d: %.1f cents off", i, cents);
        VT_FIG("melody note %d: %.2f cents from the note", i, cents);
    }
    CHECK(allFinite(out));
}

TEST(tune_corrects_the_recorded_factory_voice) {
    // The factory library holds real sung notes. Used when the library has been built; skipped cleanly when not.
    const std::string dir = std::string(PULP_SOURCE_DIR) + "/factory/Instruments/Voices/Solo Voice Aah";
    if (!std::filesystem::exists(dir)) { printf("      (no factory voice recordings here: skipped)\n"); return; }
    int files = 0;
    for (auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".flac") continue;
        const std::string name = entry.path().filename().string();
        auto smp = readAudioFile(entry.path().string());
        CHECK(smp != nullptr);
        if (!smp || smp->frames() < (int64)(2.0 * smp->sampleRate)) continue;
        ++files;
        const double sr = smp->sampleRate;
        Stereo in = silence(0.1, sr);
        Stereo body;
        body.l = smp->ch[0];
        body.r = smp->channels > 1 ? smp->ch[1] : smp->ch[0];
        const size_t keep = std::min(body.l.size(), (size_t)(2.5 * sr)) / kQuantum * kQuantum;
        body.l.resize(keep); body.r.resize(keep);
        append(in, body);
        const size_t a = (size_t)(0.4 * sr), b = (size_t)(1.4 * sr);       // the steady second of the note
        // what was sung: the detector's median over the body of the note
        vocal::PitchDetector d;
        d.prepare(sr, 0);
        std::vector<double> keysHeard;
        int analyses = 0, voiced = 0;
        for (size_t i = 0; i < in.l.size(); ++i) {
            const int64 before = d.estimates();
            d.push(0.5f * (in.l[i] + in.r[i]));
            if (d.estimates() != before && i > a && i < b) {
                ++analyses;
                if (d.latest()->voiced) { ++voiced; keysHeard.push_back(69.0 + 12.0 * std::log2(sr / (d.latest()->period * 440.0))); }
            }
        }
        CHECK_MSG(voiced >= analyses * 9 / 10, "%s: voiced in %d of %d analyses", name.c_str(), voiced, analyses);
        if (keysHeard.empty()) continue;
        std::sort(keysHeard.begin(), keysHeard.end());
        const double sungKey = keysHeard[keysHeard.size() / 2];
        const double wantKey = std::round(sungKey), want = hzOfKey(wantKey);
        // Pitch every 10 ms by an independent measurement. Real voices crack now and then, so the figures are
        // the median, the median distance from it, and the share of the time spent within 10 cents of the note.
        struct Stats { double median, spread, within10; };
        auto stats = [&](const std::vector<float>& x, size_t delay, double ref) {
            std::vector<double> c;
            for (double hz : correlationTrack(x, a + delay, b + delay, ref, sr, 10.0)) c.push_back(centsBetween(hz, ref));
            Stats st{0.0, 0.0, 0.0};
            if (c.empty()) return st;
            for (double v : c) if (std::fabs(v) <= 10.0) st.within10 += 1.0 / (double)c.size();
            std::sort(c.begin(), c.end());
            st.median = c[c.size() / 2];
            for (auto& v : c) v = std::fabs(v - st.median);
            std::sort(c.begin(), c.end());
            st.spread = c[c.size() / 2];
            return st;
        };
        const Stats si = stats(in.l, 0, want);
        // the detector and the independent measurement agree about what was sung
        CHECK_MSG(std::fabs(si.median - (sungKey - wantKey) * 100.0) < 8.0, "%s: detector median %.1f cents from the note, correlation median %.1f", name.c_str(), (sungKey - wantKey) * 100.0, si.median);

        TrueNote t;
        t.prepare(sr);
        t.pSpeed->set(0.0f);
        t.pScale->set((float)TrueNote::kCustomScale);          // only the note being sung, so a singer between two notes cannot flip
        const int pc = (((int)wantKey % 12) + 12) % 12;
        for (int i = 0; i < 12; ++i) t.pNote[i]->set(i == pc ? 1.0f : 0.0f);
        auto out = run(t, in, sr);
        CHECK(allFinite(out));
        const size_t lat = (size_t)t.latencySamples();
        const Stats so = stats(out.l, lat, want);
        const double level = db(rmsOf(out.l, a + lat, b + lat) / rmsOf(in.l, a, b));
        CHECK_MSG(std::fabs(so.median) < 5.0, "%s: output sits %.1f cents from the note", name.c_str(), so.median);
        CHECK_MSG(so.spread < 0.5 * si.spread && so.spread < 4.0, "%s: output wobbles %.1f cents (input %.1f)", name.c_str(), so.spread, si.spread);
        CHECK_MSG(so.within10 >= 0.85, "%s: within 10 cents of the note only %.0f %% of the time", name.c_str(), 100.0 * so.within10);
        CHECK_MSG(std::fabs(level) < 1.0, "%s: level changed by %.2f dB", name.c_str(), level);

        // formants on the real voice: move it three semitones and see where the weight of the spectrum (300 Hz to 4 kHz) goes
        auto centroid = [&](const std::vector<float>& x, size_t delay) {
            const auto spec = powerSpectrum(x, a + delay, b + delay);
            double num = 0.0, den = 0.0;
            for (size_t k = 0; k < spec.size(); ++k) {
                const double hz = (double)k * sr / 4096.0;
                if (hz < 300.0 || hz > 4000.0) continue;
                num += hz * spec[k]; den += spec[k];
            }
            return num / den;
        };
        const double c0 = centroid(in.l, 0);
        double moved[2];
        for (int keep = 0; keep < 2; ++keep) {
            TrueNote u;
            u.prepare(sr);
            u.pSpeed->set(0.0f); u.pAmount->set(0.0f); u.pTranspose->set(3.0f); u.pFormant->set((float)keep);
            auto shifted = run(u, in, sr);
            moved[keep] = centroid(shifted.l, lat) / c0;
            const Stats sp = stats(shifted.l, lat, want * std::pow(2.0, 0.25));
            CHECK_MSG(std::fabs(sp.median - si.median) < 6.0, "%s: transposed by %.0f cents, not 300", name.c_str(), 300.0 + sp.median - si.median);
        }
        CHECK_MSG(std::fabs(moved[1] - 1.0) < 0.07, "%s: spectral weight moved %.1f %% with formants kept", name.c_str(), 100.0 * (moved[1] - 1.0));
        CHECK_MSG(moved[0] > 1.10 && moved[0] < 1.30, "%s: spectral weight moved %.1f %% with formants free", name.c_str(), 100.0 * (moved[0] - 1.0));
        printf("      %-24s %.0f Hz: sung %+.0f cents from key %.0f (spread %.1f, %.0f %% within 10)  ->  %+.1f cents (spread %.1f, %.0f %% within 10), level %+.2f dB;"
               " +3 st moves the spectral weight %+.1f %% (formants kept) / %+.1f %% (free)\n",
               name.c_str(), sr, si.median, wantKey, si.spread, 100.0 * si.within10, so.median, so.spread, 100.0 * so.within10, level,
               100.0 * (moved[1] - 1.0), 100.0 * (moved[0] - 1.0));
    }
    CHECK(files > 0);
}
