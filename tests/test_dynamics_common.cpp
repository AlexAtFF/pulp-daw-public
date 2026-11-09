// What every dynamics effect must do: presets, state, silence, automation, and identical renders
// at any block size and thread count with no allocation on the audio thread.
#include "DynTestUtil.h"
#include <set>
using namespace pulp;
using namespace tu;
using namespace dt;

namespace {
const double kSr = 48000.0;

struct Kind { const char* id; const char* name; int minPresets; std::function<std::shared_ptr<Plugin>()> make; };
const std::vector<Kind>& kinds() {
    static const std::vector<Kind> k = {
        {"multiband", "Triband", 8, [] { return std::make_shared<Triband>(); }},
        {"transient", "Snap", 5, [] { return std::make_shared<Snap>(); }},
        {"clip", "Curb", 5, [] { return std::make_shared<Curb>(); }},
        {"enhance", "Glow", 5, [] { return std::make_shared<Glow>(); }},
        {"deess", "Sibilant", 5, [] { return std::make_shared<Sibilant>(); }},
        {"gate", "Doorman", 5, [] { return std::make_shared<Doorman>(); }},
    };
    return k;
}
}

TEST(dynamics_identity_parameters_and_groups) {
    for (auto& k : kinds()) {
        auto p = k.make();
        CHECK(std::string(p->typeId()) == k.id);
        CHECK(std::string(p->displayName()) == k.name);
        CHECK(!p->isInstrument());
        CHECK(p->wantsSidechain() == (std::string(k.id) == "gate"));
        // every parameter has a unique id and a name, clamps to its range, starts inside it and shows a value
        std::set<std::string> ids;
        for (auto& prm : p->params()) {
            CHECK_MSG(ids.insert(prm->info.id).second, "%s: duplicate id %s", k.name, prm->info.id.c_str());
            CHECK(!prm->info.name.empty());
            CHECK(prm->info.def >= prm->info.min && prm->info.def <= prm->info.max);
            CHECK(prm->get() == prm->info.def);
            prm->set(prm->info.max + 1000.0f);
            CHECK(prm->get() == prm->info.max);
            prm->set(prm->info.min - 1000.0f);
            CHECK(prm->get() == prm->info.min);
            CHECK(!prm->display().empty());
            prm->set(prm->info.def);
        }
        // groups: where there are any, every parameter sits in exactly one
        const auto groups = p->paramGroups();
        if (p->params().size() > 8) CHECK_MSG(!groups.empty(), "%s has %d parameters and no groups", k.name, (int)p->params().size());
        if (!groups.empty()) {
            std::set<std::string> grouped;
            for (auto& g : groups) {
                CHECK(!g.title.empty() && !g.ids.empty());
                for (auto& id : g.ids) {
                    CHECK_MSG(p->param(id) != nullptr, "%s: group %s names unknown parameter %s", k.name, g.title.c_str(), id.c_str());
                    CHECK_MSG(grouped.insert(id).second, "%s: %s is in two groups", k.name, id.c_str());
                }
            }
            CHECK(grouped == ids);
        }
        // the latency does not depend on any setting
        const int lat = p->latencySamples();
        for (auto& prm : p->params()) { prm->set(prm->info.max); CHECK(p->latencySamples() == lat); prm->set(prm->info.min); CHECK(p->latencySamples() == lat); }
    }
}

TEST(dynamics_presets_are_stable_and_reload) {
    for (auto& k : kinds()) {
        const auto presets = k.make()->factoryPresets();
        CHECK_MSG((int)presets.size() >= k.minPresets, "%s has %d presets", k.name, (int)presets.size());
        std::set<std::string> names;
        for (auto& pr : presets) {
            CHECK_MSG(names.insert(pr.name).second, "%s: two presets called %s", k.name, pr.name.c_str());
            // every value in the preset is a real parameter and inside its range
            auto probe = k.make();
            for (auto& prm : probe->params()) {
                const float v = pr.state["params"][prm->info.id].f(-1.0e9f);
                CHECK_MSG(v >= prm->info.min && v <= prm->info.max, "%s / %s: %s = %g is out of range", k.name, pr.name.c_str(), prm->info.id.c_str(), v);
            }
            // noise with a loud tone in front, a sine, and silence
            for (int sig = 0; sig < 3; ++sig) {
                auto p = k.make();
                p->prepare(kSr);
                p->loadState(pr.state);
                Stereo in = sig == 0 ? noise(0.5, 0.3f) : (sig == 1 ? tone(220, 0.5, 0.8f) : silence(0.5));
                if (sig == 0) for (size_t i = 0; i < 2000; ++i) in.l[i] = in.r[i] = 0.9f * (float)std::sin(i * 0.05);
                Stereo side = noise(0.5, 0.5f, kSr, 5u);
                const auto out = run(*p, in, kSr, &side);
                CHECK_MSG(allFinite(out), "%s / %s is not finite", k.name, pr.name.c_str());
                CHECK_MSG(peakOf(out.l) < 4.0f && peakOf(out.r) < 4.0f, "%s / %s runs away (peak %.2f)", k.name, pr.name.c_str(), peakOf(out.l));
                if (sig == 2) CHECK_MSG(peakOf(out.l) == 0.0f && peakOf(out.r) == 0.0f, "%s / %s makes sound out of silence", k.name, pr.name.c_str());
                if (sig == 1 && !p->param("listen")) CHECK_MSG(rmsOf(out.l, 12000) > 1.0e-5, "%s / %s is silent on a sine", k.name, pr.name.c_str());
                // save, recreate, load: same state
                auto q = k.make();
                q->prepare(kSr);
                q->loadState(p->saveState());
                CHECK(q->saveState() == p->saveState());
                // and the same state gives the same sound
                if (sig == 0) {
                    const auto again = run(*q, in, kSr, &side);
                    CHECK_MSG(again.l == out.l && again.r == out.r, "%s / %s sounds different after a reload", k.name, pr.name.c_str());
                }
            }
        }
    }
}

TEST(dynamics_silence_in_gives_silence_out) {
    for (auto& k : kinds()) {
        const auto presets = k.make()->factoryPresets();
        for (size_t pi = 0; pi <= presets.size(); ++pi) {      // every preset, and the defaults
            auto p = k.make();
            p->prepare(kSr);
            if (pi < presets.size()) p->loadState(presets[pi].state);
            const char* what = pi < presets.size() ? presets[pi].name.c_str() : "defaults";
            // loud noise, then nothing: the output must fall to exact zero, not crawl in denormals or sit on a DC offset
            Stereo in = noise(0.3, 0.8f);
            append(in, silence(2.5));
            const auto out = run(*p, in);
            CHECK(allFinite(out));
            const size_t tail = out.l.size() - 4800;
            CHECK_MSG(peakOf(out.l, tail) == 0.0f && peakOf(out.r, tail) == 0.0f, "%s / %s: %g left 2.4 s after the sound stopped", k.name, what, std::max(peakOf(out.l, tail), peakOf(out.r, tail)));
            // one second in, whatever is left is far below anything audible and has no DC in it
            const size_t mid = (size_t)(1.3 * kSr);
            CHECK_MSG(peakOf(out.l, mid) < 1.0e-6f, "%s / %s: %g left 1 s after the sound stopped", k.name, what, peakOf(out.l, mid));
            // no number in the output is a denormal
            bool denormal = false;
            for (float v : out.l) if (v != 0.0f && std::fabs(v) < 1.0e-37f) denormal = true;
            CHECK_MSG(!denormal, "%s / %s puts out denormals", k.name, what);
        }
    }
}

TEST(dynamics_reset_and_prepare_start_clean) {
    // After reset() or a new prepare() a plugin gives exactly what a new one gives: no state, no ramp left over.
    for (auto& k : kinds()) {
        const auto presets = k.make()->factoryPresets();
        const auto in = groove(0.4, 0.7f);
        auto fresh = k.make();
        fresh->loadState(presets[1].state);
        fresh->prepare(kSr);
        const auto want = run(*fresh, in);
        auto used = k.make();
        used->prepare(kSr);
        run(*used, noise(0.3, 0.9f));                   // default settings, other sound
        used->loadState(presets[1].state);
        used->reset();
        CHECK_MSG(run(*used, in).l == want.l, "%s: reset() leaves something behind", k.name);
        used->prepare(96000.0);
        run(*used, noise(0.1, 0.9f, 96000.0), 96000.0);
        used->prepare(kSr);
        CHECK_MSG(run(*used, in).l == want.l, "%s: prepare() leaves something behind", k.name);
    }
}

TEST(dynamics_survive_automation_of_every_parameter) {
    // Every parameter jumps to a new random value every quantum while noise and tones run through.
    for (auto& k : kinds()) {
        for (double sr : {44100.0, 96000.0}) {
            auto p = k.make();
            p->prepare(sr);
            dsp::Noise rnd(1234u);
            Stereo in = noise(1.0, 0.7f, sr);
            addTo(in, tone(90, 1.0, 0.5f, sr));
            Stereo side = noise(1.0, 0.5f, sr, 3u);
            Stereo out = in;
            const size_t n = in.l.size() / kQuantum * kQuantum;
            float peak = 0.0f;
            bool finite = true;
            for (size_t i = 0; i < n; i += kQuantum) {
                for (auto& prm : p->params()) prm->setNorm(rnd.uni());
                ProcessContext c;
                c.sampleRate = sr;
                c.io[0] = out.l.data() + i;
                c.io[1] = out.r.data() + i;
                c.sidechain[0] = side.l.data() + i;
                c.sidechain[1] = side.r.data() + i;
                p->process(c);
                for (int s = 0; s < kQuantum; ++s) {
                    finite = finite && std::isfinite(c.io[0][s]) && std::isfinite(c.io[1][s]);
                    peak = std::max(peak, std::max(std::fabs(c.io[0][s]), std::fabs(c.io[1][s])));
                }
            }
            CHECK_MSG(finite, "%s at %.0f Hz is not finite under automation", k.name, sr);
            // the most gain anything here can be asked for is Triband's make-up, expansion and maximize together: under 60 dB
            CHECK_MSG(peak < 2000.0f, "%s at %.0f Hz runs away under automation (peak %.1f)", k.name, sr, peak);
        }
    }
}

TEST(dynamics_parameter_moves_are_ramped) {
    // A gain-like control moved in the middle of a tone must not put a step in the output.
    struct Move { const char* kind; const char* id; float from, to; } moves[] = {
        {"multiband", "mmakeup", 0, 6}, {"multiband", "maximize", 0, 6}, {"multiband", "limit", 1, 0},
        {"transient", "out", 0, -6}, {"transient", "mix", 1, 0},
        {"clip", "out", 0, -6}, {"clip", "drive", 0, 6}, {"clip", "over", 1, 0},
        {"enhance", "out", 0, -6}, {"enhance", "amount", 0.2f, 0.8f}, {"enhance", "character", 0, 2},
        {"deess", "listen", 0, 1}, {"deess", "mode", 0, 1},
        {"gate", "listen", 0, 1}, {"gate", "look", 0, 5},
    };
    const auto in = tone(500, 0.4, 0.2f);
    const float inStep = maxStep(in.l);
    for (auto& m : moves) {
        std::shared_ptr<Plugin> p;
        for (auto& k : kinds()) if (std::string(k.id) == m.kind) p = k.make();
        p->prepare(kSr);
        if (std::string(m.kind) == "gate") p->param("thresh")->set(-60.0f);
        p->param(m.id)->set(m.from);
        Stereo a = in, b = in;
        a.l.resize(9600); a.r.resize(9600);
        b.l.erase(b.l.begin(), b.l.begin() + 9600); b.r.erase(b.r.begin(), b.r.begin() + 9600);
        auto out = run(*p, a);
        p->param(m.id)->set(m.to);
        append(out, run(*p, b));
        // the step right where the control moved is no bigger than the tone's own steepest step (allowing for the gain that was added)
        const float allowed = inStep * 2.2f;
        CHECK_MSG(maxStep(out.l, 9000) <= allowed, "%s %s %g -> %g: step %.5f, allowed %.5f", m.kind, m.id, m.from, m.to, maxStep(out.l, 9000), allowed);
    }
}

TEST(dynamics_render_is_identical_at_any_block_size_and_thread_count) {
    auto build = [](Project& p) {
        p.global->tempo.set(126.0f);
        // drums on insert 1, a tone on insert 2 (also the gate's key), bright noise on insert 3, more on 4 and 5
        auto kick = addSampleChannel(p, drums::makeKick({}, kSr), "Kick");
        p.channels.back().mixerTrack = 1;
        for (int s : {0, 4, 8, 12, 14}) Project::setStep(p.pattern().notesFor(kick.id), s, true);
        auto snare = addSampleChannel(p, drums::makeSnare({}, kSr), "Snare");
        p.channels.back().mixerTrack = 1;
        for (int s : {4, 12}) Project::setStep(p.pattern().notesFor(snare.id), s, true);
        auto pad = addSampleChannel(p, sine(220, 1.5, kSr, 0.3f), "Pad");
        p.channels.back().mixerTrack = 2;
        for (int s : {0, 8}) Project::setStep(p.pattern().notesFor(pad.id), s, true);
        auto hat = addSampleChannel(p, drums::makeHat({}, kSr), "Hat");
        p.channels.back().mixerTrack = 3;
        for (int s = 0; s < 16; s += 2) Project::setStep(p.pattern().notesFor(hat.id), s, true);
        auto bass = addSampleChannel(p, sine(55, 0.4, kSr, 0.6f), "Bass");
        p.channels.back().mixerTrack = 4;
        for (int s : {2, 6, 10, 15}) Project::setStep(p.pattern().notesFor(bass.id), s, true);
        auto lead = addSampleChannel(p, sine(880, 0.3, 44100.0, 0.25f), "Lead");
        p.channels.back().mixerTrack = 5;
        for (int s : {1, 5, 9, 13}) Project::setStep(p.pattern().notesFor(lead.id), s, true);

        auto load = [](std::shared_ptr<Plugin> plug, int preset) { plug->loadState(plug->factoryPresets()[(size_t)preset].state); return plug; };
        p.setSlot(1, 0, load(std::make_shared<Snap>(), 1));
        p.setSlot(1, 1, load(std::make_shared<Curb>(), 3));
        auto gate = std::make_shared<Doorman>();
        gate->loadState(gate->factoryPresets()[4].state);          // keyed from the sidechain
        p.setSlot(2, 0, gate);
        CHECK(p.setRoute(1, 2, true, true));                        // drums key the pad's gate
        p.setSlot(3, 0, load(std::make_shared<Sibilant>(), 5));
        p.setSlot(4, 0, load(std::make_shared<Doorman>(), 1));
        p.setSlot(4, 1, load(std::make_shared<Curb>(), 4));
        p.setSlot(5, 0, load(std::make_shared<Glow>(), 3));
        p.setSlot(0, 0, load(std::make_shared<Triband>(), 2));
        p.setSlot(0, 1, load(std::make_shared<Glow>(), 0));
    };
    std::shared_ptr<SampleData> ref;
    struct Case { int threads, block; } cases[] = {{1, 512}, {1, 64}, {1, 33}, {1, 4096}, {4, 512}, {4, 64}, {3, 1000}};
    for (auto& cs : cases) {
        Project p;
        build(p);
        RenderOptions o;
        o.lengthSamples = 72000;
        o.threads = cs.threads;
        o.blockSize = cs.block;
        const int64 before = Rt::allocationsInRt.load();
        auto out = renderOffline(p, o);
        CHECK_MSG(Rt::allocationsInRt.load() == before, "threads %d block %d: %lld allocations on the audio thread", cs.threads, cs.block, (long long)(Rt::allocationsInRt.load() - before));
        if (!ref) {
            ref = out;
            CHECK(peakOf(out->ch[0]) > 0.1f && peakOf(out->ch[0]) <= 1.0f);
            for (float v : out->ch[0]) if (!std::isfinite(v)) { CHECK(false); break; }
            continue;
        }
        CHECK_MSG(identical(*ref, *out), "threads %d block %d differs", cs.threads, cs.block);
    }
    // the project reports the latency the effects add, and the render above was trimmed by it (the kick still starts at 0)
    Project p;
    build(p);
    CHECK(p.compile()->latency > 0);
    CHECK(peakOf(ref->ch[0], 0, 480) > 0.02f);
}

TEST(dynamics_processing_does_not_allocate) {
    // Straight calls, with the real-time marker set as the engine sets it, while every parameter is being moved.
    for (auto& k : kinds()) {
        auto p = k.make();
        p->prepare(kSr);
        dsp::Noise rnd(99u);
        Stereo io = noise(0.3, 0.6f), side = noise(0.3, 0.6f, kSr, 8u);
        const int64 before = Rt::allocationsInRt.load();
        for (size_t i = 0; i + kQuantum <= io.l.size(); i += kQuantum) {
            for (auto& prm : p->params()) prm->setNorm(rnd.uni());
            ProcessContext c;
            c.io[0] = io.l.data() + i;
            c.io[1] = io.r.data() + i;
            c.sidechain[0] = side.l.data() + i;
            c.sidechain[1] = side.r.data() + i;
            Rt::Scope rt;
            p->process(c);
            if (i == 6400) p->reset();
        }
        CHECK_MSG(Rt::allocationsInRt.load() == before, "%s allocates in process() or reset()", k.name);
    }
}
