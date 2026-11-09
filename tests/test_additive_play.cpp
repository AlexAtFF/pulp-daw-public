// Sheaf as an instrument: note timing, glide and slides, velocity and pan, the envelopes and LFOs
// measured, the voice limit, clean note ends, presets, and the real-time rules.
#include "SheafTestUtil.h"
#include <cstdlib>
using namespace pulp;
using namespace sheaft;

namespace {

bool verbose() { return std::getenv("PULP_TEST_VERBOSE") != nullptr; }

// One partial: a sine at the note's pitch, 0.25 high, with a flat envelope. For measuring pitch and level over time.
std::shared_ptr<Sheaf> sineSheaf() { return plainSheaf(Sheaf::Saw, 1); }
// Four partials: a waveform with no steep edges, so a click cannot hide in it.
std::shared_ptr<Sheaf> softSheaf() { return plainSheaf(Sheaf::Saw, 4); }

double levelAt(const std::vector<float>& d, double seconds, double hz, double sr = 48000.0, int n = 1024) {
    return ampAt(d, (size_t)std::max(0.0, seconds * sr - n / 2), n, hz, sr);
}
NoteEvent noteOn(int key, uint32 id, float vel = kDefaultVelocity) {
    NoteEvent e;
    e.type = NoteEvent::On; e.key = key; e.id = id; e.velocity = vel;
    return e;
}
// Frequency of each cycle of a single sine, from its upward zero crossings: (time, Hz) pairs.
std::vector<std::pair<double, double>> cycles(const std::vector<float>& d, size_t from, size_t to, double sr) {
    std::vector<std::pair<double, double>> out;
    double last = -1.0;
    for (size_t i = from + 1; i < to && i < d.size(); ++i)
        if (d[i - 1] < 0.0f && d[i] >= 0.0f) {
            const double x = (double)(i - 1) + (double)(-d[i - 1]) / (double)(d[i] - d[i - 1]);
            if (last >= 0.0) out.push_back({0.5 * (x + last) / sr, sr / (x - last)});
            last = x;
        }
    return out;
}

} // namespace

TEST(sheaf_note_on_lands_on_the_exact_sample) {
    // 120 bpm at 48 kHz: tick 37 is sample 925.
    auto s = plainSheaf(Sheaf::Saw);
    auto a = play(s, {mk(37, 960, 60)}, {48000.0, 120.0, 512, 1, 24000});
    for (size_t i = 0; i < 925; ++i) if (a->ch[0][i] != 0.0f) { CHECK_MSG(false, "sound at sample %zu, before the note", i); break; }
    CHECK(a->ch[0][925] == 0.0f);                   // every partial starts at the zero of its sine
    CHECK(std::fabs(a->ch[0][926]) > 1.0e-3f);
    // one tick later is the same sound 25 samples later, wherever that falls in the engine's quantum
    auto b = play(s, {mk(38, 960, 60)}, {48000.0, 120.0, 512, 1, 24000});
    double worst = 0.0;
    for (size_t i = 0; i + 25 < 20000; ++i) worst = std::max(worst, (double)std::fabs(a->ch[0][i] - b->ch[0][i + 25]));
    CHECK_MSG(worst < 2.0e-6, "shifted by a tick, the note differs by %g", worst);
    // at every offset inside a quantum, driven directly
    for (int at : {0, 1, 31, 63, 64, 1000}) {
        Sheaf t;
        Drive d(t);
        d.on(at, 57, 1);
        d.run(2048);
        bool early = false;
        for (int i = 0; i <= at; ++i) early = early || d.L[(size_t)i] != 0.0f;
        CHECK_MSG(!early && d.L[(size_t)at + 1] != 0.0f, "a note at sample %d starts in the wrong place", at);
    }
    // a start that is not at zero (random phases, clones, a pulse) is faded in over a millisecond instead of stepping
    for (int which = 0; which < 3; ++which) {
        auto t = plainSheaf(which == 2 ? Sheaf::Pulse : Sheaf::Saw, 16);
        if (which == 0) t->pPhases->set(1.0f);
        if (which == 1) t->pPrism->set(4.0f);
        Drive d(*t);
        d.on(500, 57, 1);
        d.run(4800);
        CHECK(peakOf(d.L, 0, 501) == 0.0f && peakOf(d.L, 500, 560) > 1.0e-4f);
        CHECK_MSG(maxStep(d.L, 0, 600) <= maxStep(d.L, 1000, 4800) * 1.1f + 1.0e-4f, "case %d: the note starts with a step of %.4f (the sound itself steps by %.4f)", which, maxStep(d.L, 0, 600), maxStep(d.L, 1000, 4800));
    }
}

TEST(sheaf_portamento_glide_slides_and_pitch_bend) {
    const double sr = 48000.0;
    {   // the channel's portamento
        auto s = sineSheaf();
        auto out = play(s, {mk(0, 960, 60), mk(960, 1920, 72)}, {sr, 120.0, 512, 1, 72000},
                        [](Project&, ChannelLive& l) { l.porta.set(1); l.mono.set(1); l.portaTime.set(200.0f); });
        const size_t on = 960 * 25;
        const double early = measureHz(out->ch[0], on + 300, on + 1500, sr), late = measureHz(out->ch[0], on + 14400, on + 24000, sr);
        CHECK_MSG(early > keyToHz(60) * 1.01 && early < keyToHz(66), "early pitch %.1f Hz", early);
        CHECK_MSG(std::fabs(centsOff(late, keyToHz(72))) < 1.0, "late pitch %.1f Hz", late);
        // half way through the 200 ms the pitch is half way up
        const double mid = measureHz(out->ch[0], on + 4560, on + 5040, sr);
        CHECK_MSG(std::fabs(centsOff(mid, keyToHz(66))) < 30.0, "mid-glide pitch %.1f Hz", mid);
    }
    {   // its own glide: from the last note played, in the time on the knob
        auto s = sineSheaf();
        s->pGlide->set(200.0f);
        auto out = play(s, {mk(0, 900, 60), mk(960, 1920, 72)}, {sr, 120.0, 512, 1, 72000});
        const size_t on = 960 * 25;
        const double early = measureHz(out->ch[0], on + 300, on + 1500, sr), late = measureHz(out->ch[0], on + 14400, on + 24000, sr);
        CHECK_MSG(early > keyToHz(60) * 1.01 && early < keyToHz(66), "early pitch %.1f Hz", early);
        CHECK_MSG(std::fabs(centsOff(late, keyToHz(72))) < 1.0, "late pitch %.1f Hz", late);
        CHECK_NEAR(measureHz(out->ch[0], 2400, 20000, sr), keyToHz(60), 0.05);       // the first note has nothing to glide from
        // no glide set, no glide
        auto plain = sineSheaf();
        plain->pRelease->set(1.0f);        // so that the first note is gone when the second is measured
        auto direct = play(plain, {mk(0, 900, 60), mk(960, 1920, 72)}, {sr, 120.0, 512, 1, 72000});
        CHECK_MSG(std::fabs(centsOff(measureHz(direct->ch[0], on + 300, on + 1500, sr), keyToHz(72))) < 1.0, "without glide the second note starts off pitch");
    }
    {   // a slide note bends the sounding notes of its colour and no others
        auto s = sineSheaf();
        Note held = mk(0, 3840, 60), other = mk(0, 3840, 72), slide = mk(960, 960, 67);
        held.pan = -1.0f;
        other.pan = 1.0f;
        other.color = 3;
        slide.slide = true;
        auto out = play(s, {held, other, slide}, {sr, 120.0, 512, 1, 96000});
        CHECK_NEAR(measureHz(out->ch[0], 4800, 22000, sr), keyToHz(60), 0.05);
        const double mid = measureHz(out->ch[0], 960 * 25 + 9600, 960 * 25 + 14400, sr);
        CHECK_MSG(mid > keyToHz(62) && mid < keyToHz(65.5), "mid-slide pitch %.2f Hz", mid);
        CHECK_NEAR(measureHz(out->ch[0], 60000, 90000, sr), keyToHz(67), 0.05);
        CHECK_NEAR(measureHz(out->ch[1], 60000, 90000, sr), keyToHz(72), 0.05);
    }
    {   // channel pitch, coarse and fine tune, per-note fine pitch
        auto s = sineSheaf();
        Drive d(*s, sr);
        d.bend = 2.0f;
        d.on(0, 60, 1);
        d.run(24000);
        CHECK_MSG(std::fabs(centsOff(measureHz(d.L, 2000, 22000, sr), keyToHz(62))) < 0.1, "two semitones of channel pitch");
        d.bend = -7.5f;
        d.run(24000);
        CHECK_MSG(std::fabs(centsOff(measureHz(d.L, 26000, 46000, sr), keyToHz(52.5))) < 0.1, "channel pitch moved while the note sounds");
        auto t = sineSheaf();
        t->pCoarse->set(-12.0f);
        t->pFine->set(37.0f);
        Drive e(*t, sr);
        NoteEvent n = noteOn(64, 1);
        n.fineCents = -20.0f;
        e.ev(0, n);
        e.run(24000);
        CHECK_MSG(std::fabs(centsOff(measureHz(e.L, 2000, 22000, sr), keyToHz(52.17))) < 0.1, "coarse -12, fine +37, note -20 cents");
    }
}

TEST(sheaf_velocity_and_pan) {
    const double sr = 48000.0, f = keyToHz(57);
    auto run = [&](Sheaf& s, float vel, float pan = 0.0f, std::vector<float>* right = nullptr) {
        Drive d(s, sr);
        NoteEvent e = noteOn(57, 1, vel);
        e.pan = pan;
        d.ev(0, e);
        d.run(24000);
        if (right) *right = d.R;
        return d.L;
    };
    {   // velocity to level: full sensitivity follows the square of the velocity, none ignores it
        auto s = plainSheaf(Sheaf::Saw, 16);
        const auto ref = run(*s, kDefaultVelocity), soft = run(*s, 0.4f), hard = run(*s, 1.0f);
        CHECK_NEAR(db(rmsOf(soft, 4800, 24000) / rmsOf(ref, 4800, 24000)), 2.0 * db(0.4 / kDefaultVelocity), 0.02);
        CHECK_NEAR(db(rmsOf(hard, 4800, 24000) / rmsOf(ref, 4800, 24000)), 2.0 * db(1.0 / kDefaultVelocity), 0.02);
        s->pVelSens->set(0.0f);
        CHECK(run(*s, 0.4f) == ref && run(*s, 1.0f) == ref);
        s->pVelSens->set(0.5f);
        CHECK_NEAR(rmsOf(run(*s, 0.4f), 4800, 24000) / rmsOf(ref, 4800, 24000), 1.0 + 0.5 * ((0.4 / kDefaultVelocity) * (0.4 / kDefaultVelocity) - 1.0), 0.002);
    }
    {   // velocity to brightness: a tilt of so many dB per octave for a velocity twice the standard, the energy unchanged
        auto s = plainSheaf(Sheaf::Saw, 16);
        s->pVelSens->set(0.0f);
        s->pVelBright->set(12.0f);
        const auto ref = run(*s, kDefaultVelocity), soft = run(*s, 0.5f * kDefaultVelocity), hard = run(*s, 1.0f);
        auto bright = [&](const std::vector<float>& d) { return db(ampFine(d, 4800, 16384, 8 * f, sr) / ampFine(d, 4800, 16384, f, sr)); };
        CHECK_NEAR(bright(soft) - bright(ref), 12.0 * -0.5 * 3.0, 0.02);                       // three octaves from partial 1 to 8
        CHECK_NEAR(bright(hard) - bright(ref), 12.0 * (1.0 / kDefaultVelocity - 1.0) * 3.0, 0.02);
        CHECK_NEAR(db(rmsOf(soft, 4800, 4800 + 17455) / rmsOf(ref, 4800, 4800 + 17455)), 0.0, 0.05);     // a whole number of periods
        CHECK_NEAR(db(rmsOf(hard, 4800, 4800 + 17455) / rmsOf(ref, 4800, 4800 + 17455)), 0.0, 0.05);
    }
    {   // pan: the far side is turned down, as in the other instruments
        auto s = plainSheaf(Sheaf::Saw, 16);
        std::vector<float> r;
        auto l = run(*s, kDefaultVelocity, -1.0f, &r);
        CHECK(peakOf(l) > 0.2f && peakOf(r) == 0.0f);
        l = run(*s, kDefaultVelocity, 0.5f, &r);
        CHECK_NEAR(rmsOf(l, 4800, 24000) / rmsOf(r, 4800, 24000), 0.5, 0.001);
        l = run(*s, kDefaultVelocity, 0.0f, &r);
        CHECK(l == r);
    }
}

TEST(sheaf_envelopes_are_what_their_knobs_say) {
    const double sr = 48000.0, f = keyToHz(69);
    {   // amplitude: linear attack, decay to the sustain level, release
        auto s = sineSheaf();
        s->pAttack->set(200.0f);
        s->pDecay->set(400.0f);
        s->pSustain->set(0.5f);
        s->pRelease->set(300.0f);
        Drive d(*s, sr);
        d.on(0, 69, 1);
        d.off(72000, 1);            // 1.5 s
        d.run(120000);
        CHECK_NEAR(levelAt(d.L, 0.05, f) / 0.25, 0.25, 0.01);
        CHECK_NEAR(levelAt(d.L, 0.10, f) / 0.25, 0.50, 0.01);
        CHECK_NEAR(levelAt(d.L, 0.15, f) / 0.25, 0.75, 0.01);
        // the decay closes 99% of the gap to the sustain level in its time (4.6 time constants)
        CHECK_NEAR(levelAt(d.L, 0.2 + 0.2, f) / 0.25, 0.5 + 0.5 * std::exp(-2.3), 0.01);
        CHECK_NEAR(levelAt(d.L, 0.2 + 0.4, f) / 0.25, 0.5 + 0.5 * std::exp(-4.6), 0.005);
        CHECK_NEAR(levelAt(d.L, 1.4, f) / 0.25, 0.5, 0.001);
        CHECK_NEAR(levelAt(d.L, 1.5 + 0.15, f) / 0.25, 0.5 * std::exp(-2.3), 0.01);
        CHECK(levelAt(d.L, 1.5 + 0.3, f) / 0.25 < 0.012);
        CHECK(peakOf(d.L, 72000 + 30000, 120000) == 0.0f && s->activeVoices() == 0);
        // the note's own release setting scales the time: 0 is a quarter of it, 1 is 1.75 times
        auto t = sineSheaf();
        t->pRelease->set(400.0f);
        Drive e(*t, sr);
        NoteEvent n = noteOn(69, 1);
        n.release = 0.0f;
        e.ev(0, n);
        e.off(24000, 1);
        e.run(72000);
        CHECK_NEAR(levelAt(e.L, 0.5 + 0.05, f) / 0.25, std::exp(-2.3), 0.02);        // half of a 100 ms release
    }
    {   // attack 0 is full level on the first sample; sustain 0 dies away and frees the voice
        auto s = sineSheaf();
        s->pDecay->set(100.0f);
        s->pSustain->set(0.0f);
        Drive d(*s, sr);
        d.on(0, 69, 1);
        d.run(24000);
        CHECK_NEAR(d.L[1], 0.25 * std::sin(kTwoPi * f / sr), 1e-4);
        CHECK(peakOf(d.L, 12000, 24000) == 0.0f && s->activeVoices() == 0);
    }
    {   // envelope 2, held at its sustain level, moves morph, tilt and cutoff by exactly its amount times that level
        auto s = plainSheaf(Sheaf::Saw, 32);
        s->spec[1].shape->set((float)Sheaf::Square);
        s->pE2Attack->set(0.0f);
        s->pE2Decay->set(20.0f);
        s->pE2Sustain->set(0.3f);
        s->pE2Morph->set(1.0f);
        auto ref = plainSheaf(Sheaf::Saw, 32);
        ref->spec[1].shape->set((float)Sheaf::Square);
        ref->pMorph->set(0.3f);
        const auto a = hold(*s, 57, sr, 48000), b = hold(*ref, 57, sr, 48000);
        for (int k : {1, 2, 3, 8}) CHECK_NEAR(db(ampFine(a, 24000, 16384, 220.0 * k, sr) / ampFine(b, 24000, 16384, 220.0 * k, sr)), 0.0, 0.01);
        // ... and while it is still falling, the morph is on its way: more of B (less second harmonic) early than late
        s->pE2Decay->set(400.0f);
        s->pE2Sustain->set(0.0f);
        const auto c = hold(*s, 57, sr, 48000);
        const double early = ampAt(c, 1200, 2048, 440.0, sr), late = ampAt(c, 40000, 2048, 440.0, sr);
        const double mEarly = 1.0 - early / late;       // the second harmonic is (1 - morph) of its level in A
        CHECK_MSG(mEarly > 0.55 && mEarly < 0.75, "46 ms into a 400 ms decay the morph is %.2f (0.59 by the knob)", mEarly);

        auto t = plainSheaf(Sheaf::Saw, 32);
        t->pE2Attack->set(0.0f);
        t->pE2Decay->set(20.0f);
        t->pE2Sustain->set(0.5f);
        t->pE2Tilt->set(-6.0f);
        auto plain = plainSheaf(Sheaf::Saw, 32);
        const auto x = hold(*t, 57, sr, 48000), y = hold(*plain, 57, sr, 48000);
        auto slope = [&](const std::vector<float>& d) { return db(ampFine(d, 24000, 16384, 220.0 * 16, sr) / ampFine(d, 24000, 16384, 220.0, sr)); };
        CHECK_NEAR(slope(x) - slope(y), -3.0 * 4.0, 0.02);                  // -3 dB per octave over four octaves
        CHECK_NEAR(db(rmsOf(x, 24000, 24000 + 21818) / rmsOf(y, 24000, 24000 + 21818)), 0.0, 0.05);     // the energy stays
    }
    {   // the envelopes follow their knobs while a note is held
        auto s = sineSheaf();
        s->pSustain->set(1.0f);
        Drive d(*s, sr);
        d.on(0, 69, 1);
        d.run(9600);
        s->pSustain->set(0.25f);
        s->pDecay->set(50.0f);
        d.run(24000);
        CHECK_NEAR(levelAt(d.L, 0.6, f) / 0.25, 0.25, 0.005);
    }
}

TEST(sheaf_lfos_run_at_their_rate_and_reach_as_far_as_their_amounts) {
    const double sr = 48000.0, f = keyToHz(69);
    {   // to level: a 4 Hz sine takes the level down to (1 - amount) and back, four times a second
        auto s = sineSheaf();
        s->lfo[0].rate->set(4.0f);
        s->lfo[0].amp->set(0.6f);
        const auto out = hold(*s, 69, sr, 96000);
        double lo = 1e9, hi = 0.0;
        std::vector<double> minima;
        double prev2 = 0.0, prev = 0.0;
        for (int i = 0; i < 900; ++i) {
            const double t = 0.05 + i * 0.002, v = levelAt(out, t, f, sr, 512);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            if (i >= 2 && prev < prev2 && prev < v) minima.push_back(t - 0.002);
            prev2 = prev;
            prev = v;
        }
        CHECK_NEAR(hi / 0.25, 1.0, 0.01);
        CHECK_NEAR(lo / 0.25, 0.4, 0.01);
        CHECK_MSG(minima.size() == 7, "%zu dips in 1.8 seconds", minima.size());
        if (minima.size() >= 2) CHECK_NEAR((minima.back() - minima.front()) / (double)(minima.size() - 1), 0.25, 0.002);
        if (!minima.empty()) CHECK_NEAR(minima.front(), 0.1875, 0.006);          // a sine from zero: lowest three quarters of the way through
    }
    {   // to pitch: 50 cents each way, five times a second
        auto s = sineSheaf();
        s->lfo[0].rate->set(5.0f);
        s->lfo[0].pitch->set(50.0f);
        const auto out = hold(*s, 69, sr, 96000);
        const auto cyc = cycles(out, 2400, 96000, sr);
        double lo = 1e9, hi = -1e9;
        int rises = 0;
        bool above = false;
        for (auto& c : cyc) {
            const double cents = centsOff(c.second, f);
            lo = std::min(lo, cents);
            hi = std::max(hi, cents);
            if (!above && cents > 25.0) { above = true; ++rises; }
            if (above && cents < -25.0) above = false;
        }
        CHECK_MSG(std::fabs(hi - 50.0) < 1.5 && std::fabs(lo + 50.0) < 1.5, "vibrato runs from %.1f to %.1f cents", lo, hi);
        CHECK_MSG(rises == 10, "%d vibrato cycles in 1.95 seconds", rises);
        // and the average pitch stays on the note
        CHECK_MSG(std::fabs(centsOff(measureHz(out, 9600, 9600 + 76800, sr), f)) < 0.5, "vibrato drags the pitch");
    }
    {   // fade in: the depth grows in a straight line over the fade time, from the start of the note
        auto s = sineSheaf();
        s->lfo[1].rate->set(8.0f);
        s->lfo[1].pitch->set(100.0f);
        s->lfo[1].fade->set(1000.0f);
        const auto out = hold(*s, 69, sr, 96000);
        auto depth = [&](double from, double to) {
            double hi = 0.0;
            for (auto& c : cycles(out, (size_t)(from * sr), (size_t)(to * sr), sr)) hi = std::max(hi, std::fabs(centsOff(c.second, f)));
            return hi;
        };
        CHECK_MSG(depth(0.0, 0.07) < 8.0, "%.1f cents of vibrato in the first 70 ms", depth(0.0, 0.07));
        CHECK_NEAR(depth(0.44, 0.56), 53.0, 4.0);       // the cycle that peaks nearest half way
        CHECK_NEAR(depth(1.2, 1.9), 100.0, 2.0);
    }
    {   // a square LFO holds each of its two values for half a cycle: morph, tilt and cutoff sit exactly where the amounts put them
        const double hz = 220.0;
        auto lv = [&](const std::vector<float>& d, int k, bool second) { return ampFine(d, second ? 28800 : 4800, 16384, hz * k, sr); };
        // morph +0.4 then -0.4 about 0.5
        auto s = plainSheaf(Sheaf::Saw, 32);
        s->spec[1].shape->set((float)Sheaf::Square);
        s->pMorph->set(0.5f);
        s->lfo[0].wave->set(4.0f);
        s->lfo[0].rate->set(1.0f);
        s->lfo[0].morph->set(0.4f);
        const auto out = hold(*s, 57, sr, 48000);
        auto a = plainSheaf(Sheaf::Saw, 32);
        const auto ref = hold(*a, 57, sr, 48000);
        CHECK_NEAR(lv(out, 2, false) / lv(ref, 2, false), 0.1, 0.001);      // morph 0.9: a tenth of the even harmonics left
        CHECK_NEAR(lv(out, 2, true) / lv(ref, 2, true), 0.9, 0.001);        // morph 0.1
        // tilt +3 then -3 dB per octave
        auto t = plainSheaf(Sheaf::Saw, 32);
        t->lfo[1].wave->set(4.0f);
        t->lfo[1].rate->set(1.0f);
        t->lfo[1].tilt->set(3.0f);
        const auto tilted = hold(*t, 57, sr, 48000);
        auto slope = [&](const std::vector<float>& d, bool second) { return db(lv(d, 16, second) / lv(d, 1, second)); };
        CHECK_NEAR(slope(tilted, false) - slope(ref, false), 12.0, 0.02);
        CHECK_NEAR(slope(tilted, true) - slope(ref, true), -12.0, 0.02);
        // cutoff an octave up, then an octave down
        auto c = plainSheaf(Sheaf::Saw, 32);
        c->pFilterType->set((float)Sheaf::LowPass);
        c->pCutoff->set(880.0f);
        c->pSlope->set(24.0f);
        c->lfo[0].wave->set(4.0f);
        c->lfo[0].rate->set(1.0f);
        c->lfo[0].filter->set(1.0f);
        const auto swept = hold(*c, 57, sr, 48000);
        CHECK_NEAR(db(lv(swept, 16, false) / lv(ref, 16, false)), -24.0, 0.05);         // cutoff 1760 Hz, partial 16 at 3520
        CHECK_NEAR(db(lv(swept, 8, false) / lv(ref, 8, false)), 0.0, 0.05);
        CHECK_NEAR(db(lv(swept, 8, true) / lv(ref, 8, true)), -48.0, 0.05);             // cutoff 440 Hz, partial 8 at 1760
    }
    {   // every shape does what its name says (heard as pitch: 100 cents of depth at 2 Hz)
        auto shape = [&](int wave) {
            auto s = sineSheaf();
            s->lfo[0].wave->set((float)wave);
            s->lfo[0].rate->set(2.0f);
            s->lfo[0].pitch->set(100.0f);
            const auto out = hold(*s, 69, sr, 96000);
            return [out, sr, f](double t) {       // cents at a moment, over 20 ms
                return centsOff(measureHz(out, (size_t)((t - 0.01) * sr), (size_t)((t + 0.01) * sr), sr), f);
            };
        };
        auto tri = shape(1);
        CHECK_NEAR(tri(0.0625), 50.0, 3.0); CHECK_NEAR(tri(0.125), 100.0, 5.0); CHECK_NEAR(tri(0.25), 0.0, 3.0); CHECK_NEAR(tri(0.375), -100.0, 5.0);
        auto up = shape(2);
        CHECK_NEAR(up(0.125), -50.0, 3.0); CHECK_NEAR(up(0.375), 50.0, 3.0); CHECK_NEAR(up(0.625), -50.0, 3.0);
        auto down = shape(3);
        CHECK_NEAR(down(0.125), 50.0, 3.0); CHECK_NEAR(down(0.375), -50.0, 3.0);
        auto sq = shape(4);
        CHECK_NEAR(sq(0.1), 100.0, 0.5); CHECK_NEAR(sq(0.2), 100.0, 0.5); CHECK_NEAR(sq(0.3), -100.0, 0.5); CHECK_NEAR(sq(0.45), -100.0, 0.5);
        // random steps: one value for each cycle, a new one at the next, and the same ones every time the note is played
        auto rnd = shape(5), again = shape(5);
        std::vector<double> held;
        for (int c = 0; c < 3; ++c) {
            const double a = rnd(c * 0.5 + 0.1), b = rnd(c * 0.5 + 0.4);
            CHECK_MSG(std::fabs(a - b) < 0.5, "random step %d wanders from %.1f to %.1f cents", c, a, b);
            CHECK(std::fabs(a) <= 100.5);
            CHECK_NEAR(again(c * 0.5 + 0.25), rnd(c * 0.5 + 0.25), 1e-9);
            held.push_back(a);
        }
        CHECK(std::fabs(held[0] - held[1]) > 1.0 && std::fabs(held[1] - held[2]) > 1.0);
    }
}

TEST(sheaf_voice_limit_holds_and_stealing_does_not_click) {
    auto s = sineSheaf();           // one partial a note: no edges for a click to hide behind
    s->pAttack->set(5.0f);
    s->pVoices->set(4.0f);
    Drive d(*s);
    for (int i = 0; i < 12; ++i) d.on(1000 + i * 960, 40 + i * 3, (uint32)(i + 1));
    int most = 0;
    for (int q = 0; q < 16000 / kQuantum; ++q) { d.run(kQuantum); most = std::max(most, s->activeVoices()); }
    CHECK_MSG(most <= 5, "%d voices at once with a limit of 4 (one more may be fading out)", most);
    d.run(8192);
    CHECK_MSG(s->activeVoices() == 4, "%d voices held", s->activeVoices());
    // Five sines at their steepest together step by this much; a voice cut dead would add up to its amplitude (0.25).
    double natural = 0.0;
    for (int i = 7; i < 12; ++i) natural += 0.25 * kTwoPi * keyToHz(40 + i * 3) / 48000.0;
    CHECK_MSG(maxStep(d.L, 0, d.L.size()) < natural * 1.02, "largest step %.4f (the sines alone can make %.4f)", maxStep(d.L, 0, d.L.size()), natural);
    CHECK(peakOf(d.L) > 0.3f);
    // the newest four are the ones left
    for (int i = 0; i < 12; ++i) {
        const double a = ampAt(d.L, d.L.size() - 8192, 8192, keyToHz(40 + i * 3), 48000.0);
        if (i >= 8) CHECK_MSG(a > 0.2, "note %d should be sounding", i);
        else CHECK_MSG(a < 0.001, "note %d should have been stolen", i);
    }
    // a released note is taken before a held one
    {
        auto t = softSheaf();
        t->pVoices->set(2.0f);
        t->pRelease->set(3000.0f);
        Drive e(*t);
        e.on(0, 48, 1);
        e.on(100, 55, 2);
        e.off(2000, 2);             // the younger note is let go...
        e.on(6000, 64, 3);          // ...so it, not the older held one, makes room
        e.run(24000);
        CHECK(ampAt(e.L, 12000, 8192, keyToHz(48), 48000.0) > 0.1);
        CHECK(ampAt(e.L, 12000, 8192, keyToHz(55), 48000.0) < 0.005);
        CHECK(ampAt(e.L, 12000, 8192, keyToHz(64), 48000.0) > 0.1);
    }
    // far more notes than slots: still finite, still bounded, the limit still holds
    Sheaf many;
    many.pVoices->set(32.0f);
    Drive e(many);
    for (int i = 0; i < 300; ++i) e.on(i * 37, 30 + (i * 7) % 70, (uint32)(i + 1));
    e.run(48000);
    CHECK(allFinite(e.L) && many.activeVoices() <= Sheaf::kSlots);
    CHECK_MSG(many.activeVoices() == 32, "%d voices", many.activeVoices());
    // polyphony 1: each note cuts the last
    Sheaf mono;
    mono.pVoices->set(1.0f);
    Drive m(mono);
    m.on(0, 50, 1);
    m.on(4800, 57, 2);
    m.run(24000);
    CHECK(mono.activeVoices() == 1);
}

TEST(sheaf_note_ends_and_cuts_fall_smoothly_to_silence) {
    const double sr = 48000.0;
    const char* presets[] = {nullptr, "Prism Strings", "Church Bell", "Drawbar Organ"};
    for (int which = 0; which < 4; ++which) {
        Sheaf s;
        if (presets[which]) loadPreset(s, presets[which]);
        else { s.pPartials->set(8.0f); }
        Drive d(s, sr);
        d.on(0, 57, 1);
        d.off(48000, 1);
        d.run(48000 + 480000);
        const float before = maxStep(d.L, 24000, 48000), after = maxStep(d.L, 48000, d.L.size());
        CHECK_MSG(after <= before * 1.05f + 1.0e-5f, "case %d: a step of %.5f after the note-off, %.5f before it", which, after, before);
        size_t last = 0;
        for (size_t i = 0; i < d.L.size(); ++i) if (d.L[i] != 0.0f) last = i;
        CHECK_MSG(last < d.L.size() - 4800, "case %d: still sounding at the end", which);
        CHECK_MSG(peakOf(d.L, last > 48 ? last - 48 : 0, last + 1) < 1.0e-4f, "case %d: the last millisecond is at %.6f, not at silence", which, peakOf(d.L, last > 48 ? last - 48 : 0, last + 1));
        CHECK(s.activeVoices() == 0);
    }
    // Kill and KillNote fade in about 3 ms
    for (auto type : {NoteEvent::Kill, NoteEvent::KillNote}) {
        Sheaf s;
        s.pPartials->set(8.0f);
        Drive d(s, sr);
        d.on(0, 57, 1);
        NoteEvent k;
        k.type = type;
        k.id = 1;
        d.ev(24010, k);
        d.run(48000);
        CHECK(peakOf(d.L, 20000, 24000) > 0.1f);
        CHECK_MSG(maxStep(d.L, 24000, 48000) <= maxStep(d.L, 12000, 24000) * 1.3f, "the cut steps by %.4f", maxStep(d.L, 24000, 48000));
        CHECK_MSG(peakOf(d.L, 24010 + 150, 48000) == 0.0f, "still %.5f after the cut", peakOf(d.L, 24010 + 150, 48000));
        CHECK(s.activeVoices() == 0);
    }
    {   // KillNote takes only its own note
        Sheaf s;
        s.pPartials->set(1.0f);
        Drive d(s, sr);
        d.on(0, 57, 1);
        d.on(0, 69, 2);
        NoteEvent k;
        k.type = NoteEvent::KillNote;
        k.id = 1;
        d.ev(12000, k);
        d.run(36000);
        CHECK(ampAt(d.L, 20000, 8192, 220.0, sr) < 1.0e-5 && ampAt(d.L, 20000, 8192, 440.0, sr) > 0.1);
    }
    {   // AllOff releases everything
        Sheaf s;
        Drive d(s, sr);
        d.on(0, 57, 1);
        d.on(0, 64, 2);
        NoteEvent a;
        a.type = NoteEvent::AllOff;
        d.ev(12000, a);
        d.run(96000);
        CHECK(peakOf(d.L, 60000, 96000) == 0.0f && s.activeVoices() == 0);
    }
}

TEST(sheaf_is_silent_without_notes_and_starts_clean_after_reset_and_prepare) {
    {   // no notes: exactly nothing, at any setting
        Sheaf s;
        loadPreset(s, "Choir Pad");
        Drive d(s);
        d.run(9600);
        CHECK(peakOf(d.L) == 0.0f && peakOf(d.R) == 0.0f && s.activeVoices() == 0);
        // ... and exactly nothing again once every note has died away (no denormal crawl)
        d.on(d.pos, 60, 1);
        d.off(d.pos + 4800, 1);
        d.run(480000);
        CHECK(peakOf(d.L, d.L.size() - 96000, d.L.size()) == 0.0f && s.activeVoices() == 0);
    }
    for (const char* name : {"Plain Saw", "Spectral Rain", "Prism Strings", "Glass Keys"}) {
        auto notes = [&](Drive& d, int64 at) {
            for (int i = 0; i < 6; ++i) { d.on(at + i * 700, 45 + i * 4, (uint32)(100 + i)); d.off(at + i * 700 + 5000, (uint32)(100 + i)); }
        };
        Sheaf fresh;
        loadPreset(fresh, name);
        fresh.pGlide->set(80.0f);
        Drive ref(fresh);
        notes(ref, 0);
        ref.run(19200);
        // reset in the middle of sounding notes: silence at once, and then the same notes give the same samples as a new instrument
        Sheaf s;
        loadPreset(s, name);
        s.pGlide->set(80.0f);
        Drive d(s);
        notes(d, 0);
        d.run(9600);
        CHECK(s.activeVoices() > 0);
        { Rt::Scope rt; s.reset(); }
        CHECK(s.activeVoices() == 0);
        const size_t mark = d.L.size();
        d.run(6400);
        CHECK_MSG(peakOf(d.L, mark, d.L.size()) == 0.0f, "%s: sound after reset", name);
        const size_t again = d.L.size();
        notes(d, d.pos);
        d.run(19200);
        bool same = true;
        for (size_t i = 0; i < 19200; ++i) same = same && d.L[again + i] == ref.L[i] && d.R[again + i] == ref.R[i];
        CHECK_MSG(same, "%s: after reset the notes do not sound as they do on a new instrument", name);
        // prepare at another rate and back: clean again, the same samples again
        s.prepare(96000.0);
        s.prepare(48000.0);
        Drive e(s);
        notes(e, 0);
        e.run(19200);
        CHECK_MSG(e.L == ref.L && e.R == ref.R, "%s: after prepare the notes do not sound as they do on a new instrument", name);
    }
}

TEST(sheaf_presets_all_sound_right_and_reload) {
    Sheaf proto;
    const auto presets = proto.factoryPresets();
    CHECK_MSG(presets.size() >= 24, "%zu presets", presets.size());
    std::vector<std::string> names;
    float quietest = 10.0f, loudest = 0.0f;
    std::vector<std::shared_ptr<SampleData>> sounds;
    const RenderSpec rs{48000.0, 120.0, 512, 1, 288000};       // a three-second note and three seconds after it
    for (auto& pr : presets) {
        CHECK_MSG(std::find(names.begin(), names.end(), pr.name) == names.end(), "two presets called %s", pr.name.c_str());
        names.push_back(pr.name);
        auto s = std::make_shared<Sheaf>();
        s->loadState(pr.state);
        CHECK_MSG(s->spec[0].shape->getInt() != Sheaf::User && s->spec[1].shape->getInt() != Sheaf::User, "%s needs an analysis", pr.name.c_str());
        auto one = play(s, {mk(0, 5760, 60)}, rs);
        const float pk = std::max(peakOf(one->ch[0]), peakOf(one->ch[1]));
        quietest = std::min(quietest, pk);
        loudest = std::max(loudest, pk);
        if (verbose()) printf("    %-20s one note peaks at %6.1f dBFS (volume %.3f; %.3f would give -9)\n", pr.name.c_str(), gainToDb(pk), s->pVolume->get(), s->pVolume->get() * dbToGain(-9.0f) / pk);
        CHECK_MSG(allFinite(one->ch[0]) && allFinite(one->ch[1]), "%s produced a non-finite sample", pr.name.c_str());
        CHECK_MSG(pk > dbToGain(-14.0f) && pk < dbToGain(-4.0f), "%s: one note at velocity 100 peaks at %.1f dBFS (-14 to -4 wanted)", pr.name.c_str(), gainToDb(pk));
        CHECK_MSG(rmsOf(one->ch[0], 24000, 120000) > 0.005, "%s is all but silent", pr.name.c_str());
        // a chord, played hard, across the keyboard
        std::vector<Note> chord;
        for (int k : {36, 48, 55, 60, 64, 67, 72, 84}) chord.push_back(mk(0, 1920, k, 1.0f));
        auto many = play(s, chord, {48000.0, 120.0, 512, 1, 96000});
        CHECK_MSG(allFinite(many->ch[0]) && allFinite(many->ch[1]), "%s: a chord produced a non-finite sample", pr.name.c_str());
        std::vector<Note> triad;
        for (int k : {48, 60, 67}) triad.push_back(mk(0, 1920, k));
        auto three = play(s, triad, {48000.0, 120.0, 512, 1, 96000});
        CHECK_MSG(std::max(peakOf(three->ch[0]), peakOf(three->ch[1])) < 2.0f, "%s: a triad peaks at %.2f, over +6 dBFS", pr.name.c_str(), std::max(peakOf(three->ch[0]), peakOf(three->ch[1])));
        // it dies away after the note ends
        CHECK_MSG(peakOf(one->ch[0], 276000, 288000) < 0.25f * pk || s->pRelease->get() > 2600.0f, "%s never dies away", pr.name.c_str());
        // state round trip, through text: same parameters, same sound
        auto t = std::make_shared<Sheaf>();
        t->loadState(Json::parse(s->saveState().dump()));
        CHECK_MSG(t->saveState() == s->saveState(), "%s does not survive save and load", pr.name.c_str());
        auto again = play(t, {mk(0, 5760, 60)}, rs);
        CHECK_MSG(identical(*one, *again), "%s sounds different after save and load", pr.name.c_str());
        for (size_t i = 0; i < sounds.size(); ++i) CHECK_MSG(!identical(*sounds[i], *one), "%s sounds exactly like %s", pr.name.c_str(), names[i].c_str());
        sounds.push_back(one);
    }
    printf("    %zu presets; one note at velocity 100 peaks between %.1f and %.1f dBFS\n", presets.size(), gainToDb(quietest), gainToDb(loudest));
    printf("    %zu parameters in %zu groups\n", proto.params().size(), proto.paramGroups().size());
    CHECK(groupsCoverParams(proto));
    CHECK(proto.paramGroups().size() == 12);
    CHECK(std::string(proto.typeId()) == "additive" && std::string(proto.displayName()) == "Sheaf" && proto.isInstrument());
    // every parameter has a distinct id and a name, and its default is in range
    for (auto& p : proto.params()) {
        CHECK(!p->info.id.empty() && !p->info.name.empty() && p->info.def >= p->info.min && p->info.def <= p->info.max);
        int same = 0;
        for (auto& q : proto.params()) same += q->info.id == p->info.id ? 1 : 0;
        CHECK_MSG(same == 1, "parameter id %s is used %d times", p->info.id.c_str(), same);
    }
}

TEST(sheaf_render_is_identical_at_any_block_size_and_thread_count_and_never_allocates) {
    std::vector<double> lv;
    for (int k = 1; k <= 20; ++k) lv.push_back(0.3 / k);
    const auto rec = harmonicNote(196.0, 1.2, 44100.0, lv, [](int k, double t) { return std::exp(-t * 0.5 * k); });
    for (const char* name : {"Choir Pad", "Glass Keys", "Spectral Rain", "Glass Lead", "Metal Bloom", "(analysed)"}) {
        std::shared_ptr<SampleData> ref;
        struct Case { int block, threads; bool scalar; } cases[] = {{64, 1, false}, {33, 1, false}, {4096, 1, false}, {512, 4, false}, {33, 4, false}, {512, 1, true}};
        for (auto& c : cases) {
            auto s = std::make_shared<Sheaf>();
            if (std::string(name) == "(analysed)") {
                CHECK(s->analyse(rec));
                s->spec[0].shape->set((float)Sheaf::User);
                s->spec[1].shape->set((float)Sheaf::Vowel);
                s->pE2Morph->set(0.7f);
                s->pUserSpeed->set(1.7f);
                s->pUserLoopStart->set(0.2f);
                s->pUserLoopEnd->set(0.5f);
            } else loadPreset(*s, name);
            s->pGlide->set(60.0f);
            s->forceScalar = c.scalar;
            std::vector<Note> notes;
            for (int i = 0; i < 24; ++i) {
                Note n = mk(i * 97 + (i % 3) * 11, 300 + (i % 5) * 170, 40 + (i * 5) % 40, 0.4f + 0.02f * i);
                n.pan = (float)((i % 5) - 2) * 0.4f;
                n.modX = 0.3f + 0.03f * (i % 10);
                n.modY = 0.7f - 0.02f * (i % 10);
                n.fine = (float)(i % 7) * 3.0f;
                notes.push_back(n);
            }
            Note slide = mk(700, 400, 70);
            slide.slide = true;
            notes.push_back(slide);
            const int64 before = Rt::allocationsInRt.load();
            auto out = play(s, notes, {48000.0, 120.0, c.block, c.threads, 96000});
            CHECK_MSG(Rt::allocationsInRt.load() == before, "%s: %lld allocations on the audio thread", name, (long long)(Rt::allocationsInRt.load() - before));
            if (!ref) { ref = out; CHECK(peakOf(out->ch[0]) > 0.05f); continue; }
            CHECK_MSG(identical(*ref, *out), "%s: block %d, %d threads%s differs (by up to %g)", name, c.block, c.threads, c.scalar ? ", plain C++ oscillators" : "", maxAbsDiff(ref->ch[0], out->ch[0]));
        }
    }
}

TEST(sheaf_parameters_can_move_while_notes_sound) {
    // Every parameter is automatable. First all of them at once, at random, between quanta.
    for (const char* name : {"Glass Lead", "Choir Pad", "Tine Bell"}) {
        Sheaf s;
        loadPreset(s, name);
        if (std::string(name) == "Tine Bell") {         // this one with an analysed sound in it, so the "user" shape is the real thing
            std::vector<double> lv = {0.3, 0.2, 0.1, 0.05, 0.02};
            CHECK(s.analyse(harmonicNote(196.0, 0.8, 44100.0, lv, [](int k, double t) { return std::exp(-t * k); })));
        }
        Drive d(s);
        for (int i = 0; i < 8; ++i) d.on(i * 3000, 40 + i * 5, (uint32)(i + 1));
        uint32 rnd = 12345;
        for (int q = 0; q < 96000 / kQuantum; ++q) {
            for (auto& p : s.params()) {
                rnd = rnd * 1664525u + 1013904223u;
                if ((rnd >> 24) < 40) p->setNorm((float)((rnd >> 8) & 0xffff) / 65535.0f);
            }
            d.run(kQuantum);
        }
        CHECK_MSG(allFinite(d.L) && allFinite(d.R), "%s: a non-finite sample", name);
        CHECK_MSG(peakOf(d.L) < 30.0f && peakOf(d.R) < 30.0f, "%s: peak %.2f", name, std::max(peakOf(d.L), peakOf(d.R)));
    }
    // Then each on its own: swept from one end to the other and thrown back and forth, on a chord.
    Sheaf proto;
    float worst = 0.0f;
    std::string worstId;
    for (size_t pi = 0; pi < proto.params().size(); ++pi) {
        Sheaf s;
        loadPreset(s, "Morning Pad");
        s.pAttack->set(5.0f);
        Param* p = s.params()[pi].get();
        Drive d(s);
        for (int i = 0; i < 4; ++i) d.on(i * 500, 45 + i * 7, (uint32)(i + 1));
        for (int q = 0; q < 160; ++q) {
            if (q < 100) p->setNorm((float)q / 99.0f);
            else p->setNorm(q % 2 ? 0.0f : 1.0f);
            d.run(kQuantum);
        }
        const float pk = std::max(peakOf(d.L), peakOf(d.R));
        CHECK_MSG(allFinite(d.L) && allFinite(d.R), "%s: a non-finite sample", p->info.id.c_str());
        CHECK_MSG(pk < 30.0f, "%s: peak %.2f", p->info.id.c_str(), pk);
        if (pk > worst) { worst = pk; worstId = p->info.id; }
    }
    printf("    every parameter swept and toggled on a four-note chord: highest peak %.2f (%s)\n", worst, worstId.c_str());
    // Changes are ramped, not stepped: a control thrown between two values every quantum makes no step bigger than
    // the waveform's own (four partials, so the waveform has no edges).
    struct Throw { const char* id; float a, b; };
    const Throw throws[] = {{"volume", 1.0f, 0.2f}, {"morph", 0.0f, 1.0f}, {"cutoff", 300.0f, 8000.0f}, {"atilt", -12.0f, 12.0f}, {"f1gain", -12.0f, 12.0f},
                            {"partials", 1.0f, 4.0f}, {"ashape", 0.0f, 1.0f}, {"acombdepth", 0.0f, 1.0f}, {"aoddeven", -1.0f, 1.0f}, {"l1amp", 0.0f, 1.0f},
                            {"reso", 0.0f, 18.0f}, {"ftype", 1.0f, 2.0f}, {"prismspread", 0.0f, 1.0f}, {"e2morph", -1.0f, 1.0f}, {"damp", 0.0f, 60.0f}};
    for (auto& th : throws) {
        auto t = softSheaf();
        t->pFilterType->set((float)Sheaf::LowPass);
        t->spec[1].shape->set((float)Sheaf::Triangle);
        t->pPrism->set(std::string(th.id) == "prismspread" ? 2.0f : 1.0f);
        t->pPrismDetune->set(0.0f);
        t->pE2Sustain->set(0.5f);
        t->pFormFreq[0]->set(440.0f);
        Drive e(*t);
        e.on(0, 45, 1);
        for (int q = 0; q < 300; ++q) { t->param(th.id)->set(q % 2 ? th.a : th.b); e.run(kQuantum); }
        CHECK_MSG(maxStep(e.L, 2000, e.L.size()) < 0.035f && maxStep(e.R, 2000, e.R.size()) < 0.035f, "%s thrown about: largest step %.4f", th.id, std::max(maxStep(e.L, 2000, e.L.size()), maxStep(e.R, 2000, e.R.size())));
        CHECK(peakOf(e.L) > 0.02f);
    }
}

TEST(sheaf_scope_shows_the_latest_output) {
    Sheaf s;
    loadPreset(s, "Prism Strings");
    Drive d(s);
    d.on(0, 57, 1);
    d.run(48000);
    float scope[1024];
    CHECK(s.readScope(scope, 1024) == 1024);
    bool same = true;
    for (int i = 0; i < 1024; ++i) same = same && scope[i] == 0.5f * (d.L[d.L.size() - 1024 + (size_t)i] + d.R[d.R.size() - 1024 + (size_t)i]);
    CHECK_MSG(same, "the scope does not hold the last 1024 samples of the output");
}
