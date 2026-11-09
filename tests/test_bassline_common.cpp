// What all five instruments (Squelch, Twine, Low Four, Thud, Clatter) must do: presets that sound,
// sit at a sane level and reload; silence when nothing plays; a clean start after reset and prepare;
// no clicks on retrigger, note-off, kill or a stolen voice; a tail that falls into silence; the same
// samples at any block size and thread count with nothing allocated; and no parameter that can break
// the sound while it is automated.
#include "BasslineTestUtil.h"
#include "pulp/Clatter.h"
#include "pulp/LowFour.h"
#include "pulp/Squelch.h"
#include "pulp/Thud.h"
#include "pulp/Twine.h"
#include <cstdlib>
using namespace pulp;
using namespace bt;

namespace {
bool report() { return std::getenv("BASSLINE_REPORT") != nullptr; }
const double kSr = 48000.0;

struct Kind {
    const char* name;
    const char* typeId;
    const char* display;
    std::function<std::shared_ptr<Plugin>()> make;
};
const std::vector<Kind>& kinds() {
    static const std::vector<Kind> k = {
        {"squelch", "acid", "Squelch", [] { return std::make_shared<Squelch>(); }},
        {"twine", "pluck", "Twine", [] { return std::make_shared<Twine>(); }},
        {"lowfour", "bass", "Low Four", [] { return std::make_shared<LowFour>(); }},
        {"thud", "kick", "Thud", [] { return std::make_shared<Thud>(); }},
        {"clatter", "drumsynth", "Clatter", [] { return std::make_shared<Clatter>(); }},
    };
    return k;
}
NoteEvent onEv(int key, uint32 id, float vel = kDefaultVelocity) {
    NoteEvent e; e.type = NoteEvent::On; e.key = key; e.id = id; e.velocity = vel; return e;
}
// A busy two seconds for any instrument: runs, repeats, overlaps, accents, a slide note, a glide.
void busy(Drive& d, int64 length) {
    uint32 id = 1;
    for (int64 at = 0; at + 9600 < length; at += 3000) {
        const int i = (int)(at / 3000);
        NoteEvent e = onEv(48 + (i * 7) % 24, id, vel127(i % 3 == 0 ? 124 : 80 + (i * 13) % 40));
        e.pan = (float)((i * 5) % 9 - 4) / 4.0f;
        e.modX = 0.2f + 0.15f * (float)(i % 5);
        e.modY = 0.3f + 0.1f * (float)(i % 6);
        if (i % 4 == 3) { e.glideFromKey = (float)(e.key - 5); e.slideSamples = 2400.0f; }
        d.ev(at + (i * 17) % 64, e);
        d.off(at + (i % 2 ? 3600 : 1800), id);          // every second note overlaps the next
        ++id;
        if (i % 5 == 2) { NoteEvent s; s.type = NoteEvent::SlideTo; s.key = 55 + i % 7; s.group = 0; s.slideSamples = 1500.0f; d.ev(at + 900, s); }
    }
}
// Largest sample-to-sample step of (a - b) in [from, to): where the two renders part company, this is
// the size of any jump the event put into the sound.
float diffStep(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to) {
    float m = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < std::min(to, std::min(a.size(), b.size())); ++i)
        m = std::max(m, std::fabs((a[i] - b[i]) - (a[i - 1] - b[i - 1])));
    return m;
}
}

TEST(bassline_every_preset_sounds_sits_at_a_sane_level_and_reloads) {
    for (auto& k : kinds()) {
        auto proto = k.make();
        CHECK(std::string(proto->typeId()) == k.typeId && std::string(proto->displayName()) == k.display && proto->isInstrument());
        CHECK_MSG(groupsCoverParams(*proto), "%s: parameter groups do not cover the parameters exactly once", k.name);
        const auto presets = proto->factoryPresets();
        CHECK_MSG(presets.size() >= 20, "%s has %d presets", k.name, (int)presets.size());
        std::vector<std::string> names;
        std::vector<std::vector<float>> sounds;
        double lo = 1.0e9, hi = -1.0e9;
        for (auto& pr : presets) {
            CHECK_MSG(std::find(names.begin(), names.end(), pr.name) == names.end(), "%s: two presets called %s", k.name, pr.name.c_str());
            names.push_back(pr.name);
            auto a = k.make();
            a->loadState(pr.state);
            // one note, velocity 100, held half a second
            const auto out = oneNote(*a, 60, 24000, 96000, kSr);
            CHECK_MSG(allFinite(out), "%s / %s is not finite", k.name, pr.name.c_str());
            const double pk = db(peakOf(out));
            lo = std::min(lo, pk);
            hi = std::max(hi, pk);
            CHECK_MSG(pk >= -14.0 && pk <= -4.0, "%s / %s peaks at %.2f dBFS", k.name, pr.name.c_str(), pk);
            CHECK_MSG(rmsOf(out, 0, 4800) > 1.0e-3, "%s / %s is all but silent", k.name, pr.name.c_str());
            // saved and loaded into another instance, it is the same state and the same sound, sample for sample
            auto b = k.make();
            b->loadState(a->saveState());
            CHECK(b->saveState() == a->saveState());
            CHECK_MSG(oneNote(*b, 60, 24000, 96000, kSr) == out, "%s / %s sounds different after a reload", k.name, pr.name.c_str());
            // and it is not the same sound as any other preset
            for (size_t i = 0; i < sounds.size(); ++i)
                CHECK_MSG(maxAbsDiff(sounds[i], out) > 1.0e-3, "%s: %s and %s are the same sound", k.name, pr.name.c_str(), names[i].c_str());
            sounds.push_back(out);
        }
        if (report()) printf("    [%s] %d presets, single note at velocity 100 peaks between %.2f and %.2f dBFS\n", k.name, (int)presets.size(), lo, hi);
    }
}

TEST(bassline_silent_without_notes_and_after_a_kill) {
    for (auto& k : kinds()) {
        auto p = k.make();
        Drive d(*p, kSr);
        d.run(48000);
        CHECK_MSG(peakOf(d.L) == 0.0f && peakOf(d.R) == 0.0f && p->activeVoices() == 0, "%s makes sound with no notes", k.name);
        // notes, then Kill: a few milliseconds of fade, then nothing at all, for good
        auto q = k.make();
        Drive e(*q, kSr);
        busy(e, 24000);
        e.type(24000, NoteEvent::Kill);
        e.run(96000);
        CHECK_MSG(peakOf(e.L, 0, 24000) > 0.02f, "%s: the notes made no sound", k.name);
        CHECK_MSG(peakOf(e.L, 24000 + 480, 24000 + 4800) < 2.0e-3f, "%s: 10 ms after Kill it is still at %.5f", k.name, peakOf(e.L, 24000 + 480, 24000 + 4800));
        CHECK_MSG(peakOf(e.L, 60000, 96000) == 0.0f && peakOf(e.R, 60000, 96000) == 0.0f, "%s is not silent after a kill", k.name);
        CHECK_MSG(q->activeVoices() == 0, "%s still counts %d voices", k.name, q->activeVoices());
        // AllOff lets go of everything: silence follows (strings are damped, drums fade)
        auto r = k.make();
        Drive f(*r, kSr);
        busy(f, 24000);
        f.type(24000, NoteEvent::AllOff);
        f.run(480000);
        CHECK_MSG(peakOf(f.L, 400000, 480000) == 0.0f, "%s is not silent 8 s after AllOff (%.6f)", k.name, peakOf(f.L, 400000, 480000));
    }
}

TEST(bassline_reset_and_prepare_start_clean) {
    for (auto& k : kinds()) {
        auto render = [&](Plugin& p, bool prepare) {
            Drive d(p, kSr, prepare);
            busy(d, 48000);
            d.run(72000);
            return std::make_pair(d.L, d.R);
        };
        auto p = k.make();
        const auto first = render(*p, true);
        CHECK(peakOf(first.first) > 0.05f);
        // cut off in the middle of everything, then reset: the next render is the first one again
        {
            Drive d(*p, kSr);
            busy(d, 48000);
            d.run(30016);
            p->reset();
            const auto again = render(*p, false);
            CHECK_MSG(again.first == first.first && again.second == first.second, "%s: after reset() the same notes sound different (by %g)", k.name, maxAbsDiff(again.first, first.first));
        }
        // prepared at another rate, used, and prepared again at the first rate
        {
            Drive other(*p, 96000.0);
            busy(other, 48000);
            other.run(40000);
            CHECK(allFinite(other.L));
            const auto again = render(*p, true);
            CHECK_MSG(again.first == first.first && again.second == first.second, "%s: after prepare() the same notes sound different (by %g)", k.name, maxAbsDiff(again.first, first.first));
        }
        // a level set before the first block is the level from the first sample: no fade in from the default
        {
            auto a = k.make(), b = k.make();
            a->param("volume")->set(0.25f);
            const auto quiet = oneNote(*a, 60, 4800, 9600, kSr);
            const auto normal = oneNote(*b, 60, 4800, 9600, kSr);
            double worst = 0.0;
            for (size_t i = 0; i < quiet.size(); ++i) worst = std::max(worst, (double)std::fabs(quiet[i] - 0.5f * normal[i]));
            CHECK_MSG(worst < 1.0e-6 && peakOf(normal, 0, 2400) > 0.02f, "%s: level 0.25 from the start differs from half of level 0.5 by %g", k.name, worst);
        }
    }
}

TEST(bassline_velocity_and_pan_reach_every_instrument) {
    for (auto& k : kinds()) {
        auto a = k.make(), b = k.make();
        const auto soft = oneNote(*a, 60, 4800, 24000, kSr, vel127(40)), hard = oneNote(*b, 60, 4800, 24000, kSr, vel127(108));
        CHECK_MSG(db(peakOf(hard) / peakOf(soft)) > 3.0, "%s: velocity 108 against 40 is %+.2f dB", k.name, db(peakOf(hard) / peakOf(soft)));
        for (float pan : {-1.0f, 1.0f}) {
            auto p = k.make();
            Drive d(*p, kSr);
            NoteEvent e = onEv(60, 1);
            e.pan = pan;
            d.ev(0, e);
            d.run(12032);
            const float near = peakOf(pan < 0.0f ? d.L : d.R), far = peakOf(pan < 0.0f ? d.R : d.L);
            CHECK_MSG(near > 0.05f && far < 1.0e-6f, "%s: panned %+.0f, the near side peaks at %.3f and the far side at %.6f", k.name, pan, near, far);
        }
        // in the middle both sides are the same (Twine spreads its strings: that is switched off here)
        auto c = k.make();
        if (auto* s = c->param("spread")) s->set(0.0f);
        std::vector<float> r;
        const auto l = oneNote(*c, 60, 4800, 12032, kSr, kDefaultVelocity, &r);
        CHECK_MSG(l == r, "%s: a centred note is not the same on both sides", k.name);
    }
}

TEST(bassline_a_notes_own_fine_pitch_release_and_mod_y_are_heard) {
    // (velocity, pan and mod X are covered above and in each instrument's own tests)
    auto note = [](Plugin& p, const std::function<void(NoteEvent&)>& set, int64 hold, int64 total) {
        Drive d(p, kSr);
        NoteEvent e = onEv(60, 1);
        set(e);
        d.ev(0, e);
        d.off(hold, 1);
        d.run(total);
        return d.L;
    };
    auto lastSound = [](const std::vector<float>& d) { size_t last = 0; for (size_t i = 0; i < d.size(); ++i) if (d[i] != 0.0f) last = i; return last; };
    {   // Squelch: release sets how fast it lets go (4 to 32 ms)
        Squelch a, b;
        const auto quick = note(a, [](NoteEvent& e) { e.release = 0.0f; }, 9600, 24000), slow = note(b, [](NoteEvent& e) { e.release = 1.0f; }, 9600, 24000);
        CHECK_MSG(lastSound(slow) > lastSound(quick) + 480, "squelch: silent %d samples after the note with release 0, %d with release 1", (int)lastSound(quick) - 9600, (int)lastSound(slow) - 9600);
    }
    {   // Twine and Low Four: mod Y is how long the top of the string lasts
        Twine a, b;
        a.pBody->set(0.0f); b.pBody->set(0.0f);
        const auto dull = note(a, [](NoteEvent& e) { e.modY = 0.0f; }, 48000, 48000), ringing = note(b, [](NoteEvent& e) { e.modY = 1.0f; }, 48000, 48000);
        const double sd = bandShare(dull, 14400, 8192, kSr, 1000.0, 20000.0), sr = bandShare(ringing, 14400, 8192, kSr, 1000.0, 20000.0);
        CHECK_MSG(sr > 5.0 * sd, "twine: share above 1 kHz 0.3 s in: %.5f with mod Y 0, %.5f with mod Y 1", sd, sr);
        LowFour c, d;
        const auto bd = note(c, [](NoteEvent& e) { e.modY = 0.0f; }, 48000, 48000), br = note(d, [](NoteEvent& e) { e.modY = 1.0f; }, 48000, 48000);
        const double ld = bandShare(bd, 14400, 8192, kSr, 300.0, 20000.0), lr = bandShare(br, 14400, 8192, kSr, 300.0, 20000.0);
        CHECK_MSG(lr > 3.0 * ld, "lowfour: share above 300 Hz 0.3 s in: %.5f with mod Y 0, %.5f with mod Y 1", ld, lr);
    }
    {   // Thud: fine pitch moves the drum; with "note length cuts the tail", release is how fast
        Thud t;
        t.pClick->set(0.0f);
        t.pDecay->set(4000.0f);
        t.pEnd->set(110.0f);
        const auto out = note(t, [](NoteEvent& e) { e.fineCents = 50.0f; }, 72000, 72000);
        const double want = 110.0 * std::pow(2.0, 0.5 / 12.0);
        CHECK_NEAR(centsOff(preciseHz(out, 12000, kSr, want, 32768, 4096), want), 0.0, 1.0);
        Thud a, b;
        for (Thud* g : {&a, &b}) { g->pGate->set(1.0f); g->pDecay->set(4000.0f); g->pClick->set(0.0f); }
        const auto quick = note(a, [](NoteEvent& e) { e.release = 0.0f; }, 9600, 48000), slow = note(b, [](NoteEvent& e) { e.release = 1.0f; }, 9600, 48000);
        CHECK_MSG(rmsOf(slow, 9600 + 2400, 9600 + 4800) > 10.0 * rmsOf(quick, 9600 + 2400, 9600 + 4800), "thud: 50 to 100 ms after letting go, %.6f with release 0, %.6f with release 1",
                  rmsOf(quick, 9600 + 2400, 9600 + 4800), rmsOf(slow, 9600 + 2400, 9600 + 4800));
    }
    {   // Clatter: fine pitch transposes; mod Y is Colour
        Clatter c;
        c.pModel->set((float)Clatter::Tom);
        c.pDecay->set(1.0f);
        c.pSnap->set(0.0f);
        const auto out = note(c, [](NoteEvent& e) { e.fineCents = 100.0f; }, 2400, 72000);
        const double want = 110.0 * std::pow(2.0, 1.0 / 12.0);
        CHECK_NEAR(centsOff(preciseHz(out, 24000, kSr, want, 16384, 2048), want), 0.0, 2.0);
        Clatter k1, k2;
        k1.pDecay->set(0.8f); k2.pDecay->set(0.8f);
        const auto plain = note(k1, [](NoteEvent& e) { e.modY = 0.0f; }, 2400, 48000), rich = note(k2, [](NoteEvent& e) { e.modY = 1.0f; }, 2400, 48000);
        const double a = ampAt(plain, 9600, 4096, 104.0, kSr) / ampAt(plain, 9600, 4096, 52.0, kSr), b = ampAt(rich, 9600, 4096, 104.0, kSr) / ampAt(rich, 9600, 4096, 52.0, kSr);
        CHECK_MSG(b > 5.0 * a && b > 0.15, "clatter kick: second harmonic %.4f with mod Y 0, %.3f with mod Y 1", a, b);
    }
}

TEST(bassline_no_clicks_on_note_off_kill_retrigger_or_a_stolen_voice) {
    // Every case renders the same thing with and without the event (or, for a steal, the parts on their
    // own) and looks at the largest sample-to-sample step of the difference. The settings are dull
    // on purpose, so that the ordinary waveform moves slowly and a jump would stand out: a voice cut
    // off dead would step by its own amplitude, tens of times the limits used here.
    auto report1 = [&](const char* what, float step, float natural) {
        if (report()) printf("    [clicks] %-44s largest step %.5f (the plain sound's own: %.5f)\n", what, step, natural);
    };
    // ---- Squelch ----
    {
        auto dull = [] { auto s = std::make_shared<Squelch>(); s->pCutoff->set(300.0f); s->pReso->set(0.3f); s->pEnvMod->set(0.2f); s->pDrive->set(0.0f); return s; };
        auto held = dull();
        Drive h(*held, kSr);
        h.on(0, 60, 1);
        h.run(48000);
        const float natural = maxStep(h.L, 2400, 48000);
        auto a = dull(), b = dull(), c = dull(), e = dull();
        Drive off(*a, kSr), kill(*b, kSr), re(*c, kSr), mono(*e, kSr);
        off.on(0, 60, 1); off.off(12007, 1); off.run(48000);
        kill.on(0, 60, 1); kill.type(12007, NoteEvent::Kill); kill.run(48000);
        re.on(0, 60, 1); re.off(12007, 1); re.on(12007 + 190, 67, 2); re.run(48000);        // a new note 4 ms into the release
        mono.on(0, 60, 1); mono.type(12007, NoteEvent::KillNote, 1); mono.on(12007, 55, 2); mono.run(48000);    // what a mono channel sends
        const float s1 = diffStep(off.L, h.L, 12000, 16000), s2 = diffStep(kill.L, h.L, 12000, 16000);
        const float s3 = diffStep(re.L, h.L, 12000, 16000), s4 = diffStep(mono.L, h.L, 12000, 16000);
        CHECK_MSG(s1 < 2.0f * natural, "squelch note-off: step %.5f, natural %.5f", s1, natural);
        CHECK_MSG(s2 < 2.0f * natural, "squelch kill: step %.5f, natural %.5f", s2, natural);
        CHECK_MSG(s3 < 3.0f * natural, "squelch retrigger in the release: step %.5f, natural %.5f", s3, natural);
        CHECK_MSG(s4 < 3.0f * natural, "squelch kill-note and a new note on one sample: step %.5f, natural %.5f", s4, natural);
        CHECK(peakOf(off.L, 16000, 48000) == 0.0f && peakOf(kill.L, 12007 + 200, 48000) == 0.0f && peakOf(mono.L, 20000, 48000) > 0.02f);
        report1("squelch note-off", s1, natural); report1("squelch kill", s2, natural);
        report1("squelch new note 4 ms into the release", s3, natural); report1("squelch kill-note + note on one sample", s4, natural);
    }
    // ---- the two string instruments and the two drum instruments: off / kill, and a stolen voice ----
    struct Poly {
        const char* name;
        std::function<std::shared_ptr<Plugin>()> dull;
        int sounding;       // notes before one is stolen
        int spacing;        // samples between them
        bool offDoes;       // whether a note-off does anything
    };
    const std::vector<Poly> polys = {
        {"twine", [] { auto t = std::make_shared<Twine>(); t->pBright->set(0.15f); t->pNoise->set(0.0f); t->pBody->set(0.0f); t->pSpread->set(0.0f); t->pDecay->set(8.0f); t->pRelease->set(100.0f);
                       return std::static_pointer_cast<Plugin>(t); }, Twine::kSounding, 600, true},
        {"lowfour", [] { auto b = std::make_shared<LowFour>(); b->pBite->set(0.0f); b->pTone->set(0.4f); b->pSustain->set(0.9f); b->pOctave->set(-1.0f);
                         return std::static_pointer_cast<Plugin>(b); }, LowFour::kSounding, 1200, true},
        {"thud", [] { auto t = std::make_shared<Thud>(); t->pClick->set(0.0f); t->pDrive->set(0.0f); t->pDecay->set(3000.0f); t->pChoke->set(0.0f); t->pGate->set(1.0f);
                      return std::static_pointer_cast<Plugin>(t); }, Thud::kSounding, 2400, true},
        {"clatter", [] { auto c = std::make_shared<Clatter>(); c->pModel->set((float)Clatter::Tom); c->pNoise->set(0.0f); c->pSnap->set(0.0f); c->pDecay->set(1.0f);
                         return std::static_pointer_cast<Plugin>(c); }, Clatter::kSounding, 900, false},
    };
    for (auto& p : polys) {
        auto held = p.dull();
        Drive h(*held, kSr);
        h.on(0, 55, 1);
        h.run(48000);
        const float natural = maxStep(h.L, 2400, 24000);
        auto a = p.dull(), b = p.dull(), c = p.dull();
        Drive off(*a, kSr), kill(*b, kSr), all(*c, kSr);
        off.on(0, 55, 1); off.off(6007, 1); off.run(48000);
        kill.on(0, 55, 1); kill.type(6007, NoteEvent::Kill); kill.run(48000);
        all.on(0, 55, 1); all.type(6007, NoteEvent::AllOff); all.run(48000);
        const float s1 = diffStep(off.L, h.L, 6000, 12000), s2 = diffStep(kill.L, h.L, 6000, 12000), s3 = diffStep(all.L, h.L, 6000, 12000);
        CHECK_MSG(s1 < 2.0f * natural + 1.0e-5f, "%s note-off: step %.5f, natural %.5f", p.name, s1, natural);
        CHECK_MSG(s2 < 2.0f * natural + 1.0e-5f, "%s kill: step %.5f, natural %.5f", p.name, s2, natural);
        CHECK_MSG(s3 < 2.0f * natural + 1.0e-5f, "%s all-off: step %.5f, natural %.5f", p.name, s3, natural);
        CHECK_MSG(peakOf(kill.L, 6007 + 1440, 48000) < 1.0e-4f && peakOf(kill.L, 24000, 48000) == 0.0f, "%s: still sounding after a kill (%.6f)", p.name, peakOf(kill.L, 6007 + 1440, 48000));
        if (p.offDoes) CHECK_MSG(rmsOf(off.L, 20000, 24000) < 0.05 * rmsOf(h.L, 20000, 24000), "%s: the note-off did nothing", p.name);
        else CHECK_MSG(off.L == h.L, "%s: a drum should ring through its note-off", p.name);
        report1((std::string(p.name) + " note-off").c_str(), s1, natural);
        report1((std::string(p.name) + " kill").c_str(), s2, natural);

        // one note more than can sound: the oldest is stolen. The difference from (the others) + (the new note
        // alone) is just the stolen voice going away, and it must go smoothly.
        const int n = p.sounding;
        const int64 at = (int64)n * p.spacing + 37, len = at + 24000;
        auto full = p.dull(), others = p.dull(), extra = p.dull();
        Drive df(*full, kSr), dn(*others, kSr), dx(*extra, kSr);
        for (int i = 0; i < n; ++i) { df.on((int64)i * p.spacing, 40 + i, (uint32)(i + 1)); dn.on((int64)i * p.spacing, 40 + i, (uint32)(i + 1)); }
        df.on(at, 40 + n, (uint32)(n + 1));
        dx.on(at, 40 + n, (uint32)(n + 1));
        int most = 0;
        df.each = [&](int64) { most = std::max(most, full->activeVoices()); };
        df.run(len); dn.run(len); dx.run(len);
        std::vector<float> parts(df.L.size());
        for (size_t i = 0; i < parts.size(); ++i) parts[i] = dn.L[i] + dx.L[i];
        const float stolenLevel = peakOf(dn.L, (size_t)at - 600, (size_t)at);
        const float s4 = diffStep(df.L, parts, (size_t)at - 8, (size_t)at + 2400);
        const float naturalAll = maxStep(dn.L, (size_t)at - 2400, (size_t)at);
        CHECK_MSG(most == n + 1, "%s: %d voices at the steal, expected %d sounding and one fading", p.name, most, n);
        CHECK_MSG(full->activeVoices() == n, "%s: %d voices after the steal", p.name, full->activeVoices());
        CHECK_MSG(s4 < 2.0f * naturalAll + 1.0e-5f && stolenLevel > 0.01f, "%s steal: step %.5f, natural %.5f, level before %.3f", p.name, s4, naturalAll, stolenLevel);
        // and the stolen note is really gone: the full render ends up as the others without their first note
        report1((std::string(p.name) + " stolen voice").c_str(), s4, naturalAll);
    }
    // the same string plucked again (Twine, Low Four): the old note is faded over 15 ms, not cut
    for (int which = 0; which < 2; ++which) {
        auto& p = polys[(size_t)which];
        auto two = p.dull(), first = p.dull(), second = p.dull();
        Drive d2(*two, kSr), d0(*first, kSr), d1(*second, kSr);
        d2.on(0, 55, 1); d2.on(9001, 55, 2);
        d0.on(0, 55, 1);
        d1.on(9001, 55, 2);
        d2.run(24000); d0.run(24000); d1.run(24000);
        std::vector<float> parts(d2.L.size());
        for (size_t i = 0; i < parts.size(); ++i) parts[i] = d0.L[i] + d1.L[i];
        const float s = diffStep(d2.L, parts, 8990, 12000), natural = maxStep(d0.L, 2400, 9000);
        CHECK_MSG(s < 2.0f * natural + 1.0e-5f, "%s same key again: step %.5f, natural %.5f", p.name, s, natural);
        CHECK(two->activeVoices() == 1);
        report1((std::string(p.name) + " same key again").c_str(), s, natural);
    }
}

TEST(bassline_tails_fall_into_silence_and_stay_there) {
    for (auto& k : kinds()) {
        auto p = k.make();
        Drive d(*p, kSr);
        d.on(0, 60, 1);
        d.off(12000, 1);
        d.run(480000);          // ten seconds: long enough for every default patch to finish
        size_t last = 0;
        for (size_t i = 0; i < d.L.size(); ++i) if (d.L[i] != 0.0f || d.R[i] != 0.0f) last = i;
        CHECK_MSG(last > 2400 && last < 470000, "%s: last sounding sample at %d", k.name, (int)last);
        const float tail = std::max(peakOf(d.L, last - 64, last + 1), peakOf(d.R, last - 64, last + 1));
        CHECK_MSG(tail < 3.0e-5f, "%s: the last 64 samples before silence peak at %g (-90 dBFS is 3.2e-5)", k.name, tail);
        CHECK(p->activeVoices() == 0);
        if (report()) printf("    [%s] default patch: last non-zero sample %.3f s after a 0.25 s note, final 64 samples peak at %.1f dBFS\n", k.name, (double)last / kSr, db(tail));
    }
}

TEST(bassline_block_size_and_thread_count_do_not_change_a_sample_and_nothing_allocates) {
    Project p;
    p.global->tempo.set(120.0f);
    auto squelch = std::make_shared<Squelch>();
    auto twine = std::make_shared<Twine>();
    auto bass = std::make_shared<LowFour>();
    auto thud = std::make_shared<Thud>();
    auto kit = std::make_shared<Clatter>();
    kit->pModel->set((float)Clatter::Kit);
    twine->pStiff->set(0.3f);
    thud->pDecay->set(1500.0f);
    thud->pDrive->set(0.5f);
    std::shared_ptr<Plugin> all[5] = {squelch, twine, bass, thud, kit};
    const char* names[5] = {"Squelch", "Twine", "Low Four", "Thud", "Clatter"};
    int ids[5];
    for (int i = 0; i < 5; ++i) {
        auto& c = p.addChannel(all[i], names[i]);
        ids[i] = c.id;
        c.mixerTrack = i + 1;           // five mixer tracks, so worker threads have something to share out
    }
    {   // Squelch: a sixteenth-note line with overlaps (slides), accents, a slide note and a portamento note
        auto& n = p.pattern().notesFor(ids[0]);
        for (int i = 0; i < 28; ++i) {
            Note x = mk(i * 240 + (i * 7) % 11, (i % 3 == 0) ? 300 : 130, 36 + (i * 5) % 19, vel127(i % 4 == 1 ? 125 : 96));
            x.modX = 0.3f + 0.05f * (float)(i % 6);
            x.porta = i % 9 == 8;
            n.push_back(x);
        }
        Note s = mk(1300, 400, 55);
        s.slide = true;
        n.push_back(s);
        sortNotes(n);
    }
    {   // Twine: more notes than voices, all over the keyboard
        auto& n = p.pattern().notesFor(ids[1]);
        for (int i = 0; i < 64; ++i) {
            Note x = mk(i * 97 + 3, 900, 36 + (i * 7) % 48, 0.3f + 0.011f * (float)i);
            x.pan = (float)(i % 5 - 2) / 2.0f;
            x.fine = (float)((i * 13) % 40 - 20);
            n.push_back(x);
        }
        Note s = mk(2000, 600, 67);
        s.slide = true;
        n.push_back(s);
        sortNotes(n);
    }
    {   // Low Four: a bass line with a glide
        auto& n = p.pattern().notesFor(ids[2]);
        const int keys[8] = {60, 60, 63, 65, 67, 60, 58, 55};
        for (int i = 0; i < 16; ++i) {
            Note x = mk(i * 480 + 5, i % 4 == 3 ? 520 : 300, keys[i % 8], vel127(90 + (i * 9) % 30));
            x.porta = i % 5 == 4;
            n.push_back(x);
        }
        sortNotes(n);
    }
    {   // Thud: four on the floor on a long driven tail, with a slide note
        auto& n = p.pattern().notesFor(ids[3]);
        for (int i = 0; i < 8; ++i) n.push_back(mk(i * 960 + 1, 200, 60 + (i == 5 ? 3 : 0)));
        Note s = mk(4000, 500, 65);
        s.slide = true;
        n.push_back(s);
        sortNotes(n);
    }
    {   // Clatter: a whole kit, hats closing on open hats, a roll
        auto& n = p.pattern().notesFor(ids[4]);
        for (int i = 0; i < 32; ++i) n.push_back(mk(i * 240 + 2, 100, i % 4 == 2 ? 70 : 66, vel127(70 + (i * 11) % 50)));
        for (int i = 0; i < 8; ++i) n.push_back(mk(i * 960, 100, i % 2 ? 62 : 60));
        for (int i = 0; i < 12; ++i) n.push_back(mk(6000 + i * 60, 50, 62 + (i % 3 == 0 ? 2 : 0)));
        for (int k : {61, 63, 65, 67, 68, 69, 71}) n.push_back(mk(480 * (k - 60), 100, k));
        sortNotes(n);
    }
    std::shared_ptr<SampleData> ref;
    for (auto cs : std::vector<std::pair<int, int>>{{512, 1}, {64, 1}, {33, 1}, {4096, 1}, {4096, 4}, {512, 4}, {512, 1}}) {
        RenderOptions o;
        o.loop = true;
        o.lengthSamples = 48000 * 5;
        o.blockSize = cs.first;
        o.threads = cs.second;
        const int64 allocs = Rt::allocationsInRt.load();
        auto r = renderOffline(p, o);
        CHECK_MSG(Rt::allocationsInRt.load() == allocs, "block %d threads %d: %d allocations on the audio thread", cs.first, cs.second, (int)(Rt::allocationsInRt.load() - allocs));
        CHECK(r != nullptr);
        if (!r) return;
        if (!ref) {
            ref = r;
            CHECK(allFinite(r->ch[0]) && allFinite(r->ch[1]));
            CHECK_MSG(peakOf(r->ch[0]) > 0.2f && peakOf(r->ch[0]) < 4.0f, "peak %.3f", peakOf(r->ch[0]));
            continue;
        }
        CHECK_MSG(identical(*ref, *r), "block %d threads %d differs (by %g)", cs.first, cs.second, maxAbsDiff(ref->ch[0], r->ch[0]));
    }
    // each on its own, through the engine, is what the plugin gives when driven directly
    // (channel volume and master are at unity): the engine adds and takes nothing
    for (int i = 0; i < 5; ++i) CHECK(all[i]->activeVoices() >= 0);
    if (report()) printf("    [all five] 5 s of the five instruments on five mixer tracks: identical at blocks 64 / 33 / 512 / 4096 and 1 or 4 threads, 0 allocations; peak %.2f dBFS\n", db(peakOf(ref->ch[0])));
}

TEST(bassline_automating_any_parameter_never_breaks_the_sound) {
    // Every parameter in turn is thrown between its extremes while notes play: flipped end to end every
    // quantum, then swept, then jumped at random. Then all of them at once. Nothing may go non-finite or
    // run away, and when the notes are killed the instrument must fall silent.
    double worstPeak = 0.0;
    int runs = 0;
    for (auto& k : kinds()) {
        auto proto = k.make();
        const size_t count = proto->params().size();
        for (size_t pi = 0; pi <= count; ++pi) {
            auto p = k.make();
            Drive d(*p, kSr);
            const int64 len = pi == count ? 192000 : 72000;
            busy(d, len - 24000);           // the last half second is for the kill and the silence after it
            uint32 rng = 0x1234567u + (uint32)pi * 7919u;
            auto rnd = [&] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (float)(rng & 0xFFFFFF) / (float)0x1000000; };
            d.each = [&](int64 pos) {
                const int q = (int)(pos / kQuantum);
                if (pi < count) {
                    Param* prm = p->params()[pi].get();
                    const int phase = (int)(pos * 3 / len);
                    if (phase == 0) prm->setNorm(q % 2 ? 1.0f : 0.0f);
                    else if (phase == 1) prm->setNorm(0.5f - 0.5f * (float)std::cos(kTwoPi * (double)pos / 9000.0));
                    else prm->setNorm(rnd());
                } else {
                    for (auto& prm : p->params()) if (rnd() < 0.5f) prm->setNorm(rnd());
                }
            };
            d.type(len - 24000, NoteEvent::Kill);
            d.run(len);
            ++runs;
            const char* what = pi < count ? proto->params()[pi]->info.id.c_str() : "everything at once";
            CHECK_MSG(allFinite(d.L) && allFinite(d.R), "%s / %s: not finite", k.name, what);
            const float pk = std::max(peakOf(d.L), peakOf(d.R));
            worstPeak = std::max(worstPeak, (double)pk);
            if (report() && pk > 1.0f) printf("    [%s] automating %s: peak %.2f dBFS\n", k.name, what, db(pk));
            CHECK_MSG(pk < 4.0f, "%s / %s: peak %.2f", k.name, what, pk);
            CHECK_MSG(peakOf(d.L, (size_t)len - 4800, (size_t)len) < 1.0e-5f, "%s / %s: still sounding after the kill (%.6f)", k.name, what, peakOf(d.L, (size_t)len - 4800, (size_t)len));
        }
    }
    if (report()) printf("    [all five] %d automation runs (every parameter of every instrument, and all at once): highest peak %.2f dBFS\n", runs, db(worstPeak));
}

TEST(bassline_oversampled_drive_stage_does_not_alias) {
    // Low Four's amplifier stage in isolation (its strings are inharmonic by design, so aliasing cannot
    // be read off the whole instrument): a sine up to twice the rate, tanh at the hardest setting the Bite
    // knob gives, and back down.
    double worst = -200.0;
    for (double sr : {44100.0, 48000.0})
        for (double hz : {997.0, 3001.0, 5003.0}) {
            bl::Up2 up;
            bl::Down2 down;
            up.prepare(sr);
            down.prepare(sr);
            std::vector<float> out(65536);
            for (size_t i = 0; i < out.size(); ++i) {
                float a, b;
                up.process(0.7f * (float)std::sin(kTwoPi * hz * (double)i / sr), a, b);
                out[i] = down.process(std::tanh(3.2f * a), std::tanh(3.2f * b));
            }
            const double al = aliasDb(out, 8192, 32768, sr, hz, 20.0, 16000.0);
            worst = std::max(worst, al);
            CHECK_MSG(al < -60.0, "rate %.0f, %.0f Hz: %.1f dB between the harmonics", sr, hz, al);
            // and it passes the tone itself: unity for small signals
            bl::Up2 u2;
            bl::Down2 d2;
            u2.prepare(sr);
            d2.prepare(sr);
            std::vector<float> lin(32768);
            for (size_t i = 0; i < lin.size(); ++i) {
                float a, b;
                u2.process(0.1f * (float)std::sin(kTwoPi * hz * (double)i / sr), a, b);
                lin[i] = d2.process(a, b);
            }
            CHECK_NEAR(db(ampAt(lin, 8192, 16384, hz, sr) / 0.1), 0.0, 0.05);
        }
    if (report()) printf("    [lowfour] amplifier stage alone, 0.7 sine at 1 / 3 / 5 kHz into tanh at gain 3.2, 44.1 and 48 kHz: worst alias %.1f dB\n", worst);
}
