// Milestone 5: mixer routing, sends, sidechain, delay compensation, multi-core, strip controls.
#include "Util.h"
#include "pulp/Effects.h"
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0;

// A test effect that delays by N samples and reports N as its latency.
class LatentDelay : public Plugin {
public:
    explicit LatentDelay(int n) : n_(n) {}
    const char* typeId() const override { return "test-latent"; }
    const char* displayName() const override { return "Latent"; }
    void prepare(double) override { for (auto& d : d_) d.prepare(n_ + 8); }
    int latencySamples() const override { return n_; }
    void process(ProcessContext& c) override {
        for (int i = 0; i < c.numSamples; ++i)
            for (int ch = 0; ch < 2; ++ch) { d_[ch].push(c.io[ch][i]); c.io[ch][i] = d_[ch].tap(n_); }
    }
private:
    int n_;
    dsp::DelayLine d_[2];
};

std::shared_ptr<SampleData> noiseBurst(double seconds = 0.5) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = kSr;
    s->channels = 1;
    dsp::Noise nz(1234u);
    s->ch[0].resize((size_t)(seconds * kSr));
    for (auto& v : s->ch[0]) v = 0.4f * nz.next();
    return s;
}

std::shared_ptr<SampleData> render(Project& p, int64 len = 24000, int threads = 1, int block = 512) {
    RenderOptions o;
    o.lengthSamples = len;
    o.threads = threads;
    o.blockSize = block;
    return renderOffline(p, o);
}

// one channel holding the sample, one note at tick 0, routed to the given insert
Chan oneNote(Project& p, SamplePtr s, int track) {
    auto ch = addSampleChannel(p, std::move(s));
    p.channels.back().mixerTrack = track;
    Project::setStep(p.pattern().notesFor(ch.id), 0, true);
    return ch;
}
}

TEST(mixer_default_routing_is_transparent) {
    // A channel on an insert at unity must come out bit-identical to the same channel on the master.
    Project a, b;
    oneNote(a, noiseBurst(), 0);
    oneNote(b, noiseBurst(), 7);
    auto ra = render(a), rb = render(b);
    CHECK(peakOf(ra->ch[0]) > 0.2f);
    CHECK(identical(*ra, *rb));
}

TEST(mixer_fader_pan_mute_phase_swap_and_separation) {
    auto base = [] { Project p; return p; };
    auto src = sine(440, 1.0, kSr, 0.5f);
    {   // fader: 80% is unity, 100% is +5.6 dB, 40% follows the curve
        Project p = base();
        oneNote(p, src, 3);
        p.mixer[3].live->volume.set(1.0f);
        const double g = rmsOf(render(p)->ch[0], 2000, 20000) / (0.5 / std::sqrt(2.0));
        CHECK_NEAR(20.0 * std::log10(g), 5.6, 0.1);
        CHECK_NEAR(TrackLive::gainToFader(TrackLive::faderToGain(0.37f)), 0.37, 1e-4);
    }
    {   // pan hard right silences the left
        Project p = base();
        oneNote(p, src, 3);
        p.mixer[3].live->pan.set(1.0f);
        auto out = render(p);
        CHECK(peakOf(out->ch[0], 2000) < 1e-6f && peakOf(out->ch[1], 2000) > 0.4f);
    }
    {   // mute
        Project p = base();
        oneNote(p, src, 3);
        p.mixer[3].live->mute.set(1.0f);
        CHECK(peakOf(render(p)->ch[0]) == 0.0f);
    }
    {   // phase invert nulls against an un-inverted copy
        Project p = base();
        oneNote(p, src, 3);
        oneNote(p, src, 4);
        p.mixer[4].live->phase.set(1.0f);
        CHECK(peakOf(render(p)->ch[0]) < 1e-6f);
    }
    {   // swap and stereo separation: a left-only source
        Project p = base();
        auto ch = oneNote(p, src, 3);
        ch.live->pan.set(-1.0f);
        p.mixer[3].live->swap.set(1.0f);
        auto out = render(p);
        CHECK(peakOf(out->ch[0], 2000) < 1e-6f && peakOf(out->ch[1], 2000) > 0.4f);
        p.mixer[3].live->swap.set(0.0f);
        p.mixer[3].live->stereoSep.set(1.0f);       // fully merged: both sides equal
        auto mono = render(p);
        CHECK(maxAbsDiff(mono->ch[0], mono->ch[1]) < 1e-6);
        CHECK_NEAR(rmsOf(mono->ch[0], 2000, 20000), 0.25 / std::sqrt(2.0), 0.002);
    }
    {   // the strip EQ boosts where it says
        Project p = base();
        oneNote(p, sine(1000, 1.0, kSr, 0.1f), 3);
        p.mixer[3].live->eqGain[1].set(12.0f);
        const double g = rmsOf(render(p)->ch[0], 4000, 20000) / (0.1 / std::sqrt(2.0));
        CHECK_NEAR(20.0 * std::log10(g), 12.0, 0.2);
    }
}

TEST(mixer_sends_buses_and_loop_refusal) {
    auto src = sine(300, 1.0, kSr, 0.3f);
    Project p;
    oneNote(p, src, 1);
    // insert 1 also feeds bus 10 at half level; bus 10 goes to the master
    CHECK(p.setRoute(1, 10, true));
    p.mixer[1].routes.back().level->set(0.5f);
    const double both = rmsOf(render(p)->ch[0], 2000, 20000);
    CHECK_NEAR(both, 0.3 / std::sqrt(2.0) * (1.0 + 0.25), 0.002);       // send level 50% is a quarter of the gain
    // take insert 1 off the master: only the bus path remains
    CHECK(p.setRoute(1, 0, false));
    CHECK_NEAR(rmsOf(render(p)->ch[0], 2000, 20000), 0.3 / std::sqrt(2.0) * 0.25, 0.001);
    // loops and nonsense routes are refused
    CHECK(!p.canRoute(10, 1));
    CHECK(!p.setRoute(10, 1, true));
    CHECK(!p.canRoute(0, 5) && !p.canRoute(5, 5) && !p.canRoute(5, kCurrentTrack));
    CHECK(p.setRoute(10, 11, true) && p.setRoute(11, 12, true));
    CHECK(!p.canRoute(12, 1) && !p.canRoute(12, 10) && p.canRoute(12, 13));
    // routing and strip settings survive save and load, and render the same
    p.mixer[10].name = "Bus";
    p.mixer[10].live->volume.set(0.6f);
    p.mixer[10].dock = 1;
    Project q;
    CHECK(q.fromJson(p.toJson()));
    CHECK(q.toJson() == p.toJson());
    CHECK(q.mixer[1].routeTo(10) != nullptr && q.mixer[1].routeTo(0) == nullptr && q.mixer[10].name == "Bus");
    static_cast<Sampler*>(q.channels[0].live->plugin.get())->setSample(src);     // the test tone has no file to reload from
    auto rp = render(p), rq = render(q);
    CHECK(peakOf(rp->ch[0]) > 0.01f);
    CHECK(identical(*rp, *rq));
    CHECK(p.firstFreeMixerTrack() == 2);
}

TEST(delay_compensation_nulls_a_latent_parallel_path) {
    // Source on insert 1 feeds insert 2 (latent plugin) and insert 3 (dry, inverted). They must cancel.
    for (int latency : {1, 64, 240, 1000}) {
        Project p;
        oneNote(p, noiseBurst(), 1);
        p.setRoute(1, 0, false);
        p.setRoute(1, 2, true);
        p.setRoute(1, 3, true);
        p.setSlot(2, 0, std::make_shared<LatentDelay>(latency));
        p.mixer[3].live->phase.set(1.0f);
        auto out = render(p, 36000);
        CHECK_MSG(peakOf(out->ch[0]) < 1e-6f, "latency %d: residual %g", latency, peakOf(out->ch[0]));
        CHECK(p.compile()->latency == latency);
        // and the render is trimmed so the sound still starts at sample 0
        p.mixer[3].live->phase.set(0.0f);
        auto sum = render(p, 36000);
        auto ref = noiseBurst();
        double worst = 0.0;
        for (size_t i = 0; i < 20000; ++i) worst = std::max(worst, (double)std::fabs(sum->ch[0][i] - 2.0f * ref->ch[0][i]));
        CHECK_MSG(worst < 1e-5, "latency %d: aligned sum differs by %g", latency, worst);
    }
    // Without compensation they would not cancel: prove the test can fail.
    Project p;
    oneNote(p, noiseBurst(), 1);
    p.setRoute(1, 0, false);
    p.setRoute(1, 2, true);
    p.setRoute(1, 3, true);
    p.setSlot(2, 0, std::make_shared<LatentDelay>(64));
    p.mixer[3].live->phase.set(1.0f);
    auto st = p.compile();
    auto broken = std::make_shared<RenderState>(*st);
    for (auto& t : broken->tracks) for (auto& in : t.inputs) in.delay = 0;
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setState(broken);
    e.play();
    std::vector<float> l(24000), r(24000);
    float* outp[2] = {l.data(), r.data()};
    e.process(nullptr, 0, outp, 2, 24000);
    CHECK(peakOf(l) > 0.1f);
}

TEST(slot_bypass_and_mix_keep_latent_plugins_aligned) {
    Project p;
    oneNote(p, noiseBurst(), 1);
    oneNote(p, noiseBurst(), 2);
    p.setSlot(1, 0, std::make_shared<LatentDelay>(100));
    p.mixer[2].live->phase.set(1.0f);
    // fully wet, half wet and bypassed: the latent track must always null against the dry one
    for (int mode = 0; mode < 3; ++mode) {
        p.mixer[1].live->slotMix[0].set(mode == 1 ? 0.5f : 1.0f);
        p.mixer[1].live->slotBypass[0].set(mode == 2 ? 1.0f : 0.0f);
        auto out = render(p, 30000);
        CHECK_MSG(peakOf(out->ch[0]) < 1e-6f, "mode %d residual %g", mode, peakOf(out->ch[0]));
    }
}

TEST(sidechain_ducking_recipe) {
    // A steady tone on insert 2 with a compressor listening to the kick on insert 1.
    Project p;
    p.global->tempo.set(120.0f);
    auto kick = addSampleChannel(p, drums::makeKick({}, kSr), "Kick");
    p.channels.back().mixerTrack = 1;
    for (int s : {0, 8}) Project::setStep(p.pattern().notesFor(kick.id), s, true);
    auto pad = addSampleChannel(p, sine(220, 4.0, kSr, 0.3f), "Pad");
    p.channels.back().mixerTrack = 2;
    Note n; n.length = 16 * kTicksPerStep;
    p.pattern().notesFor(pad.id).push_back(n);
    kick.live->volume.set(0.0f);        // listen to the pad alone... but the kick must still reach the sidechain
    // (channel volume 0 would silence the sidechain too, so mute the kick's path to the master instead)
    kick.live->volume.set(0.78f);
    p.setRoute(1, 0, false);
    CHECK(p.setRoute(1, 2, true, true));        // sidechain only
    auto comp = std::make_shared<Squeeze>();
    comp->pThresh->set(-30.0f); comp->pRatio->set(20.0f); comp->pAttack->set(1.0f); comp->pRelease->set(120.0f); comp->pKnee->set(0.0f);
    comp->pSide->set(1.0f);
    p.setSlot(2, 0, comp);
    auto out = render(p, 48000);
    const double duringKick = rmsOf(out->ch[0], 600, 3000);
    const double between = rmsOf(out->ch[0], 38000, 46000);        // well after the kick has died away
    CHECK_MSG(duringKick < between * 0.35, "ducked %.4f, open %.4f", duringKick, between);
    CHECK_NEAR(between, 0.3 / std::sqrt(2.0), 0.01);
    // with the sidechain switch off the pad is steady
    comp->pSide->set(0.0f);
    comp->pThresh->set(0.0f);
    auto flat = render(p, 48000);
    CHECK_NEAR(rmsOf(flat->ch[0], 600, 3000), rmsOf(flat->ch[0], 38000, 46000), 0.004);
}

TEST(mixer_render_is_identical_across_threads_and_block_sizes) {
    auto build = [](Project& p) {
        p.global->tempo.set(128.0f);
        for (int i = 0; i < 12; ++i) {
            auto ch = addSampleChannel(p, i % 3 == 0 ? drums::makeKick({}, kSr) : sine(110.0 * (i + 1), 0.4, 44100.0, 0.1f));
            p.channels.back().mixerTrack = 1 + i % 6;
            for (int s = i % 4; s < 16; s += 3 + i % 3) Project::setStep(p.pattern().notesFor(ch.id), s, true);
            ch.live->pan.set((float)(i % 5 - 2) / 3.0f);
        }
        p.setRoute(1, 20, true); p.setRoute(2, 20, true); p.setRoute(3, 21, true); p.setRoute(20, 21, true);
        p.mixer[1].routes[1].level->set(0.4f);
        p.setSlot(20, 0, std::make_shared<Chamber>());
        p.setSlot(21, 0, std::make_shared<Repeater>());
        p.setSlot(4, 0, std::make_shared<Grit>());             // latent: forces delay compensation everywhere
        p.setSlot(5, 0, std::make_shared<Squeeze>());
        p.setSlot(0, 0, std::make_shared<ContourEq>());
        p.setSlot(0, 1, std::make_shared<Lid>());
        p.mixer[0].slots[0]->param("b3gain")->set(4.0f);
        p.mixer[2].live->eqGain[0].set(5.0f);
    };
    std::shared_ptr<SampleData> ref;
    struct Case { int threads, block; } cases[] = {{1, 512}, {1, 37}, {4, 512}, {8, 64}, {3, 1000}};
    for (auto& c : cases) {
        Project p;
        build(p);
        const int64 before = Rt::allocationsInRt.load();
        auto out = render(p, 48000, c.threads, c.block);
        CHECK(Rt::allocationsInRt.load() == before);
        if (!ref) { ref = out; CHECK(peakOf(out->ch[0]) > 0.05f); continue; }
        CHECK_MSG(identical(*ref, *out), "threads %d block %d differs", c.threads, c.block);
    }
}

TEST(current_track_follows_the_selected_track) {
    Project p;
    oneNote(p, sine(500, 1.0, kSr, 0.3f), 4);
    p.selectedMixerTrack = 4;
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setState(p.compile());
    e.play();
    std::vector<float> l(9600), r(9600);
    float* outp[2] = {l.data(), r.data()};
    e.process(nullptr, 0, outp, 2, 9600);
    CHECK(p.mixer[kCurrentTrack].live->peakL.load() > 0.2f);
    CHECK(p.mixer[4].live->peakL.load() > 0.2f);
    CHECK(p.mixer[9].live->peakL.load() == 0.0f);
    // the current track is a listening post: it adds nothing to the master
    CHECK_NEAR(rmsOf(l, 2000, 9000), 0.3 / std::sqrt(2.0), 0.002);
}
