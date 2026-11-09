// Squelch (acid bass): tuning, aliasing, the filter, slide and accent.
#include "BasslineTestUtil.h"
#include "pulp/Squelch.h"
#include <cstdlib>
using namespace pulp;
using namespace bt;

namespace {
bool report() { return std::getenv("BASSLINE_REPORT") != nullptr; }

// A plain tone for measuring: filter wide open, no resonance, no envelope, hardly any drive.
void plain(Squelch& s, int wave = 0) {
    s.pWave->set((float)wave);
    s.pTune->set(0.0f);
    s.pCutoff->set(12000.0f);
    s.pReso->set(0.0f);
    s.pEnvMod->set(0.0f);
    s.pDrive->set(0.0f);
    s.pAccent->set(0.0f);
    s.pAmpDecay->set(8000.0f);
}
NoteEvent onEv(int key, uint32 id, float vel = kDefaultVelocity) {
    NoteEvent e; e.type = NoteEvent::On; e.key = key; e.id = id; e.velocity = vel; return e;
}
// Worst distance in cents between the measured pitch of every cycle in [from, to) and a slide that
// started at sample t0 from key a towards key b with the given slide time (95 % of the way in that time).
double slideError(const std::vector<float>& d, size_t from, size_t to, double sr, double t0, double a, double b, double slideMs) {
    double worst = 0.0;
    for (auto& c : cycleFreqs(d, from, to, sr)) {
        const double t = (c.first - t0) / sr;
        const double key = t <= 0.0 ? a : b - (b - a) * std::exp(-3.0 * t / (slideMs * 0.001));
        worst = std::max(worst, std::fabs(centsOff(c.second, keyToHz(key))));
    }
    return worst;
}
// The key the sound is at around sample `at` (mean over the cycles within 2 ms).
double keyAt(const std::vector<float>& d, double at, double sr) {
    double sum = 0.0;
    int n = 0;
    for (auto& c : cycleFreqs(d, (size_t)(at - 0.004 * sr), (size_t)(at + 0.004 * sr), sr))
        if (std::fabs(c.first - at) < 0.002 * sr) { sum += 69.0 + 12.0 * std::log2(c.second / 440.0); ++n; }
    return n ? sum / n : 0.0;
}
// Runs a Drive and records the three read-outs once per quantum.
struct Trace { std::vector<float> cutoff, accent, env; };
Trace traced(Squelch& s, Drive& d, int64 samples) {
    Trace t;
    d.each = [&](int64 pos) { if (pos > 0) { t.cutoff.push_back(s.meter(0)); t.accent.push_back(s.meter(1)); t.env.push_back(s.meter(2)); } };
    d.run(samples);
    t.cutoff.push_back(s.meter(0)); t.accent.push_back(s.meter(1)); t.env.push_back(s.meter(2));
    return t;           // entry q is the state at the end of quantum q
}
float maxOf(const std::vector<float>& v, size_t a, size_t b) { float m = 0.0f; for (size_t i = a; i < std::min(b, v.size()); ++i) m = std::max(m, v[i]); return m; }
}

TEST(squelch_is_in_tune_at_every_rate) {
    double worst = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int wave = 0; wave < 2; ++wave)
            for (int key : {24, 36, 48, 60, 72, 84}) {
                Squelch s;
                plain(s, wave);
                const auto out = oneNote(s, key, (int64)(2.0 * sr), (int64)(2.0 * sr), sr);
                const double hz = preciseHz(out, (size_t)(0.25 * sr), sr, keyToHz(key), 65536, 8192);
                const double cents = centsOff(hz, keyToHz(key));
                worst = std::max(worst, std::fabs(cents));
                CHECK_MSG(std::fabs(cents) < 0.5, "rate %.0f wave %d key %d: %.3f Hz, %.3f cents off", sr, wave, key, hz, cents);
            }
    if (report()) printf("    [squelch] worst tuning error %.4f cents (18 notes x 2 waves x 3 rates)\n", worst);
    // tune, fine tune, the note's fine pitch and the channel pitch move it by what they say; the default is two octaves down
    {
        Squelch s;
        plain(s);
        s.pTune->set(-24.0f);
        const auto out = oneNote(s, 60, 96000, 96000);
        CHECK_NEAR(centsOff(preciseHz(out, 12000, 48000.0, keyToHz(36), 65536, 8192), keyToHz(36)), 0.0, 0.5);
        Squelch def;
        CHECK(def.pTune->get() == -24.0f);
    }
    {
        Squelch s;
        plain(s);
        s.pTune->set(7.0f);
        s.pFine->set(50.0f);
        Drive d(s);
        d.bend = 2.0f;
        NoteEvent e = onEv(48, 1);
        e.fineCents = 25.0f;
        d.ev(0, e);
        d.run(96000);
        CHECK_NEAR(centsOff(preciseHz(d.L, 12000, 48000.0, keyToHz(57.75), 65536, 8192), keyToHz(48)), 975.0, 0.5);
    }
}

TEST(squelch_oscillators_are_band_limited) {
    double worst = -200.0;
    for (double sr : {44100.0, 48000.0})
        for (int wave = 0; wave < 2; ++wave)
            for (int key : {84, 96, 103}) {
                Squelch s;
                plain(s, wave);
                const auto out = oneNote(s, key, 80000, 80000, sr);
                const double f0 = keyToHz(key);
                const double below = aliasDb(out, 8192, 32768, sr, f0, 60.0, f0 - 100.0);
                const double all = aliasDb(out, 8192, 32768, sr, f0, 60.0, 16000.0);
                worst = std::max(worst, std::max(below, all));
                CHECK_MSG(below < -70.0, "rate %.0f wave %d key %d: aliases below the fundamental at %.1f dB", sr, wave, key, below);
                CHECK_MSG(all < -70.0, "rate %.0f wave %d key %d: aliases up to 16 kHz at %.1f dB", sr, wave, key, all);
            }
    if (report()) printf("    [squelch] worst alias, plain oscillator, keys 84 / 96 / 103: %.1f dB\n", worst);
    // The hard case: a bright resonant note with the drive all the way up.
    for (double sr : {44100.0, 48000.0}) {
        Squelch s;
        plain(s, 0);
        s.pCutoff->set(3000.0f);
        s.pReso->set(0.9f);
        s.pDrive->set(1.0f);
        const auto out = oneNote(s, 60, 80000, 80000, sr);
        const double f0 = keyToHz(60);
        const double a = aliasDb(out, 8192, 32768, sr, f0, 60.0, 16000.0);
        CHECK_MSG(a < -55.0, "rate %.0f: driven resonant note aliases at %.1f dB", sr, a);
        if (report()) printf("    [squelch] rate %.0f, key 60, cutoff 3 kHz, resonance 0.9, drive 1: alias %.1f dB\n", sr, a);
    }
}

TEST(squelch_note_starts_on_its_sample_and_is_silent_before) {
    Squelch s;
    Drive d(s);
    d.on(1000, 60, 1);
    d.off(9000, 1);
    d.run(48000);
    CHECK(peakOf(d.L, 0, 1000) == 0.0f);
    CHECK(peakOf(d.L, 1000, 1016) > 0.0f);
    CHECK(peakOf(d.L, 1000, 9000) > 0.05f);
    CHECK(peakOf(d.L, 40000, 48000) == 0.0f);            // let go, faded, and really silent
    CHECK(s.activeVoices() == 0);
}

TEST(squelch_overlapping_notes_slide_without_retriggering) {
    const double sr = 48000.0;
    for (float slideMs : {60.0f, 200.0f}) {
        Squelch s;
        plain(s);
        s.pSlide->set(slideMs);
        s.pEnvMod->set(0.3f);
        s.pDecay->set(1500.0f);
        s.pCutoff->set(6000.0f);
        Drive d(s, sr);
        d.on(0, 72, 1);
        d.on(24000, 84, 2);         // while the first is still held
        d.off(30000, 1);
        d.off(72000, 2);
        const Trace t = traced(s, d, 72000);
        // before: the first key. The filter envelope is on its way down.
        CHECK_MSG(slideError(d.L, 6000, 23900, sr, 24000.0, 72, 84, slideMs) < 3.0, "before the slide: %.2f cents", slideError(d.L, 6000, 23900, sr, 24000.0, 72, 84, slideMs));
        // during: every cycle sits on the curve
        const double during = slideError(d.L, 24000, 24000 + (size_t)(2.5 * slideMs * 48.0), sr, 24000.0, 72, 84, slideMs);
        CHECK_MSG(during < 25.0, "slide %g ms: worst %.1f cents from the curve", slideMs, during);
        const double third = keyAt(d.L, 24000.0 + slideMs * 48.0 / 3.0, sr);
        CHECK_MSG(std::fabs(third - (84.0 - 12.0 * std::exp(-1.0))) < 0.25, "slide %g ms: a third of the way in it is at key %.2f", slideMs, third);
        const double full = keyAt(d.L, 24000.0 + slideMs * 48.0, sr);
        CHECK_MSG(std::fabs(full - (84.0 - 12.0 * std::exp(-3.0))) < 0.15, "slide %g ms: at the slide time it is at key %.2f", slideMs, full);
        // after: arrived and stays
        CHECK_NEAR(centsOff(preciseHz(d.L, 48000, sr, keyToHz(84), 8192, 2048), keyToHz(84)), 0.0, 0.5);
        // no retrigger: the filter envelope never rises again, and the level does not dip or jump
        const size_t q = 24000 / kQuantum;
        bool rose = false;
        for (size_t i = 8; i + 1 < t.env.size(); ++i) rose = rose || t.env[i + 1] > t.env[i] + 1.0e-6f;
        CHECK(!rose);
        CHECK(t.env[q + 4] < t.env[q - 1] && t.env[q + 4] > 0.2f);
        const double before = rmsOf(d.L, 22000, 24000), after = rmsOf(d.L, 24000, 26000);
        CHECK_MSG(std::fabs(db(after / before)) < 4.0, "level moved %.2f dB across the slide", db(after / before));
        if (report()) printf("    [squelch] slide %g ms: worst %.1f cents from the curve; key at 1/3 %.3f (expect %.3f), at slide time %.3f (expect %.3f)\n",
                             slideMs, during, third, 84.0 - 12.0 * std::exp(-1.0), full, 84.0 - 12.0 * std::exp(-3.0));
    }
    // the same two notes without the overlap: the pitch jumps and the envelope starts again
    {
        Squelch s;
        plain(s);
        s.pEnvMod->set(0.3f);
        s.pDecay->set(1500.0f);
        s.pCutoff->set(6000.0f);
        Drive d(s, sr);
        d.on(0, 72, 1);
        d.off(23000, 1);
        d.on(24000, 84, 2);
        d.off(72000, 2);
        const Trace t = traced(s, d, 72000);
        const size_t q = 24000 / kQuantum;
        CHECK(t.env[q + 4] > 0.9f && t.env[q - 1] < 0.4f);
        CHECK_MSG(std::fabs(keyAt(d.L, 24000.0 + 240.0, sr) - 84.0) < 0.05, "5 ms into a detached note it is at key %.3f", keyAt(d.L, 24000.0 + 240.0, sr));
    }
}

TEST(squelch_slide_events_and_portamento_slide_too) {
    const double sr = 48000.0;
    const float slideMs = 60.0f;
    // a slide note (SlideTo): the engine gives it its own length, Squelch uses its slide time
    {
        Squelch s;
        plain(s);
        s.pEnvMod->set(0.3f);
        s.pDecay->set(1500.0f);
        Drive d(s, sr);
        d.on(0, 72, 1);
        NoteEvent sl; sl.type = NoteEvent::SlideTo; sl.key = 79; sl.group = 0; sl.slideSamples = 24000.0f;
        d.ev(24000, sl);
        NoteEvent other = sl; other.key = 60; other.group = 5;        // another colour: not ours
        d.ev(40000, other);
        d.off(72000, 1);
        const Trace t = traced(s, d, 72000);
        const double during = slideError(d.L, 24000, 24000 + 7200, sr, 24000.0, 72, 79, slideMs);
        CHECK_MSG(during < 25.0, "slide note: worst %.1f cents from the curve", during);
        CHECK_NEAR(centsOff(preciseHz(d.L, 48000, sr, keyToHz(79), 8192, 2048), keyToHz(79)), 0.0, 0.5);
        bool rose = false;
        for (size_t i = 8; i + 1 < t.env.size(); ++i) rose = rose || t.env[i + 1] > t.env[i] + 1.0e-6f;
        CHECK(!rose);
    }
    // a portamento note that starts on the sample the one before ends (Off, then On with glideFromKey)
    {
        Squelch s;
        plain(s);
        s.pEnvMod->set(0.3f);
        s.pDecay->set(1500.0f);
        Drive d(s, sr);
        d.on(0, 72, 1);
        d.off(24000, 1);
        NoteEvent e = onEv(60, 2);
        e.glideFromKey = 72.0f;
        e.slideSamples = 9600.0f;
        d.ev(24000, e);
        d.off(72000, 2);
        const Trace t = traced(s, d, 72000);
        const double during = slideError(d.L, 24000, 24000 + 7200, sr, 24000.0, 72, 60, slideMs);
        CHECK_MSG(during < 25.0, "portamento: worst %.1f cents from the curve", during);
        CHECK_NEAR(centsOff(preciseHz(d.L, 48000, sr, keyToHz(60), 8192, 2048), keyToHz(60)), 0.0, 0.5);
        bool rose = false;
        for (size_t i = 8; i + 1 < t.env.size(); ++i) rose = rose || t.env[i + 1] > t.env[i] + 1.0e-6f;
        CHECK(!rose);
        CHECK(peakOf(d.L, 23900, 24200) > 0.5f * peakOf(d.L, 20000, 23900));       // it did not let go in between
    }
    // a portamento note from silence has nothing to tie to: it starts, and glides in from the old key
    {
        Squelch s;
        plain(s);
        Drive d(s, sr);
        NoteEvent e = onEv(84, 1);
        e.glideFromKey = 72.0f;
        e.slideSamples = 9600.0f;
        d.ev(4800, e);
        d.off(60000, 1);
        const Trace t = traced(s, d, 60032);
        CHECK(maxOf(t.env, 4800 / kQuantum, 4800 / kQuantum + 6) > 0.9f);
        CHECK_MSG(slideError(d.L, 5000, 4800 + 7200, sr, 4800.0, 72, 84, slideMs) < 25.0, "worst %.1f cents", slideError(d.L, 5000, 4800 + 7200, sr, 4800.0, 72, 84, slideMs));
    }
    // through the engine: an overlap, a slide note and a portamento note all arrive where they should
    {
        auto s = std::make_shared<Squelch>();
        plain(*s);
        Note a = mk(0, 1200, 72), b = mk(960, 960, 79), c = mk(1920, 480, 84), e = mk(2880, 480, 72);
        c.slide = true;                 // bends what is sounding (b has ended by then? no: b lasts until 1920)
        b.length = 1920;                // b is held from 960 to 2880, so the slide note at 1920 bends it
        e.porta = true;                 // starts exactly where b ends
        RenderSpec rs;
        rs.length = 96000;
        const auto out = play(s, {a, b, c, e}, rs);
        const auto& d = out->ch[0];
        CHECK_NEAR(keyAt(d, 20000.0, 48000.0), 72.0, 0.05);
        CHECK_NEAR(keyAt(d, 44000.0, 48000.0), 79.0, 0.05);          // slid up from the overlap
        CHECK_NEAR(keyAt(d, 68000.0, 48000.0), 84.0, 0.05);          // the slide note took it up
        CHECK_NEAR(keyAt(d, 82000.0, 48000.0), 72.0, 0.05);          // the portamento note brought it down
        const double mid = keyAt(d, 24000.0 + 960.0, 48000.0);       // 20 ms into the first slide
        CHECK_MSG(mid > 74.0 && mid < 78.5, "20 ms into the slide it is at key %.2f", mid);
    }
}

TEST(squelch_accent_is_louder_brighter_and_builds_up) {
    const double sr = 48000.0;
    // level: with the filter out of the way, the accent is a known gain
    {
        double rms[2];
        for (int i = 0; i < 2; ++i) {
            Squelch s;
            plain(s);
            s.pAccent->set(0.6f);
            const auto out = oneNote(s, 48, 24000, 24000, sr, vel127(110 + i));
            rms[i] = rmsOf(out, 2400, 21600);
        }
        const double gain = db(rms[1] / rms[0]);
        CHECK_MSG(gain > 3.0 && gain < 3.9, "accent at 0.6 lifts the level by %.2f dB (the gain is 3.75 dB before the drive stage)", gain);
        if (report()) printf("    [squelch] velocity 111 against 110, accent 0.6, filter open: %+.2f dB\n", gain);
    }
    // with the accent amount at zero the two velocities are the same sound
    {
        double rms[2];
        for (int i = 0; i < 2; ++i) {
            Squelch s;
            plain(s);
            s.pDecay->set(200.0f);
            const auto out = oneNote(s, 48, 24000, 24000, sr, vel127(110 + i));
            rms[i] = rmsOf(out, 2400, 21600);
        }
        CHECK_MSG(std::fabs(db(rms[1] / rms[0])) < 0.1, "%.3f dB", db(rms[1] / rms[0]));
    }
    // filter: the accented note opens further, and sounds brighter
    {
        // The accent sweep is slower than the envelope itself (that is the "wow"), so the difference
        // is largest a little after the start: the filter is read 40 ms in.
        float top[2], at40[2];
        double bright[2], level[2];
        for (int i = 0; i < 2; ++i) {
            Squelch s;
            s.pTune->set(0.0f);
            s.pCutoff->set(400.0f);
            s.pReso->set(0.5f);
            s.pEnvMod->set(0.4f);
            s.pDecay->set(200.0f);
            s.pAccent->set(0.7f);
            s.pDrive->set(0.0f);
            Drive d(s, sr);
            d.on(0, 36, 1, vel127(110 + i));
            d.off(20000, 1);
            const Trace t = traced(s, d, 24000);
            top[i] = maxOf(t.cutoff, 0, t.cutoff.size());
            at40[i] = t.cutoff[1920 / kQuantum];
            bright[i] = centroidHz(d.L, 960, 4096, sr);
            level[i] = rmsOf(d.L, 0, 9600);
        }
        CHECK_MSG(top[1] > top[0] * 1.05f, "filter peak %.0f Hz plain, %.0f Hz accented", top[0], top[1]);
        CHECK_MSG(at40[1] > at40[0] * 1.6f, "filter 40 ms in: %.0f Hz plain, %.0f Hz accented", at40[0], at40[1]);
        CHECK_MSG(bright[1] > bright[0] * 1.15, "centroid %.0f Hz plain, %.0f Hz accented", bright[0], bright[1]);
        CHECK_MSG(db(level[1] / level[0]) > 2.0, "level %+.2f dB", db(level[1] / level[0]));
        if (report()) printf("    [squelch] accent 0.7, resonance 0.5: filter peak %.0f -> %.0f Hz, 40 ms in %.0f -> %.0f Hz, centroid %.0f -> %.0f Hz, level %+.2f dB\n",
                             top[0], top[1], at40[0], at40[1], bright[0], bright[1], db(level[1] / level[0]));
    }
    // the accent charge builds over a run of accented notes at high resonance, and plain notes leave it alone
    {
        Squelch s;
        s.pReso->set(0.9f);
        s.pAccent->set(1.0f);
        Drive d(s, sr);
        for (int i = 0; i < 4; ++i) { d.on(i * 6000, 48, (uint32)(i + 1), vel127(120)); d.off(i * 6000 + 3000, (uint32)(i + 1)); }
        for (int i = 4; i < 8; ++i) { d.on(i * 6000, 48, (uint32)(i + 1), vel127(100)); d.off(i * 6000 + 3000, (uint32)(i + 1)); }
        const Trace t = traced(s, d, 60032);
        const size_t per = 6000 / kQuantum;
        const float first = maxOf(t.accent, 0, per), third = maxOf(t.accent, 2 * per + 1, 3 * per);
        CHECK_MSG(third > first * 1.1f, "accent charge peaks at %.3f on the first note and %.3f on the third", first, third);
        CHECK(maxOf(t.accent, 7 * per + 8, 8 * per) < 0.02f);        // long gone under the plain notes
        if (report()) printf("    [squelch] accent charge, four accented sixteenths at 120 bpm, resonance 0.9: %.3f, then %.3f on the third\n", first, third);
        // an accented note's envelope is always the short one
        Squelch a, b;
        a.pDecay->set(2500.0f);
        b.pDecay->set(2500.0f);
        Drive da(a, sr), db_(b, sr);
        da.on(0, 48, 1, vel127(100));
        db_.on(0, 48, 1, vel127(127));
        const Trace ta = traced(a, da, 19200), tb = traced(b, db_, 19200);
        CHECK_MSG(ta.env.back() > 0.4f && tb.env.back() < 0.01f, "envelope after 400 ms: %.3f plain, %.4f accented", ta.env.back(), tb.env.back());
    }
    // below the accent, velocity is a gentle level control
    {
        Squelch s, q;
        plain(s);
        plain(q);
        const double loud = rmsOf(oneNote(s, 48, 24000, 24000, sr, vel127(100)), 2400, 21600);
        const double soft = rmsOf(oneNote(q, 48, 24000, 24000, sr, vel127(50)), 2400, 21600);
        CHECK_MSG(db(soft / loud) < -2.0 && db(soft / loud) > -8.0, "velocity 50 against 100: %.2f dB", db(soft / loud));
    }
}

TEST(squelch_filter_peaks_at_the_cutoff_and_falls_four_poles) {
    const double sr = 48000.0;
    // resonance: of a low note's partials within an octave either side, the strongest is the one at the cutoff
    for (float fc : {500.0f, 1000.0f, 2000.0f, 4000.0f}) {
        Squelch s;
        plain(s);
        s.pCutoff->set(fc);
        s.pReso->set(0.92f);
        const auto out = oneNote(s, 24, 80000, 80000, sr);
        const double pk = peakHz(out, 8192, 65536, sr, 0.5 * fc, 2.0 * fc);
        CHECK_MSG(std::fabs(pk / fc - 1.0) < 0.06, "cutoff %.0f Hz: the peak is at %.0f Hz", fc, pk);
        if (report()) printf("    [squelch] cutoff %.0f Hz, resonance 0.92: strongest partial at %.0f Hz\n", fc, pk);
    }
    // Without resonance the filter is linear, so it can be held against the diode ladder's own response:
    // 1/8 over s^4 + 4 s^3 + 5 s^2 + 2 s + 1/8, with the corner at sqrt(2) times the cutoff (the ladder
    // peaks below its corner). That is a soft knee and then four poles: about 24 dB an octave far above.
    {
        Squelch open, shut;
        plain(open);
        plain(shut);
        shut.pCutoff->set(150.0f);
        const auto a = oneNote(open, 24, 80000, 80000, sr), b = oneNote(shut, 24, 80000, 80000, sr);
        const double f0 = keyToHz(24);
        auto ladderDb = [&](double hz, double fc) {
            const double w = std::tan(kPi * hz / (2.0 * sr)) / (1.4142135623730951 * std::tan(kPi * fc / (2.0 * sr)));
            const double re = w * w * w * w - 5.0 * w * w + 0.125, im = -4.0 * w * w * w + 2.0 * w;
            return db(0.125 / std::hypot(re, im));
        };
        double worst = 0.0, at600 = 0.0, at1200 = 0.0;
        for (double hz : {65.0, 150.0, 300.0, 600.0, 1200.0}) {
            const double h = std::round(hz / f0) * f0;
            const double got = db(ampAt(b, 8192, 65536, h, sr) / ampAt(a, 8192, 65536, h, sr));
            const double want = ladderDb(h, 150.0) - ladderDb(h, 12000.0);
            worst = std::max(worst, std::fabs(got - want));
            CHECK_MSG(std::fabs(got - want) < 0.5, "at %.0f Hz the filter takes %.2f dB, the ladder should take %.2f", h, got, want);
            if (hz == 600.0) at600 = got;
            if (hz == 1200.0) at1200 = got;
        }
        CHECK_MSG(at600 - at1200 > 20.0 && at600 - at1200 < 25.0, "from 600 Hz to 1.2 kHz it falls %.1f dB", at600 - at1200);
        if (report()) printf("    [squelch] cutoff 150 Hz, no resonance: within %.2f dB of the ladder's response from 65 Hz to 1.2 kHz; %.1f dB from 600 Hz to 1.2 kHz\n",
                             worst, at600 - at1200);
    }
    // a note's mod X moves the cutoff, mod Y the resonance
    {
        double peak[2], ratio[2];
        for (int i = 0; i < 2; ++i) {
            Squelch s;
            plain(s);
            s.pCutoff->set(600.0f);
            s.pReso->set(0.3f);
            Drive d(s, sr);
            NoteEvent e = onEv(24, 1);
            e.modX = i ? 0.75f : 0.5f;          // + 2 octaves
            e.modY = i ? 0.8f : 0.5f;           // + 0.6 resonance
            d.ev(0, e);
            d.run(80000);
            peak[i] = peakHz(d.L, 8192, 65536, sr, 1200.0, 4800.0);
            const double f0 = keyToHz(24);
            ratio[i] = ampAt(d.L, 8192, 65536, std::round(2400.0 / f0) * f0, sr) / ampAt(d.L, 8192, 65536, std::round(300.0 / f0) * f0, sr);
        }
        CHECK_MSG(std::fabs(peak[1] / 2400.0 - 1.0) < 0.06, "with mod X at 0.75 the peak is at %.0f Hz", peak[1]);
        CHECK_MSG(ratio[1] > ratio[0] * 20.0, "partial at 2.4 kHz against the one at 300 Hz: %.4f -> %.4f", ratio[0], ratio[1]);
    }
}

TEST(squelch_screams_without_blowing_up) {
    const double sr = 48000.0;
    Squelch s;
    s.pReso->set(1.0f);
    s.pEnvMod->set(1.0f);
    s.pAccent->set(1.0f);
    s.pDrive->set(1.0f);
    s.pVolume->set(1.0f);
    Drive d(s, sr);
    for (int i = 0; i < 40; ++i) {
        d.on(i * 4800, 36 + (i * 7) % 36, (uint32)(i + 1), vel127(i % 3 ? 127 : 90));
        d.off(i * 4800 + (i % 4 == 0 ? 6000 : 2400), (uint32)(i + 1));
    }
    d.each = [&](int64 pos) {
        const double x = (double)pos / 192000.0;
        s.pCutoff->setNorm((float)(0.5 - 0.5 * std::cos(kTwoPi * x * 7.0)));          // right across the range, fast
        s.pTune->set(pos % 9600 < 4800 ? 24.0f : -36.0f);
    };
    d.run(192000);
    CHECK(allFinite(d.L) && allFinite(d.R));
    const float pk = peakOf(d.L);
    CHECK_MSG(pk > 0.2f && pk < 2.0f, "peak %.3f", pk);
    if (report()) printf("    [squelch] everything at full, cutoff swept across its range: peak %.2f dBFS at level 1.0\n", db(pk));
    // resonance at full with the envelope out of it: it sings at the cutoff and stays bounded
    Squelch r;
    plain(r);
    r.pReso->set(1.0f);
    r.pCutoff->set(1000.0f);
    const auto out = oneNote(r, 24, 96000, 96000, sr);
    CHECK(allFinite(out));
    const double pk1 = peakHz(out, 16384, 65536, sr, 300.0, 12000.0);
    CHECK_MSG(std::fabs(pk1 / 1000.0 - 1.0) < 0.05, "at full resonance the peak is at %.0f Hz", pk1);
    CHECK_MSG(peakOf(out) < 1.0f && rmsOf(out, 70000, 90000) > 0.01, "peak %.3f, late level %.4f", peakOf(out), rmsOf(out, 70000, 90000));
}

TEST(squelch_drive_adds_harmonics_and_square_differs_from_saw) {
    const double sr = 48000.0, f0 = keyToHz(48);
    double odd[2], peak[2];
    for (int i = 0; i < 2; ++i) {
        Squelch s;
        plain(s);
        s.pCutoff->set(130.0f);         // resonant at the note itself: close to a sine going into the drive
        s.pReso->set(0.85f);
        s.pDrive->set(i ? 1.0f : 0.0f);
        const auto out = oneNote(s, 48, 60000, 60000, sr);
        odd[i] = (ampAt(out, 8192, 32768, 3.0 * f0, sr) + ampAt(out, 8192, 32768, 5.0 * f0, sr)) / ampAt(out, 8192, 32768, f0, sr);
        peak[i] = peakOf(out);
    }
    CHECK_MSG(odd[1] > odd[0] * 3.0 && odd[1] > 0.2, "third and fifth harmonics against the fundamental: %.4f clean, %.4f driven", odd[0], odd[1]);
    CHECK_MSG(std::fabs(db(peak[1] / peak[0])) < 9.0, "drive moves the peak by %.1f dB", db(peak[1] / peak[0]));
    if (report()) printf("    [squelch] drive 0 -> 1 on a near-sine: odd harmonics %.3f -> %.3f of the fundamental, peak %+.1f dB\n", odd[0], odd[1], db(peak[1] / peak[0]));
    // a square has no even harmonics; a saw has
    double second[2];
    for (int wave = 0; wave < 2; ++wave) {
        Squelch s;
        plain(s, wave);
        const auto out = oneNote(s, 48, 60000, 60000, sr);
        second[wave] = ampAt(out, 8192, 32768, 2.0 * f0, sr) / ampAt(out, 8192, 32768, f0, sr);
    }
    CHECK_MSG(second[0] > 0.4 && second[1] < 0.02, "second harmonic: saw %.3f, square %.4f", second[0], second[1]);
}

