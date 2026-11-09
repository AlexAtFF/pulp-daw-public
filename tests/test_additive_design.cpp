// Sheaf: the spectrum design. Base shapes, tilt, odd / even, comb, the spectral filter, formants, morph,
// prism clones and damping, each measured partial by partial.
#include "SheafTestUtil.h"
using namespace pulp;
using namespace sheaft;

namespace {

// One held note and the levels of its partials (measured over a third of a second, a tenth of a second in).
struct Tone {
    std::vector<float> l, r;
    double f0 = 0.0, sr = 48000.0;
    double amp(double ratio, bool right = false) const { return ampFine(right ? r : l, 4800, 16384, f0 * ratio, sr); }
    double rel(double ratio, double to = 1.0) const { return db(amp(ratio) / amp(to)); }
};
Tone tone(Sheaf& s, int key, double sr = 48000.0, const std::function<void(NoteEvent&)>& tweak = {}) {
    Tone t;
    t.sr = sr;
    t.f0 = keyToHz(key);
    Drive d(s, sr);
    NoteEvent e;
    e.type = NoteEvent::On; e.key = key; e.id = 1; e.velocity = kDefaultVelocity;
    if (tweak) tweak(e);
    d.ev(0, e);
    d.run(24000);
    t.l = d.L;
    t.r = d.R;
    return t;
}
const double kAbsent = -110.0;      // dB below the reference partial: "not there"

} // namespace

TEST(sheaf_base_spectra_have_the_textbook_harmonics) {
    const int key = 45;         // 110 Hz: 64 harmonics fit below 7.1 kHz
    {   // saw: 1/n. Its level: the squares of all 256 levels add up to one, times volume 0.5 x 0.5
        auto s = plainSheaf(Sheaf::Saw);
        const Tone t = tone(*s, key);
        double worst = 0.0;
        for (int k = 2; k <= 64; ++k) worst = std::max(worst, std::fabs(t.rel(k) + db(k)));
        CHECK_MSG(worst < 0.02, "saw: a harmonic is %.3f dB from 1/n", worst);
        double sum = 0.0;
        for (int k = 1; k <= 256; ++k) sum += 1.0 / ((double)k * k);
        CHECK_NEAR(t.amp(1), 0.25 / std::sqrt(sum), 0.0002);
        printf("    saw 1/n: worst harmonic %.4f dB out (2..64)\n", worst);
    }
    {   // square: odd harmonics at 1/n, no even ones
        auto s = plainSheaf(Sheaf::Square);
        const Tone t = tone(*s, key);
        double worst = 0.0, even = -300.0;
        for (int k = 3; k <= 63; k += 2) worst = std::max(worst, std::fabs(t.rel(k) + db(k)));
        for (int k = 2; k <= 64; k += 2) even = std::max(even, t.rel(k));
        CHECK_MSG(worst < 0.02, "square: an odd harmonic is %.3f dB from 1/n", worst);
        CHECK_MSG(even < kAbsent, "square: an even harmonic at %.1f dB", even);
    }
    {   // triangle: odd harmonics at 1/n^2
        auto s = plainSheaf(Sheaf::Triangle);
        const Tone t = tone(*s, key);
        double worst = 0.0, even = -300.0;
        for (int k = 3; k <= 31; k += 2) worst = std::max(worst, std::fabs(t.rel(k) + 2.0 * db(k)));
        for (int k = 2; k <= 32; k += 2) even = std::max(even, t.rel(k));
        CHECK_MSG(worst < 0.02, "triangle: an odd harmonic is %.3f dB from 1/n^2", worst);
        CHECK_MSG(even < kAbsent, "triangle: an even harmonic at %.1f dB", even);
        // and it is a triangle, not just its spectrum: the wave runs in straight lines between +peak and -peak
        const double pk = peakOf(t.l, 4800, 24000);
        double sum = 0.0;
        for (int k = 1; k <= 255; k += 2) sum += 1.0 / ((double)k * k * k * k);
        CHECK_NEAR(pk, 0.25 / std::sqrt(sum) * (kPi * kPi / 8.0), 0.004);
    }
    for (float width : {0.25f, 0.1f, 0.5f}) {     // pulse: |sin(pi n d)| / n
        auto s = plainSheaf(Sheaf::Pulse);
        s->spec[0].colour->set(width);
        const Tone t = tone(*s, key);
        double worst = 0.0;
        for (int k = 2; k <= 48; ++k) {
            const double want = std::fabs(std::sin(kPi * k * width)) / k / std::fabs(std::sin(kPi * width));
            if (want < 1.0e-6) { CHECK_MSG(t.rel(k) < kAbsent, "pulse %.2f: harmonic %d should be missing, is at %.1f dB", width, k, t.rel(k)); continue; }
            worst = std::max(worst, std::fabs(t.rel(k) - db(want)));
        }
        CHECK_MSG(worst < 0.02, "pulse %.2f: a harmonic is %.3f dB out", width, worst);
        // it is a pulse wave: two levels. Away from the edges every sample sits on one of them.
        if (width == 0.25f) {
            std::vector<float> v(t.l.begin() + 9600, t.l.begin() + 19200);
            std::sort(v.begin(), v.end());
            const double lo = v[v.size() / 10], hi = v[v.size() * 95 / 100];
            int onLevel = 0;
            for (float x : v) if (std::fabs(x - lo) < 0.12 * (hi - lo) || std::fabs(x - hi) < 0.12 * (hi - lo)) ++onLevel;
            CHECK_MSG(onLevel > (int)v.size() * 85 / 100, "pulse: only %d of %zu samples are on its two levels", onLevel, v.size());
            CHECK(hi > 0.0 && lo < 0.0 && hi > -lo * 2.0);      // a quarter of the time up three times as far
        }
    }
    {   // organ: the drawbar harmonics and no others
        auto s = plainSheaf(Sheaf::Organ);
        s->spec[0].colour->set(1.0f);
        const Tone t = tone(*s, key);
        for (int k : {2, 3, 4, 5, 6, 8}) CHECK_MSG(std::fabs(t.rel(k)) < 0.02, "organ: harmonic %d at %.2f dB", k, t.rel(k));
        CHECK_NEAR(t.rel(10), db(0.7), 0.02);
        CHECK_NEAR(t.rel(12), db(0.6), 0.02);
        CHECK_NEAR(t.rel(16), db(0.5), 0.02);
        for (int k : {7, 9, 11, 13, 14, 15, 17, 20, 24, 32}) CHECK_MSG(t.rel(k) < kAbsent, "organ: harmonic %d at %.1f dB", k, t.rel(k));
        s->spec[0].colour->set(0.0f);
        const Tone one = tone(*s, key);
        for (int k = 2; k <= 16; ++k) CHECK(one.rel(k) < kAbsent);
        CHECK_NEAR(one.amp(1), 0.25, 0.0002);       // one partial holds all the level
        s->spec[0].colour->set(0.375f);             // half way between the second and third registrations
        const Tone mid = tone(*s, key);
        CHECK_NEAR(mid.rel(2), db(0.9), 0.02);
        CHECK_NEAR(mid.rel(3), db(0.45), 0.02);
        CHECK_NEAR(mid.rel(4), db(0.6), 0.02);
    }
    {   // bell: partials on the bell's own ratios (1, 0.5, 1.2, 1.5, 2 ...). The first is the played key at every colour.
        for (float colour : {0.0f, 1.0f, 0.4f}) {
            auto s = plainSheaf(Sheaf::Bell, 16);
            s->spec[0].colour->set(colour);
            const auto out = hold(*s, 69, 48000.0, 48000);
            std::vector<double> want;
            for (int k = 1; k <= 16; ++k) if (Sheaf::bellRatio(k, colour) * 440.0 < 0.98 * 24000.0) want.push_back(Sheaf::bellRatio(k, colour) * 440.0);
            const auto got = lineFrequencies(out, 6000, 48000.0, want);
            double worst = 0.0;
            for (size_t i = 0; i < want.size(); ++i) {
                if (want[i] > 0.9 * 24000.0 || (i == 1 && colour == 1.0f)) continue;      // (a bar has no hum: that partial is silent at colour 1)
                worst = std::max(worst, std::fabs(centsOff(got[i], want[i])));
            }
            CHECK_MSG(worst < 1.0, "bell colour %.1f: a partial is %.3f cents off", colour, worst);
            CHECK_MSG(strayDb(out, 6000, 32768, 48000.0, want) < -100.0, "bell colour %.1f: something that is not a partial", colour);
            CHECK_MSG(std::fabs(centsOff(got[0], 440.0)) < 0.01, "bell colour %.1f: the note itself is %.3f cents off", colour, centsOff(got[0], 440.0));
        }
        CHECK_NEAR(Sheaf::bellRatio(1, 0.0f), 1.0, 1e-12);
        CHECK_NEAR(Sheaf::bellRatio(1, 0.6f), 1.0, 1e-12);
        CHECK_NEAR(Sheaf::bellRatio(2, 0.3f), 0.5, 1e-12);
        CHECK_NEAR(Sheaf::bellRatio(3, 0.0f), 1.2, 1e-12);
        CHECK_NEAR(Sheaf::bellRatio(3, 1.0f), (2.5 / 1.5) * (2.5 / 1.5), 1e-12);
        CHECK_NEAR(Sheaf::bellRatio(4, 1.0f), (3.5 / 1.5) * (3.5 / 1.5), 1e-12);
        for (float colour : {0.0f, 0.5f, 1.0f}) {
            CHECK(Sheaf::bellRatio(3, colour) > 1.0);
            for (int k = 4; k <= 256; ++k) CHECK(Sheaf::bellRatio(k, colour) > Sheaf::bellRatio(k - 1, colour));
        }
        auto s = plainSheaf(Sheaf::Bell, 16);
        s->spec[0].colour->set(0.0f);
        const Tone t = tone(*s, 69);
        CHECK_NEAR(t.rel(0.5), db(0.45), 0.02);     // the hum, an octave under the note
        CHECK_NEAR(t.rel(1.2), db(0.7), 0.02);      // the minor third
        CHECK_NEAR(t.rel(2.0), db(0.85), 0.02);
        s->spec[0].colour->set(1.0f);               // a bar: no hum, and the second partial where a free bar's is
        const Tone bar = tone(*s, 69);
        CHECK(bar.rel(0.5) < kAbsent);
        CHECK_NEAR(bar.rel((2.5 / 1.5) * (2.5 / 1.5)), db(std::pow(2.0, -0.8)), 0.02);
    }
    {   // vowel: a saw through the mouth's resonances. "Ah" has them near 730 and 1090 Hz, "ee" near 270 and 2290 Hz.
        auto s = plainSheaf(Sheaf::Vowel);
        auto envelope = [&](float colour, int k) {      // level of harmonic k with the saw's 1/k taken out, at key 36
            s->spec[0].colour->set(colour);
            const Tone t = tone(*s, 36);
            return db(t.amp(k) * k);
        };
        const double f0 = keyToHz(36);                  // 65.4 Hz
        auto near = [&](double hz) { return (int)std::lround(hz / f0); };
        const double ahF1 = envelope(0.0f, near(730)), ahLow = envelope(0.0f, near(250)), ahF2 = envelope(0.0f, near(1090)), ahHigh = envelope(0.0f, near(1800));
        CHECK_MSG(ahF1 > ahLow + 8.0 && ahF2 > ahHigh + 8.0, "ah: %.1f dB at 730, %.1f at 250, %.1f at 1090, %.1f at 1800", ahF1, ahLow, ahF2, ahHigh);
        const double eeF1 = envelope(0.5f, near(270)), eeMid = envelope(0.5f, near(900)), eeF2 = envelope(0.5f, near(2290));
        CHECK_MSG(eeF1 > eeMid + 8.0 && eeF2 > eeMid + 8.0, "ee: %.1f dB at 270, %.1f at 900, %.1f at 2290", eeF1, eeMid, eeF2);
    }
}

TEST(sheaf_tilt_odd_even_balance_comb_and_partial_count) {
    const int key = 45;
    for (float tilt : {-6.0f, 3.0f, 12.0f}) {       // tilt: that many dB more for every octave up
        auto s = plainSheaf(Sheaf::Saw);
        s->spec[0].tilt->set(tilt);
        const Tone t = tone(*s, key);
        double worst = 0.0;
        for (int k = 2; k <= 64; ++k) worst = std::max(worst, std::fabs(t.rel(k) - (-db(k) + tilt * std::log2((double)k))));
        CHECK_MSG(worst < 0.03, "tilt %.0f: a harmonic is %.3f dB out", tilt, worst);
    }
    {   // odd / even: -1 leaves only the odd harmonics, +1 only the even ones; in between the other set is scaled
        auto s = plainSheaf(Sheaf::Saw);
        s->spec[0].oddEven->set(-0.5f);
        Tone t = tone(*s, key);
        for (int k = 2; k <= 20; ++k) CHECK_NEAR(t.rel(k), -db(k) + ((k & 1) ? 0.0 : db(0.5)), 0.02);
        s->spec[0].oddEven->set(-1.0f);
        t = tone(*s, key);
        for (int k = 2; k <= 20; ++k) { if (k & 1) CHECK_NEAR(t.rel(k), -db(k), 0.02); else CHECK(t.rel(k) < kAbsent); }
        s->spec[0].oddEven->set(0.75f);
        t = tone(*s, key);
        for (int k = 1; k <= 20; ++k) CHECK_NEAR(t.rel(k, 2), -db(k / 2.0) + ((k & 1) ? db(0.25) : 0.0), 0.02);
        s->spec[0].oddEven->set(1.0f);
        t = tone(*s, key);
        for (int k = 1; k <= 20; k += 2) CHECK(t.rel(k, 2) < kAbsent);
        CHECK_NEAR(t.rel(4, 2), -db(2), 0.02);
    }
    {   // comb: harmonics on multiples of the spacing stay, the ones between go down by the depth
        auto s = plainSheaf(Sheaf::Saw);
        s->spec[0].comb->set(4.0f);
        s->spec[0].combDepth->set(1.0f);
        Tone t = tone(*s, key);
        for (int k = 1; k <= 24; ++k) {
            const double g = 1.0 - (0.5 - 0.5 * std::cos(kTwoPi * k / 4.0));
            if (g < 1.0e-9) CHECK_MSG(t.rel(k, 4) < kAbsent, "comb: harmonic %d at %.1f dB", k, t.rel(k, 4));
            else CHECK_NEAR(t.rel(k, 4), db(g * 4.0 / k), 0.02);
        }
        for (float spacing : {2.5f, 7.0f})
            for (float depth : {0.5f, 0.9f}) {
                s->spec[0].comb->set(spacing);
                s->spec[0].combDepth->set(depth);
                t = tone(*s, key);
                auto g = [&](int k) { return 1.0 - depth * (0.5 - 0.5 * std::cos(kTwoPi * k / spacing)); };
                double worst = 0.0;
                for (int k = 2; k <= 32; ++k) worst = std::max(worst, std::fabs(t.rel(k) - db(g(k) / k / g(1))));
                CHECK_MSG(worst < 0.03, "comb spacing %.1f depth %.1f: a harmonic is %.3f dB out", spacing, depth, worst);
            }
        s->spec[0].comb->set(1.0f);     // a spacing of one harmonic keeps them all
        t = tone(*s, key);
        for (int k = 2; k <= 16; ++k) CHECK_NEAR(t.rel(k), -db(k), 0.02);
    }
    {   // the same controls on spectrum B, heard at morph 1
        auto s = plainSheaf(Sheaf::Square);
        s->pMorph->set(1.0f);
        s->spec[1].shape->set((float)Sheaf::Saw);
        s->spec[1].tilt->set(-3.0f);
        s->spec[1].oddEven->set(-0.5f);
        s->spec[1].comb->set(3.0f);
        s->spec[1].combDepth->set(0.6f);
        const Tone t = tone(*s, key);
        auto g = [&](int k) { return std::pow((double)k, -3.0 / 6.0206) / k * ((k & 1) ? 1.0 : 0.5) * (1.0 - 0.6 * (0.5 - 0.5 * std::cos(kTwoPi * k / 3.0))); };
        double worst = 0.0;
        for (int k = 2; k <= 32; ++k) worst = std::max(worst, std::fabs(t.rel(k) - db(g(k) / g(1))));
        CHECK_MSG(worst < 0.03, "spectrum B: a harmonic is %.3f dB out", worst);
    }
    {   // the number of partials
        auto s = plainSheaf(Sheaf::Saw, 10);
        const Tone t = tone(*s, key);
        CHECK_NEAR(t.rel(10), -db(10), 0.02);
        for (int k = 11; k <= 40; ++k) CHECK(t.rel(k) < kAbsent);
        double sum = 0.0;
        for (int k = 1; k <= 10; ++k) sum += 1.0 / ((double)k * k);
        CHECK_NEAR(t.amp(1), 0.25 / std::sqrt(sum), 0.0002);       // fewer partials, the same energy
    }
    {   // every shape, tilt, balance and comb setting carries the same energy
        for (int shape : {Sheaf::Saw, Sheaf::Square, Sheaf::Triangle, Sheaf::Pulse, Sheaf::Organ})
            for (float tilt : {0.0f, 9.0f}) {
                auto s = plainSheaf(shape, 32);
                s->spec[0].tilt->set(tilt);
                s->spec[0].comb->set(3.0f);
                s->spec[0].combDepth->set(0.5f);
                const auto out = hold(*s, key, 48000.0, 48000);
                CHECK_MSG(std::fabs(db(rmsOf(out, 4800, 4800 + 43636) / (0.25 / std::sqrt(2.0)))) < 0.05, "shape %d tilt %.0f: rms %.2f dB from the standard", shape, tilt, db(rmsOf(out, 4800, 4800 + 43636) / (0.25 / std::sqrt(2.0))));
            }
    }
}

TEST(sheaf_spectral_filter_scales_the_partials) {
    const int key = 45;                 // 110 Hz; cutoff 880 Hz is harmonic 8
    auto plain = plainSheaf(Sheaf::Saw);
    const Tone ref = tone(*plain, key);
    auto gain = [&](const Tone& t, int k) { return db(t.amp(k) / ref.amp(k)); };
    auto filtered = [&](int type, float cutoff, float slope, float reso) {
        auto s = plainSheaf(Sheaf::Saw);
        s->pFilterType->set((float)type);
        s->pCutoff->set(cutoff);
        s->pSlope->set(slope);
        s->pReso->set(reso);
        return s;
    };
    for (float slope : {12.0f, 24.0f, 48.0f}) {
        auto s = filtered(Sheaf::LowPass, 880.0f, slope, 0.0f);
        const Tone t = tone(*s, key);
        double worst = 0.0;
        for (int k = 1; k <= 8; ++k) worst = std::max(worst, std::fabs(gain(t, k)));
        for (int k = 9; k <= 40; ++k) if (slope * std::log2(k / 8.0) < 80.0) worst = std::max(worst, std::fabs(gain(t, k) + slope * std::log2(k / 8.0)));
        CHECK_MSG(worst < 0.05, "low pass %.0f dB per octave: a partial is %.3f dB from the line", slope, worst);
        CHECK_NEAR(gain(t, 16), -slope, 0.05);
    }
    {   // high pass and band pass
        auto s = filtered(Sheaf::HighPass, 880.0f, 24.0f, 0.0f);
        Tone t = tone(*s, key);
        double worst = 0.0;
        for (int k = 1; k <= 8; ++k) worst = std::max(worst, std::fabs(gain(t, k) + 24.0 * std::log2(8.0 / k)));
        for (int k = 9; k <= 40; ++k) worst = std::max(worst, std::fabs(gain(t, k)));
        CHECK_MSG(worst < 0.05, "high pass: a partial is %.3f dB from the line", worst);
        s = filtered(Sheaf::BandPass, 880.0f, 18.0f, 0.0f);
        t = tone(*s, key);
        worst = 0.0;
        for (int k = 1; k <= 40; ++k) worst = std::max(worst, std::fabs(gain(t, k) + 18.0 * std::fabs(std::log2(k / 8.0))));
        CHECK_MSG(worst < 0.05, "band pass: a partial is %.3f dB from the line", worst);
        s = filtered(Sheaf::FilterOff, 880.0f, 24.0f, 12.0f);
        t = tone(*s, key);
        for (int k = 1; k <= 40; ++k) CHECK(std::fabs(gain(t, k)) < 0.001);
    }
    {   // the resonance bump: its full height at the cutoff, a quarter of it (in dB) 0.2 octaves away
        auto s = filtered(Sheaf::LowPass, 880.0f, 24.0f, 12.0f);
        const Tone t = tone(*s, key);
        CHECK_NEAR(gain(t, 8), 12.0, 0.05);
        auto bell = [](double oct) { const double x = oct / 0.2, d = 1.0 + x * x; return 1.0 / (d * d); };
        for (int k : {5, 6, 7, 9, 10, 12}) {
            const double oct = std::log2(k / 8.0);
            CHECK_NEAR(gain(t, k), 12.0 * bell(oct) - 24.0 * std::max(0.0, oct), 0.15);
        }
        CHECK_NEAR(gain(t, 2), 0.0, 0.05);
    }
    {   // key tracking at 1: the cutoff moves with the note, so the same harmonics are cut on every key (60 is the pivot)
        auto s = filtered(Sheaf::LowPass, 880.0f, 24.0f, 0.0f);
        s->pFilterKey->set(1.0f);
        for (int k2 : {33, 45, 57, 69}) {
            const Tone t = tone(*s, k2), r = tone(*plain, k2);
            // cutoff = 880 * 2^((key - 60) / 12), which is harmonic 880 / 261.63 = 3.364 of every note
            const double want = -24.0 * std::log2(8.0 / (880.0 / keyToHz(60)));
            CHECK_MSG(std::fabs(db(t.amp(8) / r.amp(8)) - want) < 0.05, "key %d: harmonic 8 at %.2f dB, expected %.2f", k2, db(t.amp(8) / r.amp(8)), want);
        }
        s->pFilterKey->set(0.5f);
        const Tone t = tone(*s, 72), r = tone(*plain, 72);         // an octave above the pivot: the cutoff goes up half an octave
        CHECK_NEAR(db(t.amp(4) / r.amp(4)), -24.0 * std::log2(4.0 * keyToHz(72) / (880.0 * std::sqrt(2.0))), 0.05);
    }
    {   // envelope 2 opens it: amount 0.5 with the envelope held at 1 is four octaves
        auto s = filtered(Sheaf::LowPass, 220.0f, 24.0f, 0.0f);
        s->pFilterEnv->set(0.5f);
        s->pE2Attack->set(0.0f);
        s->pE2Sustain->set(1.0f);
        Tone t = tone(*s, key);
        CHECK_NEAR(gain(t, 32), 0.0, 0.05);             // 3520 Hz: just at the cutoff
        CHECK_NEAR(gain(t, 64), -24.0, 0.05);
        s->pFilterEnv->set(-0.125f);                    // one octave down: 110 Hz
        t = tone(*s, key);
        CHECK_NEAR(gain(t, 4), -48.0, 0.05);
    }
    {   // mod X: five octaves either way, 0.5 is home
        auto s = filtered(Sheaf::LowPass, 880.0f, 24.0f, 0.0f);
        Tone t = tone(*s, key, 48000.0, [](NoteEvent& e) { e.modX = 0.6f; });      // one octave up
        CHECK_NEAR(gain(t, 16), 0.0, 0.05);
        CHECK_NEAR(gain(t, 32), -24.0, 0.05);
        t = tone(*s, key, 48000.0, [](NoteEvent& e) { e.modX = 0.3f; });           // two octaves down: 220 Hz
        CHECK_NEAR(gain(t, 4), -24.0, 0.05);
        CHECK_NEAR(gain(t, 2), 0.0, 0.05);
    }
}

TEST(sheaf_fixed_formants_stay_at_their_frequencies_on_every_key) {
    auto plain = plainSheaf(Sheaf::Saw);
    auto bell = [](double oct, double width) { const double x = oct / width, d = 1.0 + x * x; return 1.0 / (d * d); };
    auto withFormants = [&](bool fixed) {
        auto s = plainSheaf(Sheaf::Saw);
        s->pFormFixed->set(fixed ? 1.0f : 0.0f);
        s->pFormFreq[0]->set(1000.0f); s->pFormWidth[0]->set(0.4f); s->pFormGain[0]->set(15.0f);
        s->pFormFreq[1]->set(3000.0f); s->pFormWidth[1]->set(0.25f); s->pFormGain[1]->set(-12.0f);
        return s;
    };
    double worstFixed = 0.0, worstMoving = 0.0;
    for (int key : {28, 40, 47, 57, 64}) {
        const double f0 = keyToHz(key);
        const Tone ref = tone(*plain, key);
        auto fixed = withFormants(true), moving = withFormants(false);
        const Tone a = tone(*fixed, key), b = tone(*moving, key);
        double peakHz = 0.0, peakGain = -100.0;
        for (int k = 1; k * f0 < 6000.0 && k <= 120; ++k) {
            const double hz = k * f0;
            // fixed: the curve is a function of frequency in Hz
            const double want = 15.0 * bell(std::log2(hz / 1000.0), 0.4) - 12.0 * bell(std::log2(hz / 3000.0), 0.25);
            const double got = db(a.amp(k) / ref.amp(k));
            worstFixed = std::max(worstFixed, std::fabs(got - want));
            if (got > peakGain) { peakGain = got; peakHz = hz; }
            // not fixed: the curve is tied to the harmonics, placed as it would be for key 60
            const double hzAt60 = k * keyToHz(60);
            const double wantMoving = 15.0 * bell(std::log2(hzAt60 / 1000.0), 0.4) - 12.0 * bell(std::log2(hzAt60 / 3000.0), 0.25);
            if (hzAt60 < 20000.0) worstMoving = std::max(worstMoving, std::fabs(db(b.amp(k) / ref.amp(k)) - wantMoving));
        }
        CHECK_MSG(std::fabs(peakHz - 1000.0) <= f0 * 0.5 + 1.0, "key %d: the first formant peaks at %.0f Hz", key, peakHz);
    }
    CHECK_MSG(worstFixed < 0.1, "fixed formants: a partial is %.3f dB from the curve", worstFixed);
    CHECK_MSG(worstMoving < 0.1, "moving formants: a partial is %.3f dB from the curve", worstMoving);
    printf("    two formants over five keys: every partial within %.3f dB of the curve (fixed), %.3f dB (moving with the pitch)\n", worstFixed, worstMoving);
    // the vowel shape obeys the same switch: fixed, "ah" keeps its 730 Hz peak whatever the key
    for (int key : {31, 43}) {
        auto s = plainSheaf(Sheaf::Vowel);
        s->spec[0].colour->set(0.0f);
        const Tone t = tone(*s, key);
        const double f0 = keyToHz(key);
        double best = -300.0, bestHz = 0.0;
        for (int k = 1; k * f0 < 950.0; ++k) { const double e = db(t.amp(k) * k); if (e > best) { best = e; bestHz = k * f0; } }
        CHECK_MSG(std::fabs(bestHz - 730.0) <= f0, "vowel on key %d: the first resonance is at %.0f Hz", key, bestHz);
    }
}

TEST(sheaf_morph_runs_from_spectrum_a_to_spectrum_b) {
    const int key = 45;
    auto make = [&](float morph) {
        auto s = plainSheaf(Sheaf::Saw);
        s->spec[1].shape->set((float)Sheaf::Square);
        s->pMorph->set(morph);
        return s;
    };
    auto a = make(0.0f), b = make(1.0f), h = make(0.5f), q = make(0.25f);
    const Tone ta = tone(*a, key), tb = tone(*b, key), th = tone(*h, key), tq = tone(*q, key);
    // 0 is A alone: whatever B is set to, the samples are the same
    auto other = make(0.0f);
    other->spec[1].shape->set((float)Sheaf::Organ);
    other->spec[1].tilt->set(7.0f);
    CHECK(tone(*other, key).l == ta.l);
    auto sawOnly = plainSheaf(Sheaf::Saw), squareOnly = plainSheaf(Sheaf::Square);
    CHECK(tone(*sawOnly, key).l == ta.l);
    // 1 is B alone
    const Tone sq = tone(*squareOnly, key);
    double worst = 0.0;
    for (int k = 1; k <= 63; k += 2) worst = std::max(worst, std::fabs(db(tb.amp(k) / sq.amp(k))));
    for (int k = 2; k <= 64; k += 2) CHECK(tb.rel(k) < kAbsent);
    CHECK_MSG(worst < 0.001, "morph 1: a partial is %.4f dB from spectrum B", worst);
    // between: each partial is that far along the line from its level in A to its level in B
    worst = 0.0;
    for (int k = 1; k <= 64; ++k) {
        const double la = ta.amp(k), lb = (k & 1) ? tb.amp(k) : 0.0;
        worst = std::max(worst, std::fabs(db(th.amp(k) / (0.5 * (la + lb)))));
        worst = std::max(worst, std::fabs(db(tq.amp(k) / (0.75 * la + 0.25 * lb))));
    }
    CHECK_MSG(worst < 0.01, "morph 0.5 / 0.25: a partial is %.4f dB from the line between A and B", worst);
    CHECK(th.amp(2) < ta.amp(2) * 0.51 && th.amp(2) > ta.amp(2) * 0.49);       // the even harmonics are half way gone

    // mod Y is the morph: 0.5 leaves it, 1 adds a whole morph, 0 takes one away
    const Tone up = tone(*a, key, 48000.0, [](NoteEvent& e) { e.modY = 1.0f; });
    const Tone part = tone(*a, key, 48000.0, [](NoteEvent& e) { e.modY = 0.75f; });
    const Tone down = tone(*b, key, 48000.0, [](NoteEvent& e) { e.modY = 0.0f; });
    CHECK(db(up.amp(3) / tb.amp(3)) < 0.001 && up.rel(2) < kAbsent);
    CHECK_NEAR(db(part.amp(2) / th.amp(2)), 0.0, 0.01);
    CHECK_NEAR(db(down.amp(2) / ta.amp(2)), 0.0, 0.001);

    // the frequencies morph too: from a saw's harmonics to a bell's partials, each along a straight line in pitch
    auto sb = plainSheaf(Sheaf::Saw, 12);
    sb->spec[1].shape->set((float)Sheaf::Bell);
    sb->spec[1].colour->set(0.0f);
    for (float m : {0.0f, 0.3f, 1.0f}) {
        sb->pMorph->set(m);
        const auto out = hold(*sb, 69, 48000.0, 48000);
        std::vector<double> want;
        for (int k = 1; k <= 12; ++k) want.push_back(440.0 * std::pow((double)k, 1.0 - m) * std::pow(Sheaf::bellRatio(k, 0.0f), (double)m));
        const auto got = lineFrequencies(out, 6000, 48000.0, want);
        double off = 0.0;
        for (size_t i = 0; i < want.size(); ++i) off = std::max(off, std::fabs(centsOff(got[i], want[i])));
        CHECK_MSG(off < 1.0, "saw to bell at morph %.1f: a partial is %.3f cents off", m, off);
    }
}

TEST(sheaf_prism_clones_detune_and_spread_the_partials) {
    CHECK(Sheaf::partialsPerClone(1) == 256 && Sheaf::partialsPerClone(2) == 128 && Sheaf::partialsPerClone(3) == 84 && Sheaf::partialsPerClone(4) == 64);
    CHECK(Sheaf::partialsPerClone(5) == 48 && Sheaf::partialsPerClone(6) == 40 && Sheaf::partialsPerClone(7) == 36 && Sheaf::partialsPerClone(8) == 32);
    const double sr = 48000.0, f0 = keyToHz(69);
    auto plain = plainSheaf(Sheaf::Saw, 24);
    const auto mono = hold(*plain, 69, sr, 96000);
    {   // two clones 20 cents either side, spread fully: the flat one on the left, the sharp one on the right
        auto s = plainSheaf(Sheaf::Saw, 24);
        s->pPrism->set(2.0f);
        s->pPrismDetune->set(20.0f);
        s->pPrismSpread->set(1.0f);
        std::vector<float> r;
        const auto l = hold(*s, 69, sr, 96000, &r);
        const double flat = std::pow(2.0, -20.0 / 1200.0), sharp = std::pow(2.0, 20.0 / 1200.0);
        std::vector<double> wl, wr;
        for (int k = 1; k <= 24; ++k) { wl.push_back(f0 * k * flat); wr.push_back(f0 * k * sharp); }
        const auto gl = lineFrequencies(l, 8000, sr, wl), gr = lineFrequencies(r, 8000, sr, wr);
        double worst = 0.0;
        for (size_t i = 0; i < wl.size(); ++i) worst = std::max({worst, std::fabs(centsOff(gl[i], wl[i])), std::fabs(centsOff(gr[i], wr[i]))});
        CHECK_MSG(worst < 0.05, "two clones: a partial is %.4f cents from its detuned place", worst);
        // each side carries one clone at the level of the plain sound; nothing of the other
        CHECK_NEAR(db(ampAt(l, 8000, 65536, f0 * flat, sr) / ampAt(mono, 8000, 65536, f0, sr)), 0.0, 0.02);
        CHECK(db(ampAt(l, 8000, 65536, f0 * sharp, sr) / ampAt(l, 8000, 65536, f0 * flat, sr)) < -80.0);
        CHECK(db(ampAt(r, 8000, 65536, f0 * flat, sr) / ampAt(r, 8000, 65536, f0 * sharp, sr)) < -80.0);
        CHECK_MSG(strayDb(l, 8000, 65536, sr, wl) < -100.0 && strayDb(r, 8000, 65536, sr, wr) < -100.0, "clones: stray %.1f / %.1f dB", strayDb(l, 8000, 65536, sr, wl), strayDb(r, 8000, 65536, sr, wr));
        // no spread: both in the middle, beating, with the energy of the plain sound
        s->pPrismSpread->set(0.0f);
        std::vector<float> r0;
        const auto l0 = hold(*s, 69, sr, 96000, &r0);
        CHECK(l0 == r0);
        CHECK_NEAR(db(rmsOf(l0, 8000, 96000) / rmsOf(mono, 8000, 96000)), 0.0, 0.3);
        CHECK_NEAR(db(ampAt(l0, 8000, 65536, f0 * flat, sr) / ampAt(mono, 8000, 65536, f0, sr)), db(std::sqrt(0.5)), 0.02);
    }
    {   // more clones: spaced evenly from -detune to +detune, sharing the partials out
        auto s = plainSheaf(Sheaf::Saw, 256);
        s->pPrism->set(5.0f);
        s->pPrismDetune->set(30.0f);
        s->pPrismSpread->set(0.0f);
        const auto l = hold(*s, 69, sr, 96000);
        for (int c = 0; c < 5; ++c) {
            const double hz = f0 * std::pow(2.0, (-30.0 + 15.0 * c) / 1200.0);
            CHECK_NEAR(db(ampAt(l, 8000, 65536, hz, sr) / ampAt(mono, 8000, 65536, f0, sr)), db(std::sqrt(0.2)), 0.3);
        }
        // 48 partials per clone at five clones: nothing above the 48th (21.1 kHz would be below the limit otherwise)
        auto low = plainSheaf(Sheaf::Saw, 256);
        flatten(*low);
        low->pPrism->set(5.0f);
        low->pPrismDetune->set(0.0f);
        const auto ll = hold(*low, 45, sr, 48000);
        CHECK(db(ampAt(ll, 8000, 16384, 110.0 * 49, sr) / ampAt(ll, 8000, 16384, 110.0 * 48, sr)) < -100.0);
        CHECK(db(ampAt(ll, 8000, 16384, 110.0 * 48, sr) / ampAt(ll, 8000, 16384, 110.0, sr)) > -20.0);      // (five clones in random phase: its level is what they add up to)
    }
    {   // blur: every partial of every clone gets its own detune inside the range, so the clones stop being copies
        auto s = plainSheaf(Sheaf::Saw, 16);
        s->pPrism->set(2.0f);
        s->pPrismDetune->set(25.0f);
        s->pPrismBlur->set(1.0f);
        s->pPrismSpread->set(1.0f);
        const auto l = hold(*s, 69, sr, 96000);
        std::vector<double> guess;
        for (int k = 1; k <= 16; ++k) guess.push_back(f0 * k);
        const auto got = lineFrequencies(l, 8000, sr, guess, 32768, 1024);
        double lo = 1e9, hi = -1e9;
        for (size_t i = 0; i < guess.size(); ++i) { const double c = centsOff(got[i], guess[i]); lo = std::min(lo, c); hi = std::max(hi, c); }
        CHECK_MSG(lo >= -25.01 && hi <= 25.01 && hi - lo > 15.0, "blurred partials sit between %.1f and %.1f cents", lo, hi);
    }
    {   // one clone is exactly the plain sound, whatever the other prism controls say
        auto s = plainSheaf(Sheaf::Saw, 24);
        s->pPrismDetune->set(50.0f);
        s->pPrismBlur->set(1.0f);
        s->pPrismSpread->set(1.0f);
        CHECK(hold(*s, 69, sr, 96000) == mono);
    }
}

TEST(sheaf_damping_makes_the_high_partials_die_first) {
    const double sr = 48000.0, f0 = keyToHz(57);
    auto s = plainSheaf(Sheaf::Saw, 16);
    s->pDamp->set(12.0f);           // each partial falls 12 dB a second for every harmonic it is above the fundamental
    const auto out = hold(*s, 57, sr, 48000);
    auto at = [&](int k, double seconds) { return ampAt(out, (size_t)(seconds * sr) - 2048, 4096, f0 * k, sr); };
    for (int k : {1, 2, 3, 5, 8}) {
        const double fell = db(at(k, 0.2) / at(k, 0.7));
        CHECK_MSG(std::fabs(fell - 12.0 * (k - 1) * 0.5) < 0.2, "partial %d fell %.2f dB in half a second", k, fell);
    }
    // the bell's hum, below the note, is not damped (and does not grow)
    auto b = plainSheaf(Sheaf::Bell, 8);
    b->spec[0].colour->set(0.0f);
    b->pDamp->set(12.0f);
    const auto bo = hold(*b, 69, sr, 48000);
    CHECK_NEAR(db(ampAt(bo, 9600 - 2048, 4096, 220.0, sr) / ampAt(bo, 33600 - 2048, 4096, 220.0, sr)), 0.0, 0.05);
    // stretch and damping together are a struck string: partials sharp of the harmonics, the top going first
    auto p = plainSheaf(Sheaf::Saw, 32);
    p->pInharm->set(0.4f);
    p->pDamp->set(6.0f);
    const auto po = hold(*p, 45, sr, 48000);
    const double r10 = Sheaf::stretched(10.0, Sheaf::stiffness(0.4f));
    CHECK(r10 > 10.5);
    const double fell = db(ampAt(po, 9600 - 2048, 4096, 110.0 * r10, sr) / ampAt(po, 33600 - 2048, 4096, 110.0 * r10, sr));
    CHECK_MSG(std::fabs(fell - 6.0 * (r10 - 1.0) * 0.5) < 0.2, "the stretched tenth partial fell %.2f dB", fell);
}
