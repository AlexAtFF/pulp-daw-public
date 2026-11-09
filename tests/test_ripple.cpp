// Ripple, the wavetable synth.
#include "FmwtTestUtil.h"
#include "pulp/Ripple.h"
#include <filesystem>
#include <thread>
using namespace pulp;
using namespace ft;

namespace {

// Oscillator 1 alone, no filter, flat envelope, velocity ignored: what the table holds is what comes out.
std::shared_ptr<Ripple> plainRipple(int table = 0, float pos = 0.0f) {
    auto r = std::make_shared<Ripple>();
    r->osc[0].table->set((float)table);
    r->osc[0].pos->set(pos);
    r->osc[0].level->set(1.0f);
    r->pFType->set(0.0f);
    r->pAttack->set(0.0f);
    r->pSustain->set(1.0f);
    r->pVelSens->set(0.0f);
    return r;
}

int tableNamed(const char* name) {
    for (int i = 0; i < Ripple::numBuiltinTables(); ++i) if (Ripple::builtinTable(i).name == name) return i;
    CHECK_MSG(false, "no table called %s", name);
    return 0;
}

void loadPreset(Ripple& r, const std::string& name) {
    for (auto& pr : r.factoryPresets()) if (pr.name == name) { r.loadState(pr.state); return; }
    CHECK_MSG(false, "no preset %s", name.c_str());
}

bool verbose() { return std::getenv("PULP_TEST_VERBOSE") != nullptr; }

std::string tmpFile(const char* name) { return (std::filesystem::path(t::tmpDir()) / name).string(); }

// Writes cycles (each a list of {harmonic, amplitude}) of `len` samples one after another to a WAV file.
std::string writeCycles(const char* file, const std::vector<std::vector<std::pair<int, double>>>& cycles, int len = Ripple::kFrameSize, float scale = 0.5f) {
    SampleData s;
    s.sampleRate = 48000.0;
    s.channels = 1;
    for (auto& c : cycles)
        for (int i = 0; i < len; ++i) {
            double v = 0.0;
            for (auto& h : c) v += h.second * std::sin(kTwoPi * h.first * i / len);
            s.ch[0].push_back(scale * (float)v);
        }
    const std::string path = tmpFile(file);
    std::string err;
    CHECK_MSG(writeWavFile(path, s, WavFormat::Float32, false, &err), "%s", err.c_str());
    return path;
}

} // namespace

TEST(ripple_oscillators_are_in_tune_at_every_sample_rate) {
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int key : {36, 57, 60, 72, 84, 96}) {
            auto r = plainRipple();
            RenderSpec rs;
            rs.sr = sr;
            rs.length = (int64)sr;
            auto out = play(r, {mk(0, 3840, key)}, rs);
            CHECK(out != nullptr);
            if (!out) return;
            const double hz = preciseHz(out->ch[0], (size_t)(sr * 0.2), sr, keyToHz(key));
            CHECK_MSG(std::fabs(centsOff(hz, keyToHz(key))) < 0.2, "%.0f Hz key %d: %.3f Hz, %.3f cents off", sr, key, hz, centsOff(hz, keyToHz(key)));
            const float pk = peakOf(out->ch[0]);
            CHECK_MSG(pk > 0.25f && pk < 0.5f, "a single note peaks at %.3f (-12 to -6 dBFS wanted)", pk);
        }
}

TEST(ripple_every_oscillator_tuning_control_and_table_is_in_tune) {
    const double sr = 48000.0, f = keyToHz(60);
    auto hzOf = [&](const std::shared_ptr<Ripple>& r, double guess) {
        auto out = play(r, {mk(0, 3840, 60)});
        CHECK_MSG(ampAt(out->ch[0], 9600, 16384, guess, sr) > 0.02, "nothing at %.1f Hz", guess);
        return preciseHz(out->ch[0], 9600, sr, guess);
    };
    {   // oscillator 2, coarse and fine
        auto r = plainRipple();
        r->osc[0].level->set(0.0f);
        r->osc[1].table->set(0.0f);
        r->osc[1].level->set(1.0f);
        CHECK_NEAR(centsOff(hzOf(r, f), f), 0.0, 0.2);
        r->osc[1].coarse->set(7.0f);
        r->osc[1].fine->set(50.0f);
        CHECK_NEAR(centsOff(hzOf(r, f * std::pow(2.0, 7.5 / 12.0)), f), 750.0, 0.2);
        r->osc[1].coarse->set(-12.0f);
        r->osc[1].fine->set(-25.0f);
        CHECK_NEAR(centsOff(hzOf(r, f * std::pow(2.0, -12.25 / 12.0)), f), -1225.0, 0.2);
    }
    // the sub oscillator: every shape, one and two octaves below
    for (int shape = 0; shape < 4; ++shape)
        for (int oct = 0; oct < 2; ++oct) {
            auto r = plainRipple();
            r->osc[0].level->set(0.0f);
            r->pSubWave->set((float)shape);
            r->pSubOct->set((float)oct);
            r->pSubLevel->set(1.0f);
            const double want = f / (oct ? 4.0 : 2.0);
            CHECK_MSG(std::fabs(centsOff(hzOf(r, want), want)) < 0.2, "sub shape %d octave %d", shape, oct);
        }
    // every built-in table at three positions: whichever partial is strongest sits on a multiple of the note
    for (int t = 0; t < Ripple::numBuiltinTables(); ++t)
        for (float pos : {0.0f, 0.37f, 1.0f}) {
            auto r = plainRipple(t, pos);
            auto out = play(r, {mk(0, 3840, 60)});
            const auto m = spectrum(out->ch[0], 9600, 32768);
            size_t best = 8;
            for (size_t b = 8; b < m.size(); ++b) if (m[b] > m[best]) best = b;
            const double k = std::round(best * sr / 32768.0 / f);
            CHECK_MSG(k >= 1.0, "%s at %.2f: the strongest partial is below the note", Ripple::builtinTable(t).name.c_str(), pos);
            const double hz = preciseHz(out->ch[0], 9600, sr, k * f);
            CHECK_MSG(std::fabs(centsOff(hz, k * f)) < 0.3, "%s at %.2f: partial %g is %.3f cents off", Ripple::builtinTable(t).name.c_str(), pos, k, centsOff(hz, k * f));
        }
    {   // unison: the voices sit either side of the note, the outermost exactly the detune away
        auto r = plainRipple();
        r->osc[0].uni->set(7.0f);
        r->osc[0].detune->set(30.0f);
        auto out = play(r, {mk(0, 7680, 60)}, {sr, 120.0, 512, 1, 144000});
        for (double cents : {30.0, -30.0, 0.0}) {
            const double want = f * std::pow(2.0, cents / 1200.0);
            const double hz = preciseHz(out->ch[0], 9600, sr, want, 65536, 16384);
            CHECK_MSG(std::fabs(centsOff(hz, want)) < 0.5, "unison voice at %+.0f cents is %.3f cents off", cents, centsOff(hz, want));
        }
    }
}

TEST(ripple_wavetables_do_not_alias_anywhere_on_the_keyboard) {
    struct Case { const char* table; float pos; };
    const Case cases[] = {{"Basic Shapes", 0.5f}, {"Basic Shapes", 1.0f}, {"Pulse Width", 1.0f}, {"Saw Sync", 1.0f}, {"Digital Grit", 1.0f},
                          {"Stairs", 1.0f}, {"Harmonic Sweep", 1.0f}, {"Folded Sine", 1.0f}};
    double worst = -200.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (auto& c : cases)
            for (int key : {24, 36, 48, 60, 72, 84, 96, 108, 120}) {
                auto r = plainRipple(tableNamed(c.table), c.pos);
                Note n = mk(0, 7680, key);
                n.fine = 37.0f;         // off the grid, so aliases cannot hide under harmonics
                const int fftSize = sr > 50000.0 ? 131072 : 32768;       // fine enough to see between the harmonics of the lowest note
                auto out = play(r, {n}, {sr, 120.0, 512, 1, fftSize + 8192});
                const double db = aliasDb(out->ch[0], 4096, fftSize, sr, keyToHz(key + 0.37), 20.0, 18000.0);
                if (verbose()) printf("    %.0f Hz  %-16s %.1f key %3d: %7.1f dB\n", sr, c.table, c.pos, key, db);
                worst = std::max(worst, db);
                CHECK_MSG(db < -70.0, "%.0f Hz, %s at %.1f, key %d: aliases at %.1f dB", sr, c.table, c.pos, key, db);
            }
    printf("    worst alias below 18 kHz: %.1f dB below the strongest partial\n", worst);
    // ... and it is not done by throwing the top away: a low saw keeps its harmonics up to 14 kHz and beyond
    for (double sr : {44100.0, 48000.0}) {
        auto r = plainRipple(0, 0.5f);
        for (int key : {36, 41, 47, 53, 60}) {
            auto out = play(r, {mk(0, 3840, key)}, {sr, 120.0, 512, 1, (int64)sr});
            const double f = keyToHz(key);
            const int h = (int)(13000.0 / f);
            const double got = ampAt(out->ch[0], 9600, 16384, f * h, sr) / ampAt(out->ch[0], 9600, 16384, f, sr);
            CHECK_MSG(std::fabs(gainToDb((float)(got * h))) < 1.0, "%.0f Hz key %d: harmonic %d is %.2f dB off a saw's 1/h", sr, key, h, gainToDb((float)(got * h)));
        }
    }
}

TEST(ripple_builtin_wavetables_are_well_formed) {
    const int n = Ripple::numBuiltinTables();
    CHECK_MSG(n >= 16, "%d tables", n);
    CHECK(n == 22);
    std::vector<std::string> names;
    size_t bytes = 0;
    for (int t = 0; t < n; ++t) {
        const Ripple::Wavetable& w = Ripple::builtinTable(t);
        CHECK(!w.name.empty() && std::find(names.begin(), names.end(), w.name) == names.end());
        names.push_back(w.name);
        CHECK_MSG(w.frames >= 2 && w.frames <= Ripple::kMaxFrames, "%s has %d frames", w.name.c_str(), w.frames);
        bytes += w.data.size() * sizeof(float);
        for (int f = 0; f < w.frames; ++f) {
            const float* d = w.frame(0, f);
            const int len = Ripple::Wavetable::mipSize(0);
            double pk = 0.0, mean = 0.0;
            for (int i = 0; i < len; ++i) { pk = std::max(pk, (double)std::fabs(d[i])); mean += d[i]; }
            CHECK_MSG(std::fabs(pk - 1.0) < 1.0e-3, "%s frame %d peaks at %.4f", w.name.c_str(), f, pk);
            CHECK_MSG(std::fabs(mean / len) < 1.0e-4, "%s frame %d has DC %.5f", w.name.c_str(), f, mean / len);
            for (int m = 0; m < Ripple::kMips; ++m) {
                const float* x = w.frame(m, f);
                const int size = Ripple::Wavetable::mipSize(m);
                CHECK(x[-2] == x[size - 2] && x[-1] == x[size - 1] && x[size] == x[0] && x[size + 1] == x[1] && x[size + 2] == x[2]);
            }
        }
        // every level holds only the harmonics it is meant to
        for (int m = 0; m < Ripple::kMips; ++m) {
            const int size = Ripple::Wavetable::mipSize(m), keep = Ripple::Wavetable::mipHarmonics(m);
            const float* x = w.frame(m, w.frames - 1);
            std::vector<float> re(x, x + size), im((size_t)size, 0.0f);
            dsp::fft(re.data(), im.data(), size);
            double above = 0.0;
            for (int b = keep + 1; b <= size / 2; ++b) above = std::max(above, (double)std::hypot(re[(size_t)b], im[(size_t)b]) / size);
            CHECK_MSG(above < 1.0e-5, "%s level %d has a partial above harmonic %d (%.2g)", w.name.c_str(), m, keep, above);
        }
        // it sounds at both ends and in the middle
        for (float pos : {0.0f, 0.5f, 1.0f}) {
            auto out = play(plainRipple(t, pos), {mk(0, 960, 57)}, {48000.0, 120.0, 512, 1, 12000});
            CHECK_MSG(allFinite(out->ch[0]) && peakOf(out->ch[0]) > 0.1f && peakOf(out->ch[0]) < 0.6f, "%s at %.1f peaks at %.3f", w.name.c_str(), pos, peakOf(out->ch[0]));
        }
    }
    for (const char* want : {"Basic Shapes", "Pulse Width", "Harmonic Sweep", "Vowels", "FM Bright", "Bell Tower", "Drawbars", "Saw Sync", "Digital Grit"})
        CHECK_MSG(std::find(names.begin(), names.end(), want) != names.end(), "no table called %s", want);
    printf("    %d tables, %.1f MB in all, shared by every instance\n", n, bytes / 1048576.0);
}

TEST(ripple_position_morphs_between_neighbouring_frames) {
    const double sr = 48000.0;
    // Basic Shapes has five frames: position 0 is the sine, 0.25 the triangle, and half way is half of each.
    auto at = [&](float pos) { return play(plainRipple(0, pos), {mk(0, 960, 60)}, {sr, 120.0, 512, 1, 12000}); };
    auto a = at(0.0f), b = at(0.25f), mid = at(0.125f), quarter = at(0.0625f);
    double worst = 0.0, worstQ = 0.0;
    for (size_t i = 0; i < 12000; ++i) {
        worst = std::max(worst, (double)std::fabs(mid->ch[0][i] - 0.5f * (a->ch[0][i] + b->ch[0][i])));
        worstQ = std::max(worstQ, (double)std::fabs(quarter->ch[0][i] - (0.75f * a->ch[0][i] + 0.25f * b->ch[0][i])));
    }
    CHECK_MSG(worst < 1.0e-5 && worstQ < 1.0e-5, "off a straight blend by %g and %g", worst, worstQ);
    CHECK(maxAbsDiff(a->ch[0], b->ch[0]) > 0.02);
    // the frames are what they say: triangle and square have odd harmonics only, the saw has them all
    const double f = keyToHz(60);
    auto h = [&](const SampleData& o, int k) { return ampAt(o.ch[0], 2400, 8192, f * k, sr); };
    CHECK(h(*b, 2) < 1.0e-4 && h(*b, 3) / h(*b, 1) > 0.1 && h(*b, 3) / h(*b, 1) < 0.12);            // triangle: 1/9
    auto saw = at(0.5f), sq = at(0.75f);
    CHECK_NEAR(h(*saw, 2) / h(*saw, 1), 0.5, 0.005);
    CHECK_NEAR(h(*saw, 5) / h(*saw, 1), 0.2, 0.003);
    CHECK(h(*sq, 2) < 1.0e-4);
    CHECK_NEAR(h(*sq, 3) / h(*sq, 1), 1.0 / 3.0, 0.004);
    // sweeping the position is smooth: no step bigger than the frames themselves make
    const int fm = tableNamed("FM Bright");
    float still = 0.0f;
    for (float pos = 0.0f; pos <= 1.0f; pos += 0.125f) still = std::max(still, maxStep(play(plainRipple(fm, pos), {mk(0, 960, 60)}, {sr, 120.0, 512, 1, 12000})->ch[0], 0, 12000));
    auto sweep = plainRipple(fm, 0.0f);
    sweep->osc[0].posEnv->set(1.0f);
    sweep->pMAttack->set(400.0f);
    sweep->pMDecay->set(400.0f);
    auto moving = play(sweep, {mk(0, 1920, 60)}, {sr, 120.0, 512, 1, 48000});
    CHECK_MSG(maxStep(moving->ch[0], 0, 48000) < still * 1.2f, "a sweep steps by %.4f, the still frames by %.4f", maxStep(moving->ch[0], 0, 48000), still);
}

TEST(ripple_user_wavetable_loads_plays_and_persists) {
    const double sr = 48000.0, f = keyToHz(60);
    // four cycles of 2048 samples: harmonic 1, 2, 3 and 4
    const std::string path = writeCycles("ripple_four.wav", {{{1, 1.0}}, {{2, 1.0}}, {{3, 1.0}}, {{4, 1.0}}});
    auto r = plainRipple(Ripple::userTableIndex(), 0.0f);
    std::string err;
    CHECK_MSG(r->loadWavetable(path, &err), "%s", err.c_str());
    CHECK(r->wavetable() && r->wavetable()->frames == 4);
    CHECK(r->wavetablePath() == path);
    auto h = [&](const SampleData& o, int k) { return ampAt(o.ch[0], 9600, 16384, f * k, sr); };
    auto at = [&](const std::shared_ptr<Ripple>& s, float pos) { s->osc[0].pos->set(pos); return play(s, {mk(0, 1920, 60)}, {sr, 120.0, 512, 1, 36000}); };
    {
        auto o = at(r, 0.0f);
        CHECK_NEAR(h(*o, 1), 0.35, 0.004);          // levelled to full scale, then the default volume
        CHECK(h(*o, 2) < 1.0e-4);
        o = at(r, 1.0f / 3.0f);
        CHECK_NEAR(h(*o, 2), 0.35, 0.004);
        CHECK(h(*o, 1) < 1.0e-3 && h(*o, 3) < 1.0e-3);
        o = at(r, 0.5f);
        CHECK_NEAR(h(*o, 2), 0.175, 0.003);
        CHECK_NEAR(h(*o, 3), 0.175, 0.003);
        o = at(r, 1.0f);
        CHECK_NEAR(h(*o, 4), 0.35, 0.004);
    }
    // the choice is in the saved state under "wavetable" and comes back
    const Json st = r->saveState();
    CHECK(st["extra"]["wavetable"].str("") == path);
    {
        auto s = std::make_shared<Ripple>();
        s->loadState(Json::parse(st.dump()));
        CHECK(s->wavetablePath() == path && s->wavetable() && s->wavetable()->frames == 4);
        CHECK(s->saveState() == st);
        auto a = at(r, 0.4f), b = at(s, 0.4f);
        CHECK(identical(*a, *b));
    }
    {   // a path inside the project folder is stored relative and resolved against it
        Json j = Json::object();
        j.set("wavetable", "ripple_four.wav");
        auto s = plainRipple(Ripple::userTableIndex(), 0.0f);
        s->loadExtra(j, t::tmpDir());
        CHECK(s->wavetable() && s->wavetable()->frames == 4);
        CHECK(s->saveExtra()["wavetable"].str("") == "ripple_four.wav");
        CHECK_NEAR(h(*at(s, 1.0f), 4), 0.35, 0.004);
    }
    {   // a missing file: the fallback table plays, the reference is kept for the next save
        Json j = Json::object();
        j.set("wavetable", "no_such_table.wav");
        auto s = plainRipple(Ripple::userTableIndex(), 0.0f);
        s->loadExtra(j, t::tmpDir());
        CHECK(!s->wavetable());
        CHECK(s->saveExtra()["wavetable"].str("") == "no_such_table.wav");
        auto fallback = at(s, 0.0f), first = at(plainRipple(0, 0.0f), 0.0f);
        CHECK(identical(*fallback, *first));
        // loading a state with no table clears it
        s->loadState(Ripple().saveState());
        CHECK(s->wavetablePath().empty());
    }
    {   // a file that cannot be read leaves the current table alone and says why
        std::string e2;
        CHECK(!r->loadWavetable(tmpFile("no_such_table.wav"), &e2));
        CHECK(!e2.empty());
        CHECK(r->wavetablePath() == path && r->wavetable()->frames == 4);
        CHECK(!r->loadWavetable("", &e2));
    }
    // a single cycle of any length is resampled to 2048: 600 samples of a five-harmonic tone
    for (int len : {600, 337, 2047, 5000, 40000}) {
        const std::string one = writeCycles("ripple_one.wav", {{{1, 1.0}, {2, 0.5}, {3, 0.25}, {5, 0.125}}}, len);
        auto s = plainRipple(Ripple::userTableIndex(), 0.0f);
        CHECK_MSG(s->loadWavetable(one, &err), "%s", err.c_str());
        CHECK(s->wavetable() && s->wavetable()->frames == 1);
        auto o = at(s, 0.7f);
        CHECK_MSG(std::fabs(centsOff(preciseHz(o->ch[0], 9600, sr, f), f)) < 0.2, "a %d-sample cycle is out of tune", len);
        CHECK_MSG(std::fabs(h(*o, 2) / h(*o, 1) - 0.5) < 0.005 && std::fabs(h(*o, 3) / h(*o, 1) - 0.25) < 0.005 && std::fabs(h(*o, 5) / h(*o, 1) - 0.125) < 0.005 && h(*o, 4) / h(*o, 1) < 0.002,
                  "a %d-sample cycle: harmonics %.4f %.4f %.4f %.4f", len, h(*o, 2) / h(*o, 1), h(*o, 3) / h(*o, 1), h(*o, 4) / h(*o, 1), h(*o, 5) / h(*o, 1));
    }
    // a bright user table is band-limited like the built-in ones: a 2048-sample saw, played high
    {
        std::vector<float> saw((size_t)Ripple::kFrameSize);
        for (int i = 0; i < Ripple::kFrameSize; ++i) saw[(size_t)i] = 1.0f - 2.0f * (float)i / Ripple::kFrameSize;
        auto s = plainRipple(Ripple::userTableIndex(), 0.0f);
        s->setWavetable(Ripple::makeTable(saw.data(), 1, "saw"));
        for (int key : {36, 84, 108}) {
            Note n = mk(0, 3840, key);
            n.fine = 37.0f;
            auto o = play(s, {n});
            const double db = aliasDb(o->ch[0], 4096, 32768, sr, keyToHz(key + 0.37), 20.0, 18000.0);
            CHECK_MSG(db < -70.0, "a user saw at key %d aliases at %.1f dB", key, db);
        }
    }
}

TEST(ripple_swapping_the_user_table_while_notes_sound_is_safe_and_smooth) {
    const double sr = 48000.0;
    // Two tables that are each other upside down: a hard switch would jump by up to twice the amplitude.
    std::vector<float> up((size_t)Ripple::kFrameSize), down((size_t)Ripple::kFrameSize);
    for (int i = 0; i < Ripple::kFrameSize; ++i) { up[(size_t)i] = (float)std::sin(kTwoPi * i / Ripple::kFrameSize); down[(size_t)i] = -up[(size_t)i]; }
    auto a = Ripple::makeTable(up.data(), 1, "up"), b = Ripple::makeTable(down.data(), 1, "down");
    {
        Ripple r;
        r.osc[0].table->set((float)Ripple::userTableIndex());
        r.osc[0].level->set(1.0f);
        r.osc[0].pos->set(0.0f);        // the fallback, the first built-in table, is then a sine too
        r.pSustain->set(1.0f);
        r.setWavetable(a);
        Drive d(r, sr);
        d.on(0, 60, 1);
        d.run(9600);
        const float steady = maxStep(d.L, 4800, 9600);
        for (int i = 0; i < 40; ++i) { r.setWavetable(i % 2 ? a : b); d.run(64 * (1 + i % 5)); }
        // cut over at once this would be a jump of up to 0.7 (twice the amplitude); cross-faded over a control period it is a few hundredths
        CHECK_MSG(maxStep(d.L, 9600, d.L.size()) < 0.05f, "a swap steps by %.4f, the tone itself by %.4f", maxStep(d.L, 9600, d.L.size()), steady);
        // and the new table is what plays afterwards: after an odd number of swaps the tone is inverted
        Ripple ref;
        ref.osc[0].table->set((float)Ripple::userTableIndex());
        ref.osc[0].level->set(1.0f);
        ref.pSustain->set(1.0f);
        ref.setWavetable(a);
        Drive e(ref, sr);
        e.on(0, 60, 1);
        e.run((int64)d.L.size() + 4800);
        r.setWavetable(b);
        d.run(4800);
        double worst = 0.0;
        for (size_t i = d.L.size() - 2400; i < d.L.size(); ++i) worst = std::max(worst, (double)std::fabs(d.L[i] + e.L[i]));
        CHECK_MSG(worst < 1.0e-4, "after the swap the tone is %.5f away from the new table", worst);
        // clearing it falls back to the first built-in table, again without a jump
        const size_t before = d.L.size();
        r.setWavetable(nullptr);
        d.run(4800);
        CHECK(maxStep(d.L, before, d.L.size()) < 0.05f);
        CHECK(peakOf(d.L, d.L.size() - 2400, d.L.size()) > 0.2f);
    }
    // Another thread loads tables from disk as fast as it can while the audio thread plays chords.
    {
        const std::string p1 = writeCycles("ripple_swap_a.wav", {{{1, 1.0}}, {{1, 0.5}, {2, 0.5}}, {{3, 1.0}}});
        const std::string p2 = writeCycles("ripple_swap_b.wav", {{{1, 1.0}, {4, 0.3}}}, 733);
        Ripple r;
        r.osc[0].table->set((float)Ripple::userTableIndex());
        r.osc[1].table->set((float)Ripple::userTableIndex());
        r.osc[1].level->set(0.5f);
        r.osc[0].uni->set(3.0f);
        r.osc[0].posLfo[0]->set(0.5f);
        r.pLfoRate[0]->set(7.0f);
        Drive d(r, sr);
        for (int i = 0; i < 40; ++i) { d.on(i * 2000, 40 + (i * 7) % 40, (uint32)(i + 1)); d.off(i * 2000 + 9000, (uint32)(i + 1)); }
        std::atomic<bool> stop{false};
        std::atomic<int> loads{0};
        std::thread loader([&] {
            std::string err;
            while (!stop.load()) {
                if (!r.loadWavetable(loads.load() % 2 ? p1 : p2, &err)) break;
                if (loads.fetch_add(1) % 7 == 6) r.setWavetable(nullptr);
            }
        });
        const int64 before = Rt::allocationsInRt.load();
        while (loads.load() < 60 && d.pos < 48000 * 20) d.run(960);
        stop.store(true);
        loader.join();
        CHECK(Rt::allocationsInRt.load() == before);
        CHECK_MSG(loads.load() >= 60, "only %d loads happened", loads.load());
        CHECK(allFinite(d.L) && allFinite(d.R));
        CHECK_MSG(peakOf(d.L) > 0.05f && peakOf(d.L) < 4.0f, "peak %.3f after %lld samples", peakOf(d.L), (long long)d.pos);
    }
}

TEST(ripple_note_on_lands_on_the_exact_sample) {
    auto r = std::make_shared<Ripple>();
    r->pAttack->set(0.0f);
    auto a = play(r, {mk(37, 960, 60)}, {48000.0, 120.0, 512, 1, 24000});      // tick 37 is sample 925
    for (size_t i = 0; i < 925; ++i) if (a->ch[0][i] != 0.0f) { CHECK_MSG(false, "sound at sample %zu, before the note", i); break; }
    CHECK_MSG(std::fabs(a->ch[0][925]) > 1.0e-3f || std::fabs(a->ch[0][926]) > 1.0e-3f, "nothing at the note's first samples (%g, %g)", a->ch[0][925], a->ch[0][926]);
    auto b = play(r, {mk(38, 960, 60)}, {48000.0, 120.0, 512, 1, 24000});
    double worst = 0.0;
    for (size_t i = 0; i + 25 < 20000; ++i) worst = std::max(worst, (double)std::fabs(a->ch[0][i] - b->ch[0][i + 25]));
    CHECK_MSG(worst < 1.0e-6, "shifted by a tick, the note differs by %g", worst);
}

TEST(ripple_portamento_glide_and_slide_notes) {
    const double sr = 48000.0;
    {
        auto out = play(plainRipple(), {mk(0, 960, 60), mk(960, 1920, 72)}, {sr, 120.0, 512, 1, 72000},
                        [](Project&, ChannelLive& l) { l.porta.set(1); l.mono.set(1); l.portaTime.set(200.0f); });
        const size_t on = 960 * 25;
        const double early = measureHz(out->ch[0], on + 300, on + 1500, sr), late = measureHz(out->ch[0], on + 14400, on + 24000, sr);
        CHECK_MSG(early > keyToHz(60) * 1.01 && early < keyToHz(66), "early pitch %.1f Hz", early);
        CHECK_MSG(std::fabs(centsOff(late, keyToHz(72))) < 2.0, "late pitch %.1f Hz", late);
    }
    {
        auto r = plainRipple();
        r->pGlide->set(200.0f);
        auto out = play(r, {mk(0, 900, 60), mk(960, 1920, 72)}, {sr, 120.0, 512, 1, 72000});
        const size_t on = 960 * 25;
        const double early = measureHz(out->ch[0], on + 300, on + 1500, sr), late = measureHz(out->ch[0], on + 14400, on + 24000, sr);
        CHECK_MSG(early > keyToHz(60) * 1.01 && early < keyToHz(66), "early pitch %.1f Hz", early);
        CHECK_MSG(std::fabs(centsOff(late, keyToHz(72))) < 2.0, "late pitch %.1f Hz", late);
    }
    {
        Note held = mk(0, 3840, 60), other = mk(0, 3840, 72), slide = mk(960, 960, 67);
        held.pan = -1.0f;
        other.pan = 1.0f;
        other.color = 3;
        slide.slide = true;
        auto out = play(plainRipple(), {held, other, slide}, {sr, 120.0, 512, 1, 96000});
        CHECK_NEAR(measureHz(out->ch[0], 4800, 22000, sr), keyToHz(60), 0.5);
        const double mid = measureHz(out->ch[0], 960 * 25 + 9600, 960 * 25 + 14400, sr);
        CHECK_MSG(mid > keyToHz(62) && mid < keyToHz(65.5), "mid-slide pitch %.2f Hz", mid);
        CHECK_NEAR(measureHz(out->ch[0], 60000, 90000, sr), keyToHz(67), 0.8);
        CHECK_NEAR(measureHz(out->ch[1], 60000, 90000, sr), keyToHz(72), 0.8);
    }
    {   // the channel pitch and per-note fine pitch
        Ripple r;
        r.pSustain->set(1.0f);
        r.osc[0].pos->set(0.0f);
        Drive d(r, sr);
        d.bend = -3.0f;
        NoteEvent e;
        e.type = NoteEvent::On; e.key = 60; e.id = 1; e.fineCents = 40.0f;
        d.ev(0, e);
        d.run(48000);
        CHECK_NEAR(centsOff(preciseHz(d.L, 9600, sr, keyToHz(57.4)), keyToHz(60)), -260.0, 0.2);
    }
    {   // a long glide through several wavetable levels stays click-free and band-limited
        auto r = plainRipple(tableNamed("Basic Shapes"), 0.5f);
        r->pGlide->set(1500.0f);
        auto out = play(r, {mk(0, 400, 36), mk(480, 3000, 84)}, {sr, 120.0, 512, 1, 96000});
        CHECK(allFinite(out->ch[0]));
        // moving from level to level adds no step of its own: nothing bigger than the saw's own edge at the pitches it passes
        float edge = 0.0f;
        for (int key = 36; key <= 84; ++key)
            edge = std::max(edge, maxStep(play(plainRipple(tableNamed("Basic Shapes"), 0.5f), {mk(0, 960, key)}, {sr, 120.0, 512, 1, 12000})->ch[0], 2400, 12000));
        CHECK_MSG(maxStep(out->ch[0], 480 * 25 + 64, 96000) < edge * 1.1f, "largest step %.3f, the saw's own edge %.3f", maxStep(out->ch[0], 480 * 25 + 64, 96000), edge);
    }
}

TEST(ripple_velocity_pan_mod_x_and_mod_y_are_heard) {
    const double sr = 48000.0, f = keyToHz(48);
    auto run = [&](const std::shared_ptr<Ripple>& r, Note n) { return play(r, {n}, {sr, 120.0, 512, 1, 36000}); };
    {   // velocity
        auto r = plainRipple();
        r->pVelSens->set(1.0f);
        auto soft = run(r, mk(0, 1920, 48, 0.4f)), ref = run(r, mk(0, 1920, 48)), hard = run(r, mk(0, 1920, 48, 1.0f));
        CHECK(rmsOf(soft->ch[0], 9600, 30000) < 0.5 * rmsOf(ref->ch[0], 9600, 30000));
        CHECK(rmsOf(hard->ch[0], 9600, 30000) > 1.3 * rmsOf(ref->ch[0], 9600, 30000));
        // velocity can open the filter too
        auto v = plainRipple(0, 0.5f);
        v->pFType->set(1.0f);
        v->pCutoff->set(600.0f);
        v->pFVel->set(1.0f);
        auto dull = run(v, mk(0, 1920, 48, 0.4f)), bright = run(v, mk(0, 1920, 48, 1.0f));
        auto tilt = [&](const SampleData& o) { return ampAt(o.ch[0], 9600, 16384, 10 * f, sr) / ampAt(o.ch[0], 9600, 16384, f, sr); };
        CHECK_MSG(tilt(*bright) > 4.0 * tilt(*dull), "harmonic 10 at %.4f hard, %.4f soft", tilt(*bright), tilt(*dull));
    }
    {   // pan, per note and per oscillator
        auto r = plainRipple();
        Note n = mk(0, 1920, 48);
        n.pan = -1.0f;
        auto left = run(r, n);
        CHECK(peakOf(left->ch[0]) > 0.3f && peakOf(left->ch[1]) < 1.0e-6f);
        n.pan = 0.5f;
        auto half = run(r, n);
        CHECK_NEAR(rmsOf(half->ch[0], 9600, 30000) / rmsOf(half->ch[1], 9600, 30000), 0.5, 0.01);
        r->osc[0].pan->set(1.0f);
        auto right = run(r, mk(0, 1920, 48));
        CHECK(peakOf(right->ch[1]) > 0.3f && peakOf(right->ch[0]) < 1.0e-6f);
    }
    {   // mod X moves the filter cutoff, five octaves either way
        auto r = plainRipple(0, 0.5f);
        r->pFType->set(1.0f);
        r->pCutoff->set(1000.0f);
        r->pReso->set(0.0f);
        auto h = [&](const SampleData& o, int k) { return ampAt(o.ch[0], 9600, 16384, f * k, sr); };
        Note n = mk(0, 1920, 48);
        auto mid = run(r, n);
        n.modX = 0.7f;      // two octaves up: 4 kHz
        auto open = run(r, n);
        n.modX = 0.3f;      // two octaves down: 250 Hz
        auto shut = run(r, n);
        CHECK_MSG(h(*open, 30) > 6.0 * h(*mid, 30) && h(*shut, 8) < 0.1 * h(*mid, 8), "harmonic 30: %.5f open, %.5f mid; harmonic 8: %.5f shut, %.5f mid", h(*open, 30), h(*mid, 30), h(*shut, 8), h(*mid, 8));
        r->pCutoff->set(4000.0f);
        auto same = run(r, mk(0, 1920, 48));
        CHECK_NEAR(h(*same, 30) / h(*open, 30), 1.0, 0.02);     // mod X 0.7 on 1 kHz is the same as 4 kHz
    }
    {   // mod Y moves the wavetable position
        const int ladder = tableNamed("Overtone Ladder");       // frame k: the fundamental plus harmonic k + 2
        auto r = plainRipple(ladder, 0.0f);
        auto h = [&](const SampleData& o, int k) { return ampAt(o.ch[0], 9600, 16384, f * k, sr); };
        Note n = mk(0, 1920, 48);
        auto mid = run(r, n);
        n.modY = 1.0f;
        auto top = run(r, n);
        n.modY = 0.6f;      // +0.2 of the table: frame 3, harmonic 5
        auto part = run(r, n);
        CHECK(h(*mid, 2) > 0.1 && h(*mid, 17) < 1.0e-4);
        CHECK(h(*top, 17) > 0.1 && h(*top, 2) < 1.0e-4);
        CHECK_MSG(h(*part, 5) > 0.1 && h(*part, 2) < 1.0e-3, "harmonic 5 at %.4f", h(*part, 5));
        r->osc[0].posY->set(0.0f);
        n.modY = 1.0f;
        auto off = run(r, n);
        CHECK(identical(*off, *mid));
    }
    {   // per-note release
        auto r = plainRipple();
        r->pRelease->set(300.0f);
        Note shortN = mk(0, 480, 60), longN = mk(0, 480, 60);
        shortN.release = 0.0f;
        longN.release = 1.0f;
        auto a = run(r, shortN), b = run(r, longN);
        CHECK(rmsOf(b->ch[0], 16000, 20000) > 5.0 * rmsOf(a->ch[0], 16000, 20000));
    }
}

TEST(ripple_envelopes_and_lfos_move_the_wavetable_pitch_filter_and_level) {
    const double sr = 48000.0, f = keyToHz(48);
    const int ladder = tableNamed("Overtone Ladder");
    auto h = [&](const SampleData& o, size_t at, int k) { return ampAt(o.ch[0], at, 4096, f * k, sr); };
    {   // the mod envelope sweeps the position: from where the knob is to the far end over its attack
        auto r = plainRipple(ladder, 0.0f);
        r->osc[0].posEnv->set(1.0f);
        r->pMAttack->set(2000.0f);
        r->pMSustain->set(1.0f);
        auto out = play(r, {mk(0, 7680, 48)}, {sr, 120.0, 512, 1, 144000});
        CHECK_MSG(h(*out, 0, 2) > 0.1 && h(*out, 0, 17) < 1.0e-3, "at the start: harmonic 2 %.4f, harmonic 17 %.4f", h(*out, 0, 2), h(*out, 0, 17));
        CHECK_MSG(h(*out, 46000, 9) + h(*out, 46000, 10) > 0.1, "half way: harmonics 9 and 10 at %.4f", h(*out, 46000, 9) + h(*out, 46000, 10));
        CHECK(h(*out, 110000, 17) > 0.1 && h(*out, 110000, 2) < 1.0e-4);
        // part of the amount goes part of the way; a negative amount goes the other way
        r->osc[0].posEnv->set(0.4f);
        auto part = play(r, {mk(0, 7680, 48)}, {sr, 120.0, 512, 1, 144000});
        CHECK(h(*part, 110000, 8) > 0.1 && h(*part, 110000, 17) < 1.0e-4);
        auto n = plainRipple(ladder, 1.0f);
        n->osc[0].posEnv->set(-1.0f);
        n->pMAttack->set(2000.0f);
        n->pMSustain->set(1.0f);
        auto back = play(n, {mk(0, 7680, 48)}, {sr, 120.0, 512, 1, 144000});
        CHECK(h(*back, 0, 17) > 0.1 && h(*back, 110000, 2) > 0.1 && h(*back, 110000, 17) < 1.0e-4);
    }
    {   // an LFO sweeps the position at its rate
        auto r = plainRipple(ladder, 0.5f);
        r->osc[0].posLfo[0]->set(0.5f);
        r->pLfoWave[0]->set(0.0f);
        r->pLfoRate[0]->set(2.0f);
        auto out = play(r, {mk(0, 7680, 48)}, {sr, 120.0, 512, 1, 144000});
        // harmonic 17 is only there while the position is at the top: once per LFO cycle
        int bursts = 0;
        bool high = false;
        for (size_t a = 0; a + 2048 <= 144000; a += 1024) {
            const bool now = ampAt(out->ch[0], a, 2048, f * 17, sr) > 0.05;
            if (now && !high) ++bursts;
            high = now;
        }
        CHECK_MSG(bursts == 6, "%d sweeps in 3 s at 2 Hz", bursts);
        // LFO 2 does the same through its own amount, and with a delay
        auto s = plainRipple(ladder, 0.5f);
        s->osc[0].posLfo[1]->set(0.5f);
        s->pLfoWave[1]->set(0.0f);
        s->pLfoRate[1]->set(2.0f);
        s->pLfoDelay[1]->set(1000.0f);
        auto late = play(s, {mk(0, 7680, 48)}, {sr, 120.0, 512, 1, 144000});
        CHECK(h(*late, 4800, 17) < 1.0e-3 && h(*late, 4800 + 4096, 9) + h(*late, 4800 + 4096, 10) > 0.05);     // still in the middle of the table
        bool reached = false;
        for (size_t a = 72000; a + 4096 <= 144000; a += 2048) reached = reached || h(*late, a, 17) > 0.05;
        CHECK(reached);
    }
    {   // mod envelope to pitch
        auto r = plainRipple();
        r->pMEnvPitch->set(12.0f);
        r->pMAttack->set(0.0f);
        r->pMDecay->set(200.0f);
        auto out = play(r, {mk(0, 3840, 60)});
        const double early = measureHz(out->ch[0], 0, 480, sr), late = measureHz(out->ch[0], 24000, 46000, sr);
        CHECK_MSG(early > keyToHz(60) * 1.6 && early < keyToHz(60) * 2.01, "starts at %.1f Hz", early);
        CHECK_MSG(std::fabs(centsOff(late, keyToHz(60))) < 1.0, "ends at %.2f Hz", late);
    }
    {   // LFO to pitch
        auto r = plainRipple();
        r->pLfoWave[0]->set(0.0f);
        r->pLfoRate[0]->set(5.0f);
        r->pLfoPitch[0]->set(100.0f);
        auto out = play(r, {mk(0, 7680, 60)}, {sr, 120.0, 512, 1, 96000});
        double hi = -1e9, lo = 1e9;
        int crossings = 0;
        double prev = 0.0;
        for (size_t a = 4800; a + 960 <= 96000; a += 480) {
            const double c = centsOff(measureHz(out->ch[0], a, a + 960, sr), keyToHz(60));
            hi = std::max(hi, c); lo = std::min(lo, c);
            if (prev < 0.0 && c >= 0.0) ++crossings;
            prev = c;
        }
        CHECK_MSG(hi > 85.0 && hi < 105.0 && lo < -85.0 && lo > -105.0, "vibrato from %.1f to %.1f cents", lo, hi);
        CHECK_MSG(crossings == 9 || crossings == 10, "%d vibrato cycles in 1.9 s at 5 Hz", crossings);
    }
    {   // LFO to level and to cutoff
        auto r = plainRipple();
        r->pLfoWave[1]->set(0.0f);
        r->pLfoRate[1]->set(4.0f);
        r->pLfoAmp[1]->set(0.6f);
        auto out = play(r, {mk(0, 3840, 60)});
        float hi = 0.0f, lo = 1.0f;
        for (size_t a = 4800; a + 480 <= 48000; a += 240) { const float pk = peakOf(out->ch[0], a, a + 480); hi = std::max(hi, pk); lo = std::min(lo, pk); }
        CHECK_NEAR(lo / hi, 0.4, 0.03);
        auto c = plainRipple(0, 0.5f);
        c->pFType->set(1.0f);
        c->pCutoff->set(800.0f);
        c->pLfoWave[0]->set(4.0f);          // square: the cutoff alternates between two settings
        c->pLfoRate[0]->set(1.0f);
        c->pLfoCutoff[0]->set(2.0f);
        auto sw = play(c, {mk(0, 3840, 48)});
        CHECK_MSG(h(*sw, 9600, 16) > 5.0 * h(*sw, 9600 + 24000, 16), "harmonic 16: %.5f with the cutoff up, %.5f down", h(*sw, 9600, 16), h(*sw, 9600 + 24000, 16));
    }
    {   // the amplitude envelope
        auto r = plainRipple();
        r->pAttack->set(100.0f);
        r->pDecay->set(200.0f);
        r->pSustain->set(0.5f);
        r->pRelease->set(100.0f);
        auto out = play(r, {mk(0, 1920, 60)}, {sr, 120.0, 512, 1, 72000});
        CHECK_NEAR(peakOf(out->ch[0], 2300, 2500) / 0.35, 0.5, 0.03);
        CHECK_NEAR(peakOf(out->ch[0], 4700, 5000) / 0.35, 1.0, 0.03);
        CHECK_NEAR(peakOf(out->ch[0], 40000, 47000) / 0.35, 0.5, 0.02);
        CHECK(peakOf(out->ch[0], 48000 + 9600, 72000) < 0.002f);
    }
}

TEST(ripple_filter_modes_slopes_tracking_and_envelope) {
    const double sr = 48000.0, f = keyToHz(36);     // 65.4 Hz: a saw with a harmonic every 65 Hz
    auto h = [&](const SampleData& o, int k) { return ampAt(o.ch[0], 9600, 32768, f * k, sr); };
    auto run = [&](int type, float cutoff, float reso, int key = 36) {
        auto r = plainRipple(0, 0.5f);
        r->pFType->set((float)type);
        r->pCutoff->set(cutoff);
        r->pReso->set(reso);
        return play(r, {mk(0, 3840, key)});
    };
    auto dry = run(0, 1000, 0);
    auto db = [&](const SampleData& o, int k) { return (double)gainToDb((float)(h(o, k) / h(*dry, k))); };
    const float fc = (float)(f * 15);       // 981 Hz, on harmonic 15
    auto lp12 = run(1, fc, 0), lp24 = run(2, fc, 0), bp = run(3, fc, 0), hp = run(4, fc, 0), notch = run(5, fc, 0.3f);
    CHECK_NEAR(db(*lp12, 1), 0.0, 0.3);
    CHECK_MSG(db(*lp12, 60) < -22.0 && db(*lp12, 60) > -26.0, "low pass 12, two octaves up: %.1f dB", db(*lp12, 60));
    CHECK_MSG(db(*lp12, 120) < -34.0 && db(*lp12, 120) > -38.0, "low pass 12, three octaves up: %.1f dB", db(*lp12, 120));
    CHECK_NEAR(db(*lp24, 1), 0.0, 0.3);
    CHECK_MSG(db(*lp24, 60) < -45.0 && db(*lp24, 60) > -51.0, "low pass 24, two octaves up: %.1f dB", db(*lp24, 60));
    CHECK_MSG(db(*hp, 1) < -45.0 && db(*hp, 1) > -49.0, "high pass, almost four octaves down: %.1f dB", db(*hp, 1));
    CHECK_NEAR(db(*hp, 120), 0.0, 0.3);
    CHECK_NEAR(db(*bp, 15), 0.0, 0.5);
    CHECK(db(*bp, 1) < -18.0 && db(*bp, 150) < -15.0);
    CHECK_MSG(db(*notch, 15) < -30.0, "notch at its centre: %.1f dB", db(*notch, 15));
    CHECK_NEAR(db(*notch, 1), 0.0, 0.3);
    CHECK_NEAR(db(*notch, 120), 0.0, 0.3);
    // resonance lifts the cutoff
    auto res = run(1, fc, 0.8f);
    CHECK_MSG(db(*res, 15) > 15.0, "resonance 0.8 lifts the cutoff by %.1f dB", db(*res, 15));
    // key tracking: an octave up, a fully tracked filter does to harmonic k what it did before
    {
        auto tracked = [&](float amount, int key) {
            auto r = plainRipple(0, 0.5f);
            r->pFType->set(1.0f);
            r->pCutoff->set(500.0f);
            r->pReso->set(0.0f);
            r->pFKey->set(amount);
            auto o = play(r, {mk(0, 3840, key)});
            const double fk = keyToHz(key);
            return ampAt(o->ch[0], 9600, 32768, fk * 8, sr) / ampAt(o->ch[0], 9600, 32768, fk, sr);
        };
        CHECK_NEAR(tracked(1.0f, 60) / tracked(1.0f, 48), 1.0, 0.03);
        CHECK(tracked(0.0f, 60) < 0.35 * tracked(0.0f, 48));
    }
    // the filter envelope opens the filter at the start of the note and lets it close
    {
        auto r = plainRipple(0, 0.5f);
        r->pFType->set(2.0f);
        r->pCutoff->set(300.0f);
        r->pFEnv->set(5.0f);
        r->pFAttack->set(0.0f);
        r->pFDecay->set(300.0f);
        r->pFSustain->set(0.0f);
        auto o = play(r, {mk(0, 3840, 36)});
        const double early = ampAt(o->ch[0], 0, 2048, f * 40, sr), late = ampAt(o->ch[0], 30000, 2048, f * 40, sr);
        CHECK_MSG(early > 30.0 * late && early > 0.002, "harmonic 40: %.5f early, %.6f late", early, late);
    }
}

TEST(ripple_unison_sub_and_noise) {
    const double sr = 48000.0, f = keyToHz(60);
    auto make = [&](int voices, float detune, float spread, float phase) {
        auto r = plainRipple();
        r->osc[0].uni->set((float)voices);
        r->osc[0].detune->set(detune);
        r->osc[0].spread->set(spread);
        r->osc[0].phase->set(phase);
        return r;
    };
    auto one = play(make(1, 30, 1, 0), {mk(0, 7680, 60)}, {sr, 120.0, 512, 1, 144000});
    CHECK(identical(*one, *play(make(1, 0, 0, 0), {mk(0, 7680, 60)}, {sr, 120.0, 512, 1, 144000})));       // one voice: detune and spread do nothing
    auto seven = play(make(7, 30, 1, 1), {mk(0, 7680, 60)}, {sr, 120.0, 512, 1, 144000});
    // the level of the stack is that of one voice (within 1 dB), each voice is 1 / sqrt(7) of it
    CHECK_NEAR(gainToDb((float)(rmsOf(seven->ch[0], 9600, 144000) + rmsOf(seven->ch[1], 9600, 144000))) - gainToDb((float)(2.0 * rmsOf(one->ch[0], 9600, 144000))), -2.5, 1.5);
    const double outer = ampAt(seven->ch[0], 9600, 131072, f * std::pow(2.0, -30.0 / 1200.0), sr);       // the lowest voice is panned hard left
    CHECK_NEAR(outer, 0.35 / std::sqrt(7.0), 0.02);
    CHECK(ampAt(seven->ch[1], 9600, 131072, f * std::pow(2.0, -30.0 / 1200.0), sr) < 0.02);
    CHECK(maxAbsDiff(seven->ch[0], seven->ch[1]) > 0.1);
    auto narrow = play(make(7, 30, 0, 1), {mk(0, 3840, 60)});
    CHECK(narrow->ch[0] == narrow->ch[1]);                      // no spread: mono
    // phase randomisation: off, every note starts the same and from zero; on, notes differ but a render repeats exactly
    auto quick = [&](float phase) { auto r = make(5, 20, 0.5f, phase); r->pRelease->set(10.0f); return r; };
    auto sync = play(quick(0), {mk(0, 400, 60), mk(960, 400, 60)});
    CHECK(sync->ch[0][0] == 0.0f);
    double same = 0.0;
    for (size_t i = 0; i < 9000; ++i) same = std::max(same, (double)std::fabs(sync->ch[0][i] - sync->ch[0][i + 24000]));
    CHECK_MSG(same < 1.0e-6, "two notes without phase randomisation differ by %g", same);
    auto rnd = play(quick(1), {mk(0, 400, 60), mk(960, 400, 60)});
    double differ = 0.0;
    for (size_t i = 0; i < 9000; ++i) differ = std::max(differ, (double)std::fabs(rnd->ch[0][i] - rnd->ch[0][i + 24000]));
    CHECK(differ > 0.05);
    CHECK(identical(*rnd, *play(quick(1), {mk(0, 400, 60), mk(960, 400, 60)})));
    // the sub is added under the oscillator at its own level
    {
        auto r = plainRipple();
        r->pSubLevel->set(0.5f);
        auto out = play(r, {mk(0, 3840, 60)});
        CHECK_NEAR(ampAt(out->ch[0], 9600, 16384, f / 2, sr), 0.175, 0.003);
        CHECK_NEAR(ampAt(out->ch[0], 9600, 16384, f, sr), 0.35, 0.004);
    }
    // noise: broadband, the colour control takes the top off, and it is the same every time
    {
        auto noisy = [&](float colour) {
            auto r = plainRipple();
            r->osc[0].level->set(0.0f);
            r->pNoise->set(1.0f);
            r->pNoiseColor->set(colour);
            return play(r, {mk(0, 3840, 60)});
        };
        auto white = noisy(20000), dark = noisy(500), again = noisy(20000);
        CHECK(identical(*white, *again));
        auto band = [&](const SampleData& o, double lo, double hi) {
            const auto m = spectrum(o.ch[0], 4096, 32768);
            double e = 0.0;
            for (size_t b = (size_t)(lo * 32768 / sr); b < (size_t)(hi * 32768 / sr); ++b) e += m[b] * m[b];
            return e;
        };
        CHECK(rmsOf(white->ch[0], 4800, 48000) > 0.05);
        CHECK_NEAR(gainToDb((float)std::sqrt(band(*white, 8000, 12000) / band(*white, 200, 4200))), 0.0, 2.0);
        CHECK(band(*dark, 8000, 12000) < 0.01 * band(*dark, 100, 400) * 4000.0 / 300.0);
    }
}

TEST(ripple_chorus_and_delay) {
    const double sr = 48000.0;
    auto make = [] { auto r = plainRipple(0, 0.5f); r->pRelease->set(5.0f); return r; };
    auto dry = play(make(), {mk(0, 80, 60)}, {sr, 120.0, 512, 1, 48000});         // a 42 ms blip
    {   // mix 0 is the dry sound exactly, whatever the other effect controls say
        auto r = make();
        r->pChRate->set(3.0f); r->pChDepth->set(1.0f); r->pDTime->set(123.0f); r->pDFeed->set(0.8f);
        CHECK(identical(*dry, *play(r, {mk(0, 80, 60)}, {sr, 120.0, 512, 1, 48000})));
    }
    {   // delay: the echo is the dry sound, the delay time later to the sample, at the mix level; then again at the feedback level
        auto r = make();
        r->pDelay->set(0.5f);
        r->pDTime->set(250.0f);
        r->pDFeed->set(0.0f);
        auto out = play(r, {mk(0, 80, 60)}, {sr, 120.0, 512, 1, 48000});
        double worst = 0.0;
        for (size_t i = 0; i < 48000; ++i) {
            const float want = dry->ch[0][i] + (i >= 12000 ? 0.5f * dry->ch[0][i - 12000] : 0.0f);
            worst = std::max(worst, (double)std::fabs(out->ch[0][i] - want));
        }
        CHECK_MSG(worst < 1.0e-6, "the echo is off by %g", worst);
        r->pDFeed->set(0.5f);
        auto fb = play(r, {mk(0, 80, 60)}, {sr, 120.0, 512, 1, 48000});
        const double first = rmsOf(fb->ch[0], 12000, 15000), second = rmsOf(fb->ch[0], 24000, 27000), third = rmsOf(fb->ch[0], 36000, 39000);
        CHECK_MSG(second / first > 0.3 && second / first < 0.5 && third / second > 0.3 && third / second < 0.5, "repeats at %.3f and %.3f of the one before", second / first, third / second);
        CHECK(r->tailSeconds() > 1.0 && make()->tailSeconds() == 0.0);
    }
    {   // chorus: the sound gets wider and moves, and stays about as loud
        auto r = plainRipple(0, 0.5f);
        r->pChorus->set(0.6f);
        auto wet = play(r, {mk(0, 3840, 60)});
        auto ref = play(plainRipple(0, 0.5f), {mk(0, 3840, 60)});
        CHECK(maxAbsDiff(wet->ch[0], wet->ch[1]) > 0.02);
        CHECK(maxAbsDiff(wet->ch[0], ref->ch[0]) > 0.02);
        CHECK_NEAR(gainToDb((float)rmsOf(wet->ch[0], 9600, 48000)) - gainToDb((float)rmsOf(ref->ch[0], 9600, 48000)), 0.0, 3.0);
    }
}

TEST(ripple_voice_limit_holds_and_stealing_does_not_click) {
    Ripple r;
    r.osc[0].pos->set(0.0f);
    r.pAttack->set(5.0f);
    r.pSustain->set(1.0f);
    r.pVoices->set(4.0f);
    Drive d(r);
    for (int i = 0; i < 12; ++i) d.on(1000 + i * 960, 48 + i, (uint32)(i + 1));
    int most = 0;
    for (int q = 0; q < 16000 / kQuantum; ++q) { d.run(kQuantum); most = std::max(most, r.activeVoices()); }
    CHECK_MSG(most <= 5, "%d voices at once with a limit of 4 (one more may be fading out)", most);
    d.run(960);
    CHECK_MSG(r.activeVoices() == 4, "%d voices held", r.activeVoices());
    CHECK_MSG(maxStep(d.L, 0, d.L.size()) < 0.06f, "largest step %.3f", maxStep(d.L, 0, d.L.size()));
    CHECK(peakOf(d.L) > 0.25f);
    Ripple t;
    t.pVoices->set(32.0f);
    Drive e(t);
    for (int i = 0; i < 200; ++i) e.on(i * 37, 30 + (i * 7) % 70, (uint32)(i + 1));
    e.run(48000);
    CHECK(allFinite(e.L) && t.activeVoices() == 32);
}

TEST(ripple_note_ends_and_cuts_fall_smoothly_to_silence) {
    const double sr = 48000.0;
    for (int which = 0; which < 3; ++which) {
        Ripple r;
        r.osc[0].pos->set(0.0f);        // a sine, so any step stands out
        if (which == 1) { r.pFType->set(2.0f); r.pCutoff->set(2000.0f); r.pReso->set(0.6f); r.osc[0].uni->set(5.0f); }
        if (which == 2) { r.pRelease->set(20.0f); r.pSubLevel->set(0.5f); }
        Drive d(r, sr);
        d.on(0, 57, 1);
        d.off(24000, 1);
        d.run(24000 + 72000);
        const float before = maxStep(d.L, 12000, 24000), after = maxStep(d.L, 24000, d.L.size());
        CHECK_MSG(after <= before * 1.05f + 1.0e-5f, "case %d: a step of %.5f after the note-off, %.5f before it", which, after, before);
        size_t last = 0;
        for (size_t i = 0; i < d.L.size(); ++i) if (d.L[i] != 0.0f) last = i;
        CHECK_MSG(last < d.L.size() - 4800, "case %d: still sounding at the end", which);
        CHECK_MSG(peakOf(d.L, last > 96 ? last - 96 : 0, last + 1) < 1.0e-4f, "case %d: the tail ends at %.6f, not at silence", which, peakOf(d.L, last > 96 ? last - 96 : 0, last + 1));
        CHECK(r.activeVoices() == 0);
    }
    for (auto type : {NoteEvent::Kill, NoteEvent::KillNote}) {
        Ripple r;
        r.osc[0].pos->set(0.0f);
        Drive d(r, sr);
        d.on(0, 57, 1);
        NoteEvent k;
        k.type = type;
        k.id = 1;
        d.ev(24010, k);
        d.run(48000);
        CHECK(peakOf(d.L, 20000, 24000) > 0.1f);
        CHECK_MSG(maxStep(d.L, 24000, 48000) <= maxStep(d.L, 12000, 24000) * 1.3f, "the cut steps by %.4f", maxStep(d.L, 24000, 48000));
        CHECK(peakOf(d.L, 24010 + 150, 48000) == 0.0f);
    }
    {
        Ripple r;
        Drive d(r, sr);
        d.on(0, 57, 1);
        d.on(0, 64, 2);
        NoteEvent a;
        a.type = NoteEvent::AllOff;
        d.ev(12000, a);
        d.run(96000);
        CHECK(peakOf(d.L, 72000, 96000) == 0.0f && r.activeVoices() == 0);
    }
}

TEST(ripple_presets_all_sound_and_reload) {
    Ripple proto;
    const auto presets = proto.factoryPresets();
    CHECK_MSG(presets.size() >= 24, "%zu presets", presets.size());
    std::vector<std::string> names;
    float quietest = 10.0f, loudest = 0.0f;
    int moving = 0;
    std::vector<std::shared_ptr<SampleData>> sounds;
    for (auto& pr : presets) {
        CHECK_MSG(std::find(names.begin(), names.end(), pr.name) == names.end(), "two presets called %s", pr.name.c_str());
        names.push_back(pr.name);
        auto r = std::make_shared<Ripple>();
        r->loadState(pr.state);
        auto one = play(r, {mk(0, 1920, 60)}, {48000.0, 120.0, 512, 1, 120000});
        const float pk = std::max(peakOf(one->ch[0]), peakOf(one->ch[1]));
        quietest = std::min(quietest, pk);
        loudest = std::max(loudest, pk);
        if (verbose()) printf("    %-20s one note peaks at %6.1f dBFS\n", pr.name.c_str(), gainToDb(pk));
        CHECK_MSG(allFinite(one->ch[0]) && allFinite(one->ch[1]), "%s produced a non-finite sample", pr.name.c_str());
        CHECK_MSG(pk > 0.18f && pk < 0.56f, "%s: one note peaks at %.1f dBFS (-12 to -6 wanted, 3 dB of slack)", pr.name.c_str(), gainToDb(pk));
        std::vector<Note> chord;
        for (int k : {36, 48, 55, 60, 64, 67, 72, 84}) chord.push_back(mk(0, 1920, k, 1.0f));
        auto many = play(r, chord, {48000.0, 120.0, 512, 1, 96000});
        CHECK_MSG(allFinite(many->ch[0]) && allFinite(many->ch[1]), "%s: a chord produced a non-finite sample", pr.name.c_str());
        std::vector<Note> triad;
        for (int k : {48, 60, 67}) triad.push_back(mk(0, 1920, k));
        auto three = play(r, triad, {48000.0, 120.0, 512, 1, 96000});
        CHECK_MSG(std::max(peakOf(three->ch[0]), peakOf(three->ch[1])) < 2.0f, "%s: a triad peaks at %.2f, over +6 dBFS", pr.name.c_str(), peakOf(three->ch[0]));
        // the wavetable moves: by the mod envelope or an LFO on at least one oscillator
        bool moves = false;
        for (int o = 0; o < 2; ++o)
            if (r->osc[o].level->get() > 0.0f && (r->osc[o].posEnv->get() != 0.0f || r->osc[o].posLfo[0]->get() != 0.0f || r->osc[o].posLfo[1]->get() != 0.0f)) moves = true;
        moving += moves ? 1 : 0;
        // state round trip: same parameters, same sound
        auto s = std::make_shared<Ripple>();
        s->loadState(Json::parse(r->saveState().dump()));
        CHECK_MSG(s->saveState() == r->saveState(), "%s does not survive save and load", pr.name.c_str());
        auto again = play(s, {mk(0, 1920, 60)}, {48000.0, 120.0, 512, 1, 120000});
        CHECK_MSG(identical(*one, *again), "%s sounds different after save and load", pr.name.c_str());
        for (auto& other : sounds) CHECK_MSG(!identical(*other, *one), "%s sounds exactly like another preset", pr.name.c_str());
        sounds.push_back(one);
    }
    printf("    %zu presets, %d of them with a moving wavetable; one note at velocity 100 peaks between %.1f and %.1f dBFS\n",
           presets.size(), moving, gainToDb(quietest), gainToDb(loudest));
    CHECK_MSG(moving >= 24, "only %d presets move the wavetable", moving);
    printf("    %zu parameters in %zu groups\n", proto.params().size(), proto.paramGroups().size());
    CHECK(groupsCoverParams(proto));
    CHECK(proto.paramGroups().size() >= 8);
}

TEST(ripple_render_is_identical_at_any_block_size_and_thread_count_and_never_allocates) {
    const std::string table = writeCycles("ripple_render.wav", {{{1, 1.0}}, {{1, 0.6}, {3, 0.6}}, {{2, 1.0}, {9, 0.3}}});
    for (const char* name : {"Tidal Pad", "Wobble Bass", "Digital Rain", "Sync Scream", "user table"}) {
        std::shared_ptr<SampleData> ref;
        struct Case { int block, threads; } cases[] = {{64, 1}, {33, 1}, {4096, 1}, {512, 4}, {33, 4}};
        for (auto& c : cases) {
            auto r = std::make_shared<Ripple>();
            if (std::string(name) == "user table") {
                loadPreset(*r, "Vowel Drift");
                std::string err;
                CHECK(r->loadWavetable(table, &err));
                r->osc[0].table->set((float)Ripple::userTableIndex());
            } else loadPreset(*r, name);
            r->pGlide->set(60.0f);
            std::vector<Note> notes;
            for (int i = 0; i < 24; ++i) {
                Note n = mk(i * 97 + (i % 3) * 11, 300 + (i % 5) * 170, 40 + (i * 5) % 40, 0.4f + 0.02f * i);
                n.pan = (float)((i % 5) - 2) * 0.4f;
                n.modX = 0.4f + 0.02f * (i % 10);
                n.modY = 0.7f - 0.04f * (i % 10);
                n.fine = (float)(i % 7) * 3.0f;
                notes.push_back(n);
            }
            Note slide = mk(700, 400, 70);
            slide.slide = true;
            notes.push_back(slide);
            const int64 before = Rt::allocationsInRt.load();
            auto out = play(r, notes, {48000.0, 120.0, c.block, c.threads, 96000});
            CHECK_MSG(Rt::allocationsInRt.load() == before, "%s: %lld allocations on the audio thread", name, (long long)(Rt::allocationsInRt.load() - before));
            if (!ref) { ref = out; CHECK(peakOf(out->ch[0]) > 0.05f); continue; }
            CHECK_MSG(identical(*ref, *out), "%s: block %d, %d threads differs", name, c.block, c.threads);
        }
    }
}

TEST(ripple_parameters_can_move_while_notes_sound) {
    // Every parameter is automatable: move them all at random between quanta while notes play.
    Ripple r;
    loadPreset(r, "Tidal Pad");
    Drive d(r);
    for (int i = 0; i < 8; ++i) d.on(i * 3000, 48 + i * 3, (uint32)(i + 1));
    uint32 rnd = 777;
    for (int q = 0; q < 96000 / kQuantum; ++q) {
        for (auto& p : r.params()) {
            rnd = rnd * 1664525u + 1013904223u;
            if ((rnd >> 24) < 40) p->setNorm((float)((rnd >> 8) & 0xffff) / 65535.0f);
        }
        d.run(kQuantum);
    }
    CHECK(allFinite(d.L) && allFinite(d.R));
    CHECK_MSG(peakOf(d.L) < 100.0f, "peak %.2f", peakOf(d.L));
    // switching table, changing the unison count and stepping the level in mid-note: all cross-faded or ramped
    Ripple t;
    t.osc[0].pos->set(0.0f);
    t.osc[0].level->set(1.0f);
    t.pSustain->set(1.0f);
    Drive e(t);
    e.on(0, 45, 1);
    e.run(4800);
    const float steady = maxStep(e.L, 2400, 4800);
    const int other = tableNamed("Phase Bend");     // its first frame is a cosine: a hard switch from the sine would jump
    for (int q = 0; q < 300; ++q) {
        t.osc[0].table->set(q % 2 ? (float)other : 0.0f);
        e.run(kQuantum * 3);
    }
    // cut over at once, sine to cosine would jump by up to 0.5
    CHECK_MSG(maxStep(e.L, 4800, e.L.size()) < 0.04f, "table switches step by %.4f, the tone itself by %.4f", maxStep(e.L, 4800, e.L.size()), steady);
    t.osc[0].table->set(0.0f);
    e.run(4800);
    const size_t from = e.L.size();
    for (int q = 0; q < 300; ++q) {
        t.osc[0].uni->set((float)(1 + q % 7));
        t.pVolume->set(q % 2 ? 0.8f : 0.3f);
        e.run(kQuantum * 2);
    }
    CHECK_MSG(maxStep(e.L, from, e.L.size()) < 0.06f, "unison and level changes step by %.4f, the tone itself by %.4f", maxStep(e.L, from, e.L.size()), steady);
}
