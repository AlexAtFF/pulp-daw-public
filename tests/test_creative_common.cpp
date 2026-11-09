// What all five have to do: presets, state, silence, determinism, real-time safety.
#include "CreativeTest.h"
#include <set>
using namespace pulp;
using namespace ct;

namespace {
const double kSr = 48000.0;

struct Kind {
    const char* id;
    const char* name;
    bool instrument;
    int minPresets;
    std::function<std::shared_ptr<Plugin>()> make;
};
const std::vector<Kind>& kinds() {
    static const std::vector<Kind> k = {
        {"timewarp", "Rewind", false, 16, [] { return std::shared_ptr<Plugin>(std::make_shared<Rewind>()); }},
        {"glitch", "Hiccup", false, 8, [] { return std::shared_ptr<Plugin>(std::make_shared<Hiccup>()); }},
        {"xyfx", "Thumb Pad", false, 8, [] { return std::shared_ptr<Plugin>(std::make_shared<ThumbPad>()); }},
        {"scratch", "Platter", false, 6, [] { return std::shared_ptr<Plugin>(std::make_shared<Platter>()); }},
        {"grains", "Motes", true, 20, [] { auto m = std::make_shared<Motes>(); m->setSample(vowelSample(1.0)); return std::shared_ptr<Plugin>(m); }},
    };
    return k;
}
// Runs a plugin with random notes (for the instrument) over a signal (for the effects).
Stereo drive(Plugin& p, const Stereo& in, const Transport& t, uint32 seed, const std::function<void(size_t)>& each = {}) {
    Stereo out = in;
    if (p.isInstrument()) { std::fill(out.l.begin(), out.l.end(), 0.0f); std::fill(out.r.begin(), out.r.end(), 0.0f); }
    const size_t n = in.l.size() / kQuantum * kQuantum;
    out.l.resize(n); out.r.resize(n);
    const double tps = t.tempo * kPPQ / (60.0 * t.sr);
    dsp::Noise rnd(seed);
    uint32 id = 0;
    for (size_t i = 0; i < n; i += kQuantum) {
        if (each) each(i / kQuantum);
        NoteEvent ev[4];
        int ne = 0;
        if (p.isInstrument()) {
            const size_t q = i / kQuantum;
            if (q % 40 == 0) { ev[ne].type = NoteEvent::On; ev[ne].key = 48 + (int)(rnd.uni() * 24.0f); ev[ne].id = ++id; ev[ne].offset = (int)(rnd.uni() * 63.0f); ev[ne].velocity = 0.5f + 0.5f * rnd.uni(); ev[ne].pan = rnd.next(); ++ne; }
            if (q % 40 == 25 && id > 1) { ev[ne].type = NoteEvent::Off; ev[ne].id = id - 1; ev[ne].offset = 7; ++ne; }
            if (q % 331 == 330) { ev[ne].type = NoteEvent::Kill; ev[ne].offset = 20; ++ne; }
        }
        ProcessContext c;
        c.sampleRate = t.sr;
        c.tempo = t.tempo;
        c.playing = t.playing;
        c.tickPos = t.playing ? t.startTick + (double)i * tps : 0.0;
        c.io[0] = out.l.data() + i;
        c.io[1] = out.r.data() + i;
        c.events = ev;
        c.numEvents = ne;
        p.process(c);
    }
    return out;
}
std::string stateKey(const Preset& p) { return p.state["params"].dump() + p.state["extra"].dump(); }
}

TEST(creative_identity_parameters_presets_and_groups) {
    std::set<std::string> productNames;
    for (auto& k : kinds()) {
        auto p = k.make();
        CHECK(std::string(p->typeId()) == k.id);
        CHECK(std::string(p->displayName()) == k.name);
        CHECK(p->isInstrument() == k.instrument && !p->wantsSidechain());
        CHECK(productNames.insert(k.name).second);
        CHECK_MSG(PluginRegistry::global().find(k.id) == nullptr || std::string(PluginRegistry::global().find(k.id)->name) == k.name, "%s: the type id is taken by another plugin", k.id);
        for (auto& t : PluginRegistry::global().types()) CHECK_MSG(t.id == k.id || t.name != k.name, "%s is already the name of %s", k.name, t.id.c_str());
        std::set<std::string> ids;
        for (auto& prm : p->params()) {
            CHECK_MSG(ids.insert(prm->info.id).second, "%s: duplicate parameter %s", k.name, prm->info.id.c_str());
            CHECK(!prm->info.name.empty() && prm->info.max > prm->info.min);
            CHECK(prm->info.def >= prm->info.min && prm->info.def <= prm->info.max && prm->get() == prm->info.def);
            if (prm->info.steps > 0) CHECK_MSG(prm->info.steps == (int)std::lround(prm->info.max - prm->info.min) + 1, "%s: %s has %d steps", k.name, prm->info.id.c_str(), prm->info.steps);
        }
        const auto presets = p->factoryPresets();
        CHECK_MSG((int)presets.size() >= k.minPresets, "%s has %d presets", k.name, (int)presets.size());
        std::set<std::string> names, states;
        for (auto& pr : presets) {
            CHECK_MSG(!pr.name.empty() && names.insert(pr.name).second, "%s: preset name '%s' is empty or repeated", k.name, pr.name.c_str());
            CHECK_MSG(states.insert(stateKey(pr)).second, "%s: preset '%s' is the same as another", k.name, pr.name.c_str());
            CHECK(pr.state["type"].str("") == k.id);
            CHECK(pr.state["params"].size() == p->params().size());
            for (auto& kv : pr.state["params"].items()) {
                Param* prm = p->param(kv.first);
                CHECK_MSG(prm != nullptr, "%s / %s sets an unknown parameter %s", k.name, pr.name.c_str(), kv.first.c_str());
                if (prm) CHECK_MSG(kv.second.f() >= prm->info.min && kv.second.f() <= prm->info.max, "%s / %s: %s = %g is out of range", k.name, pr.name.c_str(), kv.first.c_str(), kv.second.num());
            }
        }
        const auto groups = p->paramGroups();
        if (p->params().size() > 8) CHECK_MSG(!groups.empty(), "%s has %d controls and no groups", k.name, (int)p->params().size());
        if (!groups.empty()) {
            std::set<std::string> seen;
            for (auto& g : groups) {
                CHECK(!g.title.empty() && !g.ids.empty());
                for (auto& id : g.ids) {
                    CHECK_MSG(p->param(id) != nullptr, "%s: group '%s' names an unknown parameter %s", k.name, g.title.c_str(), id.c_str());
                    CHECK_MSG(seen.insert(id).second, "%s: %s is in two groups", k.name, id.c_str());
                }
            }
            CHECK_MSG(seen.size() == p->params().size(), "%s: %d of %d parameters are grouped", k.name, (int)seen.size(), (int)p->params().size());
        }
        for (auto& prm : p->params()) {
            prm->set(prm->info.max + 1000.0f);
            CHECK(prm->get() == prm->info.max);
            CHECK(!prm->display().empty());
            prm->set(prm->info.min - 1000.0f);
            CHECK(prm->get() == prm->info.min);
            CHECK(!prm->display().empty());
        }
        CHECK(p->latencySamples() == 0 && p->tailSeconds() >= 0.0 && p->tailSeconds() < 30.0);
    }
}

TEST(creative_effect_presets_are_stable_for_noise_a_sine_and_silence_and_reload) {
    for (auto& k : kinds()) {
        if (k.instrument) continue;         // the instrument's presets have their own test
        const auto presets = k.make()->factoryPresets();
        for (auto& pr : presets) {
            for (int mode = 0; mode < 3; ++mode) {      // stopped, playing, and playing with the Platter held and scratched
                const bool isPlatter = std::string(k.id) == "scratch";
                if (mode == 2 && !isPlatter) continue;
                auto p = k.make();
                p->prepare(kSr);
                p->loadState(pr.state);
                Transport t;
                t.playing = mode > 0;
                t.tempo = 128.0;
                std::function<void(size_t)> each;
                if (mode == 2)      // grabbed half a second into each run, scratched, let go near the end
                    each = [&](size_t q) {
                        if (q == 375) p->param("hold")->set(1.0f);
                        if (q == 1300) p->param("hold")->set(0.0f);
                        p->param("position")->set((float)std::sin((double)q * 0.05));
                        p->param("cut")->set(q % 90 < 60 ? 1.0f : 0.0f);
                    };
                // silence first: nothing may come out of nothing
                auto quiet = run(*p, silence(0.3), t, each);
                CHECK_MSG(peak(quiet) == 0.0f, "%s / %s makes sound from silence (%g)", k.name, pr.name.c_str(), peak(quiet));
                auto out = run(*p, busy(2.0), t, each);
                auto loud = run(*p, tone(440.0, 2.0, 1.0f), t, each);
                CHECK_MSG(allFinite(out) && allFinite(loud), "%s / %s is not finite", k.name, pr.name.c_str());
                CHECK_MSG(peak(out) < 8.0f && peak(loud) < 8.0f, "%s / %s runs away (peak %.1f / %.1f)", k.name, pr.name.c_str(), peak(out), peak(loud));
                CHECK_MSG(peak(out) > 0.01f, "%s / %s is silent", k.name, pr.name.c_str());
                CHECK_MSG(!hasDenormal(out) && !hasDenormal(loud), "%s / %s puts out denormals", k.name, pr.name.c_str());
                if (mode != 1) continue;
                // save, recreate, load: same state, same sound
                auto q = k.make();
                q->prepare(kSr);
                q->loadState(p->saveState());
                CHECK(q->saveState() == p->saveState());
                auto viaText = k.make();        // through the text of a project file
                viaText->prepare(kSr);
                viaText->loadState(Json::parse(p->saveState().dump()));
                CHECK_MSG(viaText->saveState() == p->saveState(), "%s / %s changes when saved as text", k.name, pr.name.c_str());
                auto r = k.make();
                r->prepare(kSr);
                r->loadState(pr.state);
                auto a = run(*viaText, busy(1.0), t), b = run(*r, busy(1.0), t);
                CHECK_MSG(same(a, b), "%s / %s sounds different after a state round trip", k.name, pr.name.c_str());
            }
        }
    }
}

TEST(creative_effects_fall_silent_after_their_input_stops) {
    for (auto& k : kinds()) {
        if (k.instrument) continue;
        const auto presets = k.make()->factoryPresets();
        for (auto& pr : presets) {
            auto p = k.make();
            p->prepare(kSr);
            p->loadState(pr.state);
            const bool isPlatter = std::string(k.id) == "scratch";
            if (isPlatter) { p->param("hold")->set(1.0f); p->param("position")->set(-0.5f); }
            Transport t;
            t.tempo = 60.0;         // the slowest the tails are stated for
            const float top = peak(run(*p, busy(1.0), t));
            const double tail = p->tailSeconds() + 0.25;
            t.startTick = 960.0;
            auto ring = run(*p, silence(tail), t);
            t.startTick += tail * 960.0;
            auto after = run(*p, silence(1.0), t);
            // 60 dB down after the stated tail, with no DC and no denormals; and a while later exactly nothing
            CHECK_MSG(peak(after) <= 1.5e-3f * std::max(top, 0.3f), "%s / %s: %.1f dB still sounding after its %.2f s tail", k.name, pr.name.c_str(), db(peak(after) / top), tail);
            CHECK_MSG(!hasDenormal(ring) && !hasDenormal(after), "%s / %s: denormals in the tail", k.name, pr.name.c_str());
            double mean = 0.0;
            for (float v : after.l) mean += v;
            CHECK(std::fabs(mean / (double)after.l.size()) < 1e-5);
            t.startTick += 960.0;
            // (an echo at 60 % feedback needs half a minute to fall from full level to nothing at all)
            const double more = std::string(k.id) == "xyfx" ? 32.0 : 2.0;
            auto late = run(*p, silence(more), t);
            CHECK(!hasDenormal(late));
            t.startTick += more * 960.0;
            auto last = run(*p, silence(0.25), t);
            CHECK_MSG(peak(last) == 0.0f, "%s / %s never reaches zero (%g)", k.name, pr.name.c_str(), peak(last));
        }
    }
}

TEST(creative_plugins_start_the_same_after_reset_and_prepare) {
    for (auto& k : kinds()) {
        const auto presets = k.make()->factoryPresets();
        for (size_t which : {(size_t)0, presets.size() - 1, presets.size() / 2, (size_t)4}) {
            auto p = k.make();
            p->prepare(kSr);
            p->loadState(presets[which].state);
            if (std::string(k.id) == "scratch") { p->param("hold")->set(1.0f); p->param("position")->set(-0.3f); }
            Transport t;
            t.playing = which != 0;
            t.tempo = 133.0;
            const auto in = busy(2.5);
            const auto first = drive(*p, in, t, 7u);
            p->reset();
            const auto second = drive(*p, in, t, 7u);
            CHECK_MSG(same(first, second), "%s / %s differs after reset()", k.name, presets[which].name.c_str());
            p->prepare(kSr);
            const auto third = drive(*p, in, t, 7u);
            CHECK_MSG(same(first, third), "%s / %s differs after prepare()", k.name, presets[which].name.c_str());
            // a second instance agrees: nothing depends on the instance, the clock or what ran before
            auto q = k.make();
            q->prepare(kSr);
            q->loadState(presets[which].state);
            if (std::string(k.id) == "scratch") { q->param("hold")->set(1.0f); q->param("position")->set(-0.3f); }
            CHECK(same(first, drive(*q, in, t, 7u)));
            // prepared at another rate and back: still the same
            p->prepare(96000.0);
            Transport t96 = t;
            t96.sr = 96000.0;
            drive(*p, busy(0.5, 96000.0), t96, 3u);
            p->prepare(kSr);
            CHECK_MSG(same(first, drive(*p, in, t, 7u)), "%s / %s differs after a change of sample rate and back", k.name, presets[which].name.c_str());
            if (k.instrument) CHECK(peak(first) > 0.01f);
        }
    }
}

TEST(creative_plugins_survive_any_parameter_setting_without_allocating) {
    for (auto& k : kinds()) {
        for (double sr : {48000.0, 44100.0, 96000.0}) {
            auto p = k.make();
            p->prepare(sr);
            dsp::Noise rnd(1234u + (uint32)sr);
            Stereo io = busy(sr == 48000.0 ? 12.0 : 3.0, sr);
            const double tps = 133.0 * kPPQ / (60.0 * sr);
            float worst = 0.0f;
            bool finite = true;
            int64 allocations = 0;
            uint32 id = 0;
            auto* rewind = dynamic_cast<Rewind*>(p.get());
            for (size_t i = 0; i < io.l.size(); i += kQuantum) {
                const size_t q = i / kQuantum;
                // every so often every control jumps somewhere else, extremes included; in between some of them glide
                if (q % 97 == 0)
                    for (auto& prm : p->params()) { const float u = rnd.uni(); prm->setNorm(u < 0.15f ? 0.0f : (u > 0.85f ? 1.0f : u)); }
                else if (q % 3 == 0)
                    for (auto& prm : p->params()) if (prm->info.steps == 0 && rnd.uni() < 0.3f) prm->setNorm(clampT(prm->getNorm() + 0.02f * rnd.next(), 0.0f, 1.0f));
                if (rewind && q % 211 == 0) {       // new curves drawn while it plays (the interface thread's job: it may allocate)
                    std::vector<Rewind::Point> tc, vc;
                    const int n = (int)(rnd.uni() * 40.0f);
                    for (int j = 0; j < n; ++j) { tc.push_back({rnd.uni(), rnd.uni() * 16.0f, (int)(rnd.uni() * 2.99f)}); vc.push_back({rnd.uni(), rnd.uni(), (int)(rnd.uni() * 2.99f)}); }
                    rewind->setTimeCurve(tc);
                    rewind->setVolumeCurve(vc);
                }
                NoteEvent ev[3];
                int ne = 0;
                if (k.instrument) {
                    std::fill(io.l.begin() + (std::ptrdiff_t)i, io.l.begin() + (std::ptrdiff_t)i + kQuantum, 0.0f);
                    std::fill(io.r.begin() + (std::ptrdiff_t)i, io.r.begin() + (std::ptrdiff_t)i + kQuantum, 0.0f);
                    if (q % 9 == 0) { ev[ne].type = NoteEvent::On; ev[ne].key = 24 + (int)(rnd.uni() * 84.0f); ev[ne].id = ++id; ev[ne].offset = (int)(rnd.uni() * 63.0f); ev[ne].velocity = rnd.uni(); ev[ne].modX = rnd.uni(); ev[ne].modY = rnd.uni(); ++ne; }
                    if (q % 9 == 5 && id > 3) { ev[ne].type = rnd.uni() < 0.8f ? NoteEvent::Off : NoteEvent::KillNote; ev[ne].id = id - (uint32)(rnd.uni() * 3.0f); ev[ne].offset = 11; ++ne; }
                    if (q % 400 == 399) { ev[ne].type = rnd.uni() < 0.5f ? NoteEvent::AllOff : NoteEvent::Kill; ev[ne].offset = 3; ++ne; }
                    if (q % 57 == 30) { ev[ne].type = NoteEvent::SlideTo; ev[ne].key = 40 + (int)(rnd.uni() * 40.0f); ev[ne].slideSamples = 2000.0f; ev[ne].offset = 40; ++ne; }
                }
                ProcessContext c;
                c.sampleRate = sr;
                c.tempo = 133.0 + 40.0 * std::sin((double)q * 0.01);
                c.playing = (q / 700) % 2 == 0;
                c.tickPos = c.playing ? (double)((q / 2500) % 2 ? i % 200000 : i) * tps : 0.0;      // now and then the song jumps about
                c.io[0] = io.l.data() + i;
                c.io[1] = io.r.data() + i;
                c.events = ev;
                c.numEvents = ne;
                c.pitchBendSemis = 12.0f * rnd.next();
                {
                    const int64 before = Rt::allocationsInRt.load();
                    Rt::Scope rt;
                    p->process(c);
                    allocations += Rt::allocationsInRt.load() - before;
                }
                for (int s = 0; s < kQuantum; ++s) {
                    finite = finite && std::isfinite(c.io[0][s]) && std::isfinite(c.io[1][s]);
                    worst = std::max(worst, std::max(std::fabs(c.io[0][s]), std::fabs(c.io[1][s])));
                }
            }
            CHECK_MSG(allocations == 0, "%s allocates on the audio thread (%d times)", k.name, (int)allocations);
            CHECK_MSG(finite, "%s at %.0f Hz is not finite under random settings", k.name, sr);
            CHECK_MSG(worst < 60.0f, "%s at %.0f Hz peaks at %.1f under random settings", k.name, sr, worst);
            CHECK_MSG(worst > 0.01f, "%s at %.0f Hz stayed silent", k.name, sr);
            CHECK(!hasDenormal(io));
            if (sr == 48000.0) printf("    %-10s under random settings: peak %.2f (the input peaks at 0.90)\n", k.name, worst);
            // and it comes back down afterwards
            if (!k.instrument) {
                auto after = run(*p, silence(std::min(60.0, p->tailSeconds() + 12.0), sr), Transport{sr, 133.0, true, 1.0e6});
                CHECK_MSG(peak(after, after.l.size() - 4800) < 0.05f, "%s at %.0f Hz is still at %.3f after its tail", k.name, sr, peak(after, after.l.size() - 4800));
            }
        }
    }
}

TEST(creative_plugins_render_identically_at_any_block_size_and_thread_count) {
    const auto vowel = vowelSample(1.5);
    auto build = [&](Project& p) {
        p.global->tempo.set(128.0f);
        for (int i = 0; i < 6; ++i) {
            auto ch = tu::addSampleChannel(p, i % 3 == 0 ? drums::makeKick({}, kSr) : tu::sine(110.0 * (i + 1), 0.4, 44100.0, 0.12f));
            p.channels.back().mixerTrack = 1 + i % 5;
            for (int s = i % 4; s < 32; s += 3 + i % 3) Project::setStep(p.pattern().notesFor(ch.id), s, true);
            ch.live->pan.set((float)(i % 5 - 2) / 3.0f);
        }
        // the granular sampler, twice: a cloud and a stretched loop
        for (int i = 0; i < 2; ++i) {
            auto m = std::make_shared<Motes>();
            m->setSample(i == 0 ? SamplePtr(vowel) : SamplePtr(burstLoop(1.0)));
            const auto presets = m->factoryPresets();
            m->loadState(presets[i == 0 ? 5 : 3].state);
            auto& c = p.addChannel(m, i == 0 ? "Cloud" : "Stretch");
            c.mixerTrack = 6 + i;
            for (int n = 0; n < 6; ++n) { Note note; note.tick = n * 480 + i * 240; note.length = 700; note.key = 55 + (n * 7 + i * 4) % 17; note.pan = (float)(n % 3 - 1) * 0.5f; p.pattern().notesFor(c.id).push_back(note); }
        }
        auto rw = std::make_shared<Rewind>();
        { const auto pr = rw->factoryPresets(); rw->loadState(pr[9].state); }       // the stutter build
        p.setSlot(1, 0, rw);
        auto hc = std::make_shared<Hiccup>();
        hc->pProb[Hiccup::Repeat]->set(0.3f); hc->pProb[Hiccup::Reverse]->set(0.25f); hc->pProb[Hiccup::Stop]->set(0.15f); hc->pProb[Hiccup::Crush]->set(0.2f); hc->pProb[Hiccup::Gate]->set(0.25f);
        p.setSlot(2, 0, hc);
        auto echo = std::make_shared<ThumbPad>();
        echo->pMode->set((float)ThumbPad::Echo); echo->pX->set(0.4f); echo->pY->set(0.6f);
        p.setSlot(3, 0, echo);
        auto pl = std::make_shared<Platter>();
        pl->pHold->set(1.0f); pl->pSpeed->set(0.6f); pl->pPosition->set(-0.4f); pl->pRange->set(150.0f);
        p.setSlot(4, 0, pl);
        auto wash = std::make_shared<ThumbPad>();
        wash->pMode->set((float)ThumbPad::Wash); wash->pX->set(0.5f);
        p.setSlot(5, 0, wash);
        auto tape = std::make_shared<ThumbPad>();
        tape->pMode->set((float)ThumbPad::TapeSpeed); tape->pX->set(0.4f);
        p.setSlot(6, 0, tape);
        auto gate = std::make_shared<Rewind>();
        { const auto pr = gate->factoryPresets(); gate->loadState(pr[13].state); }  // a trance gate
        p.setSlot(7, 0, gate);
        auto pump = std::make_shared<Rewind>();
        { const auto pr = pump->factoryPresets(); pump->loadState(pr[1].state); }   // half speed, on the master
        p.setSlot(0, 0, pump);
        auto last = std::make_shared<Hiccup>();
        last->pGrid->set(1.0f); last->pProb[Hiccup::Repeat]->set(0.5f);
        p.setSlot(0, 1, last);
    };
    SamplePtr ref;
    struct Case { int threads, block; } cases[] = {{1, 64}, {1, 33}, {1, 4096}, {4, 512}, {4, 64}, {3, 1000}};
    for (auto& c : cases) {
        Project p;
        build(p);
        RenderOptions o;
        o.lengthSamples = 144000;
        o.threads = c.threads;
        o.blockSize = c.block;
        const int64 before = Rt::allocationsInRt.load();
        auto out = renderOffline(p, o);
        CHECK_MSG(Rt::allocationsInRt.load() == before, "threads %d block %d: %d allocations on the audio thread", c.threads, c.block, (int)(Rt::allocationsInRt.load() - before));
        CHECK(out != nullptr);
        if (!out) return;
        if (!ref) {
            ref = out;
            CHECK_MSG(tu::peakOf(out->ch[0]) > 0.05f && tu::peakOf(out->ch[0]) < 4.0f, "peak %.3f", tu::peakOf(out->ch[0]));
            bool finite = true;
            for (float v : out->ch[0]) finite = finite && std::isfinite(v);
            CHECK(finite);
            continue;
        }
        CHECK_MSG(tu::identical(*ref, *out), "threads %d block %d differs", c.threads, c.block);
    }
    {   // and each one alone on a track really is in that render: taking it out changes the result
        Project with, without;
        build(with); build(without);
        without.setSlot(1, 0, nullptr);
        RenderOptions o;
        o.lengthSamples = 96000;
        auto a = renderOffline(with, o), b = renderOffline(without, o);
        CHECK(a && b && !tu::identical(*a, *b));
    }
}

TEST(creative_effects_work_at_other_sample_rates) {
    for (double sr : {44100.0, 96000.0}) {
        Transport t;
        t.sr = sr;
        const size_t beat = (size_t)(sr / 2.0);     // 120 BPM
        {   // Rewind: half speed is an octave down, and a stutter on eighths repeats to the sample
            Rewind r;
            r.prepare(sr);
            r.setTimeCurve({{0.0f, 0.0f, Rewind::Line}, {1.0f, 2.0f, Rewind::Hold}});
            const auto out = run(r, tone(440.0, 2.0, 0.5f, sr), t);
            CHECK_NEAR(tu::measureHz(out.l, beat / 2, 3 * beat, sr), 220.0, 0.05);
            Rewind s;
            s.prepare(sr);
            std::vector<Rewind::Point> stairs;
            for (int k = 0; k < 8; ++k) stairs.push_back({(float)k / 8.0f, 0.5f * (float)k, Rewind::Hold});
            s.setTimeCurve(stairs);
            s.pSmooth->set(1.0f);
            const auto in = noise(2.0, 0.4f, sr);
            const auto st = run(s, in, t);
            size_t wrong = 0;
            const size_t fade = (size_t)(0.001 * sr) + 1;
            for (size_t k = 1; k < 8; ++k) for (size_t i = fade; i < beat / 2 && k * beat / 2 + i < st.l.size(); ++i) wrong += st.l[k * beat / 2 + i] != in.l[i];
            CHECK_MSG(wrong == 0, "%.0f Hz: %d samples of the stutter differ", sr, (int)wrong);
        }
        {   // Hiccup: the reverse trick is the step before, backwards, to the sample
            Hiccup h;
            h.prepare(sr);
            for (int k = 0; k < Hiccup::NumTricks; ++k) h.pProb[k]->set(k == Hiccup::Reverse ? 1.0f : 0.0f);
            h.pGrid->set(1.0f);         // eighths: a whole number of samples at both rates
            const auto in = noise(1.0, 0.4f, sr);
            const auto out = run(h, in, t);
            const size_t step = beat / 2, fade = (size_t)std::lround(0.002 * sr);
            size_t wrong = 0;
            for (size_t s = 1; s < 3; ++s) for (size_t j = fade; j < step; ++j) wrong += out.l[s * step + j] != in.l[s * step - 1 - j];
            CHECK_MSG(wrong == 0, "%.0f Hz: %d samples of the reversed step differ", sr, (int)wrong);
            CHECK(Hiccup::crushHold(1.0f, sr) == 1 + (int)std::lround(sr / 1500.0 - 1.0));
        }
        {   // Thumb Pad: 30 ms of echo is 30 ms, the low-pass corner is where it says, the ring's side frequencies too
            ThumbPad e;
            e.prepare(sr);
            e.pMode->set((float)ThumbPad::Echo); e.pX->set(0.0f); e.pY->set(0.0f);
            const auto h = run(e, impulse(0.2, sr), t).l;
            const size_t d = (size_t)std::llround(0.03 * sr);
            CHECK(h[0] == 1.0f && h[d] == 1.0f && tu::peakOf(h, 1, d) == 0.0f);
            ThumbPad lp;
            lp.prepare(sr);
            lp.pMode->set((float)ThumbPad::LowPass); lp.pX->set(0.5f); lp.pY->set(0.0f);
            CHECK_NEAR(gainDbAt(lp, ThumbPad::lowPassHz(0.5), 0.05f, 0.5, t), -3.01, 0.1);
            ThumbPad ring;
            ring.prepare(sr);
            ring.pMode->set((float)ThumbPad::Ring); ring.pX->set(1.0f); ring.pY->set(1.0f);
            const auto ro = run(ring, tone(1000.0, 1.0, 0.5f, sr), t);
            CHECK_NEAR(levelDb(ro.l, 8192, 16384, 5000.0, sr), db(0.25), 0.2);
            ThumbPad ph;
            ph.prepare(sr);
            ph.pMode->set((float)ThumbPad::Phaser); ph.pX->set(0.5f); ph.pY->set(0.0f);
            CHECK(gainDbAt(ph, ThumbPad::phaserHz(0.5), 0.05f, 0.5, t) < -50.0);
            for (int mode = 0; mode < ThumbPad::NumModes; ++mode) {
                ThumbPad any;
                any.prepare(sr);
                any.pMode->set((float)mode); any.pX->set(0.3f); any.pY->set(0.7f);
                const auto out = run(any, busy(0.5, sr), t);
                CHECK_MSG(allFinite(out) && peak(out) > 0.02f && peak(out) < 8.0f, "mode %d at %.0f Hz: peak %.2f", mode, sr, peak(out));
            }
        }
        {   // Platter: a 400 ms brake is 400 ms; held at speed 1 it is the input
            Platter p;
            p.prepare(sr);
            p.pSpeed->set(0.0f); p.pBrake->set(400.0f);
            const size_t grab = (size_t)(0.25 * sr) / kQuantum;
            const auto out = run(p, tone(600.0, 1.5, 0.5f, sr), t, [&](size_t q) { if (q == grab) p.pHold->set(1.0f); });
            const size_t at = grab * kQuantum + (size_t)(0.2 * sr);
            CHECK_NEAR(tu::measureHz(out.l, at - (size_t)(0.015 * sr), at + (size_t)(0.015 * sr), sr), 300.0, 6.0);
            CHECK(peak(out, grab * kQuantum + (size_t)(0.4 * sr) + 2) == 0.0f);
            Platter h;
            h.prepare(sr);
            h.pHold->set(1.0f);
            const auto in = busy(0.5, sr);
            CHECK(same(run(h, in, t), in));
        }
    }
}

TEST(creative_cpu_cost) {
    // How long each takes for ten seconds of sound at 48 kHz, as a share of real time on one core. Printed for the
    // notes; the only thing checked is that nothing is absurdly slow (the checked debug build is many times slower).
    const auto in = busy(10.0);
    auto timeIt = [&](const char* what, Plugin& p, const Transport& t, const std::function<void(size_t)>& each = {}, bool notes = false) {
        p.prepare(kSr);
        const double t0 = nowSeconds();
        if (notes) {
            Stereo out = silence(10.0);
            for (size_t i = 0; i < out.l.size(); i += kQuantum) {
                NoteEvent ev[8];
                int ne = 0;
                if (i == 0) for (int k : {48, 55, 60, 64, 67, 72, 76, 79}) { ev[ne].type = NoteEvent::On; ev[ne].key = k; ev[ne].id = (uint32)(ne + 1); ++ne; }
                ProcessContext c;
                c.sampleRate = kSr; c.io[0] = out.l.data() + i; c.io[1] = out.r.data() + i; c.events = ev; c.numEvents = ne;
                p.process(c);
            }
        } else run(p, in, t, each);
        const double share = (nowSeconds() - t0) / 10.0 * 100.0;
        printf("    cpu  %-44s %6.2f %% of one core\n", what, share);
#ifdef NDEBUG
        CHECK_MSG(share < 25.0, "%s takes %.1f %% of real time", what, share);
#endif
    };
    Transport playing;
    {
        Rewind r;
        timeIt("Rewind, nothing", r, playing);
        Rewind h;
        h.setTimeCurve({{0.0f, 0.0f, Rewind::Line}, {1.0f, 2.0f, Rewind::Hold}});
        timeIt("Rewind, half speed", h, playing);
        Rewind s;
        s.loadState(s.factoryPresets()[9].state);
        timeIt("Rewind, stutter build", s, playing);
        Rewind w;
        w.loadState(w.factoryPresets()[20].state);
        timeIt("Rewind, scratch wiggle", w, playing);
    }
    {
        Hiccup h;
        timeIt("Hiccup, as it comes", h, playing);
        Hiccup b;
        b.pProb[Hiccup::Repeat]->set(0.3f); b.pProb[Hiccup::Reverse]->set(0.25f); b.pProb[Hiccup::Stop]->set(0.3f); b.pProb[Hiccup::Crush]->set(0.2f); b.pProb[Hiccup::Gate]->set(0.25f); b.pPitch->set(5.0f);
        timeIt("Hiccup, busy with falling repeats", b, playing);
    }
    for (int mode = 0; mode < ThumbPad::NumModes; ++mode) {
        ThumbPad p;
        p.pMode->set((float)mode); p.pX->set(0.4f); p.pY->set(0.6f);
        const std::string what = "Thumb Pad, " + p.pMode->display();
        timeIt(what.c_str(), p, playing);
    }
    {
        Platter idle;
        timeIt("Platter, not held", idle, playing);
        Platter p;
        p.pHold->set(1.0f); p.pSpeed->set(0.0f); p.pRange->set(400.0f);
        timeIt("Platter, scratching", p, playing, [&](size_t q) { p.pPosition->set((float)std::sin((double)q * 0.02)); });
        Platter fast;
        fast.pHold->set(1.0f); fast.pSpeed->set(0.0f); fast.pRange->set(2000.0f); fast.pInertia->set(2.0f);
        timeIt("Platter, scratching at four times speed", fast, playing, [&](size_t q) { fast.pPosition->set((float)std::sin((double)q * 0.012)); });
    }
    {
        const auto vowel = vowelSample();
        Motes m;
        m.setSample(vowel);
        timeIt("Motes, as it comes, eight notes", m, playing, {}, true);
        Motes c;
        c.setSample(vowel);
        c.loadState(c.factoryPresets()[5].state);
        timeIt("Motes, Cloud, eight notes", c, playing, {}, true);
        Motes d;
        d.setSample(vowel);
        d.pDensity->set(200.0f); d.pSize->set(500.0f); d.pPoly->set(16.0f);
        timeIt("Motes, most grains possible, eight notes", d, playing, {}, true);
    }
}
