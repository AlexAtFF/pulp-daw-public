// Clatter (drum synthesizer): every model sounds as it should, the shared controls mean something in
// each, pitched models are in tune, hits overlap, a closed hat chokes an open one, Kit mode maps keys.
#include "BasslineTestUtil.h"
#include "pulp/Clatter.h"
#include <cstdlib>
using namespace pulp;
using namespace bt;

namespace {
bool report() { return std::getenv("BASSLINE_REPORT") != nullptr; }
const double kSr = 48000.0;

const size_t kLead = 8192;      // silence before every hit, so that analysis windows can be centred on its start

std::vector<float> hit(int model, const std::function<void(Clatter&)>& set = {}, int key = 60, double seconds = 4.0, double sr = kSr, float vel = kDefaultVelocity) {
    Clatter c;
    c.pModel->set((float)model);
    if (set) set(c);
    Drive d(c, sr);
    d.on((int64)kLead, key, 1, vel);
    d.off((int64)kLead + 2400, 1);
    d.run((int64)kLead + (int64)(seconds * sr));
    return d.L;
}
// Spectral measures over a window centred on the start of the hit.
double cen(const std::vector<float>& d, int n = 16384) { return centroidHz(d, kLead - (size_t)n / 2, n, kSr); }
double share(const std::vector<float>& d, double lo, double hi, int n = 16384) { return bandShare(d, kLead - (size_t)n / 2, n, kSr, lo, hi); }
// ... and measures a given number of samples into the hit.
double pkHz(const std::vector<float>& d, size_t into, int n, double lo, double hi) { return peakHz(d, kLead + into, n, kSr, lo, hi); }
double amp(const std::vector<float>& d, size_t into, int n, double hz) { return ampAt(d, kLead + into, n, hz, kSr); }
double len(const std::vector<float>& d, double sr = kSr) { return lengthSeconds(d, sr) - (double)kLead / sr; }
// When the envelope (peak over 1 ms windows) is at its highest, in seconds.
double peakTime(const std::vector<float>& d, double sr) {
    const size_t w = (size_t)(0.001 * sr);
    float best = 0.0f;
    size_t at = 0;
    for (size_t i = 0; i + w <= d.size(); i += w) { const float p = peakOf(d, i, i + w); if (p > best) { best = p; at = i; } }
    return (double)at / sr - (double)kLead / sr;
}
// Number of separate peaks of the envelope (1 ms windows) in the first `seconds`, each at least 6 dB above the dip before it.
int bursts(const std::vector<float>& d, double sr, double seconds) {
    const size_t w = (size_t)(0.001 * sr);
    std::vector<float> env;
    for (size_t i = kLead; i + w <= kLead + (size_t)(seconds * sr); i += w) env.push_back(peakOf(d, i, i + w));
    int n = 0;
    float low = 0.0f;
    bool rising = true;
    for (size_t i = 1; i < env.size(); ++i) {
        if (rising && env[i] < env[i - 1]) { if (env[i - 1] > 2.0f * low) ++n; rising = false; low = env[i]; }
        else if (!rising) { if (env[i] < low) low = env[i]; if (env[i] > 1.6f * low) rising = true; }
    }
    return n;
}
}

TEST(clatter_every_model_has_its_character_and_length) {
    struct Row { const char* name; double centroid, above4k, below300, length, peak; };
    Row r[Clatter::Kit];
    static const char* names[Clatter::Kit] = {"kick", "snare", "clap", "closed hat", "open hat", "tom", "cymbal", "rimshot", "cowbell", "clave", "shaker"};
    for (int m = 0; m < Clatter::Kit; ++m) {
        const auto out = hit(m);
        r[m] = {names[m], cen(out), share(out, 4000.0, 24000.0), share(out, 0.0, 300.0), len(out), db(peakOf(out))};
        CHECK_MSG(allFinite(out) && peakOf(out) > 0.1f && peakOf(out) < 1.0f, "%s peaks at %.3f", names[m], peakOf(out));
        CHECK_MSG(peakOf(out, out.size() - 4800, out.size()) == 0.0f, "%s is still sounding after 4 s", names[m]);
        if (report()) printf("    [clatter] %-10s centroid %6.0f Hz, %5.1f %% above 4 kHz, %5.1f %% below 300 Hz, %.3f s to -40 dB, peak %.1f dBFS\n",
                             names[m], r[m].centroid, 100.0 * r[m].above4k, 100.0 * r[m].below300, r[m].length, r[m].peak);
    }
    auto& k = r[Clatter::Kick]; auto& sn = r[Clatter::Snare]; auto& cl = r[Clatter::Clap]; auto& ch = r[Clatter::ClosedHat]; auto& oh = r[Clatter::OpenHat];
    auto& tm = r[Clatter::Tom]; auto& cy = r[Clatter::Cymbal]; auto& rim = r[Clatter::Rimshot]; auto& cb = r[Clatter::Cowbell]; auto& cv = r[Clatter::Clave]; auto& sh = r[Clatter::Shaker];
    // kick: nearly everything at the bottom, a few hundred milliseconds
    CHECK_MSG(k.below300 > 0.9 && k.length > 0.12 && k.length < 0.6, "kick: %.3f below 300 Hz, %.3f s", k.below300, k.length);
    // snare: a shell and wires: real energy both below 300 Hz and above 1 kHz
    {
        const auto out = hit(Clatter::Snare);
        const double wires = share(out, 1000.0, 24000.0);
        CHECK_MSG(sn.below300 > 0.15 && wires > 0.15 && sn.length > 0.05 && sn.length < 0.4, "snare: %.3f below 300 Hz, %.3f above 1 kHz, %.3f s", sn.below300, wires, sn.length);
        CHECK_MSG(std::fabs(pkHz(out, 960, 8192, 100.0, 300.0) / 185.0 - 1.0) < 0.06, "snare shell at %.0f Hz", pkHz(out, 960, 8192, 100.0, 300.0));
    }
    // clap: mid-band noise, in several bursts
    {
        const auto out = hit(Clatter::Clap);
        CHECK_MSG(cl.centroid > 700.0 && cl.centroid < 3500.0 && cl.below300 < 0.1 && cl.length > 0.06 && cl.length < 0.5, "clap: centroid %.0f Hz, %.3f s", cl.centroid, cl.length);
        CHECK_MSG(bursts(out, kSr, 0.045) >= 3, "clap: %d bursts in the first 45 ms", bursts(out, kSr, 0.045));
    }
    // hats: all top; the closed one is over at once, the open one rings
    CHECK_MSG(ch.above4k > 0.85 && ch.length < 0.1, "closed hat: %.3f above 4 kHz, %.3f s", ch.above4k, ch.length);
    CHECK_MSG(oh.above4k > 0.85 && oh.length > 0.2 && oh.length > 4.0 * ch.length, "open hat: %.3f above 4 kHz, %.3f s", oh.above4k, oh.length);
    // tom: a pitched drum an octave above the kick
    {
        const auto out = hit(Clatter::Tom);
        CHECK_MSG(tm.below300 > 0.8 && tm.length > 0.12 && tm.length < 0.7, "tom: %.3f below 300 Hz, %.3f s", tm.below300, tm.length);
        CHECK_MSG(std::fabs(pkHz(out, 4800, 8192, 60.0, 400.0) / 110.0 - 1.0) < 0.06, "tom at %.0f Hz", pkHz(out, 4800, 8192, 60.0, 400.0));
    }
    // cymbal: all top, and long
    CHECK_MSG(cy.above4k > 0.65 && cy.length > 0.6 && cy.length > 2.0 * oh.length, "cymbal: %.3f above 4 kHz, %.3f s", cy.above4k, cy.length);
    // rimshot: a short crack with two pitched modes
    {
        const auto out = hit(Clatter::Rimshot);
        CHECK_MSG(rim.length < 0.06 && rim.centroid > 400.0, "rimshot: %.3f s, centroid %.0f Hz", rim.length, rim.centroid);
        const double lo = pkHz(out, 0, 2048, 300.0, 700.0), hi = pkHz(out, 0, 2048, 1000.0, 2500.0);
        CHECK_MSG(std::fabs(lo / 470.0 - 1.0) < 0.06 && std::fabs(hi / (470.0 * 3.08) - 1.0) < 0.06, "rimshot modes at %.0f and %.0f Hz", lo, hi);
    }
    // cowbell: two partials about a fifth apart, in the middle
    {
        const auto out = hit(Clatter::Cowbell);
        const double lo = pkHz(out, 480, 8192, 400.0, 660.0), hi = pkHz(out, 480, 8192, 660.0, 1000.0);
        CHECK_MSG(std::fabs(lo / 545.0 - 1.0) < 0.03 && std::fabs(hi / (545.0 * 1.44) - 1.0) < 0.03, "cowbell partials at %.0f and %.0f Hz", lo, hi);
        CHECK_MSG(cb.centroid > 500.0 && cb.centroid < 3000.0 && cb.length > 0.05 && cb.length < 0.5, "cowbell: centroid %.0f Hz, %.3f s", cb.centroid, cb.length);
    }
    // clave: one high pitched click
    {
        const auto out = hit(Clatter::Clave);
        CHECK_MSG(std::fabs(pkHz(out, 0, 2048, 1500.0, 4000.0) / 2450.0 - 1.0) < 0.03 && cv.length < 0.1, "clave at %.0f Hz, %.3f s", pkHz(out, 0, 2048, 1500.0, 4000.0), cv.length);
    }
    // shaker: high noise that swells in rather than striking
    {
        const auto out = hit(Clatter::Shaker);
        CHECK_MSG(sh.centroid > 3000.0 && sh.below300 < 0.02 && sh.length > 0.04 && sh.length < 0.4, "shaker: centroid %.0f Hz, %.3f s", sh.centroid, sh.length);
        CHECK_MSG(peakTime(out, kSr) > 0.004, "shaker peaks %.1f ms in", 1000.0 * peakTime(out, kSr));
        CHECK(peakTime(hit(Clatter::ClosedHat), kSr) < 0.003);
    }
}

TEST(clatter_shared_controls_mean_something_in_every_model) {
    double shortest[Clatter::Kit], longest[Clatter::Kit];
    for (int m = 0; m < Clatter::Kit; ++m) {
        // Decay: longer in every model
        const auto a = hit(m, [](Clatter& c) { c.pDecay->set(0.1f); }), b = hit(m, [](Clatter& c) { c.pDecay->set(0.9f); }, 60, 9.0);
        shortest[m] = len(a);
        longest[m] = len(b);
        CHECK_MSG(longest[m] > 1.5 * shortest[m], "model %d: %.3f s at decay 0.1, %.3f s at 0.9", m, shortest[m], longest[m]);
        CHECK(peakOf(b, b.size() - 4800, b.size()) == 0.0f);
        // Tone: brighter in every model
        const auto dull = hit(m, [](Clatter& c) { c.pTone->set(0.05f); c.pNoise->set(0.6f); }), bright = hit(m, [](Clatter& c) { c.pTone->set(0.95f); c.pNoise->set(0.6f); });
        CHECK_MSG(cen(bright) > 1.08 * cen(dull), "model %d: centroid %.0f Hz at tone 0.05, %.0f Hz at 0.95", m, cen(dull), cen(bright));
        // Pitch and the key both transpose: an octave up moves the centroid up (by an octave for the pitched ones)
        const auto low = hit(m, [](Clatter& c) { c.pPitch->set(-12.0f); }), high = hit(m, [](Clatter& c) { c.pPitch->set(12.0f); });
        const auto keyHigh = hit(m, [](Clatter& c) { c.pPitch->set(0.0f); }, 72);
        CHECK_MSG(cen(high) > 1.15 * cen(low), "model %d: centroid %.0f Hz at -12, %.0f Hz at +12", m, cen(low), cen(high));
        // noise in the models is seeded by the note, so the same note at key 72 and at pitch +12 is the same sound
        CHECK_MSG(maxAbsDiff(high, keyHigh) < 1.0e-6, "model %d: key 72 and pitch +12 differ by %g", m, maxAbsDiff(high, keyHigh));
        // Snap, Noise and Colour each change the sound
        const auto base = hit(m);
        for (Param* Clatter::* which : {&Clatter::pSnap, &Clatter::pNoise, &Clatter::pColour}) {
            const auto lo = hit(m, [&](Clatter& c) { (c.*which)->set(0.0f); }), hi = hit(m, [&](Clatter& c) { (c.*which)->set(1.0f); });
            CHECK_MSG(maxAbsDiff(lo, hi) > 0.02, "model %d: a control does nothing (difference %g)", m, maxAbsDiff(lo, hi));
        }
        (void)base;
    }
    if (report()) {
        printf("    [clatter] length to -40 dB at decay 0.1 -> 0.9:");
        for (int m = 0; m < Clatter::Kit; ++m) printf(" %.3f->%.2f", shortest[m], longest[m]);
        printf(" s\n");
    }
    // a few of those meanings, measured
    {   // snare: Noise is the wires against the shell
        const auto dry = hit(Clatter::Snare, [](Clatter& c) { c.pNoise->set(0.0f); }), wet = hit(Clatter::Snare, [](Clatter& c) { c.pNoise->set(1.0f); });
        CHECK(share(wet, 1000.0, 24000.0) > 3.0 * share(dry, 1000.0, 24000.0));
    }
    {   // kick: Snap is how far the pitch drops (more top at the very start), Colour adds harmonics to the body
        const auto soft = hit(Clatter::Kick, [](Clatter& c) { c.pSnap->set(0.0f); c.pNoise->set(0.0f); }), hard = hit(Clatter::Kick, [](Clatter& c) { c.pSnap->set(1.0f); c.pNoise->set(0.0f); });
        CHECK(cen(hard, 2048) > 1.5 * cen(soft, 2048));
        const auto pure = hit(Clatter::Kick, [](Clatter& c) { c.pColour->set(0.0f); }), rich = hit(Clatter::Kick, [](Clatter& c) { c.pColour->set(1.0f); });
        const double a = amp(pure, 9600, 4096, 104.0) / amp(pure, 9600, 4096, 52.0), b = amp(rich, 9600, 4096, 104.0) / amp(rich, 9600, 4096, 52.0);
        CHECK_MSG(a < 0.03 && b > 0.25, "second harmonic %.4f plain, %.3f with colour", a, b);
    }
    {   // cowbell: Colour is the interval between its two partials
        const auto narrow = hit(Clatter::Cowbell, [](Clatter& c) { c.pColour->set(0.0f); }), wide = hit(Clatter::Cowbell, [](Clatter& c) { c.pColour->set(1.0f); });
        CHECK_NEAR(pkHz(narrow, 480, 8192, 660.0, 1000.0) / 545.0, 1.35, 0.03);
        CHECK_NEAR(pkHz(wide, 480, 8192, 660.0, 1000.0) / 545.0, 1.65, 0.03);
    }
    {   // shaker: Snap shortens the swell; clap: Colour spreads the bursts
        const auto slow = hit(Clatter::Shaker, [](Clatter& c) { c.pSnap->set(0.0f); }), fast = hit(Clatter::Shaker, [](Clatter& c) { c.pSnap->set(1.0f); });
        CHECK_MSG(peakTime(slow, kSr) > 2.0 * peakTime(fast, kSr) && peakTime(slow, kSr) > 0.015, "shaker peaks at %.1f and %.1f ms", 1000.0 * peakTime(slow, kSr), 1000.0 * peakTime(fast, kSr));
    }
    {   // velocity, pan, a note's mod X (tone) and mod Y (colour)
        Clatter c;
        c.pModel->set((float)Clatter::Tom);
        const auto soft = oneNote(c, 60, 2400, 48000, kSr, vel127(50)), hard = oneNote(c, 60, 2400, 48000, kSr, vel127(127));
        CHECK_MSG(db(peakOf(hard) / peakOf(soft)) > 8.0, "velocity 127 against 50: %+.1f dB", db(peakOf(hard) / peakOf(soft)));
        Drive d(c, kSr);
        NoteEvent e; e.type = NoteEvent::On; e.key = 60; e.id = 1; e.pan = -1.0f;
        d.ev(100, e);
        d.run(9600);
        CHECK(peakOf(d.R) == 0.0f && peakOf(d.L, 0, 100) == 0.0f && peakOf(d.L, 100, 110) > 0.0f);
        Clatter h;
        h.pModel->set((float)Clatter::ClosedHat);
        Drive lo(h, kSr), hi(h, kSr);
        e.pan = 0.0f; e.modX = 0.0f;
        lo.ev(0, e); lo.run(9600);
        e.modX = 1.0f;
        hi.ev(0, e); hi.run(9600);
        CHECK(centroidHz(hi.L, 0, 2048, kSr) > 1.1 * centroidHz(lo.L, 0, 2048, kSr));
    }
}

TEST(clatter_pitched_models_are_in_tune_at_every_rate) {
    double worst = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int key : {48, 60, 67, 72}) {
            const double r = std::pow(2.0, (key - 60) / 12.0);
            struct Case { int model; double hz; double at; int win; };
            // long decays, measured after the pitch envelope has settled
            const Case cases[] = {{Clatter::Kick, 52.0, 0.45, 32768}, {Clatter::Tom, 110.0, 0.5, 16384}, {Clatter::Clave, 2450.0, 0.02, 2048}, {Clatter::Rimshot, 470.0, 0.02, 2048}};
            for (auto& c : cases) {
                const auto out = hit(c.model, [](Clatter& d) { d.pDecay->set(1.0f); d.pNoise->set(0.0f); d.pSnap->set(0.0f); d.pColour->set(0.0f); }, key, 2.0, sr);
                const int win = sr > 50000.0 ? c.win * 2 : c.win;
                const double want = c.hz * r;
                const double cents = centsOff(preciseHz(out, kLead + (size_t)(c.at * sr), sr, want, win, win / 8), want);
                worst = std::max(worst, std::fabs(cents));
                CHECK_MSG(std::fabs(cents) < 2.0, "rate %.0f model %d key %d: %.3f cents from %.2f Hz", sr, c.model, key, cents, want);
            }
        }
    if (report()) printf("    [clatter] kick, tom, clave and rimshot at keys 48 / 60 / 67 / 72 and three rates: worst %.4f cents\n", worst);
    // the channel pitch bends a sounding drum
    Clatter c;
    c.pModel->set((float)Clatter::Tom);
    c.pDecay->set(1.0f);
    c.pSnap->set(0.0f);
    Drive d(c, kSr);
    d.bend = 3.0f;
    d.on(0, 60, 1);
    d.run(72000);
    const double want = 110.0 * std::pow(2.0, 3.0 / 12.0);
    CHECK_NEAR(centsOff(preciseHz(d.L, 24000, kSr, want, 16384, 2048), want), 0.0, 2.0);
}

TEST(clatter_square_oscillators_are_band_limited) {
    // The cowbell is two squares through a band-pass. Everything it is supposed to contain sits on odd
    // multiples of the two fundamentals; what is anywhere else up to 16 kHz is aliasing. At its own pitch
    // (545 and 785 Hz) and an octave up it is clean; two octaves up (2.2 and 3.1 kHz) the two-sample
    // step correction is at its limit, and the bound says so.
    struct Case { float pitch; double bound; };
    for (double sr : {44100.0, 48000.0})
        for (const Case& cs : {Case{0.0f, -60.0}, Case{12.0f, -50.0}, Case{24.0f, -38.0}}) {
            const auto out = hit(Clatter::Cowbell, [&](Clatter& c) { c.pPitch->set(cs.pitch); c.pDecay->set(1.0f); c.pNoise->set(0.0f); c.pSnap->set(0.0f); c.pTone->set(1.0f); }, 60, 3.0, sr);
            const auto m = spectrum(out, kLead + 2400, 16384);
            const double fa = 545.0 * std::pow(2.0, cs.pitch / 12.0), fb = fa * 1.44, bin = sr / 16384.0;
            double strongest = 0.0, alias = 0.0;
            for (int b = 8; b < 8192; ++b) {
                const double hz = b * bin;
                strongest = std::max(strongest, m[(size_t)b]);
                if (hz > 16000.0) continue;
                bool member = false;
                for (int k = 1; k < 80 && !member; k += 2) member = std::fabs(hz - k * fa) < 12.0 * bin || std::fabs(hz - k * fb) < 12.0 * bin;
                if (!member) alias = std::max(alias, m[(size_t)b]);
            }
            const double a = db(alias / strongest);
            CHECK_MSG(a < cs.bound, "rate %.0f: cowbell at %+.0f st has %.1f dB between its partials", sr, cs.pitch, a);
            if (report()) printf("    [clatter] rate %.0f: cowbell at %+.0f st (squares at %.0f and %.0f Hz): strongest non-partial %.1f dB\n", sr, cs.pitch, fa, fb, a);
        }
}

TEST(clatter_a_roll_does_not_cut_itself_but_a_closed_hat_chokes_an_open_one) {
    // sixteen snare hits 20 ms apart: every one rings out, so the roll is the sum of the hits
    {
        Clatter c;
        c.pModel->set((float)Clatter::Snare);
        c.pDecay->set(0.8f);
        Drive d(c, kSr);
        for (int i = 0; i < 16; ++i) d.on(i * 960, 60, (uint32)(i + 1));
        int most = 0;
        d.each = [&](int64) { most = std::max(most, c.activeVoices()); };
        d.run(96000);
        std::vector<float> sum(96000, 0.0f);
        for (int i = 0; i < 16; ++i) {
            Clatter one;
            one.pModel->set((float)Clatter::Snare);
            one.pDecay->set(0.8f);
            Drive s(one, kSr);
            s.on(i * 960, 60, (uint32)(i + 1));
            s.run(96000);
            for (size_t k = 0; k < sum.size(); ++k) sum[k] += s.L[k];
        }
        CHECK_MSG(maxAbsDiff(d.L, sum) < 1.0e-4, "the roll differs from the sum of its hits by %g", maxAbsDiff(d.L, sum));
        CHECK_MSG(most >= 10 && most <= Clatter::kVoices, "most voices in the roll: %d", most);
        if (report()) printf("    [clatter] 16 snare hits 20 ms apart: %d voices at once, roll equals the sum of the single hits to %g\n", most, maxAbsDiff(d.L, sum));
    }
    // Kit mode: the open hat (A#) rings until the closed hat (F#) lands on it
    {
        auto run = [&](bool close) {
            Clatter c;
            c.pModel->set((float)Clatter::Kit);
            c.pDecay->set(0.9f);
            Drive d(c, kSr);
            d.on(0, 70, 1);
            if (close) d.on(9600, 66, 2);
            std::vector<int> voices;
            d.each = [&](int64) { voices.push_back(c.activeVoices()); };
            d.run(48000);
            return std::make_pair(d.L, voices);
        };
        const auto open = run(false), choked = run(true);
        // 60 ms after the closed hat: it has gone itself (it is short), and so has the open one
        const double ringing = rmsOf(open.first, 9600 + 14400, 9600 + 19200), after = rmsOf(choked.first, 9600 + 14400, 9600 + 19200);
        CHECK_MSG(ringing > 0.003 && after < 0.02 * ringing, "300 to 400 ms after: %.5f ringing, %.6f choked", ringing, after);
        CHECK(open.second[(9600 + 2400) / kQuantum] == 1);
        CHECK_MSG(choked.second[(9600 + 960) / kQuantum] == 1, "%d voices 20 ms after the closed hat", choked.second[(9600 + 960) / kQuantum]);      // only the closed hat
        // and the choke is a fade, not a cut
        const float before = maxStep(choked.first, 4800, 9600);
        CHECK(maxStep(choked.first, 9600 + 300, 9600 + 800) <= before);
        // the other way round nothing is choked: an open hat after a closed one leaves both
        Clatter c;
        c.pModel->set((float)Clatter::Kit);
        Drive d(c, kSr);
        d.on(0, 66, 1);
        d.on(240, 70, 2);
        d.on(480, 60, 3);               // and a kick chokes nobody
        d.run(960);
        CHECK(c.activeVoices() == 3);
    }
    // the same with the model switched between notes on one instrument
    {
        Clatter c;
        c.pModel->set((float)Clatter::OpenHat);
        c.pDecay->set(0.9f);
        Drive d(c, kSr);
        d.on(0, 60, 1);
        d.on(9600, 60, 2);
        d.each = [&](int64 pos) { c.pModel->set(pos >= 9600 - 64 ? (float)Clatter::ClosedHat : (float)Clatter::OpenHat); };
        d.run(48000);
        CHECK(rmsOf(d.L, 9600 + 14400, 9600 + 19200) < 1.0e-4);
    }
    // more hits than voices: the count holds
    {
        Clatter c;
        c.pModel->set((float)Clatter::Cymbal);
        c.pDecay->set(1.0f);
        Drive d(c, kSr);
        for (int i = 0; i < 40; ++i) d.on(i * 480, 60 + i % 5, (uint32)(i + 1));
        int most = 0;
        d.each = [&](int64) { most = std::max(most, c.activeVoices()); };
        d.run(48000);
        CHECK(allFinite(d.L));
        CHECK_MSG(most <= Clatter::kVoices && c.activeVoices() == Clatter::kSounding, "most %d, at the end %d", most, c.activeVoices());
    }
}

TEST(clatter_kit_mode_maps_keys_to_drums) {
    static const int expect[12] = {Clatter::Kick, Clatter::Rimshot, Clatter::Snare, Clatter::Clap, Clatter::Tom, Clatter::Tom,
                                   Clatter::ClosedHat, Clatter::Clave, Clatter::Shaker, Clatter::Cowbell, Clatter::OpenHat, Clatter::Cymbal};
    bool seen[Clatter::Kit] = {};
    for (int k = 0; k < 12; ++k) {
        CHECK(Clatter::kitModel(60 + k) == expect[k] && Clatter::kitModel(36 + k) == expect[k]);
        seen[expect[k]] = true;
        // a key in Kit mode is the same sound as that model on its own, shifted by the slot's offset
        Clatter kit, single;
        kit.pModel->set((float)Clatter::Kit);
        single.pModel->set((float)expect[k]);
        single.pPitch->set((float)Clatter::kitOffset(60 + k));
        const auto a = oneNote(kit, 60 + k, 2400, 96000), b = oneNote(single, 60, 2400, 96000);
        CHECK_MSG(maxAbsDiff(a, b) < 1.0e-6, "kit key %d differs from model %d by %g", 60 + k, expect[k], maxAbsDiff(a, b));
    }
    for (int m = 0; m < Clatter::Kit; ++m) CHECK_MSG(seen[m], "model %d is not in the kit", m);
    // an octave down is the same kit an octave lower: the kick's tail is at 26 Hz
    Clatter kit;
    kit.pModel->set((float)Clatter::Kit);
    kit.pDecay->set(1.0f);
    kit.pSnap->set(0.0f);
    const auto low = oneNote(kit, 48, 2400, 96000);
    CHECK_NEAR(centsOff(preciseHz(low, 24000, kSr, 26.0, 32768, 4096), 26.0), 0.0, 3.0);
    // the two toms are a fifth and a bit apart
    CHECK(Clatter::kitOffset(64) == -5 && Clatter::kitOffset(65) == 2);
}
