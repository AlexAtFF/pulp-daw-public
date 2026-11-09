// What every one of the seven effects has to do: presets, state, silence, determinism, real-time safety.
#include "SpaceTest.h"
#include <set>
using namespace pulp;
using namespace st;

namespace {
const double kSr = 48000.0;

struct Kind {
    const char* id;
    const char* name;
    std::function<std::shared_ptr<Plugin>()> make;
    const char* neutral;        // the control that takes the effect out
    float neutralValue;
};
const std::vector<Kind>& kinds() {
    static const std::vector<Kind> k = {
        {"convolve", "Spaces", [] { return std::shared_ptr<Plugin>(std::make_shared<Spaces>()); }, "mix", 0.0f},
        {"multitap", "Taps", [] { return std::shared_ptr<Plugin>(std::make_shared<Taps>()); }, "mix", 0.0f},
        {"flanger", "Jet", [] { return std::shared_ptr<Plugin>(std::make_shared<Jet>()); }, "mix", 0.0f},
        {"phaser", "Swirl", [] { return std::shared_ptr<Plugin>(std::make_shared<Swirl>()); }, "mix", 0.0f},
        {"tremolo", "Flutter", [] { return std::shared_ptr<Plugin>(std::make_shared<Flutter>()); }, "depth", 0.0f},
        {"vibrato", "Waver", [] { return std::shared_ptr<Plugin>(std::make_shared<Waver>()); }, "depth", 0.0f},
        {"rotary", "Rotor", [] { return std::shared_ptr<Plugin>(std::make_shared<Rotor>()); }, "mix", 0.0f},
    };
    return k;
}
// a busy test signal: a loud sine burst, then noise with a tone in it
Stereo busy(double seconds, double sr = kSr) {
    Stereo s = noise(seconds, 0.3f, sr);
    for (size_t i = 0; i < 2000 && i < s.l.size(); ++i) s.l[i] = s.r[i] = 0.9f * (float)std::sin((double)i * 0.05);
    for (size_t i = 2000; i < s.l.size(); ++i) { const float v = 0.25f * (float)std::sin(kTwoPi * 330.0 * (double)i / sr); s.l[i] += v; s.r[i] -= 0.5f * v; }
    return s;
}
}

TEST(space_effects_identity_parameters_presets_and_groups) {
    for (auto& k : kinds()) {
        auto p = k.make();
        CHECK(std::string(p->typeId()) == k.id);
        CHECK(std::string(p->displayName()) == k.name);
        CHECK(!p->isInstrument() && !p->wantsSidechain());
        // parameters: unique ids, named, default inside the range, clamp, readable
        std::set<std::string> ids;
        for (auto& prm : p->params()) {
            CHECK_MSG(ids.insert(prm->info.id).second, "%s: duplicate parameter %s", k.name, prm->info.id.c_str());
            CHECK(!prm->info.name.empty() && prm->info.max > prm->info.min);
            CHECK(prm->info.def >= prm->info.min && prm->info.def <= prm->info.max && prm->get() == prm->info.def);
            if (prm->info.steps > 0) CHECK(prm->info.steps == (int)std::lround(prm->info.max - prm->info.min) + 1);
        }
        // presets: at least five, named, different, and every value they set is a real parameter inside its range
        const auto presets = p->factoryPresets();
        CHECK_MSG(presets.size() >= 5, "%s has %d presets", k.name, (int)presets.size());
        std::set<std::string> names, states;
        for (auto& pr : presets) {
            CHECK_MSG(!pr.name.empty() && names.insert(pr.name).second, "%s: preset name '%s' is empty or repeated", k.name, pr.name.c_str());
            CHECK_MSG(states.insert(pr.state["params"].dump()).second, "%s: preset '%s' is the same as another", k.name, pr.name.c_str());
            CHECK(pr.state["type"].str("") == k.id);
            for (auto& kv : pr.state["params"].items()) {
                Param* prm = p->param(kv.first);
                CHECK_MSG(prm != nullptr, "%s / %s sets an unknown parameter %s", k.name, pr.name.c_str(), kv.first.c_str());
                if (prm) CHECK_MSG(kv.second.f() >= prm->info.min && kv.second.f() <= prm->info.max, "%s / %s: %s = %g is out of range", k.name, pr.name.c_str(), kv.first.c_str(), kv.second.num());
            }
        }
        // groups: when given, every parameter sits in exactly one
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
        CHECK(p->latencySamples() >= 0 && p->latencySamples() <= 16384 && p->tailSeconds() >= 0.0);
    }
}

TEST(space_effect_presets_are_stable_for_noise_a_sine_and_silence) {
    for (auto& k : kinds()) {
        const auto presets = k.make()->factoryPresets();
        for (auto& pr : presets) {
            for (bool playing : {false, true}) {
                auto p = k.make();
                p->prepare(kSr);
                p->loadState(pr.state);
                Transport t;
                t.playing = playing;
                t.tempo = 128.0;
                // silence first: nothing may come out of nothing
                auto quiet = run(*p, silence(0.3), t);
                CHECK_MSG(peak(quiet) == 0.0f, "%s / %s makes sound from silence (%g)", k.name, pr.name.c_str(), peak(quiet));
                // noise with a loud burst, then a full-scale sine
                auto out = run(*p, busy(1.0), t);
                auto loud = run(*p, tone(440.0, 1.0, 1.0f), t);
                CHECK_MSG(allFinite(out) && allFinite(loud), "%s / %s is not finite", k.name, pr.name.c_str());
                CHECK_MSG(peak(out) < 8.0f && peak(loud) < 8.0f, "%s / %s runs away (peak %.1f / %.1f)", k.name, pr.name.c_str(), peak(out), peak(loud));
                CHECK_MSG(peak(out) > 0.01f, "%s / %s is silent", k.name, pr.name.c_str());
                CHECK_MSG(!hasDenormal(out) && !hasDenormal(loud), "%s / %s puts out denormals", k.name, pr.name.c_str());
                if (playing) continue;
                // save, recreate, load: same state, same sound
                auto q = k.make();
                q->prepare(kSr);
                q->loadState(p->saveState());
                CHECK(q->saveState() == p->saveState());
                auto viaText = k.make();        // through the text of a project file
                viaText->prepare(kSr);
                viaText->loadState(Json::parse(p->saveState().dump()));
                CHECK(viaText->saveState() == p->saveState());
                auto r = k.make();
                r->prepare(kSr);
                r->loadState(pr.state);
                auto a = run(*q, busy(0.25)), b = run(*r, busy(0.25));
                CHECK_MSG(a.l == b.l && a.r == b.r, "%s / %s sounds different after a state round trip", k.name, pr.name.c_str());
            }
        }
    }
}

TEST(space_effects_fall_silent_after_their_tail) {
    for (auto& k : kinds()) {
        auto probe = k.make();
        auto presets = probe->factoryPresets();
        for (auto& pr : presets) {
            auto p = k.make();
            p->prepare(kSr);
            p->loadState(pr.state);
            Stereo in = busy(0.5);
            const float top = peak(run(*p, in));
            // the tail the plugin reports, its latency, and a little more
            const double tail = p->tailSeconds() + (double)p->latencySamples() / kSr + 0.25;
            auto ring = run(*p, silence(tail));
            auto after = run(*p, silence(0.5));
            CHECK_MSG(peak(after) <= 1.5e-3f * std::max(top, 0.3f), "%s / %s: %.1f dB still sounding after its %.2f s tail", k.name, pr.name.c_str(), db(peak(after) / top), tail);
            CHECK_MSG(!hasDenormal(ring) && !hasDenormal(after), "%s / %s: denormals in the tail", k.name, pr.name.c_str());
            double mean = 0.0;
            for (float v : after.l) mean += v;
            CHECK(std::fabs(mean / (double)after.l.size()) < 1e-5);        // no DC left behind
            // It keeps falling at least as fast as the tail it reports (60 dB per tail), and once that takes it
            // below -400 dB it is exactly zero, not a crawl of tiny numbers.
            const double more = std::min(60.0, 4.0 * tail + 2.0);
            auto late = run(*p, silence(more));
            auto last = run(*p, silence(0.25));
            const double tails = (tail + 0.5 + more) / std::max(tail, 0.01);
            const double bound = std::max(top, 0.3f) * std::pow(10.0, -3.0 * tails + 1.0);
            if (bound < 1e-19) CHECK_MSG(peak(last) == 0.0f, "%s / %s never reaches zero (%g)", k.name, pr.name.c_str(), peak(last));
            else CHECK_MSG(peak(last) <= bound, "%s / %s: %g left where at most %g should be", k.name, pr.name.c_str(), peak(last), bound);
            CHECK(!hasDenormal(late) && !hasDenormal(last));
        }
    }
}

TEST(space_effects_are_transparent_with_the_effect_taken_out) {
    for (auto& k : kinds()) {
        for (bool playing : {false, true}) {
            auto p = k.make();
            p->prepare(kSr);
            // make everything else as busy as it gets, then take the effect out with its own control
            for (auto& prm : p->params()) if (prm->info.steps == 0) prm->setNorm(0.8f);
            p->param(k.neutral)->set(k.neutralValue);
            Transport t;
            t.playing = playing;
            auto in = busy(0.5);
            auto out = run(*p, in, t);
            const size_t lat = (size_t)p->latencySamples();
            bool same = true;
            for (size_t i = lat; i < out.l.size(); ++i) same = same && out.l[i] == in.l[i - lat] && out.r[i] == in.r[i - lat];
            CHECK_MSG(same, "%s with %s at %g is not the input (delayed by %d samples)", k.name, k.neutral, k.neutralValue, (int)lat);
        }
    }
}

TEST(space_effects_start_the_same_after_reset_and_prepare) {
    for (auto& k : kinds()) {
        const auto presets = k.make()->factoryPresets();
        for (size_t which : {(size_t)0, presets.size() - 1, presets.size() / 2}) {
            auto p = k.make();
            p->prepare(kSr);
            p->loadState(presets[which].state);
            Transport t;
            t.playing = which != 0;
            auto in = busy(0.6);
            auto first = run(*p, in, t);
            p->reset();
            auto second = run(*p, in, t);
            CHECK_MSG(first.l == second.l && first.r == second.r, "%s / %s differs after reset()", k.name, presets[which].name.c_str());
            p->prepare(kSr);
            auto third = run(*p, in, t);
            CHECK_MSG(first.l == third.l && first.r == third.r, "%s / %s differs after prepare()", k.name, presets[which].name.c_str());
            // a second instance agrees: nothing depends on the instance, the clock or what ran before
            auto q = k.make();
            q->prepare(kSr);
            q->loadState(presets[which].state);
            auto other = run(*q, in, t);
            CHECK(first.l == other.l && first.r == other.r);
        }
    }
}

TEST(space_effects_survive_any_parameter_setting_without_allocating) {
    for (auto& k : kinds()) {
        for (double sr : {48000.0, 44100.0, 96000.0}) {
            auto p = k.make();
            p->prepare(sr);
            dsp::Noise rnd(1234u + (uint32)sr);
            Stereo io = busy(sr == 48000.0 ? 6.0 : 2.0, sr);
            const double tps = 133.0 * kPPQ / (60.0 * sr);
            float worst = 0.0f;
            bool finite = true;
            const int64 before = Rt::allocationsInRt.load();
            {
                Rt::Scope rt;
                for (size_t i = 0; i < io.l.size(); i += kQuantum) {
                    const size_t q = i / kQuantum;
                    // every so often every control jumps somewhere else, extremes included; in between some of them glide
                    if (q % 97 == 0)
                        for (auto& prm : p->params()) { const float u = rnd.uni(); prm->setNorm(u < 0.15f ? 0.0f : (u > 0.85f ? 1.0f : u)); }
                    else if (q % 3 == 0)
                        for (auto& prm : p->params()) if (prm->info.steps == 0 && rnd.uni() < 0.3f) prm->setNorm(clampT(prm->getNorm() + 0.02f * rnd.next(), 0.0f, 1.0f));
                    ProcessContext c;
                    c.sampleRate = sr;
                    c.tempo = 133.0 + 40.0 * std::sin((double)q * 0.01);
                    c.playing = (q / 700) % 2 == 0;
                    c.tickPos = c.playing ? (double)i * tps : 0.0;
                    c.io[0] = io.l.data() + i;
                    c.io[1] = io.r.data() + i;
                    p->process(c);
                    for (int s = 0; s < kQuantum; ++s) {
                        finite = finite && std::isfinite(c.io[0][s]) && std::isfinite(c.io[1][s]);
                        worst = std::max(worst, std::max(std::fabs(c.io[0][s]), std::fabs(c.io[1][s])));
                    }
                }
            }
            CHECK_MSG(Rt::allocationsInRt.load() == before, "%s allocates on the audio thread", k.name);
            CHECK_MSG(finite, "%s at %.0f Hz is not finite under random settings", k.name, sr);
            CHECK_MSG(worst < 60.0f, "%s at %.0f Hz peaks at %.1f under random settings", k.name, sr, worst);
            CHECK(!hasDenormal(io));
            // and it comes back down afterwards
            auto after = run(*p, silence(std::min(60.0, p->tailSeconds() + 1.0), sr), Transport{sr});
            CHECK_MSG(peak(after, after.l.size() - 4800) < 0.05f, "%s at %.0f Hz is still at %.3f after its tail", k.name, sr, peak(after, after.l.size() - 4800));
        }
    }
}

TEST(space_effects_render_identically_at_any_block_size_and_thread_count) {
    auto build = [](Project& p) {
        p.global->tempo.set(128.0f);
        for (int i = 0; i < 8; ++i) {
            auto ch = tu::addSampleChannel(p, i % 3 == 0 ? drums::makeKick({}, kSr) : tu::sine(110.0 * (i + 1), 0.4, 44100.0, 0.12f));
            p.channels.back().mixerTrack = 1 + i % 7;
            for (int s = i % 4; s < 16; s += 3 + i % 3) Project::setStep(p.pattern().notesFor(ch.id), s, true);
            ch.live->pan.set((float)(i % 5 - 2) / 3.0f);
        }
        int track = 1;
        for (auto& k : kinds()) {
            auto fx = k.make();
            const auto presets = fx->factoryPresets();
            fx->loadState(presets[presets.size() / 2].state);
            p.setSlot(track++, 0, fx);
        }
        // more of them in series and on a send, with tempo-synced settings, so the song position matters
        auto pan = std::make_shared<Flutter>();
        pan->pSync->set(10.0f); pan->pPhase->set(180.0f); pan->pShape->set((float)Flutter::Square);
        p.setSlot(3, 1, pan);
        auto jet = std::make_shared<Jet>();
        jet->pSync->set(4.0f);
        p.setSlot(2, 1, jet);
        auto hall = std::make_shared<Spaces>();
        hall->pMix->set(1.0f); hall->pDecay->set(0.6f); hall->pPre->set(20.0f);
        p.setRoute(1, 20, true); p.setRoute(2, 20, true); p.setRoute(5, 20, true);
        p.setSlot(20, 0, hall);
        p.setSlot(0, 0, std::make_shared<Taps>());
        p.setSlot(0, 1, std::make_shared<Rotor>());        // latent, on the master
    };
    SamplePtr ref;
    struct Case { int threads, block; } cases[] = {{1, 64}, {1, 33}, {1, 4096}, {4, 512}, {4, 64}, {3, 1000}};
    for (auto& c : cases) {
        Project p;
        build(p);
        RenderOptions o;
        o.lengthSamples = 96000;
        o.threads = c.threads;
        o.blockSize = c.block;
        const int64 before = Rt::allocationsInRt.load();
        auto out = renderOffline(p, o);
        CHECK(Rt::allocationsInRt.load() == before);
        CHECK(out != nullptr);
        if (!out) return;
        if (!ref) {
            ref = out;
            CHECK(tu::peakOf(out->ch[0]) > 0.05f && tu::peakOf(out->ch[0]) < 4.0f);
            bool finite = true;
            for (float v : out->ch[0]) finite = finite && std::isfinite(v);
            CHECK(finite);
            continue;
        }
        CHECK_MSG(tu::identical(*ref, *out), "threads %d block %d differs", c.threads, c.block);
    }
    {   // a latent effect on one track keeps that track in time with the others (the mixer delays the rest to match)
        for (int which = 0; which < 2; ++which) {
            Project a, b;
            for (Project* p : {&a, &b}) {
                auto click = std::make_shared<SampleData>();
                click->sampleRate = kSr;
                click->ch[0].assign(64, 0.0f);
                click->ch[0][0] = 0.8f;
                auto ch = tu::addSampleChannel(*p, click);
                p->channels.back().mixerTrack = 1;
                Project::setStep(p->pattern().notesFor(ch.id), 0, true);
                Project::setStep(p->pattern().notesFor(ch.id), 8, true);
            }
            std::shared_ptr<Plugin> fx;
            if (which == 0) { auto w = std::make_shared<Waver>(); w->pDepth->set(0.0f); fx = w; }
            else { auto r = std::make_shared<Rotor>(); r->pMix->set(0.0f); fx = r; }
            b.setSlot(1, 0, fx);
            RenderOptions o;
            o.lengthSamples = 48000;
            auto plain = renderOffline(a, o), with = renderOffline(b, o);
            CHECK(plain && with);
            if (plain && with) CHECK_MSG(tu::identical(*plain, *with), "%s shifts its track in time", fx->displayName());
        }
    }
}

TEST(space_effects_work_at_other_sample_rates) {
    for (double sr : {44100.0, 96000.0}) {
        Transport t;
        t.sr = sr;
        for (auto& k : kinds()) {
            auto p = k.make();
            p->prepare(sr);
            auto out = run(*p, busy(0.5, sr), t);
            CHECK_MSG(allFinite(out) && peak(out) > 0.05f && peak(out) < 8.0f, "%s at %.0f Hz: peak %.2f", k.name, sr, peak(out));
        }
        {   // a flutter of 5 Hz is 5 Hz
            Flutter f;
            f.prepare(sr);
            f.pRate->set(5.0f);
            auto g = run(f, dc(2.0, 1.0f, sr), t);
            std::vector<double> gd(g.l.begin(), g.l.end());
            CHECK_NEAR(oscRate(gd, 0, gd.size(), sr), 5.0, 0.002);
        }
        {   // a phaser notch at 1 kHz is at 1 kHz
            Swirl s;
            s.prepare(sr);
            s.pStages->set(0.0f); s.pLow->set(1000.0f); s.pHigh->set(1000.0f); s.pDepth->set(0.0f); s.pFeedback->set(0.0f);
            auto h = run(s, impulse(0.1, sr), t).l;
            h.resize(4096);
            auto n = notches(h, sr, 100, 10000, 1, 30);
            CHECK(n.size() == 1);
            if (n.size() == 1) CHECK_NEAR(n[0], 1000.0, 1.0);
        }
        {   // a rotary horn at 6.8 turns a second is at 6.8
            Rotor r;
            r.prepare(sr);
            r.pHorn->set(Rotor::Fast); r.pDoppler->set(0.0f); r.pAmDepth->set(0.8f); r.pSpread->set(0.0f); r.pDistance->set(0.0f);
            const double f0 = sr / 24.0;
            auto tr = demod(run(r, tone(f0, 2.0, 0.3f, sr), t).l, sr, f0, 96);
            CHECK_NEAR(oscRate(tr.amp, (size_t)(0.5 * sr), (size_t)(1.9 * sr), sr), 6.8, 0.05);
        }
        {   // a multitap quarter second is a quarter second
            Taps d;
            d.prepare(sr);
            d.pSync->set(0.0f); d.pCount->set(1.0f); d.pTime[0]->set(250.0f); d.pMix->set(1.0f); d.pFeedback->set(0.0f); d.pPan[0]->set(0.0f);
            auto out = run(d, impulse(0.5, sr), t);
            size_t at = 0;
            for (size_t i = 0; i < out.l.size(); ++i) if (out.l[i] != 0.0f) { at = i; break; }
            CHECK(at == (size_t)std::lround(0.25 * sr));
        }
    }
}
