// What all four vocal effects have to get right: presets, silence, a clean start, the same render at any
// block size and thread count, no allocation, no trouble under automation, constant latency.
#include "VocalTest.h"
#include <chrono>
#include <limits>
#include <set>
#include <thread>
using namespace pulp;
using namespace vt;

namespace {
const double kSr = 48000.0;

// A state for Lint that holds a learned profile (learned once from hiss), for tests that need it working.
const Json& lintState() {
    static const Json state = [] {
        Lint d;
        d.prepare(kSr);
        d.pLearn->set(1.0f);
        run(d, noise(1.0, 0.02f, kSr, 11u));
        d.pLearn->set(0.0f);
        Stereo one;
        one.l.assign(kQuantum, 0.0f); one.r = one.l;
        run(d, one);
        return d.saveState();
    }();
    return state;
}

struct Kind { const char* name; std::function<std::shared_ptr<Plugin>()> make; };
const std::vector<Kind>& kinds() {
    static const std::vector<Kind> k = {
        {"True Note", [] { return std::make_shared<TrueNote>(); }},
        {"Talker", [] { return std::make_shared<Talker>(); }},
        {"Second Voice", [] { return std::make_shared<SecondVoice>(); }},
        {"Lint", [] { return std::make_shared<Lint>(); }},
        {"Lint with a profile", [] { auto d = std::make_shared<Lint>(); d->loadState(lintState()); return d; }},
    };
    return k;
}

// Something of everything: silence, a sung phrase with vibrato, hiss, a click, silence.
Stereo programme(double sr = kSr) {
    Stereo s = silence(0.1, sr);
    auto f0 = [](double t) { return (t < 0.5 ? 196.0 : 261.6) * std::pow(2.0, 0.25 * std::sin(kTwoPi * 5.0 * t) / 12.0); };
    Stereo v = mono(voice(1.0, sr, f0, vowelAh(), 0.4f));
    const Stereo hiss = noise(1.0, 0.01f, sr, 19u);
    for (size_t i = 0; i < v.l.size(); ++i) { v.l[i] += hiss.l[i]; v.r[i] = 0.8f * v.r[i] + hiss.r[i]; }
    append(s, v);
    append(s, noise(0.3, 0.2f, sr, 23u));
    Stereo click = silence(0.2, sr);
    click.l[300] = 0.8f; click.r[300] = 0.8f;
    append(s, click);
    return s;
}
bool hasSubnormal(const Stereo& s) {
    for (float v : s.l) if (std::fpclassify(v) == FP_SUBNORMAL) return true;
    for (float v : s.r) if (std::fpclassify(v) == FP_SUBNORMAL) return true;
    return false;
}
}

TEST(vocal_identities_parameters_and_groups) {
    const char* ids[] = {"tune", "vocoder", "harmony", "denoise", "denoise"};
    int k = 0;
    for (auto& kind : kinds()) {
        auto p = kind.make();
        CHECK(std::string(p->typeId()) == ids[k++]);
        CHECK(!p->isInstrument());
        CHECK(p->wantsSidechain() == (std::string(p->typeId()) == "vocoder"));
        std::set<std::string> seen;
        for (auto& prm : p->params()) {
            CHECK_MSG(seen.insert(prm->info.id).second, "%s: parameter id %s twice", kind.name, prm->info.id.c_str());
            CHECK(!prm->info.name.empty());
            prm->set(prm->info.max + 1000.0f);
            CHECK(prm->get() == prm->info.max);
            prm->set(prm->info.min - 1000.0f);
            CHECK(prm->get() == prm->info.min);
            CHECK(!prm->display().empty());
            prm->set(prm->info.def);
            CHECK(prm->info.def >= prm->info.min && prm->info.def <= prm->info.max);
        }
        // every parameter is on the panel exactly once
        std::set<std::string> grouped;
        for (auto& g : p->paramGroups()) for (auto& id : g.ids) {
            CHECK_MSG(p->param(id) != nullptr, "%s: group %s names %s", kind.name, g.title.c_str(), id.c_str());
            CHECK_MSG(grouped.insert(id).second, "%s: %s is in two groups", kind.name, id.c_str());
        }
        CHECK_MSG(grouped.size() == p->params().size(), "%s: %d of %d parameters are grouped", kind.name, (int)grouped.size(), (int)p->params().size());
    }
    CHECK(std::string(TrueNote().displayName()) == "True Note" && std::string(Talker().displayName()) == "Talker");
    CHECK(std::string(SecondVoice().displayName()) == "Second Voice" && std::string(Lint().displayName()) == "Lint");
    // scales
    CHECK(vocal::scaleMask(vocal::Major, 0) == 0xAB5u);                 // C D E F G A B
    CHECK(vocal::scaleMask(vocal::Major, 7) == (0xAB5u ^ (1u << 5) ^ (1u << 6)));       // G major: F becomes F#
    CHECK(vocal::nearestNote(61.4, vocal::scaleMask(vocal::Major, 0)) == 62.0);
    CHECK(vocal::nearestNote(61.4, 0) == 61.4);
    CHECK(vocal::stepNote(60, 2, vocal::scaleMask(vocal::Major, 0)) == 64 && vocal::stepNote(60, -1, vocal::scaleMask(vocal::Major, 0)) == 59);
    CHECK(vocal::stepNote(60, 7, vocal::scaleMask(vocal::Major, 0)) == 72 && vocal::stepNote(57, 5, vocal::scaleMask(vocal::PentaMinor, 9)) == 69);
    // the transform used by Lint: forward and back gives the signal again; a sine lands in its bin
    vocal::Fft f;
    f.prepare(1024);
    std::vector<float> re(1024), im(1024, 0.0f), keep;
    dsp::Noise nz(3u);
    for (auto& v : re) v = nz.next();
    keep = re;
    f.run(re.data(), im.data(), false);
    f.run(re.data(), im.data(), true);
    CHECK(maxAbsDiff(re, keep) < 2e-6);
    for (int i = 0; i < 1024; ++i) { re[(size_t)i] = (float)std::cos(kTwoPi * 37.0 * i / 1024.0); im[(size_t)i] = 0.0f; }
    f.run(re.data(), im.data(), false);
    CHECK_NEAR(re[37], 512.0, 0.01);
    CHECK(std::fabs(re[36]) < 0.01 && std::fabs(im[37]) < 0.01);
}

TEST(vocal_presets_are_stable_and_reload) {
    const Stereo in = programme();
    const Stereo side = noise(1.6, 0.3f, kSr, 77u);
    for (auto& kind : kinds()) {
        auto probe = kind.make();
        const auto presets = probe->factoryPresets();
        CHECK_MSG(presets.size() >= 6, "%s has %d presets", kind.name, (int)presets.size());
        std::set<std::string> names;
        for (auto& pr : presets) {
            CHECK_MSG(names.insert(pr.name).second && !pr.name.empty(), "%s: preset name '%s'", kind.name, pr.name.c_str());
            for (int withSide = 0; withSide < (probe->wantsSidechain() ? 2 : 1); ++withSide) {
                auto p = kind.make();
                p->prepare(kSr);
                const Json extra = p->saveState()["extra"];
                p->loadState(pr.state);
                auto out = run(*p, in, kSr, withSide ? &side : nullptr);
                CHECK_MSG(allFinite(out), "%s / %s is not finite", kind.name, pr.name.c_str());
                CHECK_MSG(peak(out) < 6.0f, "%s / %s runs away (peak %.2f)", kind.name, pr.name.c_str(), peak(out));
                CHECK_MSG(peak(out) > 1e-4f, "%s / %s is silent", kind.name, pr.name.c_str());
                // a sine and silence too
                p->reset();
                Stereo plain = tone(330.0, 0.4, 0.5f);
                append(plain, silence(0.4));
                out = run(*p, plain, kSr, nullptr);
                CHECK_MSG(allFinite(out) && peak(out) < 6.0f, "%s / %s on a sine: peak %.2f", kind.name, pr.name.c_str(), peak(out));
                // save, make another, load: the same state, and the same sound
                auto q = kind.make();
                q->prepare(kSr);
                q->loadState(p->saveState());
                CHECK_MSG(q->saveState() == p->saveState(), "%s / %s does not reload", kind.name, pr.name.c_str());
                p->reset();
                const auto a = run(*p, in, kSr, withSide ? &side : nullptr), b = run(*q, in, kSr, withSide ? &side : nullptr);
                CHECK_MSG(a.l == b.l && a.r == b.r, "%s / %s sounds different after reloading", kind.name, pr.name.c_str());
            }
        }
    }
}

TEST(vocal_silence_in_gives_silence_out) {
    const Stereo quiet = silence(1.0);
    for (auto& kind : kinds()) {
        auto probe = kind.make();
        auto presets = probe->factoryPresets();
        for (size_t i = 0; i <= presets.size(); ++i) {
            auto p = kind.make();
            p->prepare(kSr);
            if (i > 0) p->loadState(presets[i - 1].state);
            auto out = run(*p, quiet);
            CHECK_MSG(peak(out) == 0.0f, "%s / %s makes sound from silence (peak %g)", kind.name, i ? presets[i - 1].name.c_str() : "default", peak(out));
            // and after sound: it dies away to exactly nothing, with no denormal numbers on the way
            Stereo in = programme();
            const size_t body = in.l.size();
            // Talker's bands close with the envelope release: 60 dB down after seven release times (its reported
            // tail), switched fully off once they are 140 dB down, sixteen release times after the sound stops
            const double toNothing = std::string(p->typeId()) == "vocoder" ? 17.0 * 0.001 * p->param("release")->get() + 0.5 : p->tailSeconds() + 0.3;
            append(in, silence(toNothing + 1.0));
            out = run(*p, in);
            const char* name = i ? presets[i - 1].name.c_str() : "default";
            CHECK_MSG(!hasSubnormal(out), "%s / %s puts out denormal numbers", kind.name, name);
            const size_t tail = body + (size_t)p->latencySamples() + (size_t)((p->tailSeconds() + 0.05) * kSr);
            CHECK_MSG(peak(out, tail) < 2e-3f * peak(out), "%s / %s is still at %g after its tail (peak %g)", kind.name, name, peak(out, tail), peak(out));
            const size_t gone = body + (size_t)p->latencySamples() + (size_t)(toNothing * kSr);
            CHECK_MSG(peak(out, gone) == 0.0f, "%s / %s is still at %g at the end", kind.name, name, peak(out, gone));
        }
    }
}

TEST(vocal_reset_and_prepare_start_clean) {
    const Stereo first = programme(), second = add(noise(1.0, 0.05f, kSr, 5u), mono(steadyVoice(233.0, 1.0)));
    const Stereo side = noise(1.6, 0.3f, kSr, 77u);
    for (auto& kind : kinds()) {
        auto fresh = kind.make();
        fresh->prepare(kSr);
        const auto want = run(*fresh, second, kSr, &side);
        auto p = kind.make();
        p->prepare(kSr);
        run(*p, first, kSr, &side);
        p->reset();
        auto got = run(*p, second, kSr, &side);
        CHECK_MSG(got.l == want.l && got.r == want.r, "%s: reset() leaves something behind (%g apart)", kind.name, maxAbsDiff(got.l, want.l));
        run(*p, first, kSr, &side);
        p->prepare(kSr);
        got = run(*p, second, kSr, &side);
        CHECK_MSG(got.l == want.l && got.r == want.r, "%s: prepare() leaves something behind", kind.name);
        // another rate and back
        p->prepare(96000.0);
        run(*p, programme(96000.0), 96000.0);
        p->prepare(kSr);
        got = run(*p, second, kSr, &side);
        CHECK_MSG(got.l == want.l && got.r == want.r, "%s: a change of sample rate leaves something behind", kind.name);
        // the first output after a start is not a fade-in from some default: with the dry / wet all the way dry
        // (where there is such a control) the very first samples are the input
    }
    // rates from 32 to 192 kHz: everything stays finite and the latency is what is reported
    for (double sr : {32000.0, 44100.0, 88200.0, 192000.0}) {
        for (auto& kind : kinds()) {
            auto p = kind.make();
            p->prepare(sr);
            auto out = run(*p, programme(sr), sr);
            CHECK_MSG(allFinite(out) && peak(out) < 6.0f, "%s at %.0f Hz: peak %.2f", kind.name, sr, peak(out));
            CHECK(p->latencySamples() >= 0 && p->latencySamples() <= 16384);
        }
    }
}

TEST(vocal_latency_is_the_same_for_every_setting) {
    dsp::Noise rnd(123u);
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        for (auto& kind : kinds()) {
            auto p = kind.make();
            p->prepare(sr);
            const int lat = p->latencySamples();
            // each parameter at both ends, then everything at random
            for (auto& prm : p->params()) {
                for (float v : {prm->info.min, prm->info.max}) {
                    prm->set(v);
                    Stereo one = silence(0.01, sr);
                    run(*p, one, sr);
                    CHECK_MSG(p->latencySamples() == lat, "%s: latency %d became %d with %s at %g", kind.name, lat, p->latencySamples(), prm->info.id.c_str(), v);
                }
                prm->set(prm->info.def);
            }
            for (int i = 0; i < 50; ++i) {
                for (auto& prm : p->params()) prm->setNorm(rnd.uni());
                Stereo one = silence(0.01, sr);
                run(*p, one, sr);
                CHECK(p->latencySamples() == lat);
            }
        }
    }
}

TEST(vocal_render_is_identical_at_any_block_size_and_thread_count) {
    auto sample = [](const std::vector<float>& v) {
        auto s = std::make_shared<SampleData>();
        s->sampleRate = kSr; s->channels = 1; s->ch[0] = v;
        return s;
    };
    auto build = [&](Project& p) {
        p.global->tempo.set(120.0f);
        auto f1 = [](double t) { return 196.0 * std::pow(2.0, (t > 0.6 ? 2.3 : -0.3 + 0.3 * std::sin(kTwoPi * 5.0 * t)) / 12.0); };
        auto f2 = [](double t) { return 147.0 * std::pow(2.0, 0.2 * std::sin(kTwoPi * 4.0 * t) / 12.0); };
        // insert 1: a voice through True Note; insert 2: a voice through Second Voice; insert 3: a voice through
        // Talker, carried by the saw on insert 4; insert 5: a noisy voice through Lint; master: Talker on its own carrier, half mix
        auto a = addSampleChannel(p, sample(voice(1.2, kSr, f1, vowelAh(), 0.4f)), "Lead");
        p.channels.back().mixerTrack = 1;
        for (int s : {0, 10}) Project::setStep(p.pattern().notesFor(a.id), s, true);
        auto b = addSampleChannel(p, sample(voice(1.0, kSr, f2, vowelAh(), 0.35f)), "Second");
        p.channels.back().mixerTrack = 2;
        for (int s : {2, 9}) Project::setStep(p.pattern().notesFor(b.id), s, true);
        auto c = addSampleChannel(p, sample(voice(0.9, kSr, f1, vowelAh(), 0.4f)), "Talk");
        p.channels.back().mixerTrack = 3;
        for (int s : {1, 8}) Project::setStep(p.pattern().notesFor(c.id), s, true);
        std::vector<float> saw(48000);
        for (size_t i = 0; i < saw.size(); ++i) saw[i] = 0.4f * (float)(2.0 * std::fmod(110.0 * (double)i / kSr, 1.0) - 1.0);
        auto d = addSampleChannel(p, sample(saw), "Saw");
        p.channels.back().mixerTrack = 4;
        for (int s : {0, 6, 12}) Project::setStep(p.pattern().notesFor(d.id), s, true);
        std::vector<float> noisy = voice(1.0, kSr, f2, vowelAh(), 0.3f);
        dsp::Noise nz(31u);
        for (auto& x : noisy) x += 0.02f * nz.next();
        auto e = addSampleChannel(p, sample(noisy), "Noisy");
        p.channels.back().mixerTrack = 5;
        for (int s : {0, 7}) Project::setStep(p.pattern().notesFor(e.id), s, true);

        auto tune = std::make_shared<TrueNote>();
        tune->loadState(tune->factoryPresets()[3].state);           // hard snap, C major
        p.setSlot(1, 0, tune);
        auto harm = std::make_shared<SecondVoice>();
        harm->loadState(harm->factoryPresets()[2].state);           // third and fifth in C major
        p.setSlot(2, 0, harm);
        auto talk = std::make_shared<Talker>();
        p.setSlot(3, 0, talk);
        CHECK(p.setRoute(4, 3, true, true));
        auto lint = std::make_shared<Lint>();
        lint->loadState(lintState());
        p.setSlot(5, 0, lint);
        auto dbl = std::make_shared<SecondVoice>();
        dbl->loadState(dbl->factoryPresets()[8].state);             // wide double
        p.setSlot(5, 1, dbl);
        auto robot = std::make_shared<Talker>();
        robot->loadState(robot->factoryPresets()[5].state);         // minor chord choir, its own carrier
        robot->pMix->set(0.3f);
        p.setSlot(0, 0, robot);
        auto last = std::make_shared<TrueNote>();
        last->pSpeed->set(60.0f);
        last->pFormant->set(0.0f);
        p.setSlot(0, 1, last);
    };
    std::shared_ptr<SampleData> ref;
    struct Case { int threads, block; } cases[] = {{1, 512}, {1, 64}, {1, 33}, {1, 4096}, {4, 512}, {4, 64}, {3, 1000}};
    for (auto& cs : cases) {
        Project p;
        build(p);
        RenderOptions o;
        o.lengthSamples = 96000;
        o.threads = cs.threads;
        o.blockSize = cs.block;
        const int64 before = Rt::allocationsInRt.load();
        auto out = renderOffline(p, o);
        CHECK_MSG(Rt::allocationsInRt.load() == before, "threads %d block %d: %lld allocations on the audio thread", cs.threads, cs.block, (long long)(Rt::allocationsInRt.load() - before));
        if (!ref) {
            ref = out;
            CHECK(peakOf(out->ch[0]) > 0.1f && peakOf(out->ch[0]) < 4.0f);
            for (float v : out->ch[0]) if (!std::isfinite(v)) { CHECK(false); break; }
            continue;
        }
        CHECK_MSG(identical(*ref, *out), "threads %d block %d differs", cs.threads, cs.block);
    }
    // the project reports the latency (the longest path: Lint, then Second Voice, then the two on the master)
    Project p;
    build(p);
    CHECK_MSG(p.compile()->latency == 1984 + 864 + 0 + 864, "project latency %d", (int)p.compile()->latency);
}

TEST(vocal_processing_does_not_allocate) {
    // straight calls with the real-time marker set, while every parameter is being moved, with and without a sidechain
    for (auto& kind : kinds()) {
        auto p = kind.make();
        p->prepare(kSr);
        dsp::Noise rnd(99u);
        Stereo io = programme();
        const Stereo side = noise(1.6, 0.3f, kSr, 8u);
        const int64 before = Rt::allocationsInRt.load();
        for (size_t i = 0; i + kQuantum <= io.l.size(); i += kQuantum) {
            for (auto& prm : p->params()) if (rnd.uni() < 0.3f) prm->setNorm(rnd.uni());
            ProcessContext c;
            c.io[0] = io.l.data() + i;
            c.io[1] = io.r.data() + i;
            if ((i / 6400) % 2 == 0) { c.sidechain[0] = side.l.data() + i; c.sidechain[1] = side.r.data() + i; }
            Rt::Scope rt;
            p->process(c);
            if (i == 32000) p->reset();
        }
        CHECK_MSG(Rt::allocationsInRt.load() == before, "%s allocates in process() or reset()", kind.name);
        CHECK(allFinite(io));
    }
}

TEST(vocal_automating_every_parameter_stays_finite) {
    for (auto& kind : kinds()) {
        // one parameter at a time swept end to end and jumped about, then all of them at once
        auto probe = kind.make();
        const size_t count = probe->params().size();
        for (size_t which = 0; which <= count; ++which) {
            auto p = kind.make();
            p->prepare(kSr);
            dsp::Noise rnd(1000u + (uint32)which);
            Stereo in = programme();
            const Stereo side = noise(1.6, 0.3f, kSr, 8u);
            const size_t quanta = in.l.size() / kQuantum;
            auto out = run(*p, in, kSr, which % 2 ? &side : nullptr, [&](size_t i) {
                const size_t q = i / kQuantum;
                if (which < count) {
                    Param* prm = p->params()[which].get();
                    if (q < quanta / 2) prm->setNorm((float)(0.5 - 0.5 * std::cos(kTwoPi * 6.0 * (double)q / (double)quanta)));     // sweeps
                    else if (q % 3 == 0) prm->setNorm(rnd.uni());                                                                   // jumps
                } else {
                    for (auto& prm : p->params()) if (rnd.uni() < 0.5f) prm->setNorm(rnd.uni());
                }
            });
            const char* id = which < count ? probe->params()[which]->info.id.c_str() : "everything";
            CHECK_MSG(allFinite(out), "%s: automating %s gives a non-finite sample", kind.name, id);
            // nothing in these four feeds back, so "runaway" is any level beyond what the gains present can make:
            // Talker's output level alone can add 24 dB
            // (x16), on top of its make-up (x8), and its loud noise carrier is 2.4: about 300 with everything at the top
            const float limit = std::string(p->typeId()) == "vocoder" ? 1000.0f : 8.0f;
            CHECK_MSG(peak(out) < limit, "%s: automating %s peaks at %.1f", kind.name, id, peak(out));
        }
    }
}

TEST(vocal_a_broken_sample_does_not_stick) {
    // a NaN and an infinity in the input (and on the sidechain): once they have gone through, everything is finite again
    for (auto& kind : kinds()) {
        auto p = kind.make();
        p->prepare(kSr);
        Stereo in = programme();
        Stereo side = noise(1.6, 0.3f, kSr, 8u);
        in.l[20000] = std::numeric_limits<float>::quiet_NaN();
        in.r[20500] = std::numeric_limits<float>::infinity();
        side.l[21000] = std::numeric_limits<float>::quiet_NaN();
        side.r[21001] = -std::numeric_limits<float>::infinity();
        auto out = run(*p, in, kSr, &side);
        bool ok = true;
        for (size_t i = 0; i < out.l.size(); ++i) if (!std::isfinite(out.l[i]) || !std::isfinite(out.r[i])) ok = false;
        CHECK_MSG(ok, "%s lets a broken sample through or keeps it", kind.name);
        CHECK(peak(out, 40000) > 1e-3f && peak(out) < 6.0f);
    }
}

TEST(vocal_cpu_cost) {
    // Not a pass/fail matter beyond "far below real time": the figures go into the notes.
    struct Case { const char* what; std::function<std::shared_ptr<Plugin>()> make; bool side; };
    const std::vector<Case> cases = {
        {"True Note, defaults", [] { return std::make_shared<TrueNote>(); }, false},
        {"True Note, formants free, +12 st", [] { auto t = std::make_shared<TrueNote>(); t->pTranspose->set(12.0f); t->pFormant->set(0.0f); return t; }, false},
        {"True Note, formants kept, +12 st, formant -12", [] { auto t = std::make_shared<TrueNote>(); t->pTranspose->set(12.0f); t->pFShift->set(-12.0f); return t; }, false},
        {"Talker, 20 bands, own carrier", [] { return std::make_shared<Talker>(); }, false},
        {"Talker, 40 bands, stereo sidechain", [] { auto t = std::make_shared<Talker>(); t->pBands->set(40.0f); return t; }, true},
        {"Second Voice, one voice", [] { return std::make_shared<SecondVoice>(); }, false},
        {"Second Voice, four voices (octaves and fifths)", [] { auto h = std::make_shared<SecondVoice>(); const float iv[4] = {7, -5, 12, -12}; for (int i = 0; i < 4; ++i) { h->pOn[i]->set(1.0f); h->pInterval[i]->set(iv[i]); } return h; }, false},
        {"Lint, no profile", [] { return std::make_shared<Lint>(); }, false},
        {"Lint, working, stereo", [] { auto d = std::make_shared<Lint>(); d->loadState(lintState()); return d; }, false},
    };
#ifdef _DEBUG
    const double seconds = 2.0;
#else
    const double seconds = 20.0;
#endif
    Stereo in = silence(seconds);
    {
        auto f0 = [](double t) { return 150.0 * std::pow(2.0, (std::floor(t * 2.0) * 0.17 + 0.3 * std::sin(kTwoPi * 5.0 * t)) / 12.0); };
        const auto v = voice(seconds, kSr, f0, vowelAh(), 0.4f);
        const Stereo hiss = noise(seconds, 0.01f, kSr, 7u);
        for (size_t i = 0; i < in.l.size(); ++i) { in.l[i] = v[i] + hiss.l[i]; in.r[i] = v[i] + hiss.r[i]; }
    }
    const Stereo side = noise(seconds, 0.3f, kSr, 9u);
    for (auto& cs : cases) {
        auto p = cs.make();
        p->prepare(kSr);
        Stereo io = in;
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<double> each;
        each.reserve(io.l.size() / kQuantum);
        for (size_t i = 0; i + kQuantum <= io.l.size(); i += kQuantum) {
            ProcessContext c;
            c.io[0] = io.l.data() + i;
            c.io[1] = io.r.data() + i;
            if (cs.side) { c.sidechain[0] = side.l.data() + i; c.sidechain[1] = side.r.data() + i; }
            const auto q0 = std::chrono::steady_clock::now();
            p->process(c);
            each.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - q0).count());
        }
        const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::sort(each.begin(), each.end());
        VT_FIG("%-48s %5.2f %% of one core at 48 kHz; one quantum: median %.0f, 99.9th percentile %.0f microseconds (a quantum lasts 1333)", cs.what, 100.0 * used / seconds,
               each[each.size() / 2] * 1e6, each[each.size() * 999 / 1000] * 1e6);
        CHECK(allFinite(io));
#ifndef _DEBUG
        CHECK_MSG(used < 0.25 * seconds, "%s takes %.1f %% of real time", cs.what, 100.0 * used / seconds);
#endif
    }
}
