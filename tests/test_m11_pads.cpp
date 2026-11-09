// Milestone 11: Sixteen (the pad drum sampler) and Chop (the loop slicer).
#include "Util.h"
#include "pulp/Pads.h"
#include <filesystem>

using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0;
const int kTick = 25;       // samples per tick at 120 BPM and 48 kHz

struct Hit { int tick; int key; float velocity; int length; };

std::shared_ptr<SampleData> playNotes(Project& p, int channelId, const std::vector<Hit>& hits, int64 samples, int block = 512, int threads = 1) {
    p.global->tempo.set(120.0f);
    auto& notes = p.pattern().notesFor(channelId);
    notes.clear();
    for (auto& h : hits) { Note n; n.tick = h.tick; n.length = h.length; n.key = h.key; n.velocity = h.velocity; notes.push_back(n); }
    sortNotes(notes);
    p.pattern().steps = 64;
    RenderOptions o;
    o.lengthSamples = samples;
    o.blockSize = block;
    o.threads = threads;
    return renderOffline(p, o);
}

struct PadChan { int id; std::shared_ptr<PadKit> kit; };
PadChan addPads(Project& p) {
    auto k = std::make_shared<PadKit>();
    auto& c = p.addChannel(k, "Pads");
    return {c.id, k};
}

// A drum loop to cut up: a hit every eighth note for one bar at 120 BPM (two seconds), alternating a low
// thud and a bright tick, each dying away before the next.
std::shared_ptr<SampleData> makeLoop(std::vector<int64>* where = nullptr) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = kSr;
    s->channels = 1;
    s->ch[0].assign((size_t)(2.0 * kSr), 0.0f);
    dsp::Noise noise(7);
    for (int h = 0; h < 8; ++h) {
        const int64 at = (int64)(h * 0.25 * kSr);
        if (where) where->push_back(at);
        for (int i = 0; i < 9000; ++i) {
            const double t = i / kSr;
            const float env = (float)std::exp(-t * (h % 2 ? 60.0 : 22.0));
            const float x = h % 2 ? noise.next() * 0.5f : (float)std::sin(2.0 * 3.14159265358979 * (90.0 + 60.0 * std::exp(-t * 30.0)) * t) * 0.8f;
            s->ch[0][(size_t)(at + i)] += x * env;
        }
    }
    return s;
}
}

TEST(pads_play_their_own_samples_by_key_and_velocity) {
    Project p;
    auto ch = addPads(p);
    ch.kit->setLayer(0, 0, sine(220.0, 0.4, kSr));
    ch.kit->setLayer(1, 0, sine(880.0, 0.4, kSr));
    ch.kit->addLayer(2, sine(300.0, 0.4, kSr));        // soft layer
    ch.kit->addLayer(2, sine(600.0, 0.4, kSr));        // hard layer
    CHECK(ch.kit->layerCount(2) == 2);
    CHECK(ch.kit->layerCount(0) == 1);
    CHECK(ch.kit->layerCount(5) == 0);
    auto r = playNotes(p, ch.id, {{0, 60, 0.8f, 100}, {960, 61, 0.8f, 100}, {1920, 62, 0.3f, 100}, {2880, 62, 0.95f, 100}, {3840, 65, 0.8f, 100}, {4800, 59, 0.8f, 100}}, 6 * 24000);
    CHECK_NEAR(measureHz(r->ch[0], 2000, 16000, kSr), 220.0, 0.5);
    CHECK_NEAR(measureHz(r->ch[0], 26000, 40000, kSr), 880.0, 1.0);
    CHECK_NEAR(measureHz(r->ch[0], 50000, 64000, kSr), 300.0, 0.5);
    CHECK_NEAR(measureHz(r->ch[0], 74000, 88000, kSr), 600.0, 1.0);
    // an empty pad and a key below the pads are silent; drums ring past their note's end
    CHECK(peakOf(r->ch[0], 96000, 144000) < 1.0e-6f);
    CHECK(peakOf(r->ch[0], 10000, 16000) > 0.1f);
    // a harder hit is louder
    CHECK(peakOf(r->ch[0], 74000, 88000) > peakOf(r->ch[0], 50000, 64000) * 3.0f);
}

TEST(pads_level_pan_tune_length_and_cut_groups) {
    Project p;
    auto ch = addPads(p);
    ch.kit->setLayer(0, 0, sine(220.0, 0.4, kSr));
    ch.kit->setLayer(3, 0, sine(500.0, 1.5, kSr));     // an open hat, as it were
    ch.kit->setLayer(4, 0, sine(1500.0, 0.2, kSr));    // the closed hat that chokes it
    auto base = playNotes(p, ch.id, {{0, 60, 0.8f, 100}}, 24000);
    ch.kit->pLevel[0]->set(-6.0f);
    auto quiet = playNotes(p, ch.id, {{0, 60, 0.8f, 100}}, 24000);
    CHECK_NEAR(peakOf(quiet->ch[0]) / peakOf(base->ch[0]), 0.5012, 0.01);
    ch.kit->pLevel[0]->set(0.0f);
    ch.kit->pPan[0]->set(1.0f);
    auto right = playNotes(p, ch.id, {{0, 60, 0.8f, 100}}, 24000);
    CHECK(peakOf(right->ch[0]) < 1.0e-6f);
    CHECK_NEAR(peakOf(right->ch[1]) / peakOf(base->ch[1]), 1.0, 0.01);
    ch.kit->pPan[0]->set(0.0f);
    ch.kit->pTune[0]->set(12.0f);
    auto up = playNotes(p, ch.id, {{0, 60, 0.8f, 100}}, 24000);
    CHECK_NEAR(measureHz(up->ch[0], 1000, 8000, kSr), 440.0, 0.5);
    CHECK(peakOf(up->ch[0], 10200, 24000) < 1.0e-6f);          // twice as fast, half as long (0.4 s -> 0.2 s)
    ch.kit->pTune[0]->set(0.0f);
    ch.kit->pLength[0]->set(25.0f);
    auto shortened = playNotes(p, ch.id, {{0, 60, 0.8f, 100}}, 24000);
    CHECK(peakOf(shortened->ch[0], 1000, 4000) > 0.1f);
    CHECK(peakOf(shortened->ch[0], 4900, 24000) < 1.0e-6f);     // a quarter of 19200 frames
    ch.kit->pLength[0]->set(100.0f);
    // without a cut group the long sound rings through the short one; with one it stops within a few ms
    auto ring = playNotes(p, ch.id, {{0, 63, 0.8f, 100}, {480, 64, 0.8f, 100}}, 48000);
    CHECK(rmsOf(ring->ch[0], 30000, 44000) > 0.05);
    ch.kit->pGroup[3]->set(2.0f);
    ch.kit->pGroup[4]->set(2.0f);
    auto choked = playNotes(p, ch.id, {{0, 63, 0.8f, 100}, {480, 64, 0.8f, 100}}, 48000);
    CHECK(rmsOf(choked->ch[0], 4000, 11000) > 0.05);            // it sounds until the other hat lands (sample 12000)
    CHECK(peakOf(choked->ch[0], 30000, 44000) < 1.0e-5f);
    // the cut is a short fade, not a click: no jump larger than the signal's own
    float jump = 0.0f, own = 0.0f;
    for (size_t i = 11900; i < 12400; ++i) jump = std::max(jump, std::abs(choked->ch[0][i] - choked->ch[0][i - 1]));
    for (size_t i = 12500; i < 13000; ++i) own = std::max(own, std::abs(choked->ch[0][i] - choked->ch[0][i - 1]));
    CHECK_MSG(jump < own * 2.5f + 0.02f, "choke jump %.4f against %.4f", jump, own);
}

TEST(pads_find_their_places_by_name_and_survive_saving) {
    CHECK(PadKit::padForName("C:/x/808 Kick.wav") == 0);
    CHECK(PadKit::padForName("snare_03.flac") == 1);
    CHECK(PadKit::padForName("Big Clap.wav") == 2);
    CHECK(PadKit::padForName("hihat_closed.wav") == 3);
    CHECK(PadKit::padForName("808 Hat Open.wav") == 4);
    CHECK(PadKit::padForName("tom_low.wav") == 5);
    CHECK(PadKit::padForName("Tom Mid.wav") == 6);
    CHECK(PadKit::padForName("909 Tom High.wav") == 7);
    CHECK(PadKit::padForName("sidestick.wav") == 8);
    CHECK(PadKit::padForName("crash.wav") == 9);
    CHECK(PadKit::padForName("ride_bell.wav") == 10);
    CHECK(PadKit::padForName("cowbell.wav") == 11);
    CHECK(PadKit::padForName("808 Kick Long.wav") == 14);
    CHECK(PadKit::padForName("my voice.wav") == -1);

    namespace fs = std::filesystem;
    const std::string dir = t::tmpDir() + "/padkit";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const char* names[] = {"kick.wav", "snare.wav", "hat closed.wav", "hat open.wav", "odd one.wav", "kick 2.wav", "second snare.wav"};
    std::vector<std::string> files;
    for (int i = 0; i < 7; ++i) { files.push_back(dir + "/" + names[i]); writeWavFile(files.back(), *sine(200.0 + 100.0 * i, 0.2, kSr), WavFormat::Pcm24); }
    PadKit kit;
    CHECK(kit.loadKit(files) == 7);
    CHECK(kit.padName(0) == "kick");
    CHECK(kit.padName(1) == "snare");
    CHECK(kit.padName(3) == "hat closed");
    CHECK(kit.padName(4) == "hat open");
    CHECK(kit.padName(14) == "kick 2");
    CHECK(kit.padName(15) == "odd one");            // no obvious place: the last free pad
    CHECK(kit.padName(13) == "second snare");       // the snare pad was taken
    CHECK(kit.pGroup[3]->getInt() == 1 && kit.pGroup[4]->getInt() == 1);      // the hats choke each other
    kit.pTune[1]->set(-3.0f);
    const Json state = kit.saveState();
    PadKit again;
    again.loadState(Json::parse(state.dump(1)));
    for (int i = 0; i < PadKit::kPads; ++i) {
        CHECK(again.padName(i) == kit.padName(i));
        CHECK(again.layerCount(i) == kit.layerCount(i));
    }
    CHECK(again.pTune[1]->get() == -3.0f);
    // in a project file, and back
    Project p;
    auto k = std::make_shared<PadKit>();
    k->loadState(state);
    auto& c = p.addChannel(k, "Kit");
    const int id = c.id;
    auto before = playNotes(p, id, {{0, 60, 0.8f, 100}, {480, 61, 0.8f, 100}, {960, 63, 0.8f, 100}}, 48000);
    std::string err;
    CHECK(p.save(dir + "/kit.pulp", &err));
    Project q;
    CHECK(q.load(dir + "/kit.pulp", &err));
    auto after = playNotes(q, q.channels[0].id, {{0, 60, 0.8f, 100}, {480, 61, 0.8f, 100}, {960, 63, 0.8f, 100}}, 48000);
    CHECK(peakOf(before->ch[0]) > 0.1f);
    CHECK(identical(*before, *after));
}

TEST(pads_factory_kits_load_and_the_render_is_the_same_at_any_block_size) {
    namespace fs = std::filesystem;
    const std::string factory = std::string(PULP_SOURCE_DIR) + "/factory";
    Project p;
    auto ch = addPads(p);
    const auto presets = ch.kit->factoryPresets();
    CHECK(presets.size() >= 8);
    if (fs::exists(factory + "/Drums/808 Kit/808 Kick.wav")) {
        const std::string was = SampleCache::global().factoryRoot();
        SampleCache::global().setFactoryRoot(factory);
        for (auto& pr : presets) {
            const bool acoustic = pr.name.find("Acoustic") != std::string::npos;
            if (acoustic && !fs::exists(factory + "/Drums/Acoustic Kit/kick.wav")) continue;     // that kit is built by build.bat library
            ch.kit->loadState(pr.state);
            int filled = 0;
            for (int i = 0; i < PadKit::kPads; ++i) filled += ch.kit->layerCount(i) > 0 ? 1 : 0;
            CHECK_MSG(filled >= 10, "kit %s fills only %d pads", pr.name.c_str(), filled);
            auto r = playNotes(p, ch.id, {{0, 60, 0.8f, 100}, {480, 61, 0.8f, 100}, {960, 63, 0.8f, 100}}, 60000);
            CHECK_MSG(peakOf(r->ch[0]) > 0.05f && peakOf(r->ch[0]) < 4.0f, "kit %s peaks at %.3f", pr.name.c_str(), peakOf(r->ch[0]));
        }
        ch.kit->loadState(presets[0].state);
        SampleCache::global().setFactoryRoot(was);
    } else {
        for (int i = 0; i < 8; ++i) ch.kit->setLayer(i, 0, sine(150.0 + 90.0 * i, 0.3, kSr));
    }
    std::vector<Hit> hits;
    for (int i = 0; i < 64; ++i) hits.push_back({i * 90, 60 + (i * 5) % 16, 0.3f + 0.01f * i, 60});
    std::shared_ptr<SampleData> ref;
    for (auto cs : std::vector<std::pair<int, int>>{{512, 1}, {64, 1}, {31, 1}, {4096, 4}}) {
        const int64 allocs = Rt::allocationsInRt.load();
        auto r = playNotes(p, ch.id, hits, 200000, cs.first, cs.second);
        CHECK(Rt::allocationsInRt.load() == allocs);
        if (!ref) { ref = r; CHECK(peakOf(r->ch[0]) > 0.05f); continue; }
        CHECK_MSG(identical(*ref, *r), "block %d threads %d differs", cs.first, cs.second);
    }
    for (float v : ref->ch[0]) if (!std::isfinite(v)) { CHECK(false); break; }
}

TEST(chop_finds_the_hits_of_a_loop) {
    std::vector<int64> where;
    auto loop = makeLoop(&where);
    const auto found = Chop::findHits(*loop, 0.5f);
    CHECK_MSG(found.size() == 8, "found %d slices", (int)found.size());
    for (size_t i = 0; i < std::min(found.size(), where.size()); ++i)
        CHECK_MSG(std::abs((double)(found[i] - where[i])) < 0.003 * kSr, "slice %d at %lld, hit at %lld", (int)i, (long long)found[i], (long long)where[i]);
    // more sensitivity never finds fewer; silence and very short sounds give one slice
    const size_t lo = Chop::findHits(*loop, 0.0f).size(), mid = Chop::findHits(*loop, 0.5f).size(), hi = Chop::findHits(*loop, 1.0f).size();
    CHECK(lo <= mid && mid <= hi);
    SampleData silent;
    silent.sampleRate = kSr; silent.channels = 1; silent.ch[0].assign(48000, 0.0f);
    CHECK(Chop::findHits(silent, 1.0f).size() == 1);
    // quiet hits between loud ones are found only when asked for
    // (the ticks are turned down to a little above a steady bed of noise)
    auto mixed = std::make_shared<SampleData>(*loop);
    for (int h = 1; h < 8; h += 2) for (int i = 0; i < 9000; ++i) mixed->ch[0][(size_t)(h * 12000 + i)] *= 0.08f;
    dsp::Noise bed(3);
    for (auto& x : mixed->ch[0]) x += bed.next() * 0.02f;
    CHECK(Chop::findHits(*mixed, 0.0f).size() <= 4);
    CHECK(Chop::findHits(*mixed, 1.0f).size() == 8);
    // a bright tick that lands on the ringing tail of a low thud is still placed where it starts
    {
        SampleData ring;
        ring.sampleRate = kSr; ring.channels = 1; ring.ch[0].assign(48000, 0.0f);
        dsp::Noise n2(11);
        for (int i = 0; i < 40000; ++i) ring.ch[0][(size_t)i] = (float)std::sin(2.0 * 3.14159265358979 * 55.0 * i / kSr) * 0.8f * (float)std::exp(-i / kSr * 3.0);
        for (int i = 0; i < 3000; ++i) ring.ch[0][(size_t)(14400 + i)] += n2.next() * 0.3f * (float)std::exp(-i / kSr * 80.0);
        const auto at = Chop::findHits(ring, 0.5f);
        CHECK_MSG(at.size() == 2, "found %d slices in the ringing thud", (int)at.size());
        if (at.size() == 2) CHECK_MSG(std::abs((double)(at[1] - 14400)) < 0.003 * kSr, "the tick at 14400 was put at %lld", (long long)at[1]);
    }
    Chop chop;
    chop.setSample(loop);
    CHECK(chop.slices().size() == 8);
    CHECK_NEAR(chop.beats(), 4.0, 0.001);       // two seconds: one bar at 120
    chop.sliceEvenly(16);
    CHECK(chop.slices().size() == 16);
    CHECK(chop.slices()[4] == loop->frames() / 4);
    chop.pSens->set(50.0f);
    chop.detect();
    CHECK(chop.slices().size() == 8);
}

TEST(chop_each_key_plays_its_slice) {
    auto loop = makeLoop();
    Project p;
    auto chop = std::make_shared<Chop>();
    chop->setSample(loop);
    auto& c = p.addChannel(chop, "Loop");
    const int id = c.id;
    const auto starts = chop->slices();
    CHECK(starts.size() == 8);
    // key 62 is the third slice: the output is that stretch of the loop, then silence
    auto r = playNotes(p, id, {{0, 62, kDefaultVelocity, 2000}}, 48000);
    const int64 len = starts[3] - starts[2];
    double err = 0.0, ref = 0.0;
    for (int64 i = 0; i < len - 600; ++i) { const double d = r->ch[0][(size_t)i] / r->ch[0][200] * loop->ch[0][(size_t)(starts[2] + 200)] - loop->ch[0][(size_t)(starts[2] + i)]; err += d * d; ref += (double)loop->ch[0][(size_t)(starts[2] + i)] * loop->ch[0][(size_t)(starts[2] + i)]; }
    CHECK_MSG(err < ref * 0.01, "slice differs: error %.5f of %.5f", err, ref);
    CHECK(peakOf(r->ch[0], (size_t)len + 10, 48000) < 1.0e-6f);
    // a key above the last slice is silent
    CHECK(peakOf(playNotes(p, id, {{0, 60 + 8, kDefaultVelocity, 2000}}, 24000)->ch[0]) < 1.0e-6f);
    // "to the end" plays on through the following slices
    chop->pMode->set(1.0f);
    auto through = playNotes(p, id, {{0, 62, kDefaultVelocity, 2000}}, 96000);
    CHECK(peakOf(through->ch[0], (size_t)len + 2000, (size_t)len + 20000) > 0.05f);
    chop->pMode->set(0.0f);
    // an octave up is over in half the time
    chop->pPitch->set(12.0f);
    auto fast = playNotes(p, id, {{0, 62, kDefaultVelocity, 2000}}, 48000);
    CHECK(peakOf(fast->ch[0], 0, (size_t)len / 2 - 400) > 0.05f);
    CHECK(peakOf(fast->ch[0], (size_t)len / 2 + 10, 48000) < 1.0e-6f);
    chop->pPitch->set(0.0f);
    // gated: the slice stops soon after the key is released (60 ticks = 1500 samples)
    chop->pGate->set(1.0f);
    chop->pFade->set(2.0f);
    auto gated = playNotes(p, id, {{0, 60, kDefaultVelocity, 60}}, 24000);
    CHECK(peakOf(gated->ch[0], 0, 1400) > 0.05f);
    CHECK(peakOf(gated->ch[0], 2200, 24000) < 1.0e-3f);
    chop->pGate->set(0.0f);
    chop->pFade->set(8.0f);
    // reversed: the loud start of the slice comes last
    chop->pReverse->set(1.0f);
    auto rev = playNotes(p, id, {{0, 60, kDefaultVelocity, 2000}}, 24000);
    const int64 l0 = starts[1] - starts[0];
    CHECK(rmsOf(rev->ch[0], (size_t)(l0 - 3000), (size_t)(l0 - 500)) > rmsOf(rev->ch[0], 500, 3000) * 3.0);
    chop->pReverse->set(0.0f);
}

TEST(chop_slices_played_in_time_rebuild_the_loop_and_survive_saving) {
    namespace fs = std::filesystem;
    const std::string dir = t::tmpDir() + "/chop";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto made = makeLoop();
    writeWavFile(dir + "/loop.wav", *made, WavFormat::Float32);
    auto loop = SampleCache::global().load(dir + "/loop.wav");
    CHECK(loop != nullptr);
    Project p;
    auto chop = std::make_shared<Chop>();
    chop->setSample(loop);
    auto& c = p.addChannel(chop, "Loop");
    const int id = c.id;
    const auto starts = chop->slices();
    // what the program writes into the piano roll: one note per slice, where the slice sits in the loop
    std::vector<Hit> hits;
    const double ticks = chop->beats() * kPPQ;
    for (size_t i = 0; i < starts.size(); ++i) {
        const int tick = (int)std::lround((double)starts[i] / (double)loop->frames() * ticks);
        const int end = i + 1 < starts.size() ? (int)std::lround((double)starts[i + 1] / (double)loop->frames() * ticks) : (int)ticks;
        hits.push_back({tick, 60 + (int)i, kDefaultVelocity, end - tick});
    }
    auto r = playNotes(p, id, hits, loop->frames());
    // the project is at 120 and so is the loop: the slices line up and the loop comes back (but for the level
    // of the path and the short fades at the joins)
    // (compared by loudness over 5 ms stretches: a note can only start on a tick, 25 samples apart, and noise
    // moved by a few samples no longer matches sample for sample)
    double dot = 0.0, a2 = 0.0, b2 = 0.0;
    for (int64 w = 0; w + 240 <= loop->frames(); w += 240) {
        const double a = rmsOf(r->ch[0], (size_t)w, (size_t)w + 240), b = rmsOf(loop->ch[0], (size_t)w, (size_t)w + 240);
        dot += a * b; a2 += a * a; b2 += b * b;
    }
    CHECK_MSG(dot / std::sqrt(a2 * b2) > 0.985, "the loudness curve matches the loop's by %.4f", dot / std::sqrt(a2 * b2));

    chop->pFade->set(20.0f);
    std::string err;
    CHECK(p.save(dir + "/chop.pulp", &err));
    Project q;
    CHECK(q.load(dir + "/chop.pulp", &err));
    auto* again = dynamic_cast<Chop*>(q.channels[0].live->plugin.get());
    CHECK(again != nullptr);
    if (again) {
        CHECK(again->slices() == starts);
        CHECK_NEAR(again->beats(), chop->beats(), 1.0e-9);
        CHECK(again->pFade->get() == 20.0f);
        auto r1 = playNotes(p, id, hits, loop->frames());
        auto r2 = playNotes(q, q.channels[0].id, hits, loop->frames());
        CHECK(identical(*r1, *r2));
    }
    // presets change how slices play, not where they are; every one sounds
    const auto presets = chop->factoryPresets();
    CHECK(presets.size() >= 10);
    for (auto& pr : presets) {
        chop->loadState(pr.state);
        CHECK(chop->slices() == starts);
        auto out = playNotes(p, id, hits, loop->frames());
        CHECK_MSG(peakOf(out->ch[0]) > 0.02f && peakOf(out->ch[0]) < 4.0f, "preset %s peaks at %.3f", pr.name.c_str(), peakOf(out->ch[0]));
    }
    chop->loadState(presets[0].state);
    // any block size, any thread count, nothing allocated on the audio thread
    std::shared_ptr<SampleData> ref;
    for (auto cs : std::vector<std::pair<int, int>>{{512, 1}, {64, 1}, {31, 1}, {4096, 4}}) {
        const int64 allocs = Rt::allocationsInRt.load();
        auto out = playNotes(p, id, hits, loop->frames(), cs.first, cs.second);
        CHECK(Rt::allocationsInRt.load() == allocs);
        if (!ref) { ref = out; continue; }
        CHECK_MSG(identical(*ref, *out), "block %d threads %d differs", cs.first, cs.second);
    }
}
