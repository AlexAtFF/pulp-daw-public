// Motes: the granular sampler.
#include "CreativeTest.h"
#include <filesystem>
#include <set>
using namespace pulp;
using namespace ct;

namespace {
const double kSr = 48000.0;

struct Ev { size_t at; NoteEvent e; };
Ev on(double seconds, int key, uint32 id, float velocity = kDefaultVelocity, double sr = kSr) {
    Ev v;
    v.at = (size_t)std::llround(seconds * sr);
    v.e.type = NoteEvent::On; v.e.key = key; v.e.id = id; v.e.velocity = velocity;
    return v;
}
Ev off(double seconds, uint32 id, double sr = kSr) {
    Ev v;
    v.at = (size_t)std::llround(seconds * sr);
    v.e.type = NoteEvent::Off; v.e.id = id;
    return v;
}
Ev other(double seconds, NoteEvent::Type type, uint32 id = 0, double sr = kSr) {
    Ev v;
    v.at = (size_t)std::llround(seconds * sr);
    v.e.type = type; v.e.id = id;
    return v;
}
// Plays note events into an instrument, quantum by quantum, as a channel does.
Stereo play(Plugin& p, std::vector<Ev> evs, double seconds, double sr = kSr, float bend = 0.0f, const std::function<void(size_t)>& each = {}) {
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
    Stereo out = silence(seconds, sr);
    size_t next = 0;
    for (size_t i = 0; i < out.l.size(); i += kQuantum) {
        if (each) each(i / kQuantum);
        NoteEvent q[64];
        int n = 0;
        while (next < evs.size() && evs[next].at < i + kQuantum && n < 64) { q[n] = evs[next].e; q[n].offset = (int)(evs[next].at - i); ++n; ++next; }
        ProcessContext c;
        c.sampleRate = sr;
        c.io[0] = out.l.data() + i; c.io[1] = out.r.data() + i;
        c.events = q; c.numEvents = n;
        c.pitchBendSemis = bend;
        p.process(c);
    }
    return out;
}
std::shared_ptr<SampleData> sineSample(double hz = 440.0, double seconds = 2.0, double sr = 44100.0) {
    return tu::sine(hz, seconds, sr, 1.0f);
}
// one long grain a second, nothing random: a clean stretch of the sample to measure
std::shared_ptr<Motes> plain(SamplePtr s, double sr = kSr) {
    auto m = std::make_shared<Motes>();
    m->prepare(sr);
    m->setSample(std::move(s));
    m->pPosition->set(0.5f); m->pSpread->set(0.0f); m->pScan->set(0.0f); m->pJitter->set(0.0f); m->pPanSpread->set(0.0f);
    m->pSize->set(500.0f); m->pDensity->set(1.0f); m->pAttack->set(0.0f); m->pRelease->set(50.0f);
    return m;
}
double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }
double peakDb(const Stereo& s, size_t from = 0, size_t to = (size_t)-1) { return 20.0 * std::log10(std::max(1e-12f, peak(s, from, to))); }
// loudness over time: RMS in windows of `win` samples every `hop`
std::vector<double> envelope(const std::vector<float>& x, size_t win, size_t hop) {
    std::vector<double> e;
    for (size_t i = 0; i + win <= x.size(); i += hop) e.push_back(tu::rmsOf(x, i, i + win));
    return e;
}
double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    const size_t n = std::min(a.size(), b.size());
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; ++i) { sab += (a[i] - ma) * (b[i] - mb); saa += (a[i] - ma) * (a[i] - ma); sbb += (b[i] - mb) * (b[i] - mb); }
    return sab / std::sqrt(saa * sbb + 1e-30);
}
}

TEST(motes_grain_pitch_follows_the_key) {
    // A 440 Hz sine with the root key at A (69). One half-second grain: its middle 300 ms is measured.
    double worst = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int key : {45, 57, 64, 69, 76, 81}) {
            auto m = plain(sineSample(), sr);
            m->pRoot->set(69.0f);
            const auto out = play(*m, {on(0.0, key, 1, kDefaultVelocity, sr)}, 0.6, sr);
            const double hz = tu::measureHz(out.l, (size_t)(0.1 * sr), (size_t)(0.4 * sr), sr), c = cents(hz, keyToHz(key));
            worst = std::max(worst, std::fabs(c));
            CHECK_MSG(std::fabs(c) < 1.0, "key %d at %.0f Hz: %.3f Hz, %.2f cents off", key, sr, hz, c);
        }
    printf("    motes: grain pitch within %.3f cents of the key (six keys, three sample rates)\n", worst);
    auto hzOf = [&](const std::function<void(Motes&)>& set, int key = 69, float bend = 0.0f, float noteFine = 0.0f) {
        auto m = plain(sineSample());
        m->pRoot->set(69.0f);
        set(*m);
        Ev e = on(0.0, key, 1);
        e.e.fineCents = noteFine;
        const auto out = play(*m, {e}, 0.6, kSr, bend);
        return tu::measureHz(out.l, 4800, 19200, kSr);
    };
    CHECK_NEAR(cents(hzOf([](Motes& m) { m.pKeytrack->set(0.0f); }, 81), 440.0), 0.0, 1.0);          // key tracking off: the sample's own pitch
    CHECK_NEAR(cents(hzOf([](Motes& m) { m.pCoarse->set(12.0f); }), 440.0), 1200.0, 1.0);
    CHECK_NEAR(cents(hzOf([](Motes& m) { m.pCoarse->set(-7.0f); m.pFine->set(50.0f); }), 440.0), -650.0, 1.0);
    CHECK_NEAR(cents(hzOf([](Motes&) {}, 69, 2.0f), 440.0), 200.0, 1.0);                             // the channel's pitch bend
    CHECK_NEAR(cents(hzOf([](Motes&) {}, 69, 0.0f, -30.0f), 440.0), -30.0, 1.0);                     // a note's own fine pitch
    CHECK_NEAR(cents(hzOf([](Motes& m) { m.pRoot->set(57.0f); }), 440.0), 1200.0, 1.0);
    // pitch spread: each grain is detuned by its own random amount inside the spread
    {
        auto m = plain(sineSample(440.0, 4.0));
        m->pRoot->set(69.0f);
        m->pPitchSpread->set(100.0f); m->pSize->set(400.0f); m->pDensity->set(2.0f);
        const auto out = play(*m, {on(0.0, 69, 1)}, 4.0);
        std::set<int> distinct;
        for (int g = 0; g < 7; ++g) {
            const size_t start = (size_t)(g * 0.5 * kSr);
            const double c = cents(tu::measureHz(out.l, start + 4800, start + 14400, kSr), 440.0);
            CHECK_MSG(std::fabs(c) <= 100.5, "grain %d is %.1f cents off with a spread of 100", g, c);
            distinct.insert((int)std::lround(c));
        }
        CHECK(distinct.size() >= 5);
    }
    // portamento and a slide note bend the grains that are sounding
    {
        auto m = plain(sineSample(440.0, 4.0));
        m->pRoot->set(69.0f);
        m->pSize->set(500.0f); m->pDensity->set(4.0f);
        Ev e = on(0.0, 69, 1);
        e.e.glideFromKey = 57.0f; e.e.slideSamples = 0.5f * (float)kSr;
        Ev slide = other(1.5, NoteEvent::SlideTo);
        slide.e.key = 76; slide.e.slideSamples = 0.25f * (float)kSr;
        const auto out = play(*m, {e, slide}, 3.0);
        CHECK_NEAR(cents(tu::measureHz(out.l, 1000, 3000, kSr), 220.0), 1200.0 * 2000.0 / 24000.0, 40.0);     // just after the start: near the key it glides from
        CHECK_NEAR(cents(tu::measureHz(out.l, 36000, 60000, kSr), 440.0), 0.0, 2.0);                            // arrived
        CHECK_NEAR(cents(tu::measureHz(out.l, 96000, 130000, kSr), keyToHz(76)), 0.0, 2.0);                     // slid to the new key and stays
    }
}

TEST(motes_scan_speed_one_keeps_the_sample_s_length_and_loudness_curve_at_any_key) {
    // A two-second drum loop of eight hits. Scan speed 1 with 30 ms grains: at any key the hits fall where they
    // fall in the sample, and the loudness over time follows the sample's.
    const auto loop = burstLoop(2.0, 44100.0);
    const size_t win = 960, hop = 480;      // 20 ms windows every 10 ms
    // the sample's own loudness curve, on the output's clock
    std::vector<double> ref;
    for (size_t i = 0; i + win <= (size_t)(2.0 * kSr); i += hop) {
        const size_t a = (size_t)((double)i * 44100.0 / kSr), b = (size_t)((double)(i + win) * 44100.0 / kSr);
        ref.push_back(tu::rmsOf(loop->ch[0], a, b));
    }
    for (int key : {48, 55, 60, 67, 72}) {
        auto m = std::make_shared<Motes>();
        m->prepare(kSr);
        m->setSample(loop);
        m->pPosition->set(0.0f); m->pSpread->set(0.0f); m->pScan->set(1.0f); m->pLoop->set(0.0f); m->pJitter->set(0.0f);
        m->pPanSpread->set(0.0f); m->pSize->set(30.0f); m->pDensity->set(100.0f); m->pAttack->set(0.0f); m->pRelease->set(100.0f);
        const auto out = play(*m, {on(0.0, key, 1)}, 3.0);
        auto env = envelope(out.l, win, hop);
        env.resize(ref.size());
        const double corr = correlation(env, ref);
        // each hit: where the loudness rises to its peak, against the sample's
        double worstMs = 0.0;
        for (int h = 0; h < 8; ++h) {
            const size_t from = (size_t)(h * 0.25 * kSr / hop);
            size_t po = from, pr = from;
            for (size_t k = from; k < from + 20 && k < ref.size(); ++k) { if (env[k] > env[po]) po = k; if (ref[k] > ref[pr]) pr = k; }
            worstMs = std::max(worstMs, std::fabs((double)po - (double)pr) * 10.0);
        }
        printf("    motes stretch, key %d: loudness curve correlates %.3f with the sample's, hits within %.0f ms\n", key, corr, worstMs);
        CHECK_MSG(corr > 0.85, "key %d: correlation %.3f", key, corr);
        CHECK_MSG(worstMs <= 20.0, "key %d: a hit is %.0f ms off", key, worstMs);
        // the same length: the last hit is there at 1.75 s, and after the sample's two seconds the note has ended by itself
        CHECK(tu::rmsOf(out.l, (size_t)(1.76 * kSr), (size_t)(1.8 * kSr)) > 5.0 * tu::rmsOf(out.l, (size_t)(1.70 * kSr), (size_t)(1.74 * kSr)));
        CHECK_MSG(peak(out, (size_t)(2.4 * kSr)) < 1.0e-4f, "key %d: still sounding after the sample's end (%.5f)", key, peak(out, (size_t)(2.4 * kSr)));
        // but the pitch moved with the key: the low hits ring at 180 Hz in the sample, so an octave up the first hit's
        // energy is around 360 Hz and an octave down around 90 Hz, not around 180 Hz any more
        if (key == 48 || key == 72) {
            const double want = 180.0 * std::pow(2.0, (key - 60) / 12.0);
            double there = 0.0, old = 0.0;
            for (double k = 0.8; k <= 1.25; k += 0.05) {
                there += std::pow(10.0, levelDb(out.l, 0, 3840, want * k) / 10.0);
                old += std::pow(10.0, levelDb(out.l, 0, 3840, 180.0 * k) / 10.0);
            }
            CHECK_MSG(there > 4.0 * old, "key %d: %.1f dB more around %.0f Hz than around 180 Hz", key, 10.0 * std::log10(there / old), want);
        }
    }
    {   // half speed: twice the length. The last hit (1.75 s in the sample) comes at 3.5 s
        auto m = std::make_shared<Motes>();
        m->prepare(kSr);
        m->setSample(loop);
        m->pPosition->set(0.0f); m->pSpread->set(0.0f); m->pScan->set(0.5f); m->pLoop->set(0.0f); m->pJitter->set(0.0f);
        m->pSize->set(30.0f); m->pDensity->set(100.0f); m->pAttack->set(0.0f);
        const auto out = play(*m, {on(0.0, 60, 1)}, 5.0);
        CHECK(tu::rmsOf(out.l, (size_t)(3.51 * kSr), (size_t)(3.6 * kSr)) > 5.0 * tu::rmsOf(out.l, (size_t)(3.40 * kSr), (size_t)(3.48 * kSr)));
        CHECK(peak(out, (size_t)(4.5 * kSr)) < 1.0e-4f);
    }
    {   // backwards: scan -1 from the end plays the hits in reverse order (the loud first hit comes last)
        auto m = std::make_shared<Motes>();
        m->prepare(kSr);
        m->setSample(loop);
        m->pPosition->set(1.0f); m->pSpread->set(0.0f); m->pScan->set(-1.0f); m->pLoop->set(0.0f); m->pJitter->set(0.0f);
        m->pSize->set(30.0f); m->pDensity->set(100.0f); m->pAttack->set(0.0f);
        const auto out = play(*m, {on(0.0, 60, 1)}, 3.0);
        CHECK(tu::rmsOf(out.l, (size_t)(1.9 * kSr), (size_t)(2.02 * kSr)) > 3.0 * tu::rmsOf(out.l, (size_t)(1.6 * kSr), (size_t)(1.7 * kSr)));
        CHECK(peak(out, (size_t)(2.5 * kSr)) < 1.0e-4f);
    }
}

TEST(motes_frozen_position_holds_for_as_long_as_the_note) {
    // A sweep from 110 to 1760 Hz over two seconds: half way through it is at 440 Hz, a quarter of the way at 220 Hz.
    const auto sweep = sweepSample();
    for (float pos : {0.5f, 0.25f}) {
        auto m = std::make_shared<Motes>();
        m->prepare(kSr);
        m->setSample(sweep);
        m->pPosition->set(pos); m->pSpread->set(0.0f); m->pScan->set(0.0f); m->pJitter->set(0.0f); m->pPanSpread->set(0.0f);
        m->pSize->set(40.0f); m->pDensity->set(50.0f); m->pAttack->set(0.0f); m->pRelease->set(100.0f);
        const auto out = play(*m, {on(0.0, 60, 1), off(6.0, 1)}, 7.0);      // three times as long as the sample
        // Fifty identical grains a second, nothing random: if the position holds, the sound repeats every 960 samples
        // for the whole note. If it crept through the sample it could not.
        double worst = 0.0, loRms = 1e9, hiRms = 0.0;
        for (size_t i = (size_t)(0.5 * kSr); i + 960 < (size_t)(6.0 * kSr); ++i) worst = std::max(worst, (double)std::fabs(out.l[i + 960] - out.l[i]));
        for (double t = 0.5; t < 5.9; t += 0.25) {
            const double rms = tu::rmsOf(out.l, (size_t)(t * kSr), (size_t)(t * kSr) + 9600);
            loRms = std::min(loRms, rms); hiRms = std::max(hiRms, rms);
        }
        CHECK_MSG(worst < 1.0e-6, "position %.2f: the sound changes by %g from one grain period to the next", pos, worst);
        CHECK_MSG(hiRms / loRms < 1.0001, "position %.2f: level moves by %.5f dB over the note", pos, db(hiRms / loRms));
        CHECK(loRms > 0.02);
        // and it is that place of the sample: one long grain there has, at its middle, the sweep's pitch at the position
        auto one = plain(sweep);
        one->pPosition->set(pos);
        const auto g = play(*one, {on(0.0, 60, 1)}, 0.6);
        const double want = 110.0 * std::pow(16.0, (double)pos), hz = tu::measureHz(g.l, 11000, 13000, kSr);
        CHECK_MSG(std::fabs(hz / want - 1.0) < 0.01, "position %.2f: %.1f Hz, the sweep is at %.1f Hz there", pos, hz, want);
        // and it ends with the note
        CHECK(peak(out, (size_t)(6.5 * kSr)) < 1.0e-4f);
    }
}

TEST(motes_same_notes_render_identically_twice) {
    const auto vowel = vowelSample();
    auto cloud = [&](int seed) {
        auto m = std::make_shared<Motes>();
        m->prepare(kSr);
        m->setSample(vowel);
        const auto presets = m->factoryPresets();
        for (auto& p : presets) if (p.name == "Cloud") m->loadState(p.state);
        m->pSeed->set((float)seed);
        m->pAttack->set(5.0f);
        return m;
    };
    const std::vector<Ev> notes = {on(0.0, 60, 1), on(0.1, 64, 2), on(0.2, 67, 3), off(0.9, 1), off(1.0, 2), on(1.0, 60, 4), off(1.5, 3), off(1.8, 4)};
    auto a = cloud(0), b = cloud(0), c = cloud(1);
    const auto oa = play(*a, notes, 2.5), ob = play(*b, notes, 2.5), oc = play(*c, notes, 2.5);
    CHECK(same(oa, ob));
    CHECK(!same(oa, oc));
    CHECK(peak(oa) > 0.05f);
    CHECK(tu::maxAbsDiff(oa.l, oa.r) > 0.01);       // it is random: the two sides differ
    // again on the same instance after reset(), and after prepare()
    a->reset();
    CHECK(same(play(*a, notes, 2.5), oa));
    a->prepare(kSr);
    CHECK(same(play(*a, notes, 2.5), oa));
    // a note sounds the same wherever it is: the second C (at 1.0 s) is the first one again
    auto d = cloud(0);
    const auto lone = play(*d, {on(0.0, 60, 1), off(0.5, 1)}, 1.5);
    auto e = cloud(0);
    const auto later = play(*e, {on(0.7, 60, 9), off(1.2, 9)}, 2.2);
    bool equal = true;
    const size_t shift = (size_t)std::llround(0.7 * kSr);
    for (size_t i = 0; i + shift < later.l.size() && i < lone.l.size(); ++i) equal = equal && later.l[i + shift] == lone.l[i];
    CHECK(equal);
    // another key draws other numbers: the stereo picture of D is not that of C
    auto f = cloud(0);
    f->pKeytrack->set(0.0f);
    const auto key62 = play(*f, {on(0.0, 62, 1), off(0.5, 1)}, 1.5);
    auto g = cloud(0);
    g->pKeytrack->set(0.0f);
    const auto key60 = play(*g, {on(0.0, 60, 1), off(0.5, 1)}, 1.5);
    CHECK(!same(key62, key60));
}

TEST(motes_is_silent_without_a_sample) {
    Motes m;
    m.prepare(kSr);
    const std::vector<Ev> notes = {on(0.0, 60, 1), on(0.1, 67, 2), off(0.5, 1)};
    auto out = play(m, notes, 1.0);
    CHECK(peak(out) == 0.0f);
    CHECK(m.activeVoices() == 0 && m.sample() == nullptr && m.samplePath().empty());
    // with one, it sounds; taken away again, it stops and holds no voices
    m.setSample(vowelSample());
    out = play(m, {on(0.0, 60, 3)}, 1.0);
    CHECK(peak(out) > 0.02f);
    CHECK(m.activeVoices() == 1);
    CHECK(m.meter(0) >= 1.0f);
    m.setSample(nullptr);
    out = play(m, {on(0.1, 62, 4)}, 0.5);
    CHECK(peak(out) == 0.0f && m.activeVoices() == 0);
    // a sample too short to read is the same as none
    auto tiny = std::make_shared<SampleData>();
    tiny->ch[0] = {0.5f, -0.5f};
    m.setSample(tiny);
    CHECK(peak(play(m, {on(0.0, 60, 5)}, 0.2)) == 0.0f);
}

TEST(motes_sample_survives_save_and_load_in_a_project_and_presets_leave_it_alone) {
    namespace fs = std::filesystem;
    const std::string dir = t::tmpDir() + "/creative_motes";
    fs::remove_all(dir);
    fs::create_directories(dir + "/samples");
    writeWavFile(dir + "/samples/ah.wav", *vowelSample(1.0), WavFormat::Pcm24);
    Project a, b;
    a.projectDir = dir;
    auto m = std::make_shared<Motes>();
    m->setSample(SampleCache::global().load(dir + "/samples/ah.wav"));
    CHECK(m->sample() != nullptr);
    const auto presets = m->factoryPresets();
    m->loadState(presets[5].state);          // a preset does not take the sample away
    CHECK(m->sample() != nullptr && m->samplePath().find("ah.wav") != std::string::npos);
    m->pSize->set(123.0f);
    auto& ch = a.addChannel(m, "Grains");
    for (int k : {60, 64, 67}) { Note n; n.tick = 0; n.length = 960 * 2; n.key = k; a.pattern().notesFor(ch.id).push_back(n); }
    std::string err;
    CHECK(a.save(dir + "/song.pulp"));
    CHECK_MSG(b.load(dir + "/song.pulp", &err), "%s", err.c_str());
    auto* back = dynamic_cast<Motes*>(b.channels[0].live->plugin.get());
    CHECK(back != nullptr);
    if (!back) return;
    CHECK(back->sample() != nullptr && back->pSize->get() == 123.0f);
    CHECK_MSG(back->samplePath().find("ah.wav") != std::string::npos, "path %s", back->samplePath().c_str());
    // the file reference is stored under "sample", relative to the project
    CHECK(a.toJson().dump().find("samples/ah.wav") != std::string::npos);
    CHECK(m->saveState()["extra"]["sample"].isString());
    RenderOptions o;
    o.lengthSamples = 96000;
    auto ra = renderOffline(a, o), rb = renderOffline(b, o);
    CHECK(ra && rb);
    if (ra && rb) { CHECK(tu::identical(*ra, *rb)); CHECK(tu::peakOf(ra->ch[0]) > 0.02f); }
    // loading a state: no "extra" at all, or one without a sample entry, keeps the sample; an empty path clears it
    Json bare = Json::object();
    bare.set("type", "grains");
    bare.set("params", Json::object());
    back->loadState(bare);
    CHECK(back->sample() != nullptr);
    bare.set("extra", Json::object());
    back->loadState(bare);
    CHECK(back->sample() != nullptr);
    Json ex = Json::object();
    ex.set("sample", "");
    bare.set("extra", ex);
    back->loadState(bare);
    CHECK(back->sample() == nullptr);
    ex.set("sample", "samples/ah.wav");
    bare.set("extra", ex);
    back->loadState(bare, dir);
    CHECK(back->sample() != nullptr && back->sample()->frames() == 44100);
}

TEST(motes_voices_are_limited_and_stolen_without_clicks) {
    // A 440 Hz sine, frozen: steady tones, so any click shows as a step larger than the tones' own.
    auto make = [](int poly) {
        auto m = std::make_shared<Motes>();
        m->prepare(kSr);
        m->setSample(sineSample());
        m->pRoot->set(69.0f);
        m->pPosition->set(0.5f); m->pSpread->set(0.0f); m->pJitter->set(0.0f); m->pPanSpread->set(0.0f);
        m->pSize->set(100.0f); m->pDensity->set(20.0f); m->pAttack->set(5.0f); m->pRelease->set(200.0f);
        m->pPoly->set((float)poly);
        return m;
    };
    std::vector<Ev> notes;
    const int keys[] = {57, 61, 64, 69, 73, 76, 81, 57, 64, 69};
    for (int i = 0; i < 10; ++i) notes.push_back(on(0.2 + 0.07 * i, keys[i], (uint32)(i + 1)));
    auto limited = make(3), roomy = make(16);
    int most = 0, mostRoomy = 0;
    const auto out = play(*limited, notes, 1.5, kSr, 0.0f, [&](size_t q) {
        // ten milliseconds after each note the faded voices are gone: never more than three sounding
        for (int i = 0; i < 10; ++i) if (q == (size_t)((0.2 + 0.07 * i + 0.01) * kSr / kQuantum) + 1) most = std::max(most, limited->activeVoices());
    });
    const auto all = play(*roomy, notes, 1.5, kSr, 0.0f, [&](size_t) { mostRoomy = std::max(mostRoomy, roomy->activeVoices()); });
    CHECK_MSG(most == 3, "%d voices sounding with a limit of 3", most);
    CHECK(mostRoomy == 10);
    CHECK(limited->activeVoices() == 3);
    // with the limit the stolen notes fade in 3 ms: the largest step is no more than the tones' own (taken from the
    // run with room for all ten) plus that fade of one voice at its level
    const float one = peak(play(*make(16), {on(0.0, 81, 1)}, 0.5));
    const float boundSteal = maxStep(all.l) + one / 144.0f;
    CHECK_MSG(maxStep(out.l) <= boundSteal, "largest step %.4f, bound %.4f", maxStep(out.l), boundSteal);
    printf("    motes stealing: largest step %.4f with a limit of 3 (%.4f with room for all ten notes; one voice peaks at %.3f)\n", maxStep(out.l), maxStep(all.l), one);
    // a limit of one: each new note takes over
    auto mono = make(1);
    int monoMost = 0;
    const auto mo = play(*mono, notes, 1.5, kSr, 0.0f, [&](size_t q) {
        for (int i = 0; i < 10; ++i) if (q == (size_t)((0.2 + 0.07 * i + 0.01) * kSr / kQuantum) + 1) monoMost = std::max(monoMost, mono->activeVoices());
    });
    CHECK(monoMost == 1);
    CHECK(maxStep(mo.l) <= one * (float)(kTwoPi * 880.0 / kSr) * 1.5f + one / 144.0f);
    // Kill fades everything in 3 ms; KillNote one note; AllOff releases
    auto k = make(8);
    const auto killed = play(*k, {on(0.0, 69, 1), on(0.0, 76, 2), other(0.5, NoteEvent::Kill)}, 1.0);
    CHECK(peak(killed, 24000 + 146) == 0.0f && peak(killed, 24000, 24000 + 100) > 0.01f);
    CHECK(maxStep(killed.l, 23000) <= 2.0f * one * (float)(kTwoPi * 660.0 / kSr) + 2.0f * one / 144.0f);
    CHECK(k->activeVoices() == 0);
    auto kn = make(8);
    const auto killedOne = play(*kn, {on(0.0, 69, 1), on(0.0, 76, 2), other(0.5, NoteEvent::KillNote, 2)}, 1.0);
    CHECK(kn->activeVoices() == 1);
    CHECK_NEAR(tu::measureHz(killedOne.l, 30000, 46000, kSr), 440.0, 0.5);
    auto ao = make(8);
    const auto released = play(*ao, {on(0.0, 69, 1), on(0.0, 76, 2), other(0.3, NoteEvent::AllOff)}, 2.5);
    CHECK(ao->activeVoices() == 0);
    CHECK(peak(released, (size_t)(0.4 * kSr), (size_t)(0.45 * kSr)) > 0.01f);        // still ringing in the release
    CHECK(peak(released, (size_t)(2.0 * kSr)) < 1.0e-4f);
    // a released note falls smoothly to silence: no step at its end larger than the tone's own
    CHECK(maxStep(released.l, (size_t)(0.3 * kSr)) <= 2.0f * one * (float)(kTwoPi * 660.0 / kSr));
}

TEST(motes_envelope_velocity_pan_filter_and_grain_controls) {
    const auto sine = sineSample();
    auto steady = [&] {
        auto m = std::make_shared<Motes>();
        m->prepare(kSr);
        m->setSample(sine);
        m->pRoot->set(69.0f);
        m->pPosition->set(0.5f); m->pSpread->set(0.0f); m->pJitter->set(0.0f); m->pPanSpread->set(0.0f);
        m->pSize->set(100.0f); m->pDensity->set(20.0f); m->pAttack->set(0.0f); m->pRelease->set(100.0f);
        return m;
    };
    {   // the note starts on its sample
        auto m = steady();
        Ev e = on(0.0, 69, 1);
        e.at = 1000;
        const auto out = play(*m, {e}, 0.2);
        CHECK(peak(out, 0, 1001) == 0.0f);
        CHECK(out.l[1003] != 0.0f);
    }
    {   // velocity: level goes with the square, 100 being the reference
        auto a = steady(), b = steady();
        const float full = peak(play(*a, {on(0.0, 69, 1, 100.0f / 127.0f)}, 0.6), 9600);
        const float half = peak(play(*b, {on(0.0, 69, 1, 50.0f / 127.0f)}, 0.6), 9600);
        CHECK_NEAR(half / full, 0.25, 0.002);
    }
    {   // a note's own pan; pan spread throws the grains about; a mono sample with neither is the same on both sides
        auto m = steady();
        Ev e = on(0.0, 69, 1);
        e.e.pan = -1.0f;
        const auto left = play(*m, {e}, 0.5);
        CHECK(tu::peakOf(left.r) == 0.0f && tu::peakOf(left.l) > 0.05f);
        auto c = steady();
        const auto centre = play(*c, {on(0.0, 69, 1)}, 0.5);
        CHECK(centre.l == centre.r);
        auto w = steady();
        w->pPanSpread->set(1.0f);
        const auto wide = play(*w, {on(0.0, 69, 1)}, 2.0);
        CHECK(tu::maxAbsDiff(wide.l, wide.r) > 0.05);
    }
    {   // attack, decay to the sustain level, release
        auto full = steady();
        full->pAttack->set(200.0f);
        const auto up = play(*full, {on(0.0, 69, 1)}, 1.5);
        const double level = tu::rmsOf(up.l, (size_t)(1.2 * kSr), (size_t)(1.4 * kSr));
        CHECK_NEAR(tu::rmsOf(up.l, (size_t)(0.075 * kSr), (size_t)(0.125 * kSr)) / level, 0.5, 0.03);      // half way up after half the attack
        CHECK_NEAR(tu::rmsOf(up.l, (size_t)(0.3 * kSr), (size_t)(0.5 * kSr)) / level, 1.0, 0.001);
        auto m = steady();
        m->pAttack->set(200.0f); m->pDecay->set(200.0f); m->pSustain->set(0.5f); m->pRelease->set(300.0f);
        const auto out = play(*m, {on(0.0, 69, 1), off(1.5, 1)}, 3.0);
        CHECK_NEAR(tu::rmsOf(out.l, (size_t)(1.2 * kSr), (size_t)(1.4 * kSr)) / level, 0.5, 0.01);          // settled on the sustain level
        CHECK(tu::rmsOf(out.l, (size_t)(0.2 * kSr), (size_t)(0.22 * kSr)) / level > 0.8);                    // after passing the top
        // the release falls 40 dB in its time (300 ms)
        const double r0 = tu::rmsOf(out.l, (size_t)(1.4 * kSr), (size_t)(1.5 * kSr)), r1 = tu::rmsOf(out.l, (size_t)(1.75 * kSr), (size_t)(1.85 * kSr));
        CHECK_NEAR(db(r1 / r0), -40.0, 4.0);
        CHECK(peak(out, (size_t)(2.5 * kSr)) == 0.0f);
        // a note's own release setting scales it
        auto s = steady();
        s->pRelease->set(300.0f);
        Ev e = on(0.0, 69, 1);
        e.e.release = 0.0f;         // a quarter of the time
        const auto quick = play(*s, {e, off(0.5, 1)}, 1.5);
        CHECK(peak(quick, (size_t)(0.5 * kSr) + (size_t)(0.25 * 0.3 * 2.2 * kSr)) < 1.0e-3f);
    }
    {   // the filter: with the cutoff at 300 Hz a 1760 Hz tone is 30 dB down (12 dB an octave); mod X opens it again
        auto open = steady(), shut = steady(), modded = steady();
        for (auto* m : {open.get(), shut.get(), modded.get()}) m->pCoarse->set(24.0f);       // 1760 Hz
        shut->pCutoff->set(300.0f); modded->pCutoff->set(300.0f);
        const double o = tu::rmsOf(play(*open, {on(0.0, 69, 1)}, 1.0).l, 24000), s = tu::rmsOf(play(*shut, {on(0.0, 69, 1)}, 1.0).l, 24000);
        CHECK_NEAR(db(s / o), -30.7, 1.5);
        Ev e = on(0.0, 69, 1);
        e.e.modX = 1.0f;            // five octaves up: 9.6 kHz
        CHECK_NEAR(db(tu::rmsOf(play(*modded, {e}, 1.0).l, 24000) / o), 0.0, 0.5);
        // resonance lifts the cutoff frequency
        auto reso = steady();
        reso->pCutoff->set(440.0f); reso->pReso->set(1.0f);
        auto flat = steady();
        flat->pCutoff->set(440.0f);
        const double flatRms = tu::rmsOf(play(*flat, {on(0.0, 69, 1)}, 1.0).l, 24000);
        CHECK(tu::rmsOf(play(*reso, {on(0.0, 69, 1)}, 1.0).l, 24000) > 5.0 * flatRms);
        // and so does a note's mod Y
        auto viaNote = steady();
        viaNote->pCutoff->set(440.0f);
        Ev y = on(0.0, 69, 1);
        y.e.modY = 1.0f;
        CHECK(tu::rmsOf(play(*viaNote, {y}, 1.0).l, 24000) > 5.0 * flatRms);
    }
    {   // level is in dB and moves without a step
        auto a = steady(), b = steady();
        b->pLevel->set(-12.0f);
        CHECK_NEAR(db(peak(play(*b, {on(0.0, 69, 1)}, 0.5), 9600) / peak(play(*a, {on(0.0, 69, 1)}, 0.5), 9600)), -12.0, 0.01);
        auto c = steady();
        const auto moved = play(*c, {on(0.0, 69, 1)}, 1.0, kSr, 0.0f, [&](size_t q) { c->pLevel->set(q % 2 ? -36.0f : 6.0f); });
        const float pk = peak(moved);
        CHECK(maxStep(moved.l) < pk * (float)(kTwoPi * 440.0 / kSr) + pk * 2.0f / 64.0f);
    }
    {   // reverse chance 1: every grain reads backwards. On the rising sweep each grain falls in pitch
        auto m = plain(sweepSample());
        m->pReverse->set(1.0f);
        const auto out = play(*m, {on(0.0, 60, 1)}, 0.6);
        const double early = tu::measureHz(out.l, 6000, 10000, kSr), late = tu::measureHz(out.l, 14000, 18000, kSr);
        CHECK_MSG(late < early * 0.9, "%.1f Hz then %.1f Hz", early, late);
        auto f = plain(sweepSample());
        const auto fwd = play(*f, {on(0.0, 60, 1)}, 0.6);
        CHECK(tu::measureHz(fwd.l, 14000, 18000, kSr) > tu::measureHz(fwd.l, 6000, 10000, kSr) * 1.1);
        // and the middle of the grain is at the position either way: 440 Hz there
        CHECK_NEAR(tu::measureHz(out.l, 11000, 13000, kSr), 440.0, 6.0);
        CHECK_NEAR(tu::measureHz(fwd.l, 11000, 13000, kSr), 440.0, 6.0);
    }
    {   // grain size and density: one grain every 100 ms, each 20 ms long: sound for a fifth of the time
        auto m = steady();
        m->pSize->set(20.0f); m->pDensity->set(10.0f);
        const auto out = play(*m, {on(0.0, 69, 1)}, 1.0);
        size_t sounding = 0;
        for (size_t i = 0; i < 48000; ++i) sounding += out.l[i] != 0.0f || out.r[i] != 0.0f || (i > 0 && out.l[i - 1] != 0.0f && i + 1 < 48000 && out.l[i + 1] != 0.0f);
        CHECK_MSG(std::fabs((double)sounding / 48000.0 - 0.2) < 0.01, "sounding %.3f of the time", (double)sounding / 48000.0);
        CHECK(out.l[2400] == 0.0f && out.l[4800 + 480] != 0.0f);
        // jitter moves the grains off the grid
        auto j = steady();
        j->pSize->set(20.0f); j->pDensity->set(10.0f); j->pJitter->set(1.0f);
        CHECK(!same(play(*j, {on(0.0, 69, 1)}, 1.0), out));
        // position spread: grains from all over the sweep are not all at one pitch
        auto sp = plain(sweepSample());
        sp->pSpread->set(1.0f); sp->pSize->set(100.0f); sp->pDensity->set(5.0f);
        const auto spread = play(*sp, {on(0.0, 60, 1)}, 2.0);
        std::set<int> pitches;
        for (int g = 0; g < 9; ++g) pitches.insert((int)std::lround(tu::measureHz(spread.l, (size_t)(g * 9600 + 1500), (size_t)(g * 9600 + 3300), kSr) / 20.0));
        CHECK(pitches.size() >= 5);
    }
    // the grain shapes: nothing at either end, never above 1, and each its own
    for (int s = 0; s < Motes::NumWindows; ++s) {
        CHECK(Motes::windowAt(s, 0.0) == 0.0f && Motes::windowAt(s, 1.0) == 0.0f);
        float top = 0.0f;
        for (double p = 0.0; p <= 1.0; p += 0.001) { const float w = Motes::windowAt(s, p); CHECK(w >= 0.0f && w <= 1.0f); top = std::max(top, w); }
        CHECK_MSG(top > 0.8f, "shape %d peaks at %.3f", s, top);
        CHECK(Motes::windowAt(s, 0.001) < 0.05f && Motes::windowAt(s, 0.999) < 0.05f);
    }
    CHECK_NEAR(Motes::windowAt(Motes::Hann, 0.5), 1.0, 1e-6);
    CHECK_NEAR(Motes::windowAt(Motes::Triangle, 0.25), 0.5, 1e-6);
    CHECK(Motes::windowAt(Motes::FlatTop, 0.5) == 1.0f && Motes::windowAt(Motes::FlatTop, 0.3) == 1.0f);
    CHECK(Motes::windowAt(Motes::Pluck, 0.05) > Motes::windowAt(Motes::Pluck, 0.5) * 4.0f);
    CHECK_NEAR(Motes::windowAt(Motes::Swell, 0.9), Motes::windowAt(Motes::Pluck, 0.1), 1e-6);
}

TEST(motes_every_preset_sounds_at_a_sensible_level_and_reloads) {
    Motes probe;
    const auto presets = probe.factoryPresets();
    CHECK(presets.size() >= 20);
    std::set<std::string> names;
    const SamplePtr samples[] = {vowelSample(), sweepSample()};
    const char* sampleNames[] = {"vowel", "sweep"};
    double lowest = 0.0, highest = -200.0, loudestChord = -200.0;
    for (auto& pr : presets) {
        CHECK_MSG(names.insert(pr.name).second, "preset name %s is used twice", pr.name.c_str());
        CHECK(pr.state["extra"].isNull());      // a preset carries no sample
        for (int s = 0; s < 2; ++s) {
            Motes m;
            m.prepare(kSr);
            m.setSample(samples[s]);
            m.loadState(pr.state);
            CHECK(m.sample() == samples[s]);
            // one note at velocity 100, held for four seconds, then its release
            const auto out = play(m, {on(0.0, 60, 1), off(4.0, 1)}, 9.0);
            CHECK_MSG(allFinite(out), "%s is not finite", pr.name.c_str());
            const double pk = peakDb(out);
            lowest = std::min(lowest, pk); highest = std::max(highest, pk);
            CHECK_MSG(pk >= -14.0 && pk <= -4.0, "%s on the %s: peak %.1f dBFS", pr.name.c_str(), sampleNames[s], pk);
            CHECK_MSG(peak(out, (size_t)(8.5 * kSr)) < 1.0e-3f, "%s on the %s: still at %.4f long after the note", pr.name.c_str(), sampleNames[s], peak(out, (size_t)(8.5 * kSr)));
            CHECK(!hasDenormal(out));
            // a three-note chord stays below +6 dBFS
            Motes c;
            c.prepare(kSr);
            c.setSample(samples[s]);
            c.loadState(pr.state);
            const auto chord = play(c, {on(0.0, 48, 1), on(0.0, 60, 2), on(0.0, 67, 3)}, 3.0);
            CHECK_MSG(allFinite(chord) && peakDb(chord) < 6.0 && peakDb(chord) > -20.0, "%s: a three-note chord peaks at %.1f dBFS", pr.name.c_str(), peakDb(chord));
            loudestChord = std::max(loudestChord, peakDb(chord));
            // save, recreate, load: the same state, the same sound
            Motes q;
            q.prepare(kSr);
            q.setSample(samples[s]);
            Json st = Json::parse(m.saveState().dump());
            st.set("extra", Json());         // (the sample made in code has no file to come back from)
            q.loadState(st);
            for (auto& prm : m.params()) CHECK(q.param(prm->info.id)->get() == prm->get());
            if (s == 0) {
                Motes r;
                r.prepare(kSr);
                r.setSample(samples[s]);
                r.loadState(pr.state);
                CHECK_MSG(same(play(q, {on(0.0, 60, 1), off(0.5, 1)}, 1.0), play(r, {on(0.0, 60, 1), off(0.5, 1)}, 1.0)), "%s sounds different after a state round trip", pr.name.c_str());
            }
        }
    }
    printf("    motes: %d presets, one note at velocity 100 peaks between %.1f and %.1f dBFS on a full-scale vowel and a full-scale sweep; the loudest three-note chord %.1f dBFS\n", (int)presets.size(), lowest, highest, loudestChord);
    // the state as it comes (no preset): in range too
    Motes d;
    d.prepare(kSr);
    d.setSample(samples[0]);
    const double pk = peakDb(play(d, {on(0.0, 60, 1)}, 3.0));
    CHECK_MSG(pk >= -14.0 && pk <= -4.0, "defaults: %.1f dBFS", pk);
}
