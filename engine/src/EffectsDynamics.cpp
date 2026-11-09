// Contour EQ, Squeeze (compressor) and Lid (limiter).
#include "pulp/Effects.h"

namespace pulp {

std::vector<Preset> buildPresets(const Plugin& p, const std::vector<PresetDef>& defs) {
    std::vector<Preset> out;
    for (auto& d : defs) {
        Json ps = Json::object();
        for (auto& prm : p.params()) ps.set(prm->info.id, prm->info.def);
        for (auto& pv : d.values) ps.set(pv.id, pv.v);
        Json st = Json::object();
        st.set("type", p.typeId());
        st.set("params", ps);
        out.push_back({d.name, st});
    }
    return out;
}

// ------------------------------------------------------------------ Contour EQ

ContourEq::ContourEq() {
    const std::vector<std::string> types = {"Peak", "Low shelf", "High shelf", "Low cut", "High cut", "Notch"};
    const float freqs[kBands] = {60, 150, 400, 1000, 2500, 6000, 12000};
    const int defType[kBands] = {LowShelf, Peak, Peak, Peak, Peak, Peak, HighShelf};
    for (int b = 0; b < kBands; ++b) {
        const std::string n = "b" + std::to_string(b + 1), d = "Band " + std::to_string(b + 1) + " ";
        pType[b] = addChoice((n + "type").c_str(), (d + "type").c_str(), types, defType[b]);
        pFreq[b] = addFloat((n + "freq").c_str(), (d + "frequency").c_str(), 20, 20000, freqs[b], "Hz", 3.0f);
        pGain[b] = addFloat((n + "gain").c_str(), (d + "gain").c_str(), -24, 24, 0, "dB");
        pQ[b] = addFloat((n + "q").c_str(), (d + "width").c_str(), 0.1f, 18, 0.9f, "", 2.0f);
        for (int k = 0; k < 4; ++k) seen_[b][k] = -1.0e9f;
    }
    pOut = addFloat("out", "Output gain", -24, 24, 0, "dB");
}

std::vector<ParamGroup> ContourEq::paramGroups() const {
    std::vector<ParamGroup> g;
    for (int b = 1; b <= kBands; ++b) {
        const std::string n = "b" + std::to_string(b);
        g.push_back({"Band " + std::to_string(b), {n + "type", n + "freq", n + "gain", n + "q"}});
    }
    g.push_back({"Output", {"out"}});
    return g;
}

void ContourEq::design(dsp::Biquad& b, int type, double hz, double gainDb, double q, double sr) {
    switch (type) {
        case LowShelf: b.lowShelf(hz, q, gainDb, sr); break;
        case HighShelf: b.highShelf(hz, q, gainDb, sr); break;
        case LowCut: b.highpass(hz, q, sr); break;
        case HighCut: b.lowpass(hz, q, sr); break;
        case Notch: b.notch(hz, q, sr); break;
        default: b.peak(hz, q, gainDb, sr); break;
    }
}

void ContourEq::prepare(double sr) {
    sr_ = sr;
    for (int b = 0; b < kBands; ++b) for (int k = 0; k < 4; ++k) seen_[b][k] = -1.0e9f;
    reset();
}

void ContourEq::reset() {
    for (auto& band : bq_) for (auto& c : band) c.reset();
    primed_ = false;
}

void ContourEq::update() {
    for (int b = 0; b < kBands; ++b) {
        const float t = pType[b]->get(), f = pFreq[b]->get(), g = pGain[b]->get(), q = pQ[b]->get();
        if (t == seen_[b][0] && f == seen_[b][1] && g == seen_[b][2] && q == seen_[b][3]) continue;
        seen_[b][0] = t; seen_[b][1] = f; seen_[b][2] = g; seen_[b][3] = q;
        const int type = (int)std::lround(t);
        active_[b] = type >= LowCut || g != 0.0f;
        for (int c = 0; c < 2; ++c) design(bq_[b][c], type, f, g, q, sr_);
    }
}

double ContourEq::magnitudeDb(double hz) const {
    double mag = 1.0;
    for (int b = 0; b < kBands; ++b) {
        const int type = pType[b]->getInt();
        const float g = pGain[b]->get();
        if (type < LowCut && g == 0.0f) continue;
        dsp::Biquad bq;
        design(bq, type, pFreq[b]->get(), g, pQ[b]->get(), sr_);
        mag *= bq.magnitude(hz, sr_);
    }
    return 20.0 * std::log10(std::max(mag, 1.0e-9)) + pOut->get();
}

void ContourEq::process(ProcessContext& c) {
    update();
    float* L = c.io[0];
    float* R = c.io[1];
    for (int b = 0; b < kBands; ++b) {
        if (!active_[b]) continue;
        for (int i = 0; i < c.numSamples; ++i) { L[i] = bq_[b][0].process(L[i]); R[i] = bq_[b][1].process(R[i]); }
    }
    const float g = dbToGain(pOut->get());
    if (!primed_) { out_.snap(g); primed_ = true; }
    out_.target(g, c.numSamples);
    if (!(out_.left == 0 && g == 1.0f))
        for (int i = 0; i < c.numSamples; ++i) { const float gg = out_.next(); L[i] *= gg; R[i] *= gg; }
    scope_.push(L, R, c.numSamples);
}

std::vector<Preset> ContourEq::factoryPresets() const {
    return buildPresets(*this, {
        {"Flat", {}},
        {"Low cut 80 Hz", {{"b1type", LowCut}, {"b1freq", 80}, {"b1q", 0.71f}}},
        {"Vocal presence", {{"b1type", LowCut}, {"b1freq", 100}, {"b1q", 0.71f}, {"b3freq", 300}, {"b3gain", -3}, {"b5freq", 3500}, {"b5gain", 3.5f}, {"b7gain", 2.5f}}},
        {"Kick punch", {{"b1type", Peak}, {"b1freq", 60}, {"b1gain", 4}, {"b3freq", 350}, {"b3gain", -5}, {"b3q", 1.4f}, {"b6freq", 4000}, {"b6gain", 3}}},
        {"Air", {{"b7type", HighShelf}, {"b7freq", 10000}, {"b7gain", 5}}},
        {"Telephone", {{"b1type", LowCut}, {"b1freq", 400}, {"b7type", HighCut}, {"b7freq", 3200}, {"b4gain", 4}}},
        {"Smile", {{"b1gain", 5}, {"b4freq", 800}, {"b4gain", -4}, {"b4q", 0.6f}, {"b7gain", 5}}},
    });
}

// ------------------------------------------------------------------ Squeeze

Squeeze::Squeeze() {
    pThresh = addFloat("thresh", "Threshold", -60, 0, -18, "dB");
    pRatio = addFloat("ratio", "Ratio", 1, 30, 4, ":1", 2.5f);
    pAttack = addFloat("attack", "Attack", 0.1f, 300, 10, "ms", 3.0f);
    pRelease = addFloat("release", "Release", 5, 2000, 120, "ms", 3.0f);
    pKnee = addFloat("knee", "Knee", 0, 24, 6, "dB");
    pMakeup = addFloat("makeup", "Make-up gain", 0, 36, 0, "dB");
    pSide = addBool("side", "Sidechain input", false);
}

float Squeeze::curveDb(float in, float thresh, float ratio, float knee) {
    const float over = in - thresh;
    if (knee > 0.0f && std::fabs(over) <= knee * 0.5f) {
        const float x = over + knee * 0.5f;
        return in + (1.0f / ratio - 1.0f) * x * x / (2.0f * knee);
    }
    return over <= 0.0f ? in : thresh + over / ratio;
}

void Squeeze::process(ProcessContext& c) {
    const float thresh = pThresh->get(), ratio = pRatio->get(), knee = pKnee->get();
    const float makeup = pMakeup->get();
    const float att = (float)(1.0 - std::exp(-1.0 / (0.001 * pAttack->get() * sr_)));
    const float rel = (float)(1.0 - std::exp(-1.0 / (0.001 * pRelease->get() * sr_)));
    const bool useSide = pSide->getBool() && c.sidechain[0] != nullptr;
    const float* dl = useSide ? c.sidechain[0] : c.io[0];
    const float* dr = useSide ? c.sidechain[1] : c.io[1];
    float peakGr = 0.0f;
    for (int i = 0; i < c.numSamples; ++i) {
        const float level = std::max(std::fabs(dl[i]), std::fabs(dr[i]));
        const float inDb = level > 1.0e-7f ? 20.0f * std::log10(level) : -140.0f;
        const float target = std::max(0.0f, inDb - curveDb(inDb, thresh, ratio, knee));
        envDb_ += (target - envDb_) * (target > envDb_ ? att : rel);
        if (envDb_ < 1.0e-6f) envDb_ = 0.0f;
        peakGr = std::max(peakGr, envDb_);
        const float g = dbToGain(makeup - envDb_);
        c.io[0][i] *= g;
        c.io[1][i] *= g;
    }
    grPub_.store(peakGr, std::memory_order_relaxed);
}

std::vector<Preset> Squeeze::factoryPresets() const {
    return buildPresets(*this, {
        {"Default", {}},
        {"Gentle glue", {{"thresh", -14}, {"ratio", 2}, {"attack", 30}, {"release", 200}, {"knee", 10}, {"makeup", 2}}},
        {"Vocal leveller", {{"thresh", -22}, {"ratio", 3.5f}, {"attack", 8}, {"release", 150}, {"knee", 8}, {"makeup", 5}}},
        {"Drum smack", {{"thresh", -20}, {"ratio", 6}, {"attack", 25}, {"release", 80}, {"knee", 2}, {"makeup", 6}}},
        {"Sidechain pump", {{"thresh", -30}, {"ratio", 10}, {"attack", 1}, {"release", 180}, {"knee", 0}, {"side", 1}}},
        {"Hard limit-ish", {{"thresh", -10}, {"ratio", 20}, {"attack", 0.5f}, {"release", 60}, {"knee", 0}, {"makeup", 6}}},
    });
}

// ------------------------------------------------------------------ Lid

Lid::Lid() {
    pGain = addFloat("gain", "Input gain", 0, 24, 0, "dB");
    pCeiling = addFloat("ceiling", "Ceiling", -12, 0, -0.3f, "dB");
    pRelease = addFloat("release", "Release", 1, 1000, 80, "ms", 3.0f);
    pGate = addFloat("gate", "Gate threshold (lowest = off)", -90, -20, -90, "dB");
    pGateRelease = addFloat("gaterel", "Gate release", 5, 1000, 120, "ms", 3.0f);
    // the compressor in front of the limiter (off until switched on)
    pComp = addBool("comp", "Compressor", false);
    pCompThresh = addFloat("cthresh", "Compressor threshold", -60, 0, -18, "dB");
    pCompRatio = addFloat("cratio", "Compressor ratio", 1, 30, 3, ":1", 2.5f);
    pCompKnee = addFloat("cknee", "Compressor knee", 0, 24, 6, "dB");
    pCompAttack = addFloat("cattack", "Compressor attack", 0.1f, 300, 10, "ms", 3.0f);
    pCompRelease = addFloat("crelease", "Compressor release", 5, 2000, 150, "ms", 3.0f);
}

std::vector<ParamGroup> Lid::paramGroups() const {
    return {{"Limiter", {"gain", "ceiling", "release"}}, {"Gate", {"gate", "gaterel"}},
            {"Compressor", {"comp", "cthresh", "cratio", "cknee", "cattack", "crelease"}}};
}

void Lid::prepare(double sr) {
    sr_ = sr;
    look_ = std::max(8, (int)std::lround(0.005 * sr));      // 5 ms of look-ahead
    for (auto& d : dl_) d.assign((size_t)look_, 0.0f);
    avg_.assign((size_t)look_, 1.0f);
    dqN_.assign((size_t)look_ + 2, 0);
    dqV_.assign((size_t)look_ + 2, 1.0f);
    reset();
}

void Lid::reset() {
    for (auto& d : dl_) std::fill(d.begin(), d.end(), 0.0f);
    std::fill(avg_.begin(), avg_.end(), 1.0f);
    avgSum_ = (double)look_;
    dqHead_ = dqCount_ = 0;
    w_ = 0;
    n_ = 0;
    env_ = 1.0f;
    gateEnv_ = 1.0f;
    gateLevel_ = 0.0f;
    compDb_ = 0.0f;
    compDet_ = 0.0f;
}

void Lid::process(ProcessContext& c) {
    const float inGain = dbToGain(pGain->get());
    const float ceiling = dbToGain(pCeiling->get());
    const float rel = (float)(1.0 - std::exp(-1.0 / (0.001 * pRelease->get() * sr_)));
    const float gateDb = pGate->get();
    const bool gateOn = gateDb > -89.5f;
    const float gateThr = dbToGain(gateDb);
    const float gateRel = (float)std::exp(-1.0 / (0.001 * pGateRelease->get() * sr_));
    const float gateAtt = (float)(1.0 - std::exp(-1.0 / (0.002 * sr_)));
    const int cap = (int)dqN_.size();
    // the running sum is rebuilt each block so rounding cannot build up
    avgSum_ = 0.0;
    for (float v : avg_) avgSum_ += v;
    float minGain = 1.0f, minGate = 1.0f;
    const bool compOn = pComp->getBool();
    const float cThresh = pCompThresh->get(), cRatio = pCompRatio->get(), cKnee = pCompKnee->get();
    const float cAtt = (float)(1.0 - std::exp(-1.0 / (0.001 * pCompAttack->get() * sr_)));
    const float cRel = (float)(1.0 - std::exp(-1.0 / (0.001 * pCompRelease->get() * sr_)));
    const float cDetFall = (float)std::exp(-1.0 / (0.020 * sr_));
    float compPeak = 0.0f;

    for (int i = 0; i < c.numSamples; ++i) {
        float xl = c.io[0][i] * inGain, xr = c.io[1][i] * inGain;
        if (compOn || compDb_ > 0.0f) {
            // the compressor works on the peak level, ahead of the gate and the limiter; switched off, what
            // reduction it still holds dies away at its release time
            // (a peak follower that falls over 20 ms, so that a steady tone reads as its peak, not as its waveform)
            compDet_ = std::max(std::max(std::fabs(xl), std::fabs(xr)), compDet_ * cDetFall);
            const float inDb = gainToDb(compDet_);
            const float target = compOn ? std::max(0.0f, inDb - Squeeze::curveDb(inDb, cThresh, cRatio, cKnee)) : 0.0f;
            compDb_ += (target - compDb_) * (target > compDb_ ? cAtt : cRel);
            if (compDb_ < 1.0e-6f) compDb_ = 0.0f;
            compPeak = std::max(compPeak, compDb_);
            const float cg = dbToGain(-compDb_);
            xl *= cg; xr *= cg;
        }
        if (gateOn) {
            const float lvl = std::max(std::fabs(xl), std::fabs(xr));
            gateLevel_ = std::max(lvl, gateLevel_ * gateRel);
            const float target = gateLevel_ >= gateThr ? 1.0f : 0.0f;
            gateEnv_ += (target - gateEnv_) * gateAtt;
            xl *= gateEnv_; xr *= gateEnv_;
            minGate = std::min(minGate, gateEnv_);
        }
        const float peak = std::max(std::fabs(xl), std::fabs(xr));
        const float req = peak > ceiling ? ceiling / peak : 1.0f;

        // sliding minimum of the required gain over the last look_ samples
        while (dqCount_ > 0 && dqV_[(size_t)((dqHead_ + dqCount_ - 1) % cap)] >= req) --dqCount_;
        const int tail = (dqHead_ + dqCount_) % cap;
        dqN_[(size_t)tail] = n_;
        dqV_[(size_t)tail] = req;
        ++dqCount_;
        while (dqN_[(size_t)dqHead_] <= n_ - look_) { dqHead_ = (dqHead_ + 1) % cap; --dqCount_; }
        const float hold = dqV_[(size_t)dqHead_];

        // falls at once, recovers at the release rate, then a moving average rounds the corners
        env_ = std::min(hold, env_ + (1.0f - env_) * rel);
        avgSum_ += (double)env_ - (double)avg_[(size_t)w_];
        avg_[(size_t)w_] = env_;
        const float g = (float)std::min(1.0, avgSum_ / (double)look_);
        minGain = std::min(minGain, g);

        // the audio is read look_-1 samples late, by which time the gain has fully arrived
        dl_[0][(size_t)w_] = xl;
        dl_[1][(size_t)w_] = xr;
        const int r = (w_ + 1) % look_;
        c.io[0][i] = clampT(dl_[0][(size_t)r] * g, -ceiling, ceiling);
        c.io[1][i] = clampT(dl_[1][(size_t)r] * g, -ceiling, ceiling);
        w_ = r;
        ++n_;
    }
    grPub_.store(-gainToDb(minGain) + compPeak, std::memory_order_relaxed);        // the limiter's and the compressor's together
    compPub_.store(compPeak, std::memory_order_relaxed);
    gatePub_.store(minGate, std::memory_order_relaxed);
}

std::vector<Preset> Lid::factoryPresets() const {
    return buildPresets(*this, {
        {"Master safety", {}},
        {"Loud master", {{"gain", 6}, {"ceiling", -0.3f}, {"release", 120}}},
        {"Transparent", {{"gain", 0}, {"ceiling", -1}, {"release", 250}}},
        {"Drum bus", {{"gain", 4}, {"ceiling", -0.5f}, {"release", 40}}},
        {"Gate noisy input", {{"gate", -50}, {"gaterel", 200}}},
        {"Compress and limit", {{"comp", 1}, {"cthresh", -18}, {"cratio", 3}, {"gain", 3}}},
        {"Vocal: level, then catch the peaks", {{"comp", 1}, {"cthresh", -22}, {"cratio", 4}, {"cattack", 5}, {"crelease", 180}, {"gain", 4}, {"ceiling", -1}}},
        {"Bass: firm and even", {{"comp", 1}, {"cthresh", -20}, {"cratio", 6}, {"cattack", 20}, {"crelease", 120}, {"gain", 3}}},
        {"Glue for the master", {{"comp", 1}, {"cthresh", -14}, {"cratio", 2}, {"cknee", 12}, {"cattack", 30}, {"crelease", 250}, {"gain", 2}}},
    });
}

} // namespace pulp
