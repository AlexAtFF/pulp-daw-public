// Low Four (electric bass): tuning in every style, what the four knobs do, styles, release and polyphony.
#include "BasslineTestUtil.h"
#include "pulp/LowFour.h"
#include <cstdlib>
using namespace pulp;
using namespace bt;

namespace {
bool report() { return std::getenv("BASSLINE_REPORT") != nullptr; }
// The key that sounds: Low Four plays two octaves down by default.
double soundingHz(const LowFour& b, int key) { return keyToHz(key + 12.0 * (double)b.pOctave->get()); }
}

TEST(lowfour_is_in_tune_in_every_style_at_every_rate) {
    double worst = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int style = 0; style < LowFour::NumStyles; ++style)
            for (int key : {43, 48, 60, 72, 84}) {
                LowFour b;
                b.pStyle->set((float)style);
                b.pSustain->set(style == LowFour::Muted ? 1.0f : 0.8f);
                const auto out = oneNote(b, key, (int64)(1.6 * sr), (int64)(1.6 * sr), sr);
                const double want = soundingHz(b, key);
                // the fretless scoop is over well before 0.3 s
                const double cents = centsOff(preciseHz(out, (size_t)(0.3 * sr), sr, want, sr > 50000.0 ? 65536 : 32768, 1024), want);
                worst = std::max(worst, std::fabs(cents));
                CHECK_MSG(std::fabs(cents) < 2.0, "rate %.0f style %d key %d (%.2f Hz): %.3f cents off", sr, style, key, want, cents);
            }
    if (report()) printf("    [lowfour] worst tuning error, 5 styles x keys 43 to 84 x 3 rates: %.4f cents\n", worst);
    // the octave switch, a note's fine pitch and the channel pitch
    LowFour b;
    CHECK(b.pOctave->get() == -2.0f);
    b.pOctave->set(0.0f);
    b.pSustain->set(0.9f);
    Drive d(b);
    d.bend = -1.0f;
    NoteEvent e; e.type = NoteEvent::On; e.key = 48; e.id = 1; e.fineCents = 50.0f;
    d.ev(0, e);
    d.run(72000);
    CHECK_NEAR(centsOff(preciseHz(d.L, 14400, 48000.0, keyToHz(47.5), 32768, 1024), keyToHz(47.5)), 0.0, 2.0);
}

TEST(lowfour_sounds_like_a_bass_straight_away) {
    // The default patch, the default key: the fundamental is at 65.4 Hz and is the strongest partial,
    // nearly all the power is below 1 kHz, the attack is brighter than the body, and it dies away.
    const double sr = 48000.0;
    LowFour b;
    const auto out = oneNote(b, 60, 72000, 96000, sr);
    const double f0 = keyToHz(36);
    const double pk = peakHz(out, 4800, 32768, sr, 20.0, 5000.0);
    CHECK_MSG(std::fabs(pk / f0 - 1.0) < 0.03, "strongest partial at %.1f Hz", pk);
    CHECK_MSG(bandShare(out, 4800, 32768, sr, 0.0, 1000.0) > 0.97, "share below 1 kHz: %.3f", bandShare(out, 4800, 32768, sr, 0.0, 1000.0));
    const double early = bandShare(lead(out, 1024), 0, 2048, sr, 400.0, 20000.0), later = bandShare(out, 24000, 2048, sr, 400.0, 20000.0);
    CHECK_MSG(early > 2.0 * later, "share of the power above 400 Hz: %.4f at the pluck, %.5f half a second in", early, later);
    CHECK(rmsOf(out, 60000, 72000) < 0.6 * rmsOf(out, 2400, 14400));
    CHECK(peakOf(out, 90000, 96000) == 0.0f && b.activeVoices() == 0);
    if (report()) printf("    [lowfour] default patch, key 60: strongest partial %.1f Hz, %.1f %% of the power below 1 kHz, %.2f %% above 400 Hz at the pluck and %.3f %% half a second in\n",
                         pk, 100.0 * bandShare(out, 4800, 32768, sr, 0.0, 1000.0), 100.0 * early, 100.0 * later);
}

TEST(lowfour_four_knobs_do_what_they_say) {
    const double sr = 48000.0, f0 = keyToHz(36);
    // Body: more of the bottom
    {
        LowFour thin, fat;
        thin.pBody->set(0.0f);
        fat.pBody->set(1.0f);
        const auto a = oneNote(thin, 60, 48000, 48000), b = oneNote(fat, 60, 48000, 48000);
        const double ra = ampAt(a, 4800, 16384, f0, sr) / ampAt(a, 4800, 16384, 6.0 * f0, sr), rb = ampAt(b, 4800, 16384, f0, sr) / ampAt(b, 4800, 16384, 6.0 * f0, sr);
        CHECK_MSG(db(rb / ra) > 8.0, "fundamental against the sixth partial: %+.1f dB from Body 0 to 1", db(rb / ra));
        CHECK_MSG(std::fabs(db(peakOf(b) / peakOf(a))) < 6.0, "peak moves %.1f dB", db(peakOf(b) / peakOf(a)));
    }
    // Bite: a harder, brighter attack
    {
        LowFour soft, hard;
        soft.pBite->set(0.0f);
        hard.pBite->set(1.0f);
        const auto a = oneNote(soft, 60, 48000, 48000), b = oneNote(hard, 60, 48000, 48000);
        const double sa = bandShare(lead(a, 2048), 0, 4096, sr, 400.0, 20000.0), sb = bandShare(lead(b, 2048), 0, 4096, sr, 400.0, 20000.0);
        CHECK_MSG(sb > 5.0 * sa, "share of the attack above 400 Hz: %.4f -> %.4f", sa, sb);
        CHECK_MSG(std::fabs(db(peakOf(b) / peakOf(a))) < 6.0, "peak moves %.1f dB", db(peakOf(b) / peakOf(a)));
    }
    // Sustain: 0.35 s at the bottom, 2.2 s in the middle, 14 s at the top (60 dB times of the fundamental at this key)
    {
        double got[3];
        const float set[3] = {0.0f, 0.5f, 1.0f};
        const double want[3] = {0.35, 0.35 * std::sqrt(40.0), 14.0};
        for (int i = 0; i < 3; ++i) {
            LowFour b;
            b.pSustain->set(set[i]);
            // played softly: the amplifier stage holds a loud note up a little, as an amplifier does
            const auto out = oneNote(b, 60, 96000, 96000, sr, vel127(25));
            got[i] = i == 0 ? t60Of(out, f0, sr, 2400, 9600, 4096) : t60Of(out, f0, sr, 9600, 57600, 16384);
            CHECK_MSG(std::fabs(got[i] / want[i] - 1.0) < 0.08, "sustain %.1f: %.2f s, expected %.2f s", set[i], got[i], want[i]);
        }
        if (report()) printf("    [lowfour] sustain 0 / 0.5 / 1: fundamental falls 60 dB in %.2f / %.2f / %.2f s\n", got[0], got[1], got[2]);
    }
    // Tone: the tone knob is a low-pass
    {
        LowFour dark, open;
        dark.pTone->set(0.1f);
        open.pTone->set(1.0f);
        dark.pBite->set(0.8f);
        open.pBite->set(0.8f);
        const auto a = oneNote(dark, 60, 24000, 24000), b = oneNote(open, 60, 24000, 24000);
        const double sa = bandShare(a, 0, 8192, sr, 1500.0, 20000.0), sb = bandShare(b, 0, 8192, sr, 1500.0, 20000.0);
        CHECK_MSG(sb > 20.0 * sa, "share above 1.5 kHz: %.5f at tone 0.1, %.5f at tone 1", sa, sb);
    }
    // a note's mod X opens the pluck; velocity is louder and brighter; pan pans
    {
        LowFour b;
        Drive lo(b), hi(b);
        NoteEvent e; e.type = NoteEvent::On; e.key = 60; e.id = 1; e.modX = 0.2f;
        lo.ev(0, e);
        lo.run(24000);
        e.modX = 0.9f;
        hi.ev(0, e);
        hi.run(24000);
        CHECK(bandShare(lead(hi.L, 2048), 0, 4096, sr, 400.0, 20000.0) > 3.0 * bandShare(lead(lo.L, 2048), 0, 4096, sr, 400.0, 20000.0));
        LowFour s, h;
        const auto soft = oneNote(s, 60, 24000, 24000, sr, vel127(50)), hard = oneNote(h, 60, 24000, 24000, sr, vel127(125));
        CHECK_MSG(db(rmsOf(hard) / rmsOf(soft)) > 6.0, "velocity 125 against 50: %+.1f dB", db(rmsOf(hard) / rmsOf(soft)));
        Drive p(b);
        e.modX = 0.5f; e.pan = -1.0f;
        p.ev(0, e);
        p.run(12032);
        CHECK(peakOf(p.R) == 0.0f && peakOf(p.L) > 0.05f);
    }
}

TEST(lowfour_styles_differ) {
    const double sr = 48000.0, f0 = keyToHz(36);
    double length[LowFour::NumStyles], attack[LowFour::NumStyles];
    for (int style = 0; style < LowFour::NumStyles; ++style) {
        LowFour b;
        b.pStyle->set((float)style);
        const auto out = oneNote(b, 60, 96000, 96000, sr);
        length[style] = lengthSeconds(out, sr, 30.0);
        attack[style] = centroidHz(lead(out, 2048), 0, 4096, sr);
        CHECK(peakOf(out) > 0.1f);
    }
    CHECK_MSG(length[LowFour::Muted] < 0.3 * length[LowFour::Finger], "muted %.2f s, finger %.2f s", length[LowFour::Muted], length[LowFour::Finger]);
    CHECK_MSG(attack[LowFour::Pick] > 1.3 * attack[LowFour::Finger] && attack[LowFour::Slap] > 1.3 * attack[LowFour::Pick],
              "attack centroid: finger %.0f, pick %.0f, slap %.0f Hz", attack[LowFour::Finger], attack[LowFour::Pick], attack[LowFour::Slap]);
    CHECK(attack[LowFour::Fretless] < attack[LowFour::Finger]);
    // fretless: the note starts flat and comes up to pitch
    LowFour b;
    b.pStyle->set((float)LowFour::Fretless);
    b.pOctave->set(0.0f);           // high enough to read the pitch over a few milliseconds
    const auto out = oneNote(b, 72, 48000, 48000, sr);
    const double early = centsOff(preciseHz(out, 240, sr, keyToHz(72) * 0.985, 1024, 256), keyToHz(72));
    const double late = centsOff(preciseHz(out, 24000, sr, keyToHz(72), 8192, 1024), keyToHz(72));
    CHECK_MSG(early < -8.0 && early > -45.0 && std::fabs(late) < 2.0, "fretless: %.1f cents at 5 to 30 ms, %.2f cents at half a second", early, late);
    if (report()) {
        printf("    [lowfour] attack centroid finger / pick / slap / muted / fretless: %.0f / %.0f / %.0f / %.0f / %.0f Hz\n", attack[0], attack[1], attack[2], attack[3], attack[4]);
        printf("    [lowfour] length to -30 dB, held 2 s: %.2f / %.2f / %.2f / %.2f / %.2f s; fretless scoop %.1f cents early\n", length[0], length[1], length[2], length[3], length[4], early);
    }
    (void)f0;
}

TEST(lowfour_release_polyphony_and_slides) {
    const double sr = 48000.0;
    // letting go stops the string quickly
    {
        LowFour b;
        b.pSustain->set(1.0f);
        const auto out = oneNote(b, 60, 24000, 48000);
        CHECK_MSG(db(rmsOf(out, 24000 + 4800, 24000 + 7200) / rmsOf(out, 21600, 24000)) < -40.0, "100 ms after letting go: %.1f dB",
                  db(rmsOf(out, 24000 + 4800, 24000 + 7200) / rmsOf(out, 21600, 24000)));
        CHECK(peakOf(out, 40000, 48000) == 0.0f);
    }
    // more notes than voices: the count holds and the newest still sound
    {
        LowFour b;
        b.pSustain->set(1.0f);
        b.pOctave->set(0.0f);
        Drive d(b, sr);
        for (int i = 0; i < 20; ++i) d.on(i * 960, 40 + i, (uint32)(i + 1));
        int most = 0;
        d.each = [&](int64) { most = std::max(most, b.activeVoices()); };
        d.run(48000);
        CHECK(allFinite(d.L));
        CHECK_MSG(most <= LowFour::kVoices && b.activeVoices() == LowFour::kSounding, "most %d, at the end %d", most, b.activeVoices());
        CHECK(ampAt(d.L, 36000, 8192, keyToHz(59), sr) > 0.01);
    }
    // slide notes and portamento
    {
        LowFour b;
        b.pSustain->set(1.0f);
        b.pOctave->set(0.0f);
        Drive d(b, sr);
        d.on(0, 60, 1);
        NoteEvent sl; sl.type = NoteEvent::SlideTo; sl.key = 65; sl.group = 0; sl.slideSamples = 4800.0f;
        d.ev(12000, sl);
        d.run(60032);
        CHECK_NEAR(centsOff(preciseHz(d.L, 4800, sr, keyToHz(60), 4096, 512), keyToHz(60)), 0.0, 2.0);
        CHECK_NEAR(centsOff(preciseHz(d.L, 24000, sr, keyToHz(65), 16384, 512), keyToHz(65)), 0.0, 2.0);
        LowFour p;
        p.pOctave->set(0.0f);
        Drive g(p, sr);
        NoteEvent e; e.type = NoteEvent::On; e.key = 55; e.id = 1; e.glideFromKey = 60.0f; e.slideSamples = 4800.0f;
        g.ev(0, e);
        g.run(48000);
        CHECK_NEAR(centsOff(preciseHz(g.L, 9600, sr, keyToHz(55), 16384, 512), keyToHz(55)), 0.0, 2.0);
    }
    // note-on is on its sample
    {
        LowFour b;
        Drive d(b, sr);
        d.on(901, 60, 1);
        d.run(4800);
        CHECK(peakOf(d.L, 0, 901) == 0.0f && peakOf(d.L, 901, 920) > 0.0f);
    }
}
