// Grit (distortion), Sweep (filter) and Stereo Kit.
#include "pulp/Effects.h"

namespace pulp {

// ------------------------------------------------------------------ Grit

Grit::Grit() {
    pDrive = addFloat("drive", "Drive", 0, 40, 12, "dB");
    pType = addChoice("type", "Character", {"Soft clip", "Hard clip", "Fold", "Tube"}, 0);
    pTone = addFloat("tone", "Tone", 500, 20000, 20000, "Hz", 3.0f);
    pBias = addFloat("bias", "Bias (even harmonics)", 0, 1, 0, "%");
    pOut = addFloat("out", "Output", -30, 12, -6, "dB");
    pMix = addFloat("mix", "Dry / wet", 0, 1, 1, "%");
    // Low-pass at the original Nyquist frequency, Blackman window.
    const double fc = 0.5 / kOver;
    double sum = 0.0;
    for (int n = 0; n < kTaps; ++n) {
        const double m = n - (kTaps - 1) / 2.0;
        const double sinc = m == 0.0 ? 2.0 * fc : std::sin(kTwoPi * fc * m) / (kPi * m);
        const double w = 0.42 - 0.5 * std::cos(kTwoPi * n / (kTaps - 1)) + 0.08 * std::cos(2.0 * kTwoPi * n / (kTaps - 1));
        fir_[n] = (float)(sinc * w);
        sum += fir_[n];
    }
    for (float& v : fir_) v = (float)(v / sum);
}

void Grit::prepare(double sr) { sr_ = sr; reset(); }

void Grit::reset() {
    std::memset(up_, 0, sizeof up_);
    std::memset(down_, 0, sizeof down_);
    std::memset(dryDl_, 0, sizeof dryDl_);
    upPos_ = downPos_ = dryPos_ = 0;
    for (int c = 0; c < 2; ++c) { tone_[c].reset(); dc_[c].reset(); }
    primed_ = false;
}

void Grit::process(ProcessContext& c) {
    const float driveT = dbToGain(pDrive->get()), outT = dbToGain(pOut->get()), mixT = pMix->get();
    if (!primed_) { drive_.snap(driveT); out_.snap(outT); mix_.snap(mixT); primed_ = true; }
    drive_.target(driveT, c.numSamples);
    out_.target(outT, c.numSamples);
    mix_.target(mixT, c.numSamples);
    const int type = pType->getInt();
    const float bias = pBias->get() * 0.6f;
    const float biasOut = type == 1 ? clampT(bias, -1.0f, 1.0f) : (type == 2 ? std::sin(bias * (float)kPi * 0.5f) : std::tanh(bias));
    for (int ch = 0; ch < 2; ++ch) { tone_[ch].setHz(pTone->get(), sr_); dc_[ch].setHz(8.0, sr_); }
    const bool toneOn = pTone->get() < 19990.0f;

    for (int i = 0; i < c.numSamples; ++i) {
        const float drive = drive_.next(), outG = out_.next(), mix = mix_.next();
        for (int ch = 0; ch < 2; ++ch) {
            const float x = c.io[ch][i];
            up_[ch][upPos_] = x;
            // four output phases from the 17 newest input samples (the zero-stuffed taps are skipped)
            for (int p = 0; p < kOver; ++p) {
                float u = 0.0f;
                int idx = upPos_;
                for (int j = p; j < kTaps; j += kOver) { u += fir_[j] * up_[ch][idx]; idx = idx == 0 ? kUpHist - 1 : idx - 1; }
                u *= (float)kOver * drive;
                u += bias;
                float v;
                switch (type) {
                    case 1: v = clampT(u, -1.0f, 1.0f); break;
                    case 2: v = std::sin(u * (float)kPi * 0.5f); break;
                    case 3: v = u >= 0.0f ? std::tanh(u) : 0.7f * std::tanh(u / 0.7f) * 1.2f; break;
                    default: v = std::tanh(u); break;
                }
                down_[ch][(downPos_ + p) % kTaps] = v - biasOut;
            }
            // one filtered sample out for every four in
            // (taken at the first of the four new samples, so the total delay is a whole 16 samples)
            float y = 0.0f;
            int idx = downPos_ % kTaps;
            for (int j = 0; j < kTaps; ++j) { y += fir_[j] * down_[ch][idx]; idx = idx == 0 ? kTaps - 1 : idx - 1; }
            if (toneOn) y = tone_[ch].lp(y);
            if (bias > 0.0f || type == 3) y = dc_[ch].hp(y);
            const float dry = dryDl_[ch][dryPos_];
            dryDl_[ch][dryPos_] = x;
            c.io[ch][i] = dry + (y * outG - dry) * mix;
        }
        upPos_ = (upPos_ + 1) % kUpHist;
        downPos_ = (downPos_ + kOver) % kTaps;
        dryPos_ = (dryPos_ + 1) % kLatency;
    }
}

std::vector<Preset> Grit::factoryPresets() const {
    return buildPresets(*this, {
        {"Warm drive", {}},
        {"Gentle tape", {{"drive", 6}, {"type", 0}, {"tone", 9000}, {"out", -3}}},
        {"Crunch", {{"drive", 22}, {"type", 0}, {"tone", 6500}, {"out", -9}}},
        {"Hard fuzz", {{"drive", 34}, {"type", 1}, {"tone", 4500}, {"out", -10}}},
        {"Folded", {{"drive", 18}, {"type", 2}, {"out", -6}}},
        {"Tube glow", {{"drive", 14}, {"type", 3}, {"bias", 0.35f}, {"tone", 12000}, {"out", -7}}},
        {"Parallel grit", {{"drive", 28}, {"type", 0}, {"mix", 0.35f}, {"out", -8}}},
    });
}

// ------------------------------------------------------------------ Sweep

Sweep::Sweep() {
    pType = addChoice("type", "Type", {"Low-pass", "High-pass", "Band-pass", "Notch"}, 0);
    pCutoff = addFloat("cutoff", "Cutoff", 20, 20000, 2000, "Hz", 3.0f);
    pReso = addFloat("reso", "Resonance", 0, 1, 0.1f, "%");
    pSlope = addChoice("slope", "Slope", {"12 dB / octave", "24 dB / octave"}, 0);
    pMix = addFloat("mix", "Dry / wet", 0, 1, 1, "%");
}

void Sweep::reset() {
    for (auto& ch : svf_) for (auto& s : ch) s.reset();
    cut_ = -1.0f;
    primed_ = false;
}

void Sweep::process(ProcessContext& c) {
    const int type = pType->getInt();
    const bool two = pSlope->getInt() == 1;
    const float target = pCutoff->get();
    // resonance 0 gives a flat (Butterworth) corner; the second stage of the 24 dB slope stays gentle
    const float q = 0.7071f + pReso->get() * pReso->get() * 20.0f;
    if (!primed_) { mix_.snap(pMix->get()); primed_ = true; }
    if (cut_ < 0.0f) cut_ = target;
    mix_.target(pMix->get(), c.numSamples);
    for (int i = 0; i < c.numSamples; ++i) {
        if ((i & 7) == 0) {
            if (cut_ != target) { cut_ += (target - cut_) * 0.04f; if (std::fabs(cut_ - target) < 0.01f * target) cut_ = target; }
            svf_[0][0].set(cut_, q, (float)sr_);
            svf_[0][1].set(cut_, 0.7071f, (float)sr_);
            for (int s = 0; s < 2; ++s) { svf_[1][s].g = svf_[0][s].g; svf_[1][s].k = svf_[0][s].k; svf_[1][s].a1 = svf_[0][s].a1; svf_[1][s].a2 = svf_[0][s].a2; svf_[1][s].a3 = svf_[0][s].a3; }
        }
        const float mix = mix_.next();
        for (int ch = 0; ch < 2; ++ch) {
            const float x = c.io[ch][i];
            float y = x;
            for (int s = 0; s < (two ? 2 : 1); ++s) {
                float lp, bp, hp;
                svf_[ch][s].process(y, lp, bp, hp);
                switch (type) {
                    case 1: y = hp; break;
                    case 2: y = bp * svf_[ch][s].k; break;      // unity gain at the centre frequency
                    case 3: y = lp + hp; break;
                    default: y = lp; break;
                }
            }
            c.io[ch][i] = x + (y - x) * mix;
        }
    }
}

std::vector<Preset> Sweep::factoryPresets() const {
    return buildPresets(*this, {
        {"Low-pass 2 kHz", {}},
        {"Dark and steep", {{"cutoff", 600}, {"slope", 1}}},
        {"Rumble cut", {{"type", 1}, {"cutoff", 90}, {"slope", 1}}},
        {"Radio band", {{"type", 2}, {"cutoff", 1400}, {"reso", 0.25f}}},
        {"Resonant sweep start", {{"cutoff", 350}, {"reso", 0.6f}, {"slope", 1}}},
        {"Hum notch 50 Hz", {{"type", 3}, {"cutoff", 50}, {"reso", 0.7f}}},
    });
}

// ------------------------------------------------------------------ Stereo Kit

StereoKit::StereoKit() {
    pWidth = addFloat("width", "Width", 0, 2, 1, "%");
    pBalance = addFloat("balance", "Balance", -1, 1, 0);
    pGain = addFloat("gain", "Gain", -36, 24, 0, "dB");
    pInvL = addBool("invl", "Invert left", false);
    pInvR = addBool("invr", "Invert right", false);
    pSwap = addBool("swap", "Swap sides", false);
    pMonoBelow = addFloat("monobelow", "Mono below (lowest = off)", 20, 500, 20, "Hz", 2.0f);
}

void StereoKit::reset() {
    hpSide_.reset();
    hpSeen_ = -1.0f;
    primed_ = false;
}

void StereoKit::process(ProcessContext& c) {
    const float g = dbToGain(pGain->get());
    const float bal = pBalance->get();
    const float tl = g * (bal > 0.0f ? 1.0f - bal : 1.0f) * (pInvL->getBool() ? -1.0f : 1.0f);
    const float tr = g * (bal < 0.0f ? 1.0f + bal : 1.0f) * (pInvR->getBool() ? -1.0f : 1.0f);
    if (!primed_) { width_.snap(pWidth->get()); gL_.snap(tl); gR_.snap(tr); primed_ = true; }
    width_.target(pWidth->get(), c.numSamples);
    gL_.target(tl, c.numSamples);
    gR_.target(tr, c.numSamples);
    const float monoHz = pMonoBelow->get();
    const bool monoOn = monoHz > 20.5f;
    if (monoOn && monoHz != hpSeen_) { hpSeen_ = monoHz; hpSide_.highpass(monoHz, 0.7071, sr_); }
    const bool swap = pSwap->getBool();
    for (int i = 0; i < c.numSamples; ++i) {
        float l = c.io[0][i], r = c.io[1][i];
        if (swap) std::swap(l, r);
        const float mid = 0.5f * (l + r);
        float side = 0.5f * (l - r) * width_.next();
        if (monoOn) side = hpSide_.process(side);       // no side signal below the corner: the lows are mono
        c.io[0][i] = (mid + side) * gL_.next();
        c.io[1][i] = (mid - side) * gR_.next();
    }
}

std::vector<Preset> StereoKit::factoryPresets() const {
    return buildPresets(*this, {
        {"Unchanged", {}},
        {"Wider", {{"width", 1.5f}}},
        {"Mono", {{"width", 0}}},
        {"Mono bass", {{"monobelow", 140}}},
        {"Wide with mono bass", {{"width", 1.6f}, {"monobelow", 160}}},
        {"Swap sides", {{"swap", 1}}},
    });
}

} // namespace pulp
