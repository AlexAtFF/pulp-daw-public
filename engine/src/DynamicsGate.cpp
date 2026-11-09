// Sibilant (de-esser) and Doorman (noise gate / expander).
#include "pulp/Dynamics.h"

namespace pulp {

using namespace dyn;

// ------------------------------------------------------------------ Sibilant

namespace {
const float kSibKneeDb = 6.0f;
const double kSibAttackMs = 0.5, kSibPeakDecayMs = 2.0;
const double kButter4Q[2] = {0.54119610, 1.30656296};
}

Sibilant::Sibilant() {
    pFreq = addFloat("freq", "Frequency", 2000, 14000, 6500, "Hz", 2.0f);
    pWidth = addFloat("width", "Width", 0.3f, 3, 1.5f, "oct");
    pThresh = addFloat("thresh", "Threshold", -60, 0, -30, "dB");
    pRange = addFloat("range", "Range", 0, 24, 9, "dB");
    pRelease = addFloat("release", "Release", 10, 300, 60, "ms", 3.0f);
    pMode = addChoice("mode", "Reduce", {"The band only", "The whole signal"}, 0);
    pListen = addBool("listen", "Listen to the band", false);
    prepare(48000.0);
}

void Sibilant::prepare(double sr) {
    sr_ = sr;
    seenF_ = seenW_ = -1.0f;
    pkDecay_ = (float)std::exp(-1.0 / (0.001 * kSibPeakDecayMs * sr));
    att_ = timeCoef(kSibAttackMs, sr);
    reset();
}

void Sibilant::reset() {
    for (int ch = 0; ch < 2; ++ch) {
        for (int s = 0; s < 2; ++s) { detHp_[ch][s].reset(); detLp_[ch][s].reset(); }
        bell_[ch].reset();
    }
    pk_ = 0.0f;
    envDb_ = 0.0f;
    primed_ = false;
}

void Sibilant::update() {
    const float f = pFreq->get(), w = pWidth->get();
    if (f == seenF_ && w == seenW_) return;
    seenF_ = f; seenW_ = w;
    // detector: everything between the band edges, 24 dB per octave outside them (fourth-order Butterworth each side)
    const double lo = f * std::pow(2.0, -0.5 * w), hi = std::min(f * std::pow(2.0, 0.5 * w), 0.45 * sr_);
    // the band that is turned down: a bell of the same width, exactly 1 with no phase shift at its centre
    const double q = 1.0 / (2.0 * std::sinh(0.5 * std::log(2.0) * w));
    for (int ch = 0; ch < 2; ++ch) {
        for (int s = 0; s < 2; ++s) {
            detHp_[ch][s].highpass(lo, kButter4Q[s], sr_);
            detLp_[ch][s].lowpass(hi, kButter4Q[s], sr_);
        }
        bell_[ch].bandpass(f, q, sr_);
    }
}

void Sibilant::process(ProcessContext& c) {
    update();
    const float thr = pThresh->get(), range = pRange->get(), rel = timeCoef(pRelease->get(), sr_);
    const float wideT = pMode->getInt() == 1 ? 1.0f : 0.0f, listenT = pListen->getBool() ? 1.0f : 0.0f;
    if (!primed_) { wide_.snap(wideT); listen_.snap(listenT); primed_ = true; }
    wide_.target(wideT, c.numSamples);
    listen_.target(listenT, c.numSamples);
    float peakGr = 0.0f;
    for (int i = 0; i < c.numSamples; ++i) {
        float x[2], det[2], band[2];
        for (int k = 0; k < 2; ++k) {
            x[k] = c.io[k][i];
            det[k] = detLp_[k][1].process(detLp_[k][0].process(detHp_[k][1].process(detHp_[k][0].process(x[k]))));
            band[k] = bell_[k].process(x[k]);
        }
        pk_ = std::max(std::max(std::fabs(det[0]), std::fabs(det[1])), pk_ * pkDecay_);
        const float over = levelDb(pk_) - thr;
        // everything over the threshold is taken off (soft knee), but never more than the range
        float target = over <= -0.5f * kSibKneeDb ? 0.0f
                     : (over >= 0.5f * kSibKneeDb ? over : (over + 0.5f * kSibKneeDb) * (over + 0.5f * kSibKneeDb) / (2.0f * kSibKneeDb));
        target = std::min(target, range);
        envDb_ += (target - envDb_) * (target > envDb_ ? att_ : rel);
        if (target == 0.0f && envDb_ < 1.0e-6f) envDb_ = 0.0f;
        peakGr = std::max(peakGr, envDb_);
        const float cut = 1.0f - dbGain(-envDb_);
        const float wide = wide_.next(), lis = listen_.next();
        for (int k = 0; k < 2; ++k) {
            const float reduce = band[k] + (x[k] - band[k]) * wide;     // what is turned down: the band, or everything
            const float y = x[k] - cut * reduce;
            c.io[k][i] = y + (det[k] - y) * lis;
        }
    }
    for (int k = 0; k < 2; ++k) {
        for (int s = 0; s < 2; ++s) { flush(detHp_[k][s]); flush(detLp_[k][s]); }
        flush(bell_[k]);
    }
    flushDenormal(pk_);
    grPub_.store(peakGr, std::memory_order_relaxed);
}

std::vector<Preset> Sibilant::factoryPresets() const {
    return buildPresets(*this, {
        {"Gentle vocal", {{"freq", 6500}, {"width", 1.5f}, {"thresh", -32}, {"range", 6}}},
        {"Airy lead", {{"freq", 8000}, {"width", 1.2f}, {"thresh", -34}, {"range", 8}, {"release", 40}}},
        {"Deep voice", {{"freq", 5000}, {"width", 1.5f}, {"thresh", -30}, {"range", 8}}},
        {"Broadcast speech", {{"freq", 6000}, {"width", 2.0f}, {"thresh", -30}, {"range", 10}, {"mode", 1}, {"release", 80}}},
        {"Harsh cymbals", {{"freq", 9000}, {"width", 1.5f}, {"thresh", -28}, {"range", 6}, {"release", 100}}},
        {"Hi-hat tamer", {{"freq", 10000}, {"width", 1.0f}, {"thresh", -30}, {"range", 9}, {"release", 30}}},
        {"Hear the band", {{"listen", 1}}},
    });
}

// ------------------------------------------------------------------ Doorman

Doorman::Doorman() {
    pThresh = addFloat("thresh", "Threshold", -80, 0, -40, "dB");
    pHyst = addFloat("hyst", "Hysteresis", 0, 24, 3, "dB");
    pRange = addFloat("range", "Range", 0, 100, 80, "dB");
    pAttack = addFloat("attack", "Attack", 0.05f, 100, 0.5f, "ms", 3.0f);
    pHold = addFloat("hold", "Hold", 0, 1000, 50, "ms", 3.0f);
    pRelease = addFloat("release", "Release", 5, 2000, 150, "ms", 3.0f);
    pLook = addFloat("look", "Look-ahead", 0, (float)kMaxLookMs, 2, "ms");
    pSide = addBool("side", "Sidechain input", false);
    pScHp = addFloat("schp", "Detector high-pass (lowest = off)", 20, 5000, 20, "Hz", 3.0f);
    pScLp = addFloat("sclp", "Detector low-pass (highest = off)", 60, 20000, 20000, "Hz", 3.0f);
    pListen = addBool("listen", "Listen to the detector", false);
    prepare(48000.0);
}

std::vector<ParamGroup> Doorman::paramGroups() const {
    return {
        {"Gate", {"thresh", "hyst", "range"}},
        {"Timing", {"attack", "hold", "release", "look"}},
        {"Detector", {"side", "schp", "sclp", "listen"}},
    };
}

void Doorman::prepare(double sr) {
    sr_ = sr;
    lat_ = (int)std::lround(0.001 * kMaxLookMs * sr);
    for (auto& a : audio_) a.prepare(lat_ + 1);
    gain_.prepare(lat_ + 2);
    envDecay_ = (float)std::exp(-1.0 / (0.001 * kDetectorDecayMs * sr));
    seenHp_ = seenLp_ = -1.0f;
    reset();
}

void Doorman::reset() {
    for (auto& a : audio_) a.clear();
    for (int ch = 0; ch < 2; ++ch) { hp_[ch].reset(); lp_[ch].reset(); }
    env_ = 0.0f;
    open_ = false;          // it starts closed, as it would be after silence
    holdLeft_ = 0;
    gDb_ = -(double)pRange->get();
    gLin_ = std::pow(10.0, gDb_ / 20.0);
    std::fill(gain_.buf.begin(), gain_.buf.end(), (float)gLin_);
    primed_ = false;
}

void Doorman::process(ProcessContext& c) {
    const float hpHz = pScHp->get(), lpHz = pScLp->get();
    if (hpHz != seenHp_) { seenHp_ = hpHz; for (auto& f : hp_) f.highpass(hpHz, 0.70710678, sr_); }
    if (lpHz != seenLp_) { seenLp_ = lpHz; for (auto& f : lp_) f.lowpass(lpHz, 0.70710678, sr_); }
    const bool hpOn = hpHz > 20.5f, lpOn = lpHz < 19990.0f;
    if (!hpOn) for (auto& f : hp_) f.reset();
    if (!lpOn) for (auto& f : lp_) f.reset();
    const float thrDb = pThresh->get();
    const float thrOpen = dbToGain(thrDb), thrClose = dbToGain(thrDb - pHyst->get());
    const double rangeDb = pRange->get(), closedLin = std::pow(10.0, -rangeDb / 20.0);
    const double att = 1.0 - std::exp(-1.0 / (0.001 * pAttack->get() * sr_));
    const double rel = 1.0 - std::exp(-1.0 / (0.001 * pRelease->get() * sr_));
    const int holdN = (int)std::lround(0.001 * pHold->get() * sr_);
    // The sound is always lat_ samples late. The gain is read (lat_ - look-ahead) samples late, so it moves early.
    const float delayT = (float)lat_ - clampT((float)(0.001 * pLook->get() * sr_), 0.0f, (float)lat_);
    const float listenT = pListen->getBool() ? 1.0f : 0.0f;
    if (!primed_) { gainDelay_.snap(delayT); listen_.snap(listenT); primed_ = true; }
    gainDelay_.target(delayT, c.numSamples);
    listen_.target(listenT, c.numSamples);
    const bool useSide = pSide->getBool() && c.sidechain[0] != nullptr;
    const float* dl = useSide ? c.sidechain[0] : c.io[0];
    const float* dr = useSide ? c.sidechain[1] : c.io[1];
    float minGain = 1.0f;
    for (int i = 0; i < c.numSamples; ++i) {
        float d0 = dl[i], d1 = dr[i];
        if (hpOn) { d0 = hp_[0].process(d0); d1 = hp_[1].process(d1); }
        if (lpOn) { d0 = lp_[0].process(d0); d1 = lp_[1].process(d1); }
        env_ = std::max(std::max(std::fabs(d0), std::fabs(d1)), env_ * envDecay_);
        if (env_ >= thrOpen) { open_ = true; holdLeft_ = holdN; }
        else if (open_) {
            if (env_ >= thrClose) holdLeft_ = holdN;                // between the two thresholds it stays as it is
            else { if (holdLeft_ > 0) --holdLeft_; if (holdLeft_ == 0) open_ = false; }
        }
        const double tDb = open_ ? 0.0 : -rangeDb, tLin = open_ ? 1.0 : closedLin;
        if (gDb_ < tDb) {               // opening: the amplitude closes in on its target
            gLin_ += (tLin - gLin_) * att;
            if (tLin - gLin_ < 1.0e-6 * tLin) { gLin_ = tLin; gDb_ = tDb; }
            else gDb_ = 20.0 * std::log10(gLin_);
        } else if (gDb_ > tDb) {        // closing: the level in dB closes in on its target
            gDb_ += (tDb - gDb_) * rel;
            if (gDb_ - tDb < 1.0e-3) { gDb_ = tDb; gLin_ = tLin; }
            else gLin_ = std::pow(10.0, gDb_ / 20.0);
        }
        gain_.push((float)gLin_);
        audio_[0].push(c.io[0][i]);
        audio_[1].push(c.io[1][i]);
        const float g = gain_.tapFrac(gainDelay_.next());
        minGain = std::min(minGain, g);
        const float y0 = audio_[0].tap(lat_) * g, y1 = audio_[1].tap(lat_) * g, lis = listen_.next();
        c.io[0][i] = y0 + (d0 - y0) * lis;
        c.io[1][i] = y1 + (d1 - y1) * lis;
    }
    for (int ch = 0; ch < 2; ++ch) { flush(hp_[ch]); flush(lp_[ch]); }
    flushDenormal(env_);
    grPub_.store(-gainToDb(minGain), std::memory_order_relaxed);
    openPub_.store(open_ ? 1.0f : 0.0f, std::memory_order_relaxed);
}

std::vector<Preset> Doorman::factoryPresets() const {
    return buildPresets(*this, {
        {"Noise floor cleaner", {{"thresh", -50}, {"hyst", 4}, {"range", 80}, {"attack", 1}, {"hold", 80}, {"release", 250}, {"look", 2}}},
        {"Tight drums", {{"thresh", -28}, {"hyst", 6}, {"range", 60}, {"attack", 0.2f}, {"hold", 30}, {"release", 80}, {"look", 3}}},
        {"Soft expander", {{"thresh", -35}, {"hyst", 2}, {"range", 12}, {"attack", 5}, {"hold", 40}, {"release", 200}}},
        {"Breath control", {{"thresh", -38}, {"hyst", 3}, {"range", 15}, {"attack", 4}, {"hold", 60}, {"release", 180}}},
        {"Keyed chop", {{"side", 1}, {"thresh", -30}, {"hyst", 2}, {"range", 100}, {"attack", 0.5f}, {"hold", 10}, {"release", 40}, {"look", 0}}},
        {"Kick only", {{"sclp", 150}, {"thresh", -24}, {"hyst", 6}, {"range", 80}, {"attack", 0.2f}, {"hold", 40}, {"release", 100}, {"look", 3}}},
        {"Snare without hats", {{"schp", 150}, {"sclp", 3000}, {"thresh", -26}, {"hyst", 6}, {"range", 50}, {"attack", 0.2f}, {"hold", 30}, {"release", 120}, {"look", 3}}},
        {"Hum gate", {{"thresh", -46}, {"hyst", 5}, {"range", 40}, {"attack", 2}, {"hold", 120}, {"release", 400}}},
    });
}

} // namespace pulp
