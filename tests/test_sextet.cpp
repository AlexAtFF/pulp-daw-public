// Sextet, the six-operator FM synth.
#include "FmwtTestUtil.h"
#include "pulp/Sextet.h"
#include <thread>
using namespace pulp;
using namespace ft;

namespace {

void setp(Sextet& s, const std::string& id, float v) {
    Param* p = s.param(id);
    CHECK_MSG(p != nullptr, "no parameter %s", id.c_str());
    if (p) p->set(v);
}

// One sine operator straight to the output with a flat envelope: a clean tone for measuring.
std::shared_ptr<Sextet> plainSextet() {
    auto s = std::make_shared<Sextet>();
    s->pAlgo->set(5.0f);        // all carriers
    for (int i = 0; i < Sextet::kOps; ++i) {
        s->op[i].level->set(i == 0 ? 1.0f : 0.0f);
        s->op[i].attack->set(0.0f);
        s->op[i].sustain->set(1.0f);
        s->op[i].vel->set(0.0f);
    }
    return s;
}

// Operator 2 modulating operator 1 ("Three pairs", only the first pair in use), flat envelopes.
std::shared_ptr<Sextet> pairSextet(float modRatio, float modLevel) {
    auto s = plainSextet();
    s->pAlgo->set(3.0f);
    s->op[1].coarse->set(modRatio);
    s->op[1].level->set(modLevel);
    return s;
}

double loadPreset(Sextet& s, const std::string& name) {
    for (auto& pr : s.factoryPresets()) if (pr.name == name) { s.loadState(pr.state); return 1.0; }
    CHECK_MSG(false, "no preset %s", name.c_str());
    return 0.0;
}

bool verbose() { return std::getenv("PULP_TEST_VERBOSE") != nullptr; }

} // namespace

TEST(halfband_decimator_passes_the_audio_band_and_rejects_the_rest) {
    // Feed sines at the doubled rate and measure what comes out at the base rate.
    const double osr = 96000.0;
    auto gainAt = [&](double hz) {
        parts::Decimator2 d;
        d.prepare();
        std::vector<float> out;
        for (int i = 0; i < 16384; ++i)
            out.push_back(d.process((float)std::sin(kTwoPi * hz * (2 * i) / osr), (float)std::sin(kTwoPi * hz * (2 * i + 1) / osr)));
        return rmsOf(out, 8192) * std::sqrt(2.0);
    };
    for (double hz : {100.0, 1000.0, 10000.0, 18000.0, 20000.0})
        CHECK_MSG(std::fabs(20.0 * std::log10(gainAt(hz))) < 0.05, "pass band at %.0f Hz: %.3f dB", hz, 20.0 * std::log10(gainAt(hz)));
    // Anything from 28 kHz up would fold to 20 kHz or below after decimation.
    for (double hz : {28000.0, 30000.0, 36000.0, 42000.0, 47000.0})
        CHECK_MSG(20.0 * std::log10(gainAt(hz)) < -90.0, "stop band at %.0f Hz: %.1f dB", hz, 20.0 * std::log10(gainAt(hz)));
    // No latency: the first input sample is in the first output sample.
    parts::Decimator2 d;
    d.prepare();
    CHECK(d.process(0.0f, 1.0f) != 0.0f);
}

TEST(sextet_operators_are_in_tune_at_every_sample_rate) {
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int key : {36, 57, 60, 72, 84, 96}) {
            auto s = plainSextet();
            RenderSpec rs;
            rs.sr = sr;
            rs.length = (int64)sr;
            auto out = play(s, {mk(0, 3840, key)}, rs);
            CHECK(out != nullptr);
            if (!out) return;
            const double hz = preciseHz(out->ch[0], (size_t)(sr * 0.2), sr, keyToHz(key));
            CHECK_MSG(std::fabs(centsOff(hz, keyToHz(key))) < 0.2, "%.0f Hz key %d: %.3f Hz, %.3f cents off", sr, key, hz, centsOff(hz, keyToHz(key)));
            const float pk = peakOf(out->ch[0]);
            CHECK_MSG(pk > 0.25f && pk < 0.5f, "a single note peaks at %.3f (-12 to -6 dBFS wanted)", pk);
        }
}

TEST(sextet_ratio_fine_detune_fixed_and_every_shape_are_in_tune) {
    const double sr = 48000.0, f = keyToHz(60);
    auto hzOf = [&](const std::shared_ptr<Sextet>& s, double guess, int key = 60) {
        auto out = play(s, {mk(0, 3840, key)});
        return preciseHz(out->ch[0], 9600, sr, guess);
    };
    // every operator, alone
    for (int i = 0; i < Sextet::kOps; ++i) {
        auto s = plainSextet();
        s->op[0].level->set(0.0f);
        s->op[i].level->set(1.0f);
        CHECK_MSG(std::fabs(centsOff(hzOf(s, f), f)) < 0.2, "operator %d", i + 1);
    }
    struct Case { float coarse, fine, detune; double ratio; } cases[] = {
        {0, 0, 0, 0.5}, {2, 0, 0, 2.0}, {3, 0, 0, 3.0}, {7, 0, 0, 7.0}, {1, 0.5f, 0, 1.5}, {2, 0.25f, 0, 2.5},
        {1, 0, 50, std::pow(2.0, 50.0 / 1200.0)}, {1, 0, -100, std::pow(2.0, -100.0 / 1200.0)}};
    for (auto& c : cases) {
        auto s = plainSextet();
        s->op[0].coarse->set(c.coarse);
        s->op[0].fine->set(c.fine);
        s->op[0].detune->set(c.detune);
        const double hz = hzOf(s, f * c.ratio);
        CHECK_MSG(std::fabs(centsOff(hz, f * c.ratio)) < 0.2, "coarse %g fine %g detune %g: %.3f Hz, %.3f cents off", c.coarse, c.fine, c.detune, hz, centsOff(hz, f * c.ratio));
    }
    // a fixed operator ignores the key
    for (int key : {48, 72}) {
        auto s = plainSextet();
        s->op[0].mode->set(1.0f);
        s->op[0].hz->set(1000.0f);
        CHECK_MSG(std::fabs(centsOff(hzOf(s, 1000.0, key), 1000.0)) < 0.2, "fixed 1000 Hz at key %d", key);
    }
    // every shape (the rectified sine repeats twice per cycle, so it sounds an octave up)
    for (int w = 0; w < 7; ++w) {
        auto s = plainSextet();
        s->op[0].wave->set((float)w);
        const double want = w == 2 ? 2.0 * f : f;
        auto out = play(s, {mk(0, 3840, 60)});
        const double hz = preciseHz(out->ch[0], 9600, sr, want);
        CHECK_MSG(std::fabs(centsOff(hz, want)) < 0.2, "shape %d: %.3f Hz, %.3f cents off", w, hz, centsOff(hz, want));
        CHECK_MSG(ampAt(out->ch[0], 9600, 16384, want, sr) > 0.1, "shape %d has no fundamental", w);
        // a note with an instant attack still starts from silence
        CHECK_MSG(std::fabs(out->ch[0][0]) < 0.02f && maxStep(out->ch[0], 0, 64) < 0.1f, "shape %d clicks at the start (%.3f, step %.3f)", w, out->ch[0][0], maxStep(out->ch[0], 0, 64));
    }
    // per-note fine pitch and the channel pitch
    {
        auto s = plainSextet();
        Note n = mk(0, 3840, 60);
        n.fine = 25.0f;
        auto out = play(s, {n});
        CHECK_NEAR(centsOff(preciseHz(out->ch[0], 9600, sr, f * std::pow(2.0, 25.0 / 1200.0)), f), 25.0, 0.2);
        Sextet d;
        setp(d, "algo", 5);
        d.op[0].attack->set(0); d.op[0].sustain->set(1);
        Drive dr(d);
        dr.bend = 2.0f;
        dr.on(0, 60, 1);
        dr.run(48000);
        CHECK_NEAR(centsOff(preciseHz(dr.L, 9600, sr, keyToHz(62)), f), 200.0, 0.2);
    }
}

TEST(sextet_phase_modulation_matches_the_bessel_functions) {
    // A sine at f modulated by a sine at 3f with index 1.5: partials at f, 2f, 4f, 5f, 7f, 8f, 10f
    // with amplitudes J0, J1, J1, J2, J2, J3, J3 of the index. Index = modulator level x 4 pi.
    const double sr = 48000.0, f = keyToHz(48), index = 1.5;
    auto s = pairSextet(3, (float)(index / (4.0 * kPi)));
    auto out = play(s, {mk(0, 3840, 48)});
    const double carrier = 0.35;        // level 1 x the default volume
    const struct { double mult; int order; } parts[] = {{1, 0}, {2, 1}, {4, 1}, {5, 2}, {7, 2}, {8, 3}, {10, 3}};
    for (auto& pt : parts) {
        const double got = ampAt(out->ch[0], 9600, 32768, f * pt.mult, sr), want = carrier * std::fabs(std::cyl_bessel_j(pt.order, index));
        CHECK_MSG(std::fabs(got / want - 1.0) < 0.01, "partial %g f: %.5f, expected %.5f", pt.mult, got, want);
    }
    CHECK(ampAt(out->ch[0], 9600, 32768, f * 3.0, sr) < 1.0e-4);       // nothing where no sideband belongs
    CHECK(ampAt(out->ch[0], 9600, 32768, f * 6.0, sr) < 1.0e-4);
    // the same routing written into the matrix by hand gives the same sound, sample for sample
    auto m = plainSextet();
    m->pAlgo->set(0.0f);
    m->op[1].coarse->set(3.0f);
    m->op[1].level->set((float)(index / (4.0 * kPi)));
    setp(*m, "m12", 1.0f);
    setp(*m, "out1", 1.0f);
    auto out2 = play(m, {mk(0, 3840, 48)});
    CHECK(identical(*out, *out2));
    // half the matrix amount is half the index
    setp(*m, "m12", 0.5f);
    auto out3 = play(m, {mk(0, 3840, 48)});
    CHECK_NEAR(ampAt(out3->ch[0], 9600, 32768, f * 2.0, sr) / ampAt(out3->ch[0], 9600, 32768, f, sr),
               std::cyl_bessel_j(1, 0.75) / std::cyl_bessel_j(0, 0.75), 0.005);
    // modulation depth scales it too
    setp(*m, "moddepth", 2.0f);
    auto out4 = play(m, {mk(0, 3840, 48)});
    CHECK_NEAR(ampAt(out4->ch[0], 9600, 32768, f * 2.0, sr) / ampAt(out4->ch[0], 9600, 32768, f, sr),
               std::cyl_bessel_j(1, 1.5) / std::cyl_bessel_j(0, 1.5), 0.01);
}

TEST(sextet_every_algorithm_is_the_matrix_it_stands_for) {
    CHECK(Sextet::numAlgorithms() >= 8);
    std::vector<std::shared_ptr<SampleData>> outs;
    for (int a = 1; a <= Sextet::numAlgorithms(); ++a) {
        auto make = [&] {
            auto s = std::make_shared<Sextet>();
            for (int i = 0; i < Sextet::kOps; ++i) {
                s->op[i].level->set(i == 0 ? 0.9f : 0.12f + 0.03f * i);
                s->op[i].coarse->set((float)(1 + i));
                s->op[i].sustain->set(1.0f);
            }
            s->op[5].fb->set(0.3f);
            s->pAlgo->set((float)a);
            return s;
        };
        auto fixed = make(), written = make();
        written->applyAlgorithm(a);
        CHECK(written->pAlgo->getInt() == 0);
        auto o1 = play(fixed, {mk(0, 960, 55)}, {48000.0, 120.0, 512, 1, 24000});
        auto o2 = play(written, {mk(0, 960, 55)}, {48000.0, 120.0, 512, 1, 24000});
        CHECK_MSG(identical(*o1, *o2), "algorithm %d (%s) differs from its matrix", a, Sextet::algorithmName(a));
        CHECK_MSG(peakOf(o1->ch[0]) > 0.05f && allFinite(o1->ch[0]), "algorithm %d is silent or broken", a);
        for (auto& prev : outs) CHECK_MSG(!identical(*prev, *o1), "algorithm %d sounds like another one", a);
        outs.push_back(o1);
    }
    // the ones the brief names
    std::vector<std::string> names;
    for (int a = 1; a <= Sextet::numAlgorithms(); ++a) names.push_back(Sextet::algorithmName(a));
    for (const char* want : {"Stack of six", "Three pairs", "One carrier, five modulators", "All carriers (additive)"})
        CHECK_MSG(std::find(names.begin(), names.end(), want) != names.end(), "no algorithm called %s", want);
}

TEST(sextet_feedback_is_stable_and_the_output_has_no_dc) {
    const double sr = 48000.0;
    // full feedback on every operator, every routing cell open, loudest velocity: bounded and finite
    for (int guard = 0; guard < 2; ++guard) {
        auto s = std::make_shared<Sextet>();
        s->pAlgo->set(0.0f);
        s->pGuard->set((float)guard);
        for (int i = 0; i < Sextet::kOps; ++i) {
            s->op[i].level->set(1.0f);
            s->op[i].sustain->set(1.0f);
            s->op[i].fb->set(1.0f);
            s->op[i].out->set(1.0f);
            s->op[i].coarse->set((float)(1 + 2 * i));
            for (int j = 0; j < Sextet::kOps; ++j) s->matrix[i][j]->set(1.0f);
        }
        s->pModDepth->set(2.0f);
        std::vector<Note> notes;
        for (int k : {24, 48, 72, 96, 120}) { Note n = mk(0, 3840, k, 1.0f); n.modX = 1.0f; n.modY = 1.0f; notes.push_back(n); }
        auto out = play(s, notes, {sr, 120.0, 512, 1, 96000});
        CHECK(allFinite(out->ch[0]) && allFinite(out->ch[1]));
        CHECK_MSG(peakOf(out->ch[0]) < 2.0f, "guard %d: peak %.2f", guard, peakOf(out->ch[0]));
        CHECK(peakOf(out->ch[0]) > 0.01f);
    }
    auto dcOf = [](const std::vector<float>& d, size_t from, size_t to) { double m = 0; for (size_t i = from; i < to; ++i) m += d[i]; return m / (double)(to - from); };
    // a feedback saw, a half sine (a shape with a DC offset of its own) and a carrier whose modulator
    // sits a few cents off it (plain phase modulation leaves a slow offset there)
    for (int which = 0; which < 3; ++which) {
        auto s = plainSextet();
        if (which == 0) s->op[0].fb->set(0.6f);
        if (which == 1) s->op[0].wave->set(1.0f);
        if (which == 2) { s->pAlgo->set(3.0f); s->op[1].level->set(0.16f); s->op[1].detune->set(3.0f); }
        auto out = play(s, {mk(0, 7680, 60)}, {sr, 120.0, 512, 1, 192000});
        const double dc = dcOf(out->ch[0], 48000, 192000), pk = peakOf(out->ch[0]);
        CHECK_MSG(std::fabs(dc) < (which == 2 ? 0.01 : 0.002) * pk, "case %d: DC %.5f against a peak of %.3f", which, dc, pk);
        // and no slow wander either: every 100 ms stretch averages to nearly nothing
        double worst = 0.0;
        for (size_t a = 48000; a + 4800 <= 192000; a += 2400) worst = std::max(worst, std::fabs(dcOf(out->ch[0], a, a + 4800)));
        CHECK_MSG(worst < 0.05 * pk, "case %d: wanders by %.4f against a peak of %.3f", which, worst, pk);
    }
}

TEST(sextet_aliasing_stays_far_below_the_signal) {
    // The brightest things the synth can do, played high: what is not a harmonic of the note is an alias.
    struct Case { const char* name; int which; };
    const Case cases[] = {{"index 12.6, ratio 1", 0}, {"index 6.3, ratio 8", 1}, {"full feedback", 2}, {"stack of three", 3},
                          {"soft saw carrier", 4}, {"five modulators", 5}};
    double worstOn = -200.0;
    for (double sr : {44100.0, 48000.0})
        for (auto& c : cases)
            for (int key : {48, 60, 72, 84, 96, 108}) {
                double db[2];
                for (int guard = 1; guard >= 0; --guard) {
                    auto s = plainSextet();
                    s->pGuard->set((float)guard);
                    switch (c.which) {
                        case 0: s->pAlgo->set(3); s->op[1].level->set(1.0f); break;
                        case 1: s->pAlgo->set(3); s->op[1].level->set(0.5f); s->op[1].coarse->set(8); break;
                        case 2: s->op[0].fb->set(1.0f); break;
                        case 3: s->pAlgo->set(2); s->op[1].level->set(0.5f); s->op[1].coarse->set(2); s->op[2].level->set(0.5f); s->op[2].coarse->set(3); break;
                        case 4: s->pAlgo->set(3); s->op[0].wave->set(6); s->op[1].level->set(0.3f); break;
                        default: s->pAlgo->set(4); for (int i = 1; i < 6; ++i) { s->op[i].level->set(0.25f); s->op[i].coarse->set((float)(i + 1)); } break;
                    }
                    Note n = mk(0, 3840, key);
                    n.fine = 37.0f;         // off the grid, so aliases cannot hide under harmonics
                    auto out = play(s, {n}, {sr, 120.0, 512, 1, (int64)sr});
                    db[guard] = aliasDb(out->ch[0], 4096, 32768, sr, keyToHz(key + 0.37), 20.0, 18000.0);
                }
                if (verbose()) printf("    %.0f Hz  %-22s key %3d: alias %7.1f dB (guard off %7.1f dB)\n", sr, c.name, key, db[1], db[0]);
                worstOn = std::max(worstOn, db[1]);
                CHECK_MSG(db[1] < -60.0, "%.0f Hz, %s, key %d: aliases at %.1f dB", sr, c.name, key, db[1]);
            }
    printf("    worst alias with the guard on: %.1f dB below the strongest partial\n", worstOn);
    // The guard does not dull what fits: two octaves above middle C, index 12.6 still reaches past 15 kHz.
    {
        auto s = pairSextet(1, 1.0f);
        auto out = play(s, {mk(0, 3840, 84)});
        const double f = keyToHz(84);
        CHECK_MSG(ampAt(out->ch[0], 9600, 16384, f * 15, 48000.0) > 0.01 * ampAt(out->ch[0], 9600, 16384, f, 48000.0) || ampAt(out->ch[0], 9600, 16384, f * 14, 48000.0) > 0.01 * ampAt(out->ch[0], 9600, 16384, f, 48000.0),
                  "harmonics 14 and 15 are gone");
    }
    // The guard leaves low and middle notes exactly as they are.
    for (int key : {36, 48}) {
        std::shared_ptr<SampleData> o[2];
        for (int guard = 0; guard < 2; ++guard) {
            auto s = pairSextet(1, 0.3f);
            s->pGuard->set((float)guard);
            o[guard] = play(s, {mk(0, 960, key)}, {48000.0, 120.0, 512, 1, 24000});
        }
        CHECK_MSG(identical(*o[0], *o[1]), "the guard changed key %d", key);
    }
}

TEST(sextet_note_on_lands_on_the_exact_sample) {
    // 120 bpm at 48 kHz: tick 37 is sample 925.
    auto s = pairSextet(2, 0.1f);
    auto a = play(s, {mk(37, 960, 60)}, {48000.0, 120.0, 512, 1, 24000});
    for (size_t i = 0; i < 925; ++i) if (a->ch[0][i] != 0.0f) { CHECK_MSG(false, "sound at sample %zu, before the note", i); break; }
    CHECK(a->ch[0][925] != 0.0f || a->ch[0][926] != 0.0f);
    CHECK(std::fabs(a->ch[0][926]) > 1.0e-4f);
    // one tick later is the same sound 25 samples later, wherever that falls in the engine's quantum
    auto b = play(s, {mk(38, 960, 60)}, {48000.0, 120.0, 512, 1, 24000});
    double worst = 0.0;
    for (size_t i = 0; i + 25 < 20000; ++i) worst = std::max(worst, (double)std::fabs(a->ch[0][i] - b->ch[0][i + 25]));
    CHECK_MSG(worst < 1.0e-6, "shifted by a tick, the note differs by %g", worst);
}

TEST(sextet_portamento_glide_and_slide_notes) {
    const double sr = 48000.0;
    // the channel's portamento
    {
        auto s = plainSextet();
        auto out = play(s, {mk(0, 960, 60), mk(960, 1920, 72)}, {sr, 120.0, 512, 1, 72000},
                        [](Project&, ChannelLive& l) { l.porta.set(1); l.mono.set(1); l.portaTime.set(200.0f); });
        const size_t on = 960 * 25;
        const double early = measureHz(out->ch[0], on + 300, on + 1500, sr), late = measureHz(out->ch[0], on + 14400, on + 24000, sr);
        CHECK_MSG(early > keyToHz(60) * 1.01 && early < keyToHz(66), "early pitch %.1f Hz", early);
        CHECK_MSG(std::fabs(centsOff(late, keyToHz(72))) < 2.0, "late pitch %.1f Hz", late);
    }
    // the synth's own glide
    {
        auto s = plainSextet();
        s->pGlide->set(200.0f);
        auto out = play(s, {mk(0, 900, 60), mk(960, 1920, 72)}, {sr, 120.0, 512, 1, 72000});
        const size_t on = 960 * 25;
        const double early = measureHz(out->ch[0], on + 300, on + 1500, sr), late = measureHz(out->ch[0], on + 14400, on + 24000, sr);
        CHECK_MSG(early > keyToHz(60) * 1.01 && early < keyToHz(66), "early pitch %.1f Hz", early);
        CHECK_MSG(std::fabs(centsOff(late, keyToHz(72))) < 2.0, "late pitch %.1f Hz", late);
    }
    // a slide note bends the sounding notes of its colour and no others
    {
        auto s = plainSextet();
        Note held = mk(0, 3840, 60), other = mk(0, 3840, 72), slide = mk(960, 960, 67);
        held.pan = -1.0f;
        other.pan = 1.0f;
        other.color = 3;
        slide.slide = true;
        auto out = play(s, {held, other, slide}, {sr, 120.0, 512, 1, 96000});
        CHECK_NEAR(measureHz(out->ch[0], 4800, 22000, sr), keyToHz(60), 0.5);
        const double mid = measureHz(out->ch[0], 960 * 25 + 9600, 960 * 25 + 14400, sr);
        CHECK_MSG(mid > keyToHz(62) && mid < keyToHz(65.5), "mid-slide pitch %.2f Hz", mid);
        CHECK_NEAR(measureHz(out->ch[0], 60000, 90000, sr), keyToHz(67), 0.8);
        CHECK_NEAR(measureHz(out->ch[1], 60000, 90000, sr), keyToHz(72), 0.8);
    }
}

TEST(sextet_velocity_pan_mod_x_and_mod_y_are_heard) {
    const double sr = 48000.0, f = keyToHz(60);
    auto run = [&](const std::shared_ptr<Sextet>& s, Note n) { return play(s, {n}, {sr, 120.0, 512, 1, 36000}); };
    auto bright = [&](const SampleData& o) { return ampAt(o.ch[0], 9600, 16384, 2 * f, sr) / ampAt(o.ch[0], 9600, 16384, f, sr); };
    // velocity: louder through a carrier, brighter through a modulator
    {
        auto s = pairSextet(1, 0.1f);
        s->op[0].vel->set(1.0f);
        s->op[1].vel->set(1.0f);
        auto soft = run(s, mk(0, 1920, 60, 0.4f)), ref = run(s, mk(0, 1920, 60)), hard = run(s, mk(0, 1920, 60, 1.0f));
        CHECK(rmsOf(soft->ch[0], 9600, 30000) < 0.5 * rmsOf(ref->ch[0], 9600, 30000));
        CHECK(rmsOf(hard->ch[0], 9600, 30000) > 1.3 * rmsOf(ref->ch[0], 9600, 30000));
        CHECK_MSG(bright(*hard) > 1.3 * bright(*ref) && bright(*soft) < 0.5 * bright(*ref), "brightness %.3f / %.3f / %.3f", bright(*soft), bright(*ref), bright(*hard));
        s->op[0].vel->set(0.0f);
        s->op[1].vel->set(0.0f);
        auto flat = run(s, mk(0, 1920, 60, 0.4f));
        auto flatRef = run(s, mk(0, 1920, 60));
        CHECK(identical(*flat, *flatRef));          // sensitivity 0: velocity does nothing
    }
    // pan, per note and per operator
    {
        auto s = plainSextet();
        Note n = mk(0, 1920, 60);
        n.pan = -1.0f;
        auto left = run(s, n);
        CHECK(peakOf(left->ch[0]) > 0.3f && peakOf(left->ch[1]) < 1.0e-6f);
        n.pan = 0.5f;
        auto half = run(s, n);
        CHECK_NEAR(rmsOf(half->ch[0], 9600, 30000) / rmsOf(half->ch[1], 9600, 30000), 0.5, 0.01);
        s->op[0].pan->set(1.0f);
        auto right = run(s, mk(0, 1920, 60));
        CHECK(peakOf(right->ch[1]) > 0.3f && peakOf(right->ch[0]) < 1.0e-6f);
    }
    // mod X is the overall modulation depth: 0.5 is as programmed, 0 is none, 1 is double
    {
        auto s = pairSextet(1, 0.06f);
        Note n = mk(0, 1920, 60);
        auto mid = run(s, n);
        n.modX = 0.0f;
        auto none = run(s, n);
        n.modX = 1.0f;
        auto twice = run(s, n);
        CHECK(bright(*none) < 1.0e-3);
        CHECK_MSG(bright(*twice) > 1.8 * bright(*mid), "mod X 1 gives %.3f, mod X 0.5 gives %.3f", bright(*twice), bright(*mid));
    }
    // mod Y is the feedback
    {
        auto s = plainSextet();
        s->op[0].fb->set(0.2f);
        Note n = mk(0, 1920, 60);
        auto mid = run(s, n);
        n.modY = 0.0f;
        auto none = run(s, n);
        n.modY = 1.0f;
        auto twice = run(s, n);
        CHECK(bright(*none) < 1.0e-3);
        CHECK_MSG(bright(*twice) > 1.3 * bright(*mid) && bright(*mid) > 0.05, "mod Y 1 gives %.3f, mod Y 0.5 gives %.3f", bright(*twice), bright(*mid));
    }
    // per-note release
    {
        auto s = plainSextet();
        s->op[0].release->set(300.0f);
        Note shortN = mk(0, 480, 60), longN = mk(0, 480, 60);
        shortN.release = 0.0f;
        longN.release = 1.0f;
        auto a = run(s, shortN), b = run(s, longN);
        CHECK(rmsOf(b->ch[0], 16000, 20000) > 5.0 * rmsOf(a->ch[0], 16000, 20000));
    }
}

TEST(sextet_envelopes_key_scaling_pitch_envelope_and_lfos) {
    const double sr = 48000.0, f = keyToHz(60);
    // the operator envelope: attack, decay to the sustain level, release
    {
        auto s = plainSextet();
        s->op[0].attack->set(100.0f);
        s->op[0].decay->set(200.0f);
        s->op[0].sustain->set(0.5f);
        s->op[0].release->set(100.0f);
        auto out = play(s, {mk(0, 1920, 60)}, {sr, 120.0, 512, 1, 72000});
        const float full = 0.35f;
        CHECK_NEAR(peakOf(out->ch[0], 2300, 2500) / full, 0.5, 0.03);         // half way up the attack
        CHECK_NEAR(peakOf(out->ch[0], 4700, 5000) / full, 1.0, 0.03);         // the top
        CHECK_NEAR(peakOf(out->ch[0], 40000, 47000) / full, 0.5, 0.02);       // sustain
        CHECK(peakOf(out->ch[0], 48000 + 9600, 72000) < 0.002f);              // released
    }
    // rate key scaling: the same decay is shorter high up
    {
        auto tail = [&](float rks, int key) {
            auto s = plainSextet();
            s->op[0].decay->set(800.0f);
            s->op[0].sustain->set(0.0f);
            s->op[0].rks->set(rks);
            auto out = play(s, {mk(0, 3840, key)}, {sr, 120.0, 512, 1, 48000});
            return rmsOf(out->ch[0], 14400, 19200) / rmsOf(out->ch[0], 0, 2400);
        };
        CHECK_NEAR(tail(0.0f, 60), tail(0.0f, 84), 0.01);
        CHECK(tail(1.0f, 84) < 0.2 * tail(1.0f, 60));
    }
    // level key scaling: +/- 1 dB per semitone at full
    {
        auto level = [&](float ks, int key) {
            auto s = plainSextet();
            s->op[0].kscale->set(ks);
            auto out = play(s, {mk(0, 1920, key)}, {sr, 120.0, 512, 1, 24000});
            return gainToDb(peakOf(out->ch[0], 4800, 24000));
        };
        CHECK_NEAR(level(-1.0f, 72) - level(-1.0f, 60), -12.0, 0.1);
        CHECK_NEAR(level(-0.5f, 48) - level(-0.5f, 60), 6.0, 0.1);
        CHECK_NEAR(level(0.0f, 84) - level(0.0f, 60), 0.0, 0.05);
    }
    // the pitch envelope starts the note off by its amount and comes home
    {
        auto s = plainSextet();
        s->pPEnvAmt->set(12.0f);
        s->pPEnvTime->set(200.0f);
        auto out = play(s, {mk(0, 3840, 60)});
        const double early = measureHz(out->ch[0], 0, 480, sr), late = measureHz(out->ch[0], 24000, 46000, sr);
        CHECK_MSG(early > f * 1.6 && early < f * 2.0, "starts at %.1f Hz", early);
        CHECK_MSG(std::fabs(centsOff(late, f)) < 1.0, "ends at %.2f Hz", late);
    }
    // LFO to pitch: rate, depth and delay
    {
        auto s = plainSextet();
        s->pLfoRate[0]->set(5.0f);
        s->pLfoPitch[0]->set(100.0f);
        s->pLfoDelay[0]->set(500.0f);
        auto out = play(s, {mk(0, 7680, 60)}, {sr, 120.0, 512, 1, 144000});
        // pitch per 10 ms stretch
        std::vector<double> cents;
        for (size_t a = 0; a + 960 <= 144000; a += 480) cents.push_back(centsOff(measureHz(out->ch[0], a, a + 960, sr), f));
        double before = 0.0, hi = -1e9, lo = 1e9;
        for (size_t i = 0; i < 45; ++i) before = std::max(before, std::fabs(cents[i]));         // first 450 ms
        int crossings = 0;
        for (size_t i = 100; i + 1 < 290; ++i) {                                                // 1.0 s to 2.9 s
            hi = std::max(hi, cents[i]); lo = std::min(lo, cents[i]);
            if (cents[i] < 0.0 && cents[i + 1] >= 0.0) ++crossings;
        }
        CHECK_MSG(before < 2.0, "the LFO moved the pitch by %.1f cents before its delay", before);
        CHECK_MSG(hi > 85.0 && hi < 105.0 && lo < -85.0 && lo > -105.0, "vibrato from %.1f to %.1f cents", lo, hi);
        CHECK_MSG(crossings == 9 || crossings == 10, "%d vibrato cycles in 1.9 s at 5 Hz", crossings);
    }
    // LFO to operator level: depth follows the operator's sensitivity
    {
        auto depthOf = [&](float sens) {
            auto s = plainSextet();
            s->pLfoRate[1]->set(4.0f);
            s->pLfoAmp[1]->set(0.6f);
            s->op[0].lfo->set(sens);
            auto out = play(s, {mk(0, 3840, 60)});
            float hi = 0.0f, lo = 1.0f;
            for (size_t a = 4800; a + 480 <= 48000; a += 240) { const float pk = peakOf(out->ch[0], a, a + 480); hi = std::max(hi, pk); lo = std::min(lo, pk); }
            return lo / hi;
        };
        CHECK_NEAR(depthOf(1.0f), 0.4, 0.03);
        CHECK_NEAR(depthOf(0.5f), 0.7, 0.03);
        CHECK_NEAR(depthOf(0.0f), 1.0, 0.01);
    }
}

TEST(sextet_filter_and_level) {
    const double sr = 48000.0, f = keyToHz(48);
    auto make = [] { auto s = plainSextet(); s->op[0].fb->set(0.5f); return s; };     // a saw-like tone
    auto ref = play(make(), {mk(0, 1920, 48)}, {sr, 120.0, 512, 1, 36000});
    auto h = [&](const SampleData& o, int n) { return ampAt(o.ch[0], 9600, 16384, f * n, sr); };
    {
        auto s = make();
        s->pFilter->set(1.0f);
        s->pCutoff->set(400.0f);
        s->pReso->set(0.0f);
        auto lp = play(s, {mk(0, 1920, 48)}, {sr, 120.0, 512, 1, 36000});
        CHECK_NEAR(gainToDb((float)(h(*lp, 1) / h(*ref, 1))), 0.0, 0.5);          // 131 Hz passes
        const double at12 = gainToDb((float)(h(*lp, 12) / h(*ref, 12)));          // 1570 Hz, two octaves above the cutoff
        CHECK_MSG(at12 < -21.0 && at12 > -27.0, "two octaves above the cutoff: %.1f dB (12 dB per octave wanted)", at12);
        s->pFilter->set(2.0f);
        auto hp = play(s, {mk(0, 1920, 48)}, {sr, 120.0, 512, 1, 36000});
        CHECK(gainToDb((float)(h(*hp, 1) / h(*ref, 1))) < -15.0);
        CHECK_NEAR(gainToDb((float)(h(*hp, 12) / h(*ref, 12))), 0.0, 0.5);
        s->pFilter->set(3.0f);
        s->pCutoff->set((float)(f * 4));
        s->pReso->set(0.6f);
        auto bp = play(s, {mk(0, 1920, 48)}, {sr, 120.0, 512, 1, 36000});
        CHECK(h(*bp, 4) / h(*ref, 4) > 4.0 * h(*bp, 1) / h(*ref, 1));
        CHECK(h(*bp, 4) / h(*ref, 4) > 4.0 * h(*bp, 16) / h(*ref, 16));
    }
    {
        auto s = make();
        s->pVolume->set(0.25f);
        auto half = play(s, {mk(0, 1920, 48)}, {sr, 120.0, 512, 1, 36000});
        CHECK_NEAR(rmsOf(half->ch[0], 4800, 30000) / rmsOf(ref->ch[0], 4800, 30000), 0.5, 0.001);
    }
    // several carriers together are no louder than one
    {
        auto s = plainSextet();
        for (int i = 0; i < 6; ++i) s->op[i].level->set(1.0f);
        auto six = play(s, {mk(0, 1920, 48)}, {sr, 120.0, 512, 1, 36000});
        CHECK(peakOf(six->ch[0]) < 0.5f && peakOf(six->ch[0]) > 0.2f);
    }
}

TEST(sextet_voice_limit_holds_and_stealing_does_not_click) {
    Sextet s;
    s.pAlgo->set(5.0f);
    s.op[0].attack->set(5.0f);
    s.op[0].sustain->set(1.0f);
    s.pVoices->set(4.0f);
    Drive d(s);
    for (int i = 0; i < 12; ++i) d.on(1000 + i * 960, 48 + i, (uint32)(i + 1));
    int most = 0;
    for (int q = 0; q < 16000 / kQuantum; ++q) { d.run(kQuantum); most = std::max(most, s.activeVoices()); }
    CHECK_MSG(most <= 5, "%d voices at once with a limit of 4 (one more may be fading out)", most);
    d.run(960);
    CHECK_MSG(s.activeVoices() == 4, "%d voices held", s.activeVoices());
    // a voice cut dead would jump by up to its amplitude (0.35); the fades keep every step small
    CHECK_MSG(maxStep(d.L, 0, d.L.size()) < 0.06f, "largest step %.3f", maxStep(d.L, 0, d.L.size()));
    CHECK(peakOf(d.L) > 0.3f);
    // far more notes than slots: still finite, still bounded
    Sextet t;
    t.pVoices->set(32.0f);
    Drive e(t);
    for (int i = 0; i < 200; ++i) e.on(i * 37, 30 + (i * 7) % 70, (uint32)(i + 1));
    e.run(48000);
    CHECK(allFinite(e.L) && t.activeVoices() <= Sextet::kSlots);
    CHECK(t.activeVoices() == 32);
}

TEST(sextet_note_ends_and_cuts_fall_smoothly_to_silence) {
    const double sr = 48000.0;
    for (int which = 0; which < 3; ++which) {
        Sextet s;
        if (which == 1) loadPreset(s, "Brass Section");
        if (which == 2) loadPreset(s, "Glass Tines");
        Drive d(s, sr);
        d.on(0, 57, 1);
        d.off(24000, 1);
        d.run(24000 + 96000);
        const float before = maxStep(d.L, 12000, 24000), after = maxStep(d.L, 24000, d.L.size());
        CHECK_MSG(after <= before * 1.05f + 1.0e-5f, "case %d: a step of %.5f after the note-off, %.5f before it", which, after, before);
        size_t last = 0;
        for (size_t i = 0; i < d.L.size(); ++i) if (std::fabs(d.L[i]) > 1.0e-7f) last = i;
        CHECK_MSG(last < d.L.size() - 4800, "case %d: still sounding at the end", which);
        CHECK_MSG(peakOf(d.L, last > 480 ? last - 480 : 0, last + 1) < 1.0e-4f, "case %d: the tail ends at %.6f, not at silence", which, peakOf(d.L, last > 480 ? last - 480 : 0, last + 1));
        CHECK(s.activeVoices() == 0);
    }
    // Kill fades everything in about 3 ms
    for (auto type : {NoteEvent::Kill, NoteEvent::KillNote}) {
        Sextet s;
        Drive d(s, sr);
        d.on(0, 57, 1);
        NoteEvent k;
        k.type = type;
        k.id = 1;
        d.ev(24010, k);
        d.run(48000);
        CHECK(peakOf(d.L, 20000, 24000) > 0.1f);
        CHECK_MSG(maxStep(d.L, 24000, 48000) <= maxStep(d.L, 12000, 24000) * 1.3f, "the cut steps by %.4f", maxStep(d.L, 24000, 48000));
        CHECK_MSG(peakOf(d.L, 24010 + 400, 48000) < 2.0e-3f, "still %.5f after the cut", peakOf(d.L, 24010 + 400, 48000));
        CHECK(peakOf(d.L, 36000, 48000) < 1.0e-6f);
    }
    // AllOff releases everything
    {
        Sextet s;
        Drive d(s, sr);
        d.on(0, 57, 1);
        d.on(0, 64, 2);
        NoteEvent a;
        a.type = NoteEvent::AllOff;
        d.ev(12000, a);
        d.run(96000);
        CHECK(peakOf(d.L, 72000, 96000) < 1.0e-6f && s.activeVoices() == 0);
    }
}

TEST(sextet_presets_all_sound_and_reload) {
    Sextet proto;
    const auto presets = proto.factoryPresets();
    CHECK_MSG(presets.size() >= 24, "%zu presets", presets.size());
    std::vector<std::string> names;
    float quietest = 10.0f, loudest = 0.0f;
    std::vector<std::shared_ptr<SampleData>> sounds;
    for (auto& pr : presets) {
        CHECK_MSG(std::find(names.begin(), names.end(), pr.name) == names.end(), "two presets called %s", pr.name.c_str());
        names.push_back(pr.name);
        auto s = std::make_shared<Sextet>();
        s->loadState(pr.state);
        // one note at velocity 100
        auto one = play(s, {mk(0, 1920, 60)}, {48000.0, 120.0, 512, 1, 96000});
        const float pk = std::max(peakOf(one->ch[0]), peakOf(one->ch[1]));
        quietest = std::min(quietest, pk);
        loudest = std::max(loudest, pk);
        if (verbose()) printf("    %-20s one note peaks at %6.1f dBFS\n", pr.name.c_str(), gainToDb(pk));
        CHECK_MSG(allFinite(one->ch[0]) && allFinite(one->ch[1]), "%s produced a non-finite sample", pr.name.c_str());
        CHECK_MSG(pk > 0.18f && pk < 0.56f, "%s: one note peaks at %.1f dBFS (-12 to -6 wanted, 3 dB of slack)", pr.name.c_str(), gainToDb(pk));
        // a chord, played hard, across the keyboard
        std::vector<Note> chord;
        for (int k : {36, 48, 55, 60, 64, 67, 72, 84}) chord.push_back(mk(0, 1920, k, 1.0f));
        auto many = play(s, chord, {48000.0, 120.0, 512, 1, 96000});
        CHECK_MSG(allFinite(many->ch[0]) && allFinite(many->ch[1]), "%s: a chord produced a non-finite sample", pr.name.c_str());
        std::vector<Note> triad;
        for (int k : {48, 60, 67}) triad.push_back(mk(0, 1920, k));
        auto three = play(s, triad, {48000.0, 120.0, 512, 1, 96000});
        CHECK_MSG(std::max(peakOf(three->ch[0]), peakOf(three->ch[1])) < 2.0f, "%s: a triad peaks at %.2f, over +6 dBFS", pr.name.c_str(), peakOf(three->ch[0]));
        // it stops after the note ends
        CHECK_MSG(peakOf(one->ch[0], 48000 + 40000, 96000) < 0.5f * pk + 1.0e-4f || s->param("op1release")->get() > 2000.0f, "%s never dies away", pr.name.c_str());
        // state round trip: same parameters, same sound
        auto t = std::make_shared<Sextet>();
        t->loadState(s->saveState());
        CHECK_MSG(t->saveState() == s->saveState(), "%s does not survive save and load", pr.name.c_str());
        const Json reparsed = Json::parse(s->saveState().dump());
        auto u = std::make_shared<Sextet>();
        u->loadState(reparsed);
        auto again = play(u, {mk(0, 1920, 60)}, {48000.0, 120.0, 512, 1, 96000});
        CHECK_MSG(identical(*one, *again), "%s sounds different after save and load", pr.name.c_str());
        for (auto& other : sounds) CHECK_MSG(!identical(*other, *one), "%s sounds exactly like another preset", pr.name.c_str());
        sounds.push_back(one);
    }
    printf("    %zu presets; one note at velocity 100 peaks between %.1f and %.1f dBFS\n", presets.size(), gainToDb(quietest), gainToDb(loudest));
    printf("    %zu parameters in %zu groups\n", proto.params().size(), proto.paramGroups().size());
    CHECK(groupsCoverParams(proto));
    CHECK(proto.paramGroups().size() == 9);     // six operators, the matrix, pitch / LFO, global
}

TEST(sextet_render_is_identical_at_any_block_size_and_thread_count_and_never_allocates) {
    for (const char* name : {"Aurora Pad", "Glass Tines", "Alien Signal", "Warm Strings"}) {
        std::shared_ptr<SampleData> ref;
        struct Case { int block, threads; } cases[] = {{64, 1}, {33, 1}, {4096, 1}, {512, 4}, {33, 4}};
        for (auto& c : cases) {
            auto s = std::make_shared<Sextet>();
            loadPreset(*s, name);
            s->pGlide->set(60.0f);
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
            CHECK_MSG(identical(*ref, *out), "%s: block %d, %d threads differs", name, c.block, c.threads);
        }
    }
}

TEST(sextet_parameters_can_move_while_notes_sound) {
    // Every parameter is automatable: move them all at random between quanta while notes play. Nothing may
    // blow up, and level changes must be ramped (no steps bigger than the tone itself makes).
    Sextet s;
    loadPreset(s, "Brass Section");
    Drive d(s);
    for (int i = 0; i < 8; ++i) d.on(i * 3000, 48 + i * 3, (uint32)(i + 1));
    uint32 rnd = 12345;
    for (int q = 0; q < 96000 / kQuantum; ++q) {
        for (auto& p : s.params()) {
            rnd = rnd * 1664525u + 1013904223u;
            if ((rnd >> 24) < 40) p->setNorm((float)((rnd >> 8) & 0xffff) / 65535.0f);
        }
        d.run(kQuantum);
    }
    CHECK(allFinite(d.L) && allFinite(d.R));
    CHECK_MSG(peakOf(d.L) < 100.0f, "peak %.2f", peakOf(d.L));
    // the master level alone, stepped every quantum: the output follows without a jump
    Sextet t;
    t.pAlgo->set(5.0f);
    t.op[0].sustain->set(1.0f);
    Drive e(t);
    e.on(0, 45, 1);
    for (int q = 0; q < 400; ++q) { t.pVolume->set(q % 2 ? 1.0f : 0.2f); e.run(kQuantum); }
    CHECK_MSG(maxStep(e.L, 2000, e.L.size()) < 0.03f, "largest step %.4f", maxStep(e.L, 2000, e.L.size()));
}
