// Thud (kick synthesizer): the pitch sweep and its end points, the decay, click, drive, sub, tone,
// key transposition and clean retriggering.
#include "BasslineTestUtil.h"
#include "pulp/Thud.h"
#include <cstdlib>
using namespace pulp;
using namespace bt;

namespace {
bool report() { return std::getenv("BASSLINE_REPORT") != nullptr; }

// Just the swept sine: no click, no sub, as clean as the drive goes, a long tail.
void bare(Thud& t) {
    t.pClick->set(0.0f);
    t.pSub->set(0.0f);
    t.pDrive->set(0.0f);
    t.pDecay->set(4000.0f);
}
// Upward zero crossings (sample positions) of a signal.
std::vector<double> crossings(const std::vector<float>& d, size_t from, size_t to) {
    std::vector<double> x;
    for (size_t i = std::max<size_t>(from, 1); i < std::min(to, d.size()); ++i)
        if (d[i - 1] < 0.0f && d[i] >= 0.0f) x.push_back((double)(i - 1) + (double)(-d[i - 1]) / (double)(d[i] - d[i - 1]));
    return x;
}
// Cycles the model sweep goes through between two times (seconds), by Simpson's rule.
double cyclesBetween(const Thud& t, double a, double b, double ratio = 1.0) {
    const int n = 200;
    double sum = 0.0;
    for (int i = 0; i <= n; ++i) sum += t.sweepHz(a + (b - a) * i / n) * (i == 0 || i == n ? 1.0 : (i % 2 ? 4.0 : 2.0));
    return ratio * sum * (b - a) / (3.0 * n);
}
}

TEST(thud_sweep_runs_from_the_start_pitch_to_the_end_pitch_along_its_curve) {
    double worstCycle = 0.0, worstEnd = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (float curve : {0.0f, 0.5f, 1.0f}) {
            Thud t;
            bare(t);
            t.pStart->set(400.0f);
            t.pEnd->set(50.0f);
            t.pSweep->set(120.0f);
            t.pCurve->set(curve);
            const auto out = oneNote(t, 60, (int64)(1.5 * sr), (int64)(1.5 * sr), sr);
            // Between every two upward zero crossings the stated sweep must go through exactly one cycle.
            // (The first few samples are skipped: the output filters delay the wave by about two samples.)
            const auto x = crossings(out, 8, (size_t)(0.5 * sr));
            CHECK(x.size() > 20);
            const double lag = (1.94 + 1.414 * sr / (kTwoPi * 20000.0)) / sr;      // delay of the 2x decimator and the (open) tone filter down here
            for (size_t i = 1; i < x.size(); ++i) {
                const double c = cyclesBetween(t, x[i - 1] / sr - lag, x[i] / sr - lag);
                worstCycle = std::max(worstCycle, std::fabs(c - 1.0));
                CHECK_MSG(std::fabs(c - 1.0) < 0.01, "rate %.0f curve %.1f: the sweep makes %.4f cycles between crossings at %.2f and %.2f ms",
                          sr, curve, c, 1000.0 * x[i - 1] / sr, 1000.0 * x[i] / sr);
            }
            // the start: the first upward crossing comes after exactly one cycle of the model from time zero
            CHECK_MSG(std::fabs(cyclesBetween(t, 0.0, x[0] / sr - lag) - 1.0) < 0.02, "rate %.0f curve %.1f: first cycle %.4f", sr, curve, cyclesBetween(t, 0.0, x[0] / sr - lag));
            // the end: once the sweep time has passed it sits on the end pitch
            const double endHz = preciseHz(out, (size_t)(0.2 * sr), sr, 50.0, 32768, 4096);
            worstEnd = std::max(worstEnd, std::fabs(centsOff(endHz, 50.0)));
            CHECK_MSG(std::fabs(centsOff(endHz, 50.0)) < 1.0, "rate %.0f curve %.1f: after the sweep it is at %.3f Hz", sr, curve, endHz);
        }
    if (report()) printf("    [thud] sweep 400 -> 50 Hz in 120 ms, curves 0 / 0.5 / 1, three rates: every cycle within %.2f %% of the stated sweep; end pitch within %.3f cents\n",
                         100.0 * worstCycle, worstEnd);
    // the model itself: end points and time
    Thud t;
    t.pStart->set(400.0f);
    t.pEnd->set(50.0f);
    t.pSweep->set(120.0f);
    t.pCurve->set(0.0f);
    CHECK_NEAR(t.sweepHz(0.0), 400.0, 1.0e-3);
    CHECK_NEAR(t.sweepHz(0.06), 225.0, 1.0e-3);         // a straight line at curve 0
    CHECK_NEAR(t.sweepHz(0.12), 50.0, 1.0e-3);
    CHECK_NEAR(t.sweepHz(0.5), 50.0, 1.0e-3);
    t.pCurve->set(1.0f);
    CHECK(t.sweepHz(0.03) < 100.0 && t.sweepHz(0.03) > 50.0);      // steep: most of the fall is in the first quarter
}

TEST(thud_key_transposes_and_key_60_is_the_stated_tuning) {
    const double sr = 48000.0;
    auto tail = [&](Thud& t, int key, float bend = 0.0f) {
        Drive d(t, sr);
        d.bend = bend;
        d.on(0, key, 1);
        d.run(72000);
        return d.L;
    };
    double worst = 0.0;
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (int key : {48, 60, 72, 84}) {
            Thud t;
            bare(t);
            t.pEnd->set(55.0f);
            const auto out = oneNote(t, key, (int64)(1.5 * rate), (int64)(1.5 * rate), rate);
            const double want = 55.0 * std::pow(2.0, (key - 60) / 12.0);
            const double cents = centsOff(preciseHz(out, (size_t)(0.25 * rate), rate, want, 32768, 4096), want);
            worst = std::max(worst, std::fabs(cents));
            CHECK_MSG(std::fabs(cents) < 1.0, "rate %.0f key %d: %.3f cents from %.2f Hz", rate, key, cents, want);
        }
    if (report()) printf("    [thud] end pitch at keys 48 / 60 / 72 / 84, three rates: worst %.4f cents\n", worst);
    {   // key follow off: every key is the same drum
        Thud t;
        bare(t);
        t.pEnd->set(55.0f);
        t.pKeyTrack->set(0.0f);
        CHECK_NEAR(centsOff(preciseHz(tail(t, 84), 12000, sr, 55.0, 32768, 4096), 55.0), 0.0, 1.0);
    }
    {   // tune, fine and the channel pitch
        Thud t;
        bare(t);
        t.pEnd->set(55.0f);
        t.pTune->set(5.0f);
        t.pFine->set(-30.0f);
        const double want = 55.0 * std::pow(2.0, 6.7 / 12.0);
        CHECK_NEAR(centsOff(preciseHz(tail(t, 60, 2.0f), 12000, sr, want, 32768, 4096), want), 0.0, 1.0);
    }
    {   // a slide note bends a long hit to another key (a gliding bass drum line), portamento glides in
        Thud t;
        bare(t);
        t.pEnd->set(55.0f);
        Drive d(t, sr);
        d.on(0, 60, 1);
        NoteEvent sl; sl.type = NoteEvent::SlideTo; sl.key = 67; sl.group = 0; sl.slideSamples = 9600.0f;
        d.ev(24000, sl);
        d.run(96000);
        const double want = 55.0 * std::pow(2.0, 7.0 / 12.0);
        CHECK_NEAR(centsOff(preciseHz(d.L, 9600, sr, 55.0, 8192, 2048), 55.0), 0.0, 1.5);
        CHECK_NEAR(centsOff(preciseHz(d.L, 40000, sr, want, 32768, 4096), want), 0.0, 1.0);
        const double mid = peakHz(d.L, 24000 + 4800 - 4096, 8192, sr, 40.0, 120.0);
        CHECK_MSG(mid > 57.0 && mid < 80.0, "mid-slide %.1f Hz", mid);
    }
}

TEST(thud_decay_is_the_time_to_fall_60_dB) {
    double worst = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (float decay : {150.0f, 500.0f, 2000.0f}) {
            Thud t;
            bare(t);
            t.pStart->set(500.0f);          // no sweep: a steady tone whose level can be read
            t.pEnd->set(500.0f);
            t.pDecay->set(decay);
            const double secs = decay * 0.001;
            const auto out = oneNote(t, 60, (int64)(1.2 * secs * sr), (int64)(1.2 * secs * sr), sr);
            const int win = (int)(0.08 * secs * sr);
            const double t60 = t60Of(out, 500.0, sr, (size_t)(0.1 * secs * sr), (size_t)(0.6 * secs * sr), win);
            worst = std::max(worst, std::fabs(t60 / secs - 1.0));
            CHECK_MSG(std::fabs(t60 / secs - 1.0) < 0.03, "rate %.0f decay %.0f ms: falls 60 dB in %.1f ms", sr, decay, 1000.0 * t60);
        }
    if (report()) printf("    [thud] decay 150 / 500 / 2000 ms at three rates: measured 60 dB time within %.2f %%\n", 100.0 * worst);
    // and it really ends: exact silence a little after
    Thud t;
    bare(t);
    t.pDecay->set(200.0f);
    const auto out = oneNote(t, 60, 4800, 48000);
    CHECK(peakOf(out, 30000, 48000) == 0.0f && t.activeVoices() == 0);
    CHECK(peakOf(out, 0, 4800) > 0.2f);
    // the tail falls smoothly into that silence
    size_t last = 0;
    for (size_t i = 0; i < out.size(); ++i) if (out[i] != 0.0f) last = i;
    CHECK_MSG(peakOf(out, last - 64, last + 1) < 1.0e-5f, "last 64 samples peak at %g", peakOf(out, last - 64, last + 1));
}

TEST(thud_click_drive_sub_and_tone) {
    const double sr = 48000.0;
    // click: a burst of band-passed noise at the very start, where the control says, and nowhere else
    {
        Thud off, on, high;
        off.pClick->set(0.0f);
        on.pClick->set(1.0f);
        high.pClick->set(1.0f);
        on.pClickTone->set(2000.0f);
        high.pClickTone->set(8000.0f);
        const auto a = oneNote(off, 60, 24000, 24000), b = oneNote(on, 60, 24000, 24000), c = oneNote(high, 60, 24000, 24000);
        // (windows are centred on the start of the hit: 1024 samples of silence are put in front)
        const double sa = bandShare(lead(a, 1024), 0, 2048, sr, 1000.0, 20000.0), sb = bandShare(lead(b, 1024), 0, 2048, sr, 1000.0, 20000.0);
        CHECK_MSG(sb > 0.05 && sb > 20.0 * sa, "share of the first 21 ms above 1 kHz: %.5f without, %.4f with", sa, sb);
        std::vector<float> clickLow(b.size()), clickHigh(c.size());
        for (size_t i = 0; i < b.size(); ++i) { clickLow[i] = b[i] - a[i]; clickHigh[i] = c[i] - a[i]; }
        CHECK(peakOf(clickLow, 4800, 24000) < 1.0e-4f);             // it is over within tens of milliseconds
        CHECK_MSG(peakOf(clickLow, 0, 480) > 0.15f * peakOf(a), "click peaks at %.3f, the body at %.3f", peakOf(clickLow, 0, 480), peakOf(a));
        const double cl = centroidHz(lead(clickLow, 1024), 0, 2048, sr), ch = centroidHz(lead(clickHigh, 1024), 0, 2048, sr);
        CHECK_MSG(cl > 1200.0 && cl < 7000.0 && ch > 1.5 * cl, "click centroid %.0f Hz at 2 kHz, %.0f Hz at 8 kHz", cl, ch);
        if (report()) printf("    [thud] click at full: peak %.3f against the body's %.3f, %.1f %% of the first 21 ms above 1 kHz; centroid %.0f Hz at click tone 2 kHz, %.0f Hz at 8 kHz\n",
                             peakOf(clickLow, 0, 480), peakOf(a), 100.0 * sb, cl, ch);
    }
    // drive: harmonics, with the peak left where it was
    {
        double third[2], peak[2];
        for (int i = 0; i < 2; ++i) {
            Thud t;
            bare(t);
            t.pStart->set(110.0f);
            t.pEnd->set(110.0f);
            t.pDrive->set(i ? 1.0f : 0.0f);
            const auto out = oneNote(t, 60, 48000, 48000);
            third[i] = ampAt(out, 2400, 8192, 330.0, sr) / ampAt(out, 2400, 8192, 110.0, sr);
            peak[i] = peakOf(out);
        }
        CHECK_MSG(third[0] < 0.01 && third[1] > 0.2, "third harmonic %.4f clean, %.3f driven", third[0], third[1]);
        CHECK_MSG(std::fabs(db(peak[1] / peak[0])) < 1.5, "drive moves the peak by %.2f dB", db(peak[1] / peak[0]));
        if (report()) printf("    [thud] drive 0 -> 1: third harmonic %.4f -> %.3f of the fundamental, peak %+.2f dB\n", third[0], third[1], db(peak[1] / peak[0]));
        // a note's mod Y adds drive
        Thud t;
        bare(t);
        t.pStart->set(110.0f);
        t.pEnd->set(110.0f);
        Drive d(t, sr);
        NoteEvent e; e.type = NoteEvent::On; e.key = 60; e.id = 1; e.modY = 1.0f;
        d.ev(0, e);
        d.run(24000);
        CHECK(ampAt(d.L, 2400, 8192, 330.0, sr) / ampAt(d.L, 2400, 8192, 110.0, sr) > 0.05);
    }
    // the drive stage does not alias: a steady driven tone has nothing between its harmonics
    {
        double worst = -200.0;
        for (double rate : {44100.0, 48000.0})
            for (float hz : {110.0f, 220.0f, 440.0f}) {
                Thud t;
                bare(t);
                t.pStart->set(hz);
                t.pEnd->set(hz);
                t.pDrive->set(1.0f);
                t.pDecay->set(8000.0f);
                const auto out = oneNote(t, 60, 60000, 60000, rate);
                const double a = aliasDb(out, 4096, 32768, rate, hz, 30.0, 16000.0);
                worst = std::max(worst, a);
                CHECK_MSG(a < -60.0, "rate %.0f, %.0f Hz at full drive: %.1f dB between the harmonics", rate, hz, a);
            }
        if (report()) printf("    [thud] full drive on a steady 110 / 220 / 440 Hz tone at 44.1 and 48 kHz: worst alias %.1f dB\n", worst);
    }
    // sub: a clean copy of the swept sine under the body, in step with it whatever the sweep did
    {
        double low[2];
        for (int i = 0; i < 2; ++i) {
            Thud t;
            bare(t);
            t.pStart->set(600.0f);
            t.pEnd->set(180.0f);
            t.pSweep->set(4.0f);
            t.pDrive->set(0.0f);
            t.pSub->set(i ? 1.0f : 0.0f);
            const auto out = oneNote(t, 60, 48000, 48000);
            low[i] = ampAt(out, 2400, 8192, 180.0, sr);
        }
        CHECK_MSG(db(low[1] / low[0]) > 5.0 && db(low[1] / low[0]) < 7.0, "sub at full adds %.2f dB at the end pitch (a sine as big as the body is +6)", db(low[1] / low[0]));
    }
    // tone: a low-pass after everything; a note's mod X moves it
    {
        Thud open, shut;
        for (Thud* t : {&open, &shut}) { bare(*t); t->pStart->set(110.0f); t->pEnd->set(110.0f); t->pDrive->set(1.0f); }
        shut.pTone->set(400.0f);
        const auto a = oneNote(open, 60, 24000, 24000), b = oneNote(shut, 60, 24000, 24000);
        const double ha = ampAt(a, 2400, 8192, 1210.0, sr), hb = ampAt(b, 2400, 8192, 1210.0, sr);
        CHECK_MSG(db(hb / ha) < -15.0 && db(hb / ha) > -24.0, "eleventh harmonic (1.2 kHz) with the tone at 400 Hz: %.1f dB (two poles: about -19)", db(hb / ha));
        Drive d(shut, sr);
        NoteEvent e; e.type = NoteEvent::On; e.key = 60; e.id = 1; e.modX = 1.0f;
        d.ev(0, e);
        d.run(24000);
        CHECK(db(ampAt(d.L, 2400, 8192, 1210.0, sr) / ha) > -3.0);
    }
}

TEST(thud_retriggers_without_a_click_and_plays_as_one_drum) {
    const double sr = 48000.0;
    // A new hit every 150 ms on a two-second tail, no click generator: the only corners in the
    // waveform would be the retriggers themselves.
    for (int choke = 0; choke < 2; ++choke) {
        Thud t;
        bare(t);
        t.pChoke->set((float)choke);
        t.pDecay->set(2000.0f);
        Drive d(t, sr);
        int most = 0;
        d.each = [&](int64) { most = std::max(most, t.activeVoices()); };
        for (int i = 0; i < 12; ++i) d.on(i * 7211, 60, (uint32)(i + 1));
        d.run(96000);
        // the hardest bend an untouched hit makes, for comparison
        Thud ref;
        bare(ref);
        ref.pDecay->set(2000.0f);
        const auto one = oneNote(ref, 60, 48000, 48000);
        const float clean = maxBend(one, 2, 48000);
        float worst = 0.0f;
        for (int i = 1; i < 12; ++i) worst = std::max(worst, maxBend(d.L, (size_t)(i * 7211 - 32), (size_t)(i * 7211 + 400)));
        CHECK_MSG(worst < 4.0f * clean + 1.0e-4f, "choke %d: hardest bend at a retrigger %.5f, in a single hit %.5f", choke, worst, clean);
        CHECK(maxStep(d.L, 0, 96000) < 4.0f * maxStep(one, 0, 48000));
        if (choke) CHECK_MSG(most <= 2, "voices at once with choke: %d", most);
        else CHECK_MSG(most > 2 && most <= Thud::kVoices, "voices at once without choke: %d", most);
        if (report()) printf("    [thud] retrigger every 150 ms on a 2 s tail, choke %d: hardest second difference %.5f (a single hit: %.5f), most voices %d\n", choke, worst, clean, most);
    }
    // with choke, 10 ms after a new hit only the new hit is left: it is the same sound as a hit on its own
    {
        Thud t, alone;
        bare(t);
        bare(alone);
        Drive d(t, sr);
        d.on(0, 60, 1);
        d.on(9600, 60, 2);
        d.run(24000);
        const auto one = oneNote(alone, 60, 14400, 14400);
        double diff = 0.0;
        for (size_t i = 480; i < 14400; ++i) diff = std::max(diff, (double)std::fabs(d.L[9600 + i] - one[i]));
        CHECK_MSG(diff < 2.0e-3, "difference from a hit on its own, from 10 ms: %g", diff);
    }
    // the note's length does nothing unless asked; then letting go cuts the tail
    {
        Thud ring, gated;
        bare(ring);
        bare(gated);
        gated.pGate->set(1.0f);
        const auto a = oneNote(ring, 60, 4800, 48000), b = oneNote(gated, 60, 4800, 48000);
        CHECK(rmsOf(a, 19200, 24000) > 0.05);
        CHECK_MSG(db(rmsOf(b, 4800 + 3840, 4800 + 4800) / rmsOf(b, 3840, 4800)) < -50.0, "80 to 100 ms after letting go: %.1f dB", db(rmsOf(b, 4800 + 3840, 4800 + 4800) / rmsOf(b, 3840, 4800)));
        CHECK(maxBend(b, 4700, 9600) < 4.0f * maxBend(a, 2, 4700) + 1.0e-4f);
    }
    // velocity, pan, and the hit lands on its sample
    {
        Thud s, h;
        const auto soft = oneNote(s, 60, 9600, 9600, sr, vel127(60)), hard = oneNote(h, 60, 9600, 9600, sr, vel127(127));
        CHECK_MSG(db(peakOf(hard) / peakOf(soft)) > 6.0, "velocity 127 against 60: %+.1f dB", db(peakOf(hard) / peakOf(soft)));
        Thud t;
        Drive d(t, sr);
        NoteEvent e; e.type = NoteEvent::On; e.key = 60; e.id = 1; e.pan = 1.0f;
        d.ev(333, e);
        d.run(9600);
        CHECK(peakOf(d.L) == 0.0f && peakOf(d.R, 0, 333) == 0.0f && peakOf(d.R, 333, 345) > 0.0f);
    }
}
