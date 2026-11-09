// True Note: automatic pitch correction for one voice.
#include "pulp/Vocal.h"

namespace pulp {
using namespace vocal;

TrueNote::TrueNote() {
    pKey = addChoice("key", "Key", keyNames(), 0);
    auto scales = scaleNames();
    scales.push_back("Custom");
    pScale = addChoice("scale", "Scale", scales, Chromatic);
    const auto names = keyNames();
    for (int i = 0; i < 12; ++i)
        pNote[i] = addBool(("n" + std::to_string(i)).c_str(), ("Custom scale: " + names[(size_t)i]).c_str(), true);
    pSpeed = addFloat("speed", "Retune time", 0, 400, 25, "ms", 2.5f);
    pAmount = addFloat("amount", "Correction amount", 0, 1, 1, "%");
    ParamInfo tr;
    tr.id = "transpose"; tr.name = "Transpose"; tr.min = -12; tr.max = 12; tr.def = 0; tr.steps = 25; tr.unit = "st";
    pTranspose = add(tr);
    pRange = addChoice("range", "Voice range", {"Low (60 to 600 Hz)", "High (110 to 1200 Hz)"}, 0);
    pFormant = addBool("formant", "Keep formants", true);
    pFShift = addFloat("fshift", "Formant shift", -12, 12, 0, "st");
    pMix = addFloat("mix", "Dry / wet", 0, 1, 1, "%");
}

std::vector<ParamGroup> TrueNote::paramGroups() const {
    std::vector<std::string> notes;
    for (int i = 0; i < 12; ++i) notes.push_back("n" + std::to_string(i));
    return {{"Scale", {"key", "scale"}},
            {"Custom scale notes", notes},
            {"Correction", {"speed", "amount", "transpose", "range"}},
            {"Formant", {"formant", "fshift"}},
            {"Output", {"mix"}}};
}

void TrueNote::prepare(double sr) {
    sr_ = sr;
    latency_ = latencyFor(sr);
    const int ring = latency_ + 7 * (int)(sr / 55.0) + 64;
    for (auto& h : hist_) h.prepare(ring);
    det_.prepare(sr, 0);
    float* hp[2] = {hist_[0].d, hist_[1].d};
    sh_.prepare(sr, latency_ - 6, hp, hist_[0].mask, 2);
    reset();
}

void TrueNote::reset() {
    for (auto& h : hist_) h.clear();
    det_.reset();
    n_ = 0;
    sh_.reset(-(double)latency_);
    wasVoiced_ = false;
    corr_ = 0.0;
    lastMark_ = -(double)latency_;
    primed_ = false;
}

// Decides what the grain at the next output mark does: how far the pitch moves and where the formants go.
void TrueNote::spawnGrain() {
    const double s = sh_.nextMark();
    GrainCtl c;
    double T = 0.0;
    c.voiced = det_.periodAt(s, T);
    c.period = T;
    if (!c.voiced) {
        wasVoiced_ = false;
        corr_ = 0.0;
        lastMark_ = s;
        sh_.spawn(c, n_);
        return;
    }
    // the period right here, measured against the previous grain: no lag, so a snap is exact
    T = sh_.localPeriod(T, n_);
    c.period = T;
    const double keyIn = 69.0 + 12.0 * std::log2(sr_ / (T * 440.0));
    double target = keyIn;
    if (mask_ != 0) {
        const double cand = nearestNote(keyIn, mask_);
        const bool held = wasVoiced_ && ((mask_ >> ((((int)note_ % 12) + 12) % 12)) & 1u);
        // a held note is only left when another one is clearly nearer, so vibrato does not flip between neighbours
        if (!held || (cand != note_ && std::fabs(keyIn - note_) - std::fabs(keyIn - cand) > 0.3)) note_ = cand;
        target = note_;
    }
    const double want = (target - keyIn) * amount_;
    if (snap_) corr_ = want;
    else if (!wasVoiced_) corr_ = 0.0;                  // a new note starts where it was sung and glides
    else corr_ += (want - corr_) * (1.0 - std::exp(-std::max(0.0, s - lastMark_) * rateK_));
    wasVoiced_ = true;
    lastMark_ = s;
    c.centre = (amount_ > 0.0 && mask_ != 0) || transpose_ != 0.0 || fshift_ != 1.0;
    c.pitch = std::pow(2.0, (corr_ + transpose_) / 12.0);
    c.formant = preserve_ ? fshift_ : c.pitch * fshift_;
    sh_.spawn(c, n_);
}

void TrueNote::process(ProcessContext& c) {
    // parameters, once per quantum; the pitch controls take effect grain by grain, so they cannot click
    const int scale = pScale->getInt();
    if (scale >= kCustomScale) {
        mask_ = 0;
        for (int i = 0; i < 12; ++i) if (pNote[i]->getBool()) mask_ |= 1u << i;
    } else {
        mask_ = scaleMask(scale, pKey->getInt());
    }
    amount_ = pAmount->get();
    const double speedMs = pSpeed->get();
    snap_ = speedMs < 0.5;
    rateK_ = snap_ ? 0.0 : std::log(10.0) / (0.001 * speedMs * sr_);       // nine tenths of the way in the stated time
    transpose_ = (double)std::lround(pTranspose->get());
    fshift_ = std::pow(2.0, (double)pFShift->get() / 12.0);
    preserve_ = pFormant->getBool();
    det_.setRange(pRange->getInt());
    const float mix = pMix->get();
    if (!primed_) { mix_.snap(mix); primed_ = true; }
    mix_.target(mix, c.numSamples);

    float* L = c.io[0];
    float* R = c.io[1];
    float* hp[2] = {hist_[0].d, hist_[1].d};
    const int64 mask = hist_[0].mask;
    for (int i = 0; i < c.numSamples; ++i) {
        float l = L[i], r = R[i];
        if (!std::isfinite(l)) l = 0.0f;        // a broken sample upstream must not get into the detector for good
        if (!std::isfinite(r)) r = 0.0f;
        hp[0][n_ & mask] = l;
        hp[1][n_ & mask] = r;
        det_.push(0.5f * (l + r));
        const int64 t = n_ - latency_;
        int guard = 0;
        while (sh_.due(t) && guard++ < 32) spawnGrain();
        float w[2];
        sh_.render(t, w);
        const float dl = hp[0][t & mask], dr = hp[1][t & mask];
        const float m = mix_.next();
        L[i] = dl + m * (w[0] - dl);
        R[i] = dr + m * (w[1] - dr);
        ++n_;
    }

    const PitchDetector::Estimate* e = det_.latest();
    const bool voiced = e && e->voiced;
    mPitch_.store(voiced ? (float)(69.0 + 12.0 * std::log2(sr_ / (e->period * 440.0))) : 0.0f, std::memory_order_relaxed);
    mCents_.store((float)(corr_ * 100.0), std::memory_order_relaxed);
    mVoiced_.store(voiced ? 1.0f : 0.0f, std::memory_order_relaxed);
    scope_.push(L, R, c.numSamples);
}

std::vector<Preset> TrueNote::factoryPresets() const {
    return buildPresets(*this, {
        {"Natural touch-up", {{"speed", 90}, {"amount", 1.0f}}},
        {"Gentle nudge, half way", {{"speed", 200}, {"amount", 0.5f}}},
        {"Hard snap, chromatic", {{"speed", 0}, {"amount", 1.0f}}},
        {"Hard snap, C major", {{"speed", 0}, {"scale", Major}, {"key", 0}}},
        {"Hard snap, A minor", {{"speed", 0}, {"scale", Minor}, {"key", 9}}},
        {"Pentatonic lock in G", {{"speed", 8}, {"scale", PentaMajor}, {"key", 7}}},
        {"Blues lead in E", {{"speed", 40}, {"scale", Blues}, {"key", 4}}},
        {"High voice, quick and clean", {{"speed", 15}, {"range", 1}}},
        {"Octave up, small voice", {{"speed", 20}, {"transpose", 12}, {"formant", 0}}},
        {"Deep giant", {{"speed", 20}, {"fshift", -5}}},
        {"Bright and thin", {{"speed", 20}, {"fshift", 4}}},
        {"Parallel robot, half mix", {{"speed", 0}, {"scale", Major}, {"mix", 0.5f}}},
    });
}

} // namespace pulp
