// Twine (plucked string): tuning across the keyboard and sample rates, decay time across keys,
// damping, pick position, stiffness, body, spread, release and polyphony.
#include "BasslineTestUtil.h"
#include "pulp/Twine.h"
#include <cstdlib>
using namespace pulp;
using namespace bt;

namespace {
bool report() { return std::getenv("BASSLINE_REPORT") != nullptr; }

// A bare string for measuring: long, hardly damped, no body, in the middle.
void bare(Twine& t) {
    t.pDecay->set(12.0f);
    t.pDamping->set(0.1f);
    t.pBody->set(0.0f);
    t.pSpread->set(0.0f);
    t.pNoise->set(0.0f);
    t.pRing->set(1.0f);
}
double fundamentalCents(Twine& t, int key, double sr, double seconds = 1.2) {
    const auto out = oneNote(t, key, (int64)(seconds * sr), (int64)(seconds * sr), sr);
    const double want = keyToHz(key + (double)t.pTune->get());
    return centsOff(preciseHz(out, (size_t)(0.1 * sr), sr, want, 32768, 1024), want);
}
}

TEST(twine_every_key_is_in_tune_at_every_rate) {
    double worst = 0.0, worstTop = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int key : {24, 31, 36, 43, 48, 55, 60, 67, 72, 79, 84, 91, 96, 103, 108}) {
            Twine t;
            bare(t);
            const double cents = fundamentalCents(t, key, sr);
            if (report() && std::fabs(cents) > 0.02) printf("    [twine] rate %.0f key %d: %.4f cents\n", sr, key, cents);
            worst = std::max(worst, std::fabs(cents));
            if (key >= 96) worstTop = std::max(worstTop, std::fabs(cents));
            CHECK_MSG(std::fabs(cents) < 1.0, "rate %.0f key %d: %.3f cents off", sr, key, cents);
        }
    if (report()) printf("    [twine] worst tuning error, keys 24 to 108 at 44.1 / 48 / 96 kHz: %.4f cents (%.4f from key 96 up)\n", worst, worstTop);
    // the loss filter, the stiffness and a palm mute all sit in the loop: none of them may move the pitch
    double worstSet = 0.0;
    for (float damping : {0.0f, 0.5f, 0.9f})
        for (float stiff : {0.0f, 0.5f, 1.0f})
            for (int key : {36, 60, 84, 108}) {
                Twine t;
                bare(t);
                t.pDamping->set(damping);
                t.pStiff->set(stiff);
                const double cents = fundamentalCents(t, key, 44100.0, 0.9);
                worstSet = std::max(worstSet, std::fabs(cents));
                CHECK_MSG(std::fabs(cents) < 3.0, "damping %.1f stiffness %.1f key %d: %.3f cents off", damping, stiff, key, cents);
            }
    if (report()) printf("    [twine] worst tuning error across damping 0 / 0.5 / 0.9 and stiffness 0 / 0.5 / 1 at 44.1 kHz: %.4f cents\n", worstSet);
    // tune, fine, a note's fine pitch and the channel pitch
    {
        Twine t;
        bare(t);
        t.pTune->set(-12.0f);
        t.pFine->set(30.0f);
        Drive d(t);
        d.bend = 1.0f;
        NoteEvent e; e.type = NoteEvent::On; e.key = 60; e.id = 1; e.fineCents = 20.0f;
        d.ev(0, e);
        d.run(60032);
        const double want = keyToHz(49.5);
        CHECK_NEAR(centsOff(preciseHz(d.L, 4800, 48000.0, want, 32768, 1024), want), 0.0, 1.0);
    }
}

TEST(twine_decay_time_is_the_same_on_every_key) {
    double worst = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int key : {36, 48, 60, 72, 84, 96, 103, 108}) {
            Twine t;
            bare(t);
            t.pDecay->set(2.0f);
            t.pDamping->set(0.4f);
            const auto out = oneNote(t, key, (int64)(1.5 * sr), (int64)(1.5 * sr), sr);
            const int win = sr > 50000.0 ? 16384 : 8192;
            const double t60 = t60Of(out, keyToHz(key), sr, (size_t)(0.2 * sr), (size_t)(1.0 * sr), win);
            worst = std::max(worst, std::fabs(t60 / 2.0 - 1.0));
            CHECK_MSG(std::fabs(t60 / 2.0 - 1.0) < 0.02, "rate %.0f key %d: the fundamental takes %.3f s to fall 60 dB, 2 s was asked for", sr, key, t60);
        }
    if (report()) printf("    [twine] decay 2 s: fundamental's 60 dB time within %.2f %% on keys 36 to 108 at 44.1 / 48 / 96 kHz\n", 100.0 * worst);
    // How long a string can be made to ring at the very top: the interpolator in the loop loses a little
    // of a very high fundamental, more or less of it depending on where between samples the period falls,
    // and that sets a ceiling on the decay time. It must be comfortably above ordinary settings up to C8.
    {
        double shortest[2] = {1.0e9, 1.0e9};
        int at[2] = {0, 0};
        const double rates[2] = {44100.0, 48000.0};
        for (int ri = 0; ri < 2; ++ri)
            for (int key = 84; key <= 108; ++key) {
                bl::StringMemory mem;
                mem.prepare(rates[ri], 1);
                bl::StringLoop loop;
                loop.attach(mem.line(0), mem.size);
                bl::StringSetting s;
                s.hz = keyToHz(key);
                s.t60 = 1000.0;
                s.highLoss = 0.0;
                loop.retune(s, rates[ri]);
                const double w0 = kTwoPi / loop.period;
                double re = 0.0, im = 0.0;
                for (int k = 0; k < 6; ++k) { re += loop.h[k] * std::cos(w0 * k); im -= loop.h[k] * std::sin(w0 * k); }
                const double round = (double)loop.gain * std::hypot(re, im) * (1.0 - loop.pole) / std::sqrt(1.0 - 2.0 * loop.pole * std::cos(w0) + loop.pole * loop.pole);
                const double longest = -bl::kLn1000 / (s.hz * std::log(round));
                if (longest < shortest[ri]) { shortest[ri] = longest; at[ri] = key; }
            }
        CHECK_MSG(shortest[0] > 5.0 && shortest[1] > 5.0, "longest possible decay from key 84 to 108: %.1f s at 44.1 kHz, %.1f s at 48 kHz", shortest[0], shortest[1]);
        if (report()) printf("    [twine] longest decay a string can have, worst key from 84 to 108: %.1f s at 44.1 kHz (key %d), %.1f s at 48 kHz (key %d)\n", shortest[0], at[0], shortest[1], at[1]);
    }
    // other settings of the control
    for (float decay : {0.5f, 6.0f}) {
        Twine t;
        bare(t);
        t.pDecay->set(decay);
        const auto out = oneNote(t, 57, 48000, 48000);
        const double t60 = t60Of(out, keyToHz(57), 48000.0, 4800, 24000, 8192);
        CHECK_MSG(std::fabs(t60 / decay - 1.0) < 0.05, "decay %.1f s measured %.3f s", decay, t60);
    }
    // decay key follow at full: an octave up lasts half as long
    {
        double t60[3];
        int i = 0;
        for (int key : {48, 60, 72}) {
            Twine t;
            bare(t);
            t.pDecay->set(2.0f);
            t.pDecayTrack->set(1.0f);
            const auto out = oneNote(t, key, 72000, 72000);
            t60[i++] = t60Of(out, keyToHz(key), 48000.0, 4800, 28800, 8192);
        }
        CHECK_MSG(std::fabs(t60[0] / 4.0 - 1.0) < 0.05 && std::fabs(t60[1] / 2.0 - 1.0) < 0.05 && std::fabs(t60[2] / 1.0 - 1.0) < 0.05,
                  "%.3f, %.3f, %.3f s", t60[0], t60[1], t60[2]);
    }
}

TEST(twine_damping_takes_the_top_first) {
    // The partial nearest 2 kHz of a low string: its extra decay rate is what the damping control sets,
    // and it is the same on every key.
    const double sr = 48000.0;
    double rate[2][2];
    int ki = 0;
    for (int key : {33, 45}) {
        int di = 0;
        for (float damping : {0.3f, 0.6f}) {
            Twine t;
            bare(t);
            t.pDecay->set(8.0f);
            t.pDamping->set(damping);
            t.pBright->set(1.0f);
            const auto out = oneNote(t, key, 48000, 48000, sr);
            const double f0 = keyToHz(key), hz = std::round(2000.0 / f0) * f0;
            const double fund = t60Of(out, f0, sr, 2400, 14400, 8192), top = t60Of(out, hz, sr, 2400, 14400, 8192);
            rate[ki][di] = bl::kLn1000 / top - bl::kLn1000 / fund;
            const double want = bl::kLn1000 / (40.0 * std::pow(0.03 / 40.0, (double)damping)) * std::pow(hz / 2000.0, 2.0);
            CHECK_MSG(top < fund, "key %d damping %.1f: the partial at %.0f Hz lasts %.2f s, the fundamental %.2f s", key, damping, hz, top, fund);
            CHECK_MSG(std::fabs(rate[ki][di] / want - 1.0) < 0.2, "key %d damping %.1f: extra decay rate at %.0f Hz is %.2f /s, %.2f /s was asked for", key, damping, hz, rate[ki][di], want);
            ++di;
        }
        ++ki;
    }
    CHECK(rate[0][1] > rate[0][0] * 5.0);
    if (report()) printf("    [twine] extra decay rate near 2 kHz: key 33 %.2f and %.2f /s, key 45 %.2f and %.2f /s (damping 0.3 and 0.6)\n",
                         rate[0][0], rate[0][1], rate[1][0], rate[1][1]);
}

TEST(twine_pick_position_brightness_and_velocity_shape_the_pluck) {
    const double sr = 48000.0, f0 = keyToHz(45);
    auto partial = [&](const std::vector<float>& d, int h) { return ampAt(d, 480, 8192, h * f0, sr); };
    // plucked in the middle there are no even partials; at a quarter the fourth is missing
    {
        Twine mid, quarter, near;
        bare(mid); bare(quarter); bare(near);
        mid.pPosition->set(0.5f);
        quarter.pPosition->set(0.25f);
        near.pPosition->set(0.1f);
        const auto a = oneNote(mid, 45, 24000, 24000), b = oneNote(quarter, 45, 24000, 24000), c = oneNote(near, 45, 24000, 24000);
        CHECK_MSG(partial(a, 2) < 0.03 * partial(a, 1) && partial(a, 3) > 0.15 * partial(a, 1), "middle: partial 2 at %.4f, 3 at %.4f of the first", partial(a, 2) / partial(a, 1), partial(a, 3) / partial(a, 1));
        CHECK_MSG(partial(b, 4) < 0.03 * partial(b, 1) && partial(b, 2) > 0.3 * partial(b, 1), "quarter: partial 4 at %.4f, 2 at %.4f of the first", partial(b, 4) / partial(b, 1), partial(b, 2) / partial(b, 1));
        CHECK(partial(c, 2) > 0.6 * partial(c, 1) && partial(c, 4) > 0.3 * partial(c, 1));          // near the bridge: thin and bright
    }
    // brightness opens the pluck; a harder hit is louder and brighter
    {
        Twine dull, bright;
        bare(dull); bare(bright);
        dull.pBright->set(0.2f);
        bright.pBright->set(0.9f);
        const double cd = centroidHz(oneNote(dull, 45, 24000, 24000), 0, 8192, sr), cb = centroidHz(oneNote(bright, 45, 24000, 24000), 0, 8192, sr);
        CHECK_MSG(cb > cd * 1.8, "centroid %.0f Hz dull, %.0f Hz bright", cd, cb);
        Twine s, h;
        bare(s); bare(h);
        const auto soft = oneNote(s, 45, 24000, 24000, sr, vel127(50)), hard = oneNote(h, 45, 24000, 24000, sr, vel127(120));
        CHECK_MSG(db(rmsOf(hard) / rmsOf(soft)) > 6.0, "velocity 120 against 50: %+.1f dB", db(rmsOf(hard) / rmsOf(soft)));
        CHECK_MSG(centroidHz(hard, 0, 8192, sr) > centroidHz(soft, 0, 8192, sr) * 1.1, "centroid %.0f Hz soft, %.0f Hz hard", centroidHz(soft, 0, 8192, sr), centroidHz(hard, 0, 8192, sr));
    }
    // a note's mod X opens the pluck as well
    {
        double c[2];
        for (int i = 0; i < 2; ++i) {
            Twine t;
            bare(t);
            Drive d(t);
            NoteEvent e; e.type = NoteEvent::On; e.key = 45; e.id = 1; e.modX = i ? 0.9f : 0.2f;
            d.ev(0, e);
            d.run(24000);
            c[i] = centroidHz(d.L, 0, 8192, sr);
        }
        CHECK_MSG(c[1] > c[0] * 1.3, "centroid %.0f -> %.0f Hz", c[0], c[1]);
    }
}

TEST(twine_stiffness_stretches_the_partials) {
    const double sr = 48000.0, f0 = keyToHz(45);
    double sharp[3];
    int i = 0;
    for (float stiff : {0.0f, 0.5f, 1.0f}) {
        Twine t;
        bare(t);
        t.pStiff->set(stiff);
        t.pPosition->set(0.13f);
        const auto out = oneNote(t, 45, 48000, 48000, sr);
        // look for the fourth partial in a band around where a perfect string has it
        const double p4 = peakHz(out, 2400, 32768, sr, 3.6 * f0, 7.0 * f0);
        const double exact = preciseHz(out, 2400, sr, p4, 16384, 1024);
        sharp[i++] = centsOff(exact, 4.0 * f0);
        CHECK_NEAR(centsOff(preciseHz(out, 2400, sr, f0, 32768, 1024), f0), 0.0, 1.0);        // the first partial stays put
    }
    CHECK_MSG(std::fabs(sharp[0]) < 3.0, "a string without stiffness has its fourth partial %.2f cents out", sharp[0]);
    CHECK_MSG(sharp[1] > 5.0 && sharp[2] > 100.0, "fourth partial: %.1f cents sharp at 0.5, %.1f at 1", sharp[1], sharp[2]);
    if (report()) printf("    [twine] fourth partial of key 45: %+.1f cents at stiffness 0, %+.1f at 0.5, %+.1f at 1\n", sharp[0], sharp[1], sharp[2]);
}

TEST(twine_body_spread_release_and_mute) {
    const double sr = 48000.0;
    // the body lifts what falls on its resonances (the lowest is at 105 Hz at the default size)
    {
        Twine dry, wet;
        bare(dry); bare(wet);
        wet.pBody->set(1.0f);
        const int key = 44;         // 103.8 Hz
        const auto a = oneNote(dry, key, 24000, 24000), b = oneNote(wet, key, 24000, 24000);
        const double lift = db(ampAt(b, 2400, 16384, keyToHz(key), sr) / ampAt(a, 2400, 16384, keyToHz(key), sr));
        const double high = db(ampAt(b, 2400, 16384, 20 * keyToHz(key), sr) / ampAt(a, 2400, 16384, 20 * keyToHz(key), sr));
        CHECK_MSG(lift > 5.0 && std::fabs(high) < 1.5, "body at full: %+.1f dB at 104 Hz, %+.1f dB at 2 kHz", lift, high);
        // a bigger body puts the resonance lower
        Twine big;
        bare(big);
        big.pBody->set(1.0f);
        big.pBodySize->set(1.0f);
        const auto c = oneNote(big, 32, 24000, 24000), c0 = oneNote(dry, 32, 24000, 24000);      // 51.9 Hz, where the big body's air mode is
        CHECK(db(ampAt(c, 2400, 16384, keyToHz(32), sr) / ampAt(c0, 2400, 16384, keyToHz(32), sr)) > 5.0);
    }
    // spread: without it every string is in the middle; with it they sit in different places
    {
        Twine t;
        bare(t);
        std::vector<float> r;
        const auto l = oneNote(t, 60, 12000, 12000, sr, kDefaultVelocity, &r);
        CHECK(l == r);
        Twine w;
        bare(w);
        w.pSpread->set(1.0f);
        double lo = 1.0e9, hi = -1.0e9;
        for (int key = 48; key < 72; ++key) {
            std::vector<float> rr;
            const auto ll = oneNote(w, key, 6000, 6000, sr, kDefaultVelocity, &rr);
            const double bal = db(rmsOf(rr) / rmsOf(ll));
            lo = std::min(lo, bal);
            hi = std::max(hi, bal);
        }
        CHECK_MSG(lo < -6.0 && hi > 6.0, "left / right balance runs from %.1f to %.1f dB across two octaves", lo, hi);
        // a note's own pan moves it further
        Drive d(t);
        NoteEvent e; e.type = NoteEvent::On; e.key = 60; e.id = 1; e.pan = 1.0f;
        d.ev(0, e);
        d.run(12032);
        CHECK(peakOf(d.L) == 0.0f && peakOf(d.R) > 0.05f);
    }
    // letting go stops the string in about the release time; with "let ring" it does not
    {
        Twine t;
        bare(t);
        t.pRing->set(0.0f);
        t.pRelease->set(100.0f);
        const auto out = oneNote(t, 57, 12000, 48000);
        const double held = rmsOf(out, 9600, 12000), after = rmsOf(out, 12000 + 4800, 12000 + 7200);
        const double t60 = t60Of(out, keyToHz(57), sr, 12000 + 480, 12000 + 2880, 2048);
        CHECK_MSG(db(after / held) < -50.0, "100 to 150 ms after letting go it is at %.1f dB", db(after / held));
        CHECK_MSG(std::fabs(t60 / 0.1 - 1.0) < 0.15, "release 100 ms: the fundamental falls 60 dB in %.3f s", t60);
        CHECK(peakOf(out, 40000, 48000) == 0.0f && t.activeVoices() == 0);
        Twine ring;
        bare(ring);
        const auto on = oneNote(ring, 57, 12000, 48000);
        CHECK(db(rmsOf(on, 16800, 19200) / rmsOf(on, 9600, 12000)) > -3.0);
        // a note's release value scales it
        Twine q;
        bare(q);
        q.pRing->set(0.0f);
        q.pRelease->set(100.0f);
        Drive d(q);
        NoteEvent e; e.type = NoteEvent::On; e.key = 57; e.id = 1; e.release = 1.0f;
        d.ev(0, e);
        d.off(12000, 1);
        d.run(48000);
        CHECK_NEAR(t60Of(d.L, keyToHz(57), sr, 12000 + 480, 12000 + 4800, 2048), 0.175, 0.03);
    }
    // palm mute: short and dull
    {
        Twine open, muted;
        bare(open); bare(muted);
        open.pDecay->set(4.0f);
        muted.pDecay->set(4.0f);
        muted.pMute->set(0.7f);
        const auto a = oneNote(open, 45, 48000, 48000), b = oneNote(muted, 45, 48000, 48000);
        CHECK_MSG(lengthSeconds(b, sr) < 0.5 && lengthSeconds(a, sr) > 0.9, "muted %.2f s, open %.2f s", lengthSeconds(b, sr), lengthSeconds(a, sr));
        CHECK_MSG(bandShare(b, 0, 4096, sr, 1000.0, 20000.0) < 0.5 * bandShare(a, 0, 4096, sr, 1000.0, 20000.0), "share of the power above 1 kHz: %.4f muted, %.4f open",
                  bandShare(b, 0, 4096, sr, 1000.0, 20000.0), bandShare(a, 0, 4096, sr, 1000.0, 20000.0));
    }
}

TEST(twine_polyphony_stealing_and_slides) {
    const double sr = 48000.0;
    // 60 notes held at once: never more than the voice count, the newest all sound, nothing breaks
    {
        Twine t;
        bare(t);
        Drive d(t, sr);
        for (int i = 0; i < 60; ++i) d.on(i * 480, 36 + i, (uint32)(i + 1));
        int most = 0;
        d.each = [&](int64) { most = std::max(most, t.activeVoices()); };
        d.run(48000);
        CHECK(allFinite(d.L));
        CHECK_MSG(most <= Twine::kVoices && most >= Twine::kSounding, "most voices at once: %d", most);
        // after the fades, exactly the limit is left
        CHECK_MSG(t.activeVoices() == Twine::kSounding, "%d voices", t.activeVoices());
        // the last note is there: its fundamental is in the mix
        CHECK(ampAt(d.L, 36000, 8192, keyToHz(95), sr) > 0.01);
        CHECK(ampAt(d.L, 36000, 8192, keyToHz(36), sr) < 0.002);         // the first was stolen long ago
    }
    // the same key again and again does not pile voices up
    {
        Twine t;
        bare(t);
        Drive d(t, sr);
        for (int i = 0; i < 40; ++i) d.on(i * 2400, 60, (uint32)(i + 1));
        int most = 0;
        d.each = [&](int64) { most = std::max(most, t.activeVoices()); };
        d.run(96000);
        CHECK_MSG(most <= 2, "most voices at once: %d", most);
        CHECK(peakOf(d.L) < 1.0f);
    }
    // a slide note bends a ringing string to the new key over its own length; portamento glides in
    {
        Twine t;
        bare(t);
        Drive d(t, sr);
        d.on(0, 60, 1);
        NoteEvent sl; sl.type = NoteEvent::SlideTo; sl.key = 67; sl.group = 0; sl.slideSamples = 9600.0f;
        d.ev(12000, sl);
        d.run(60032);
        CHECK_NEAR(centsOff(preciseHz(d.L, 2400, sr, keyToHz(60), 8192, 512), keyToHz(60)), 0.0, 2.0);
        const double mid = peakHz(d.L, 12000 + 2752, 4096, sr, 200.0, 500.0);        // centred on the middle of the slide
        CHECK_MSG(mid > keyToHz(62) && mid < keyToHz(65.5), "mid-slide pitch %.1f Hz", mid);
        CHECK_NEAR(centsOff(preciseHz(d.L, 26000, sr, keyToHz(67), 16384, 512), keyToHz(67)), 0.0, 2.0);
        Twine p;
        bare(p);
        Drive g(p, sr);
        NoteEvent e; e.type = NoteEvent::On; e.key = 72; e.id = 1; e.glideFromKey = 60.0f; e.slideSamples = 4800.0f;
        g.ev(0, e);
        g.run(48000);
        CHECK_NEAR(centsOff(preciseHz(g.L, 9600, sr, keyToHz(72), 16384, 512), keyToHz(72)), 0.0, 2.0);
        CHECK(ampAt(g.L, 0, 2048, keyToHz(63), sr) > 3.0 * ampAt(g.L, 9600, 2048, keyToHz(63), sr));     // it passed through on the way
    }
    // note-on is on its sample
    {
        Twine t;
        Drive d(t, sr);
        d.on(777, 60, 1);
        d.run(4800);
        CHECK(peakOf(d.L, 0, 777) == 0.0f && peakOf(d.L, 777, 790) > 0.0f);
    }
}

TEST(string_loop_is_tuned_exactly_and_never_has_gain_above_one) {
    // The loop behind Twine and Low Four, on its own, for thousands of settings across the whole range:
    // (1) the delays round the loop at the fundamental add up to exactly one period (that is the tuning);
    // (2) the gain round the loop at the fundamental is what the decay time asks for;
    // (3) at no frequency is the gain round the loop above one, so no setting can make it grow.
    uint32 rng = 0xC0FFEE11u;
    auto rnd = [&] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (double)(rng & 0xFFFFFF) / (double)0x1000000; };
    double worstPeriod = 0.0, worstGain = 0.0, highest = 0.0;
    int clamped = 0;
    for (int i = 0; i < 6000; ++i) {
        const double sr = i % 3 == 0 ? 44100.0 : (i % 3 == 1 ? 48000.0 : 96000.0);
        bl::StringMemory mem;
        mem.prepare(sr, 1);
        bl::StringLoop loop;
        loop.attach(mem.line(0), mem.size);
        bl::StringSetting s;
        s.hz = 16.0 * std::pow(2.0, 9.0 * rnd());               // 16 Hz to 8.2 kHz
        s.t60 = 0.005 * std::pow(2.0, 13.0 * rnd());            // 5 ms to 40 s
        s.highLoss = std::pow(10.0, 5.0 * rnd() - 2.0);         // 0.01 to 1000 per second
        s.stiffness = i % 4 == 0 ? 0.0 : rnd();
        loop.retune(s, sr);
        const double P = loop.period, w0 = kTwoPi / P;
        CHECK(std::isfinite(loop.gain) && loop.gain >= 0.0f && loop.gain < 1.0f && loop.pole >= 0.0f && loop.pole < 1.0f && std::fabs(loop.apA) < 1.0f);
        CHECK(loop.tap >= 0 && loop.tap + 7 < mem.size);
        auto interp = [&](double w, double& gain, double& delay) {
            double re = 0.0, im = 0.0;
            for (int k = 0; k < 6; ++k) { re += loop.h[k] * std::cos(w * k); im -= loop.h[k] * std::sin(w * k); }
            gain = std::hypot(re, im);
            delay = -std::atan2(im, re) / w;
        };
        const double p = loop.pole, a = loop.apA;
        auto poleGain = [&](double w) { return (1.0 - p) / std::sqrt(1.0 - 2.0 * p * std::cos(w) + p * p); };
        double gi, di;
        interp(w0, gi, di);
        const double total = (double)loop.tap + 1.0 + di + 4.0 * (1.0 - 2.0 * std::atan2(a * std::sin(w0), 1.0 + a * std::cos(w0)) / w0)
                           + std::atan2(p * std::sin(w0), 1.0 - p * std::cos(w0)) / w0;
        const bool inRange = s.hz < sr / 8.6 && s.hz > sr / (double)(mem.size - 12);
        if (inRange) {
            const double cents = 1200.0 * std::log2(total / P);
            worstPeriod = std::max(worstPeriod, std::fabs(cents));
            CHECK_MSG(std::fabs(cents) < 0.05, "%.2f Hz at %.0f: the loop is %.4f cents long or short (stiffness %.2f, loss %.3g)", s.hz, sr, cents, s.stiffness, s.highLoss);
            CHECK_NEAR(sr / P, s.hz, s.hz * 1.0e-9);
            // the decay: gain round the loop at the fundamental, as a 60 dB time (the gain is capped just under one)
            const double g0 = (double)loop.gain * gi * poleGain(w0);
            // Where the interpolator's own (tiny) loss at this pitch is already more than the decay time
            // allows, the loop gain sits at its cap and the note is shorter than asked: counted, not checked.
            const double want = std::exp(-bl::kLn1000 / (s.hz * s.t60));
            if (loop.gain < 0.999985f) {
                worstGain = std::max(worstGain, std::fabs(std::log(g0) / std::log(want) - 1.0));
                CHECK_MSG(std::fabs(std::log(g0) / std::log(want) - 1.0) < 0.01, "%.2f Hz, %.3f s: decay rate off by %.3f %%", s.hz, s.t60, 100.0 * (std::log(g0) / std::log(want) - 1.0));
            } else ++clamped;
        }
        for (int k = 1; k <= 96; ++k) {
            const double w = kPi * k / 96.0;
            double g, d;
            interp(w, g, d);
            const double round = (double)loop.gain * g * poleGain(w);
            highest = std::max(highest, round);
            if (round > 1.0) { CHECK_MSG(false, "%.2f Hz at %.0f: gain round the loop is %.6f at %.3f of the rate", s.hz, sr, round, w / kTwoPi); break; }
        }
    }
    if (report()) printf("    [string loop] 6000 random settings: loop length within %.5f cents of one period, decay rate within %.3f %%, highest gain round the loop %.6f (%d cases where the loop gain is at its cap are left out of the decay check)\n",
                         worstPeriod, 100.0 * worstGain, highest, clamped);
}
