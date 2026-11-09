// Chamber (reverb), Repeater (delay) and Shimmer (chorus).
#include "pulp/Effects.h"

namespace pulp {

// Dry/wet law shared by the time-based effects: both at full level in the middle,
// dry only at 0, wet only at 1.
static inline void mixGains(float mix, float& dry, float& wet) {
    dry = mix <= 0.5f ? 1.0f : 2.0f * (1.0f - mix);
    wet = mix >= 0.5f ? 1.0f : 2.0f * mix;
}

// ------------------------------------------------------------------ Chamber

Chamber::Chamber() {
    pSize = addFloat("size", "Room size", 0, 1, 0.5f, "%");
    pDecay = addFloat("decay", "Decay time", 0.2f, 12, 1.8f, "s", 2.0f);
    pDamp = addFloat("damp", "High damping", 0, 1, 0.4f, "%");
    pPre = addFloat("predelay", "Pre-delay", 0, 200, 15, "ms");
    pEarly = addFloat("early", "Early / late balance", 0, 1, 0.3f, "%");
    pWidth = addFloat("width", "Stereo width", 0, 1, 1, "%");
    pLowCut = addFloat("lowcut", "Low cut", 20, 1000, 120, "Hz", 2.0f);
    pMix = addFloat("mix", "Dry / wet", 0, 1, 0.22f, "%");
}

void Chamber::prepare(double sr) {
    sr_ = sr;
    const int maxLen = (int)(0.12 * sr) + 64;       // longest comb at the largest room size
    for (int c = 0; c < 2; ++c) {
        for (auto& cb : comb_[c]) cb.d.prepare(maxLen);
        for (auto& a : ap_[c]) a.d.prepare((int)(0.03 * sr) + 64);
        pre_[c].prepare((int)(0.36 * sr) + 64);
    }
    lastSize_ = -1.0f;
    reset();
}

void Chamber::reset() {
    for (int c = 0; c < 2; ++c) {
        for (auto& cb : comb_[c]) { cb.d.clear(); cb.lp = 0.0f; }
        for (auto& a : ap_[c]) a.d.clear();
        pre_[c].clear();
        hp_[c].reset();
    }
    primed_ = false;
}

void Chamber::process(ProcessContext& c) {
    static const int combLen[kCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int apLen[kAllpass] = {556, 441, 341, 225};
    static const float tapMs[kTaps] = {7.0f, 11.3f, 17.9f, 23.1f, 31.7f, 41.3f};
    static const float tapGain[kTaps] = {0.8f, 0.65f, 0.55f, 0.42f, 0.33f, 0.25f};
    const float size = pSize->get();
    const float scale = (float)(sr_ / 44100.0) * (0.55f + size * 1.1f);
    if (size != lastSize_) {
        lastSize_ = size;
        for (int ch = 0; ch < 2; ++ch) {
            for (int i = 0; i < kCombs; ++i) comb_[ch][i].len = std::max(8, (int)((combLen[i] + (ch ? 23 : 0)) * scale));
            for (int i = 0; i < kAllpass; ++i) ap_[ch][i].len = std::max(4, (int)((apLen[i] + (ch ? 23 : 0)) * (float)(sr_ / 44100.0)));
        }
    }
    const float decay = pDecay->get();
    const float damp = pDamp->get() * 0.7f;
    for (int ch = 0; ch < 2; ++ch)
        for (auto& cb : comb_[ch]) cb.fb = (float)std::pow(10.0, -3.0 * cb.len / (decay * sr_));
    const int pre = (int)(pPre->get() * 0.001 * sr_);
    const float early = pEarly->get(), width = pWidth->get();
    hp_[0].setHz(pLowCut->get(), sr_);
    hp_[1].a = hp_[0].a;
    if (!primed_) { mix_.snap(pMix->get()); primed_ = true; }
    mix_.target(pMix->get(), c.numSamples);
    int tapSamp[kTaps];
    for (int t = 0; t < kTaps; ++t) tapSamp[t] = (int)(tapMs[t] * (0.6f + size) * 0.001 * sr_);

    for (int i = 0; i < c.numSamples; ++i) {
        const float inL = c.io[0][i], inR = c.io[1][i];
        pre_[0].push(inL);
        pre_[1].push(inR);
        const float pl = pre_[0].tap(pre), pr = pre_[1].tap(pre);
        // early reflections: a handful of taps, alternating sides
        float eL = 0.0f, eR = 0.0f;
        for (int t = 0; t < kTaps; ++t) {
            const float a = pre_[t & 1].tap(pre + tapSamp[t]) * tapGain[t];
            if (t & 1) eR += a; else eL += a;
        }
        // late field: parallel damped combs into series all-passes
        const float feed = (pl + pr) * 0.015f;
        float wet[2];
        for (int ch = 0; ch < 2; ++ch) {
            float acc = 0.0f;
            for (auto& cb : comb_[ch]) {
                const float y = cb.d.tap(cb.len - 1);
                cb.lp = y * (1.0f - damp) + cb.lp * damp;
                flushDenormal(cb.lp);
                cb.d.push(feed + cb.lp * cb.fb);
                acc += y;
            }
            for (auto& a : ap_[ch]) {
                const float delayed = a.d.tap(a.len - 1);
                const float y = delayed - 0.5f * acc;
                a.d.push(acc + 0.5f * y);
                acc = y;
            }
            wet[ch] = acc * 3.0f;
        }
        const float w1 = 0.5f + 0.5f * width, w2 = 0.5f - 0.5f * width;
        float wl = (wet[0] * w1 + wet[1] * w2) * (1.0f - early) + eL * early * 0.6f;
        float wr = (wet[1] * w1 + wet[0] * w2) * (1.0f - early) + eR * early * 0.6f;
        wl = hp_[0].hp(wl);
        wr = hp_[1].hp(wr);
        float dg, wg;
        mixGains(mix_.next(), dg, wg);
        c.io[0][i] = inL * dg + wl * wg;
        c.io[1][i] = inR * dg + wr * wg;
    }
}

std::vector<Preset> Chamber::factoryPresets() const {
    return buildPresets(*this, {
        {"Room", {}},
        {"Small booth", {{"size", 0.15f}, {"decay", 0.5f}, {"predelay", 3}, {"early", 0.6f}, {"mix", 0.18f}}},
        {"Vocal plate", {{"size", 0.45f}, {"decay", 1.6f}, {"damp", 0.25f}, {"predelay", 28}, {"early", 0.15f}, {"lowcut", 250}, {"mix", 0.2f}}},
        {"Big hall", {{"size", 0.9f}, {"decay", 4.5f}, {"damp", 0.5f}, {"predelay", 35}, {"early", 0.2f}, {"mix", 0.28f}}},
        {"Dark cave", {{"size", 1.0f}, {"decay", 7.5f}, {"damp", 0.85f}, {"predelay", 60}, {"early", 0.1f}, {"mix", 0.35f}}},
        {"Drum ambience", {{"size", 0.3f}, {"decay", 0.8f}, {"damp", 0.3f}, {"predelay", 8}, {"early", 0.5f}, {"lowcut", 300}, {"mix", 0.15f}}},
        {"Wet send", {{"size", 0.6f}, {"decay", 2.4f}, {"mix", 1.0f}}},
    });
}

// ------------------------------------------------------------------ Repeater

Repeater::Repeater() {
    pSync = addChoice("sync", "Tempo sync", {"Off", "1/16", "1/8 triplet", "1/8", "1/8 dotted", "1/4 triplet", "1/4", "1/4 dotted", "1/2", "1 bar"}, 3);
    pTime = addFloat("time", "Time (sync off)", 1, 2000, 300, "ms", 2.0f);
    pFeedback = addFloat("feedback", "Feedback", 0, 0.95f, 0.4f, "%");
    pPing = addBool("ping", "Ping-pong", false);
    pLowCut = addFloat("lowcut", "Feedback low cut", 20, 2000, 120, "Hz", 2.0f);
    pHighCut = addFloat("highcut", "Feedback high cut", 500, 20000, 7000, "Hz", 2.0f);
    pModDepth = addFloat("moddepth", "Modulation depth", 0, 10, 0, "ms");
    pModRate = addFloat("modrate", "Modulation rate", 0.05f, 8, 0.5f, "Hz", 2.0f);
    pDrive = addFloat("drive", "Feedback saturation", 0, 1, 0, "%");
    pOffset = addFloat("offset", "Right offset", -50, 50, 0, "ms");
    pMix = addFloat("mix", "Dry / wet", 0, 1, 0.25f, "%");
}

double Repeater::delaySamples(double tempo) const {
    static const int ticks[10] = {0, 240, 320, 480, 720, 640, 960, 1440, 1920, 3840};
    const int s = pSync->getInt();
    if (s == 0) return pTime->get() * 0.001 * sr_;
    return ticks[s] * 60.0 * sr_ / (tempo * kPPQ);
}

void Repeater::prepare(double sr) {
    sr_ = sr;
    const int max = (int)(sr * 4.0) + 64;       // four seconds: a bar at 60 BPM; longer synced times are capped
    for (auto& d : d_) d.prepare(max);
    reset();
}

void Repeater::reset() {
    for (auto& d : d_) d.clear();
    for (int c = 0; c < 2; ++c) { lp_[c].reset(); hp_[c].reset(); }
    primed_ = false;
    phase_ = 0.0;
}

void Repeater::process(ProcessContext& c) {
    const float maxDelay = (float)(d_[0].mask - 8);
    const float target0 = clampT((float)delaySamples(c.tempo), 1.0f, maxDelay);
    const float target1 = clampT(target0 + (float)(pOffset->get() * 0.001 * sr_), 1.0f, maxDelay);
    if (!primed_) { cur_[0] = target0; cur_[1] = target1; mix_.snap(pMix->get()); primed_ = true; }
    const float fb = pFeedback->get();
    const bool ping = pPing->getBool();
    const float depth = (float)(pModDepth->get() * 0.001 * sr_);
    const double inc = pModRate->get() / sr_;
    const float drive = pDrive->get();
    for (int ch = 0; ch < 2; ++ch) { lp_[ch].setHz(pHighCut->get(), sr_); hp_[ch].setHz(pLowCut->get(), sr_); }
    mix_.target(pMix->get(), c.numSamples);
    const bool steady = cur_[0] == target0 && cur_[1] == target1;

    for (int i = 0; i < c.numSamples; ++i) {
        if (!steady) {      // glide to a new time, like turning a tape delay's speed knob
            cur_[0] += (target0 - cur_[0]) * 0.0015f;
            cur_[1] += (target1 - cur_[1]) * 0.0015f;
            if (std::fabs(cur_[0] - target0) < 0.01f) cur_[0] = target0;
            if (std::fabs(cur_[1] - target1) < 0.01f) cur_[1] = target1;
        }
        float mod = 0.0f;
        if (depth > 0.0f) { mod = depth * (0.5f + 0.5f * (float)std::sin(kTwoPi * phase_)); phase_ += inc; if (phase_ >= 1.0) phase_ -= 1.0; }
        const float inL = c.io[0][i], inR = c.io[1][i];
        // tapFrac(d - 1) is the sample pushed d samples ago
        float outL = d_[0].tapFrac(std::max(0.0f, cur_[0] - 1.0f + mod));
        float outR = d_[1].tapFrac(std::max(0.0f, cur_[1] - 1.0f + mod));
        auto shape = [&](float v, int ch) {
            v = hp_[ch].hp(lp_[ch].lp(v));
            if (drive > 0.0f) { const float g = 1.0f + drive * 5.0f; v = std::tanh(v * g) / g * (1.0f + drive); }
            return v;
        };
        const float fL = shape(outL, 0) * fb, fR = shape(outR, 1) * fb;
        if (ping) {
            d_[0].push(0.5f * (inL + inR) + fR);
            d_[1].push(fL);
        } else {
            d_[0].push(inL + fL);
            d_[1].push(inR + fR);
        }
        float dg, wg;
        mixGains(mix_.next(), dg, wg);
        c.io[0][i] = inL * dg + outL * wg;
        c.io[1][i] = inR * dg + outR * wg;
    }
}

std::vector<Preset> Repeater::factoryPresets() const {
    return buildPresets(*this, {
        {"Eighth echo", {}},
        {"Slapback", {{"sync", 0}, {"time", 95}, {"feedback", 0.1f}, {"mix", 0.3f}}},
        {"Dotted eighth", {{"sync", 4}, {"feedback", 0.45f}, {"highcut", 5000}, {"mix", 0.3f}}},
        {"Ping-pong quarter", {{"sync", 6}, {"ping", 1}, {"feedback", 0.5f}, {"mix", 0.3f}}},
        {"Tape wobble", {{"sync", 3}, {"feedback", 0.55f}, {"highcut", 3200}, {"lowcut", 250}, {"moddepth", 2.5f}, {"modrate", 0.7f}, {"drive", 0.4f}}},
        {"Wide doubler", {{"sync", 0}, {"time", 18}, {"offset", 14}, {"feedback", 0}, {"mix", 0.5f}}},
        {"Dub throw", {{"sync", 7}, {"feedback", 0.78f}, {"highcut", 2500}, {"lowcut", 400}, {"drive", 0.6f}, {"mix", 0.4f}}},
    });
}

// ------------------------------------------------------------------ Shimmer

Shimmer::Shimmer() {
    pRate = addFloat("rate", "Rate", 0.05f, 8, 0.6f, "Hz", 2.0f);
    pDepth = addFloat("depth", "Depth", 0, 10, 3, "ms");
    pDelay = addFloat("delay", "Delay", 2, 30, 12, "ms");
    pFeedback = addFloat("feedback", "Feedback", 0, 0.8f, 0, "%");
    pVoices = add({"voices", "Voices", 1, 3, 2, 1, 3, "", {}});
    pSpread = addFloat("spread", "Stereo spread", 0, 1, 0.8f, "%");
    pMix = addFloat("mix", "Dry / wet", 0, 1, 0.5f, "%");
}

void Shimmer::prepare(double sr) {
    sr_ = sr;
    for (auto& d : d_) d.prepare((int)(0.06 * sr) + 64);
    reset();
}

void Shimmer::reset() {
    for (auto& d : d_) d.clear();
    phase_ = 0.0;
    primed_ = false;
}

void Shimmer::process(ProcessContext& c) {
    const double inc = pRate->get() / sr_;
    const float depth = (float)(pDepth->get() * 0.001 * sr_), base = (float)(pDelay->get() * 0.001 * sr_);
    const float fb = pFeedback->get(), spread = pSpread->get();
    const int voices = pVoices->getInt();
    if (!primed_) { mix_.snap(pMix->get()); primed_ = true; }
    mix_.target(pMix->get(), c.numSamples);
    for (int i = 0; i < c.numSamples; ++i) {
        const float in[2] = {c.io[0][i], c.io[1][i]};
        float wet[2] = {0.0f, 0.0f};
        for (int ch = 0; ch < 2; ++ch)
            for (int v = 0; v < voices; ++v) {
                const double ph = phase_ + (double)v / voices + (ch ? 0.25 * spread : 0.0);
                const float lfo = 0.5f + 0.5f * (float)std::sin(kTwoPi * ph);
                wet[ch] += d_[ch].tapFrac(base + depth * lfo);
            }
        wet[0] /= (float)voices;
        wet[1] /= (float)voices;
        d_[0].push(in[0] + wet[0] * fb);
        d_[1].push(in[1] + wet[1] * fb);
        phase_ += inc;
        if (phase_ >= 1.0) phase_ -= 1.0;
        float dg, wg;
        mixGains(mix_.next(), dg, wg);
        c.io[0][i] = in[0] * dg + wet[0] * wg;
        c.io[1][i] = in[1] * dg + wet[1] * wg;
    }
}

std::vector<Preset> Shimmer::factoryPresets() const {
    return buildPresets(*this, {
        {"Classic chorus", {}},
        {"Slow and wide", {{"rate", 0.2f}, {"depth", 5}, {"delay", 18}, {"voices", 3}, {"spread", 1}}},
        {"Subtle thickener", {{"rate", 0.9f}, {"depth", 1.2f}, {"delay", 8}, {"mix", 0.3f}}},
        {"Flanger-ish", {{"rate", 0.25f}, {"depth", 3}, {"delay", 2}, {"feedback", 0.65f}, {"voices", 1}}},
        {"Vibrato", {{"rate", 5.5f}, {"depth", 1.5f}, {"delay", 5}, {"voices", 1}, {"mix", 1}}},
    });
}

} // namespace pulp
