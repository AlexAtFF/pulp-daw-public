// Taps: multitap delay. Up to eight taps read one stereo delay line; each has its own time, level,
// pan, tone and feedback, and the longest tap carries the global feedback so the whole pattern repeats.
#include "pulp/Space.h"
#include <cstdio>

namespace pulp {

namespace {
constexpr int kGrids = 7;
const int kGridTicks[kGrids] = {120, 160, 240, 320, 480, 640, 960};
constexpr double kMaxSeconds = 8.0;
constexpr float kMaxFeedback = 0.95f;
}

Taps::Taps() {
    pCount = add({"taps", "Taps in use", 1, (float)kTaps, 4, 1, kTaps, "", {}});
    pSync = addBool("sync", "Tempo sync", true);
    pGrid = addChoice("grid", "Step size", {"1/32", "1/16 triplet", "1/16", "1/8 triplet", "1/8", "1/4 triplet", "1/4"}, 2);
    pSwing = addFloat("swing", "Swing (odd steps late)", 0, 1, 0, "%");
    pFeedback = addFloat("feedback", "Feedback (longest tap)", 0, kMaxFeedback, 0.2f, "%");
    pLowCut = addFloat("lowcut", "Feedback low cut", 20, 2000, 100, "Hz", 2.0f);
    pHighCut = addFloat("highcut", "Feedback high cut", 500, 20000, 8000, "Hz", 2.0f);
    pSpread = addFloat("spread", "Pan spread", 0, 1, 1, "%");
    pMix = addFloat("mix", "Dry / wet", 0, 1, 0.3f, "%");
    static const float steps[kTaps] = {3, 6, 9, 12, 15, 18, 21, 24};
    static const float level[kTaps] = {0.8f, 0.62f, 0.48f, 0.36f, 0.28f, 0.22f, 0.17f, 0.13f};
    static const float pan[kTaps] = {-0.6f, 0.6f, -0.3f, 0.3f, -0.8f, 0.8f, -0.45f, 0.45f};
    for (int i = 0; i < kTaps; ++i) {
        char id[32], name[64];
        snprintf(id, sizeof id, "t%dsteps", i + 1); snprintf(name, sizeof name, "Tap %d steps (sync on)", i + 1);
        pSteps[i] = add({id, name, 1, 32, steps[i], 1, 32, "", {}});
        snprintf(id, sizeof id, "t%dtime", i + 1); snprintf(name, sizeof name, "Tap %d time (sync off)", i + 1);
        pTime[i] = addFloat(id, name, 1, 4000, 125.0f * (float)(i + 1), "ms", 2.0f);
        snprintf(id, sizeof id, "t%dlevel", i + 1); snprintf(name, sizeof name, "Tap %d level", i + 1);
        pLevel[i] = addFloat(id, name, 0, 1, level[i], "%");
        snprintf(id, sizeof id, "t%dpan", i + 1); snprintf(name, sizeof name, "Tap %d pan", i + 1);
        pPan[i] = addFloat(id, name, -1, 1, pan[i]);
        snprintf(id, sizeof id, "t%dfb", i + 1); snprintf(name, sizeof name, "Tap %d feedback", i + 1);
        pTapFb[i] = addFloat(id, name, 0, kMaxFeedback, 0, "%");
        snprintf(id, sizeof id, "t%dtone", i + 1); snprintf(name, sizeof name, "Tap %d tone (dark / thin)", i + 1);
        pTone[i] = addFloat(id, name, -1, 1, 0);
    }
}

double Taps::tapSamples(int tap, double tempo) const {
    tap = clampT(tap, 0, kTaps - 1);
    if (!pSync->getBool()) return pTime[tap]->get() * 0.001 * sr_;
    const int steps = pSteps[tap]->getInt();
    const double grid = kGridTicks[clampT(pGrid->getInt(), 0, kGrids - 1)];
    double ticks = steps * grid;
    if (steps & 1) ticks += pSwing->get() * grid / 3.0;         // full swing puts the off-steps on the triplet
    return ticks * 60.0 * sr_ / (std::max(1.0, tempo) * kPPQ);
}

void Taps::prepare(double sr) {
    sr_ = sr;
    for (auto& d : d_) d.prepare((int)(sr * kMaxSeconds) + 64);
    reset();
}

void Taps::reset() {
    for (auto& d : d_) d.clear();
    for (int c = 0; c < 2; ++c) { lp_[c].reset(); hp_[c].reset(); }
    for (auto& t : tone_) { t[0].reset(); t[1].reset(); }
    primed_ = false;
    prevCount_ = 0;
}

double Taps::tailSeconds() const {
    const double tempo = lastTempo_.load(std::memory_order_relaxed);
    const int count = pCount->getInt();
    double longest = 0.0, total = pFeedback->get();
    for (int i = 0; i < count; ++i) { longest = std::max(longest, tapSamples(i, tempo) / sr_); total += pTapFb[i]->get(); }
    longest = std::min(longest, kMaxSeconds);
    total = std::min(total, (double)kMaxFeedback);
    const double repeats = total > 0.001 ? std::log(0.001) / std::log(total) : 0.0;
    return std::min(60.0, longest * (1.0 + repeats));
}

void Taps::process(ProcessContext& c) {
    const int n = c.numSamples;
    const double tempo = std::max(1.0, c.tempo);
    lastTempo_.store((float)tempo, std::memory_order_relaxed);
    const int count = clampT(pCount->getInt(), 1, (int)kTaps);
    const double maxDelay = std::min((double)(d_[0].mask - 8), kMaxSeconds * sr_);
    const float spread = pSpread->get();
    const int fadeLen = std::max(8, (int)(0.02 * sr_));

    // tap times, rounded to whole samples: a tap is an exact copy, however often it goes round
    int target[kTaps];
    int longest = 0;
    for (int i = 0; i < kTaps; ++i) {
        target[i] = (int)std::llround(clampT(tapSamples(i, tempo), 1.0, maxDelay));
        if (i < count && target[i] > target[longest]) longest = i;
    }
    // feedback: the longest tap carries the global amount; the sum is held below one
    float fbT[kTaps], total = pFeedback->get();
    for (int i = 0; i < kTaps; ++i) { fbT[i] = i < count ? pTapFb[i]->get() : 0.0f; total += fbT[i]; }
    fbT[longest] += pFeedback->get();
    if (total > kMaxFeedback) for (float& f : fbT) f *= kMaxFeedback / total;

    float tone[kTaps];
    const float fadeStep = 1.0f / (float)fadeLen;
    const int live = std::max(count, prevCount_);
    for (int i = 0; i < kTaps; ++i) {
        // pan: at the centre the tap keeps its stereo image; towards one side the other side's
        // signal is folded in rather than thrown away. A mono signal follows the mixer's pan law.
        const float p = clampT(pPan[i]->get() * spread, -1.0f, 1.0f), q = std::fabs(p);
        const float lv = i < count ? pLevel[i]->get() : 0.0f;
        const float keep = lv / (1.0f + q), fold = lv * q / (1.0f + q), thin = lv * (1.0f - q);
        const float ll = p >= 0.0f ? thin : keep, rr = p >= 0.0f ? keep : thin;
        const float lr = p >= 0.0f ? fold : 0.0f, rl = p >= 0.0f ? 0.0f : fold;
        if (!primed_ || i >= prevCount_) {
            posA_[i] = target[i];
            fadeLeft_[i] = 0;
            if (!primed_) { gLL_[i].snap(ll); gRR_[i].snap(rr); gLR_[i].snap(lr); gRL_[i].snap(rl); fb_[i].snap(fbT[i]); }
            tone_[i][0].reset(); tone_[i][1].reset();
        }
        gLL_[i].target(ll, n); gRR_[i].target(rr, n); gLR_[i].target(lr, n); gRL_[i].target(rl, n);
        fb_[i].target(fbT[i], n);
        if (fadeLeft_[i] == 0 && posA_[i] != target[i]) { posB_[i] = target[i]; fadeLeft_[i] = fadeLen; }      // the tap moves
        tone[i] = pTone[i]->get();
        if (tone[i] < 0.0f) { const double hz = 20000.0 * std::pow(0.01, (double)-tone[i]); tone_[i][0].setHz(hz, sr_); tone_[i][1].a = tone_[i][0].a; }
        else if (tone[i] > 0.0f) { const double hz = 20.0 * std::pow(250.0, (double)tone[i]); tone_[i][0].setHz(hz, sr_); tone_[i][1].a = tone_[i][0].a; }
    }
    for (int ch = 0; ch < 2; ++ch) { lp_[ch].setHz(pHighCut->get(), sr_); hp_[ch].setHz(pLowCut->get(), sr_); }
    if (!primed_) { mix_.snap(pMix->get()); primed_ = true; }
    mix_.target(pMix->get(), n);

    for (int s = 0; s < n; ++s) {
        const float inL = c.io[0][s], inR = c.io[1][s];
        float outL = 0.0f, outR = 0.0f, fbL = 0.0f, fbR = 0.0f;
        for (int i = 0; i < live; ++i) {
            // tap(d - 1) is the sample pushed d samples ago
            float tl = d_[0].tap(posA_[i] - 1), tr = d_[1].tap(posA_[i] - 1);
            if (fadeLeft_[i] > 0) {     // crossfade from the old time to the new one: no pitch sweep, no click
                const float t = (float)(fadeLen - fadeLeft_[i] + 1) * fadeStep;
                tl += (d_[0].tap(posB_[i] - 1) - tl) * t;
                tr += (d_[1].tap(posB_[i] - 1) - tr) * t;
                if (--fadeLeft_[i] == 0) posA_[i] = posB_[i];
            }
            if (tone[i] < 0.0f) { tl = tone_[i][0].lp(tl); tr = tone_[i][1].lp(tr); }
            else if (tone[i] > 0.0f) { tl = tone_[i][0].hp(tl); tr = tone_[i][1].hp(tr); }
            outL += gLL_[i].next() * tl + gRL_[i].next() * tr;
            outR += gRR_[i].next() * tr + gLR_[i].next() * tl;
            const float f = fb_[i].next();
            fbL += f * tl;
            fbR += f * tr;
        }
        fbL = hp_[0].hp(lp_[0].lp(fbL));
        fbR = hp_[1].hp(lp_[1].lp(fbR));
        flushDenormal(fbL);
        flushDenormal(fbR);
        d_[0].push(inL + fbL);
        d_[1].push(inR + fbR);
        float dg, wg;
        space::mixGains(mix_.next(), dg, wg);
        c.io[0][s] = inL * dg + outL * wg;
        c.io[1][s] = inR * dg + outR * wg;
    }
    for (int ch = 0; ch < 2; ++ch) { flushDenormal(lp_[ch].z); flushDenormal(hp_[ch].z); }
    for (int i = 0; i < live; ++i) { flushDenormal(tone_[i][0].z); flushDenormal(tone_[i][1].z); }
    prevCount_ = count;
}

std::vector<Preset> Taps::factoryPresets() const {
    return buildPresets(*this, {
        {"Dotted eighths", {}},
        {"Swung eighths", {{"taps", 4}, {"grid", 4}, {"swing", 1.0f}, {"t1steps", 1}, {"t2steps", 2}, {"t3steps", 3}, {"t4steps", 4},
                           {"t1level", 0.7f}, {"t2level", 0.6f}, {"t3level", 0.42f}, {"t4level", 0.36f},
                           {"t1pan", -0.5f}, {"t2pan", 0.5f}, {"t3pan", -0.25f}, {"t4pan", 0.25f}, {"feedback", 0.25f}}},
        {"Gallop", {{"taps", 6}, {"grid", 2}, {"t1steps", 2}, {"t2steps", 3}, {"t3steps", 4}, {"t4steps", 6}, {"t5steps", 7}, {"t6steps", 8},
                    {"t1level", 0.75f}, {"t2level", 0.45f}, {"t3level", 0.6f}, {"t4level", 0.5f}, {"t5level", 0.3f}, {"t6level", 0.4f},
                    {"t1pan", -0.4f}, {"t2pan", 0.2f}, {"t3pan", 0.5f}, {"t4pan", -0.5f}, {"t5pan", -0.2f}, {"t6pan", 0.4f},
                    {"feedback", 0.15f}, {"highcut", 6000}}},
        {"Ping-pong spread", {{"taps", 6}, {"grid", 4}, {"t1steps", 1}, {"t2steps", 2}, {"t3steps", 3}, {"t4steps", 4}, {"t5steps", 5}, {"t6steps", 6},
                              {"t1level", 0.8f}, {"t2level", 0.66f}, {"t3level", 0.54f}, {"t4level", 0.44f}, {"t5level", 0.36f}, {"t6level", 0.3f},
                              {"t1pan", -1.0f}, {"t2pan", 1.0f}, {"t3pan", -0.75f}, {"t4pan", 0.75f}, {"t5pan", -0.5f}, {"t6pan", 0.5f},
                              {"feedback", 0.3f}}},
        {"Slapback cluster", {{"taps", 4}, {"sync", 0}, {"t1time", 62}, {"t2time", 87}, {"t3time", 104}, {"t4time", 131},
                              {"t1level", 0.7f}, {"t2level", 0.5f}, {"t3level", 0.42f}, {"t4level", 0.32f},
                              {"t1pan", -0.4f}, {"t2pan", 0.5f}, {"t3pan", -0.8f}, {"t4pan", 0.8f}, {"feedback", 0.0f}, {"mix", 0.35f}}},
        {"Triplet cascade", {{"taps", 6}, {"grid", 3}, {"t1steps", 1}, {"t2steps", 2}, {"t3steps", 3}, {"t4steps", 4}, {"t5steps", 5}, {"t6steps", 6},
                             {"t1level", 0.8f}, {"t2level", 0.66f}, {"t3level", 0.55f}, {"t4level", 0.45f}, {"t5level", 0.37f}, {"t6level", 0.3f},
                             {"t1tone", -0.1f}, {"t2tone", -0.2f}, {"t3tone", -0.3f}, {"t4tone", -0.4f}, {"t5tone", -0.5f}, {"t6tone", -0.6f},
                             {"t1pan", -0.2f}, {"t2pan", 0.3f}, {"t3pan", -0.45f}, {"t4pan", 0.6f}, {"t5pan", -0.75f}, {"t6pan", 0.9f},
                             {"feedback", 0.1f}}},
        {"Quarter-note build", {{"taps", 4}, {"grid", 6}, {"t1steps", 1}, {"t2steps", 2}, {"t3steps", 3}, {"t4steps", 4},
                                {"t1level", 0.22f}, {"t2level", 0.36f}, {"t3level", 0.52f}, {"t4level", 0.72f},
                                {"t1pan", -0.7f}, {"t2pan", 0.7f}, {"t3pan", -0.35f}, {"t4pan", 0.0f}, {"feedback", 0.0f}}},
        {"Sixteenth roll", {{"taps", 8}, {"grid", 2}, {"t1steps", 1}, {"t2steps", 2}, {"t3steps", 3}, {"t4steps", 4}, {"t5steps", 5}, {"t6steps", 6}, {"t7steps", 7}, {"t8steps", 8},
                            {"t1level", 0.7f}, {"t2level", 0.6f}, {"t3level", 0.52f}, {"t4level", 0.45f}, {"t5level", 0.38f}, {"t6level", 0.32f}, {"t7level", 0.27f}, {"t8level", 0.22f},
                            {"t1tone", 0.1f}, {"t2tone", 0.15f}, {"t3tone", 0.2f}, {"t4tone", 0.25f}, {"t5tone", 0.3f}, {"t6tone", 0.35f}, {"t7tone", 0.4f}, {"t8tone", 0.45f},
                            {"feedback", 0.0f}, {"mix", 0.25f}}},
        {"Wide doubler", {{"taps", 4}, {"sync", 0}, {"t1time", 11}, {"t2time", 17}, {"t3time", 23}, {"t4time", 29},
                          {"t1level", 0.7f}, {"t2level", 0.7f}, {"t3level", 0.5f}, {"t4level", 0.5f},
                          {"t1pan", -1.0f}, {"t2pan", 1.0f}, {"t3pan", -0.6f}, {"t4pan", 0.6f}, {"feedback", 0.0f}, {"mix", 0.4f}}},
        {"Dub scatter", {{"taps", 4}, {"grid", 2}, {"t1steps", 3}, {"t2steps", 7}, {"t3steps", 10}, {"t4steps", 14},
                         {"t1level", 0.7f}, {"t2level", 0.55f}, {"t3level", 0.5f}, {"t4level", 0.4f},
                         {"t1pan", -0.7f}, {"t2pan", 0.4f}, {"t3pan", 0.8f}, {"t4pan", -0.3f}, {"t2fb", 0.2f},
                         {"feedback", 0.5f}, {"highcut", 3000}, {"lowcut", 300}, {"mix", 0.35f}}},
    });
}

std::vector<ParamGroup> Taps::paramGroups() const {
    std::vector<ParamGroup> g;
    g.push_back({"Pattern", {"taps", "sync", "grid", "swing", "spread"}});
    g.push_back({"Feedback", {"feedback", "lowcut", "highcut"}});
    for (int i = 0; i < kTaps; ++i) {
        const std::string t = "t" + std::to_string(i + 1);
        g.push_back({"Tap " + std::to_string(i + 1), {t + "steps", t + "time", t + "level", t + "pan", t + "fb", t + "tone"}});
    }
    g.push_back({"Output", {"mix"}});
    return g;
}

} // namespace pulp
