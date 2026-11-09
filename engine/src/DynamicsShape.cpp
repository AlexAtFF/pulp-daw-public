// Snap (transient shaper) and Curb (soft clipper).
#include "pulp/Dynamics.h"

namespace pulp {

using namespace dyn;

// ------------------------------------------------------------------ Snap

namespace {
// Envelope time constants. The attack side compares a 0.75 ms and a 20 ms follower, the sustain side a 30 ms and
// a 300 ms one; both pairs follow a 12 ms peak hold, so a steady tone down to about 40 Hz gives no difference at all.
const double kSnapHoldMs = 12.0, kSnapFastAttMs = 0.75, kSnapSlowAttMs = 20.0, kSnapFastSusMs = 30.0, kSnapSlowSusMs = 300.0;
const float kSnapAttackScale = 0.5f;    // dB of gain per dB of envelope difference at 100 %
const float kSnapSustainScale = 0.5f;
const float kSnapMaxDiffAtt = 30.0f, kSnapMaxDiffSus = 36.0f;     // so the gain change is at most 15 and 18 dB
const float kSnapEps = 1.0e-9f;
}

Snap::Snap() {
    pAttack = addFloat("attack", "Attack", -1, 1, 0, "%");
    pSustain = addFloat("sustain", "Sustain", -1, 1, 0, "%");
    pOut = addFloat("out", "Output gain", -24, 24, 0, "dB");
    pMix = addFloat("mix", "Dry / wet", 0, 1, 1, "%");
    prepare(48000.0);
}

void Snap::prepare(double sr) {
    sr_ = sr;
    peak_.prepare((int)std::lround(0.001 * kSnapHoldMs * sr));
    cFa_ = timeCoef(kSnapFastAttMs, sr);
    cSa_ = timeCoef(kSnapSlowAttMs, sr);
    cFs_ = timeCoef(kSnapFastSusMs, sr);
    cSs_ = timeCoef(kSnapSlowSusMs, sr);
    reset();
}

void Snap::reset() {
    peak_.reset();
    fa_ = sa_ = fs_ = ss_ = 0.0f;
    primed_ = false;
}

void Snap::process(ProcessContext& c) {
    const float attT = pAttack->get(), susT = pSustain->get(), outT = dbToGain(pOut->get()), mixT = pMix->get();
    if (!primed_) { att_.snap(attT); sus_.snap(susT); out_.snap(outT); mix_.snap(mixT); primed_ = true; }
    att_.target(attT, c.numSamples);
    sus_.target(susT, c.numSamples);
    out_.target(outT, c.numSamples);
    mix_.target(mixT, c.numSamples);
    float maxCut = 0.0f, maxBoost = 0.0f;
    float* L = c.io[0];
    float* R = c.io[1];
    for (int i = 0; i < c.numSamples; ++i) {
        const float p = peak_.push(std::max(std::fabs(L[i]), std::fabs(R[i])));
        fa_ += (p - fa_) * cFa_;
        sa_ += (p - sa_) * cSa_;
        fs_ += (p - fs_) * cFs_;
        ss_ += (p - ss_) * cSs_;
        // Ratios of envelopes: the same whatever the level of the signal, so there is no threshold.
        const float dA = clampT(20.0f * std::log10((fa_ + kSnapEps) / (sa_ + kSnapEps)), 0.0f, kSnapMaxDiffAtt);   // above 0 while the level rises
        const float dS = clampT(20.0f * std::log10((ss_ + kSnapEps) / (fs_ + kSnapEps)), 0.0f, kSnapMaxDiffSus);   // above 0 while it dies away
        const float gDb = clampT(att_.next() * kSnapAttackScale * dA + sus_.next() * kSnapSustainScale * dS, -24.0f, 24.0f);
        maxCut = std::max(maxCut, -gDb);
        maxBoost = std::max(maxBoost, gDb);
        const float g = dbGain(gDb), og = out_.next(), mix = mix_.next();
        const float wl = L[i] * g, wr = R[i] * g;
        L[i] = (L[i] + (wl - L[i]) * mix) * og;
        R[i] = (R[i] + (wr - R[i]) * mix) * og;
    }
    flushDenormal(fa_); flushDenormal(sa_); flushDenormal(fs_); flushDenormal(ss_);
    cutPub_.store(maxCut, std::memory_order_relaxed);
    boostPub_.store(maxBoost, std::memory_order_relaxed);
}

std::vector<Preset> Snap::factoryPresets() const {
    return buildPresets(*this, {
        {"Punchier drums", {{"attack", 0.5f}}},
        {"Snare crack", {{"attack", 0.8f}, {"sustain", -0.2f}, {"out", -2}}},
        {"Tighter room", {{"sustain", -0.6f}}},
        {"Soften the pick", {{"attack", -0.5f}}},
        {"Longer tails", {{"sustain", 0.6f}, {"out", -1}}},
        {"Parallel punch", {{"attack", 1.0f}, {"mix", 0.5f}, {"out", -1.5f}}},
        {"Pillow", {{"attack", -0.8f}, {"sustain", 0.3f}}},
    });
}

// ------------------------------------------------------------------ Curb

namespace {
// Straight up to the start of the knee, then a tanh curve that runs into the threshold and never passes it.
inline float curbShape(float u, float kneeStart, float threshold, float span, float invSpan) {
    const float au = std::fabs(u);
    if (au <= kneeStart) return u;
    const float y = invSpan > 0.0f ? kneeStart + span * std::tanh((au - kneeStart) * invSpan) : threshold;
    return u < 0.0f ? -y : y;
}
}

float Curb::shape(float x, float threshold, float knee) {
    const float a = threshold * (1.0f - clampT(knee, 0.0f, 1.0f)), span = threshold - a;
    return clampT(curbShape(x, a, threshold, span, span > 1.0e-9f ? 1.0f / span : 0.0f), -threshold, threshold);
}

Curb::Curb() {
    pDrive = addFloat("drive", "Input drive", 0, 24, 0, "dB");
    pThresh = addFloat("thresh", "Threshold", -24, 0, -3, "dB");
    pKnee = addFloat("knee", "Knee (hard to soft)", 0, 1, 0.3f, "%");
    pOver = addBool("over", "4x oversampling", true);
    pOut = addFloat("out", "Output gain", -24, 12, 0, "dB");
    pMatch = addBool("match", "Match level (undo the drive)", false);
    prepare(48000.0);
}

void Curb::prepare(double sr) {
    sr_ = sr;
    lim_.prepare(std::max(4, (int)std::lround(sr / 3000.0)));       // a third of a millisecond
    limRel_ = timeCoef(10.0, sr);
    reset();
}

void Curb::reset() {
    for (auto& o : os_) o.reset();
    lim_.reset();
    std::memset(naive_, 0, sizeof naive_);
    naivePos_ = 0;
    primed_ = false;
}

void Curb::process(ProcessContext& c) {
    const float driveT = dbToGain(pDrive->get()), thrT = dbToGain(pThresh->get()), kneeT = pKnee->get();
    const float outT = dbToGain(pOut->get()) / (pMatch->getBool() ? driveT : 1.0f), osT = pOver->getBool() ? 1.0f : 0.0f;
    if (!primed_) { drive_.snap(driveT); thr_.snap(thrT); knee_.snap(kneeT); out_.snap(outT); osMix_.snap(osT); primed_ = true; }
    drive_.target(driveT, c.numSamples);
    thr_.target(thrT, c.numSamples);
    knee_.target(kneeT, c.numSamples);
    out_.target(outT, c.numSamples);
    osMix_.target(osT, c.numSamples);
    lim_.beginBlock();
    float over = 0.0f;
    for (int i = 0; i < c.numSamples; ++i) {
        const float d = drive_.next(), T = thr_.next(), k = knee_.next(), og = out_.next(), osm = osMix_.next();
        const float a = T * (1.0f - k), span = T - a, inv = span > 1.0e-9f ? 1.0f / span : 0.0f;
        float y[2];
        for (int ch = 0; ch < 2; ++ch) {
            const float x = c.io[ch][i] * d;
            over = std::max(over, std::fabs(x) / T);
            // The clipper runs at four times the rate, and only what it takes away comes back down through the filter:
            // below the knee that is nothing at all, so the signal passes untouched (only late).
            float u[4], r[4];
            os_[ch].up(x, u);
            for (int p = 0; p < 4; ++p) r[p] = curbShape(u[p], a, T, span, inv) - u[p];
            const float fine = os_[ch].down(r);
            const float plain = naive_[ch][naivePos_];                  // the same thing without oversampling, equally late
            naive_[ch][naivePos_] = curbShape(x, a, T, span, inv) - x;
            y[ch] = os_[ch].delay(x) + plain + (fine - plain) * osm;
        }
        naivePos_ = (naivePos_ + 1) % Over4::kLatency;
        // The down filter rings past the threshold by a little. The limiter turns just those moments down (its gain is
        // exactly 1 otherwise), and the clamp keeps the promise that nothing ever passes the threshold.
        const float g = lim_.process(y[0], y[1], T, limRel_);
        c.io[0][i] = clampT(y[0] * g, -T, T) * og;
        c.io[1][i] = clampT(y[1] * g, -T, T) * og;
    }
    grPub_.store(over > 1.0f ? 20.0f * std::log10(over) : 0.0f, std::memory_order_relaxed);
}

std::vector<Preset> Curb::factoryPresets() const {
    return buildPresets(*this, {
        {"Peak shaver", {{"drive", 0}, {"thresh", -2}, {"knee", 0.3f}}},
        {"Hard ceiling", {{"drive", 0}, {"thresh", -0.1f}, {"knee", 0}}},
        {"Master loudness", {{"drive", 4}, {"thresh", -0.3f}, {"knee", 0.5f}}},
        {"Drum bus crack", {{"drive", 6}, {"thresh", -3}, {"knee", 0.15f}, {"out", 2}}},
        {"Warm soft knee", {{"drive", 8}, {"thresh", -4}, {"knee", 1.0f}, {"out", 2}}},
        {"Sub flattener", {{"drive", 10}, {"thresh", -6}, {"knee", 0.6f}, {"match", 1}, {"out", 4}}},
        {"Matched crunch", {{"drive", 14}, {"thresh", -8}, {"knee", 0.4f}, {"match", 1}, {"out", 6}}},
    });
}

} // namespace pulp
