// Shared dynamics building blocks, Triband (three-band compressor and maximizer) and Glow (one-knob enhancer).
#include "pulp/Dynamics.h"

namespace pulp {

namespace dyn {

// ------------------------------------------------------------------ shared pieces

void Limiter::prepare(int latency) {
    look = std::max(0, latency) + 1;
    for (auto& d : dl) d.assign((size_t)look, 0.0f);
    avg.assign((size_t)look, 1.0f);
    peak.prepare(look);
    reset();
}

void Limiter::reset() {
    for (auto& d : dl) std::fill(d.begin(), d.end(), 0.0f);
    std::fill(avg.begin(), avg.end(), 1.0f);
    avgSum = (double)look;
    peak.reset();
    w = 0;
    env = 1.0f;
}

void Split3::set(double fLow, double fHigh, double sr) {
    const double q = 0.70710678118654752;       // two Butterworth sections in a row make one Linkwitz-Riley slope
    lp1a.lowpass(fLow, q, sr);  lp1b.lowpass(fLow, q, sr);
    hp1a.highpass(fLow, q, sr); hp1b.highpass(fLow, q, sr);
    lp2a.lowpass(fHigh, q, sr); lp2b.lowpass(fHigh, q, sr);
    hp2a.highpass(fHigh, q, sr); hp2b.highpass(fHigh, q, sr);
    ap.allpass(fHigh, q, sr);                   // what the upper low-pass and high-pass add up to
}

void Split3::reset() {
    for (dsp::Biquad* b : {&lp1a, &lp1b, &hp1a, &hp1b, &lp2a, &lp2b, &hp2a, &hp2b, &ap}) b->reset();
}

void Split3::flushAll() {
    for (dsp::Biquad* b : {&lp1a, &lp1b, &hp1a, &hp1b, &lp2a, &lp2b, &hp2a, &hp2b, &ap}) flush(*b);
}

static double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 80; ++k) {
        const double h = x / (2.0 * k);
        term *= h * h;
        sum += term;
        if (term < 1.0e-18 * sum) break;
    }
    return sum;
}

const Over4::Tables& Over4::tables() {
    static const Tables tab = [] {
        Tables t{};
        // Low-pass at the original Nyquist frequency (an eighth of the fast rate), Kaiser window: about 85 dB down
        // from 28 kHz on (at 48 kHz) and flat to 20 kHz.
        const double fc = 0.125, beta = 8.5;
        const int mid = (kTaps - 1) / 2;
        double h[kTaps], sum = 0.0;
        for (int n = 0; n < kTaps; ++n) {
            const double m = (double)(n - mid);
            const double sinc = n == mid ? 2.0 * fc : std::sin(kTwoPi * fc * m) / (kPi * m);
            const double r = m / (double)mid;
            h[n] = sinc * besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(beta);
            sum += h[n];
        }
        for (int n = 0; n < kTaps; ++n) t.fir[n] = (float)(h[n] / sum);
        for (int p = 0; p < 4; ++p)
            for (int k = 0; k < kUp; ++k) {
                const int idx = p + 4 * k;
                t.phase[p][k] = idx < kTaps ? 4.0f * t.fir[idx] : 0.0f;
            }
        return t;
    }();
    return tab;
}

void Over4::reset() {
    std::memset(up_, 0, sizeof up_);
    std::memset(down_, 0, sizeof down_);
    std::memset(dry_, 0, sizeof dry_);
    upPos_ = downPos_ = dryPos_ = 0;
}

} // namespace dyn

using namespace dyn;

// ------------------------------------------------------------------ Triband

Triband::Triband() {
    pXLow = addFloat("xlow", "Low / mid crossover", 40, 1500, 160, "Hz", 3.0f);
    pXHigh = addFloat("xhigh", "Mid / high crossover", 800, 14000, 3200, "Hz", 3.0f);
    const char* tag[kBands] = {"l", "m", "h"};
    const char* label[kBands] = {"Low ", "Mid ", "High "};
    const float att[kBands] = {30, 15, 5}, rel[kBands] = {200, 150, 100};
    for (int b = 0; b < kBands; ++b) {
        const std::string t = tag[b], n = label[b];
        pThresh[b] = addFloat((t + "thresh").c_str(), (n + "threshold").c_str(), -60, 0, -24, "dB");
        pRatio[b] = addFloat((t + "ratio").c_str(), (n + "ratio (below 1 expands)").c_str(), 0.5f, 20, 2, ":1", 2.5f);
        pAttack[b] = addFloat((t + "attack").c_str(), (n + "attack").c_str(), 0.1f, 300, att[b], "ms", 3.0f);
        pRelease[b] = addFloat((t + "release").c_str(), (n + "release").c_str(), 5, 2000, rel[b], "ms", 3.0f);
        pKnee[b] = addFloat((t + "knee").c_str(), (n + "knee").c_str(), 0, 24, 8, "dB");
        pMakeup[b] = addFloat((t + "makeup").c_str(), (n + "make-up gain").c_str(), -12, 24, 0, "dB");
        pMute[b] = addBool((t + "mute").c_str(), (n + "mute").c_str(), false);
        pSolo[b] = addBool((t + "solo").c_str(), (n + "solo").c_str(), false);
    }
    pMaximize = addFloat("maximize", "Maximize", 0, 18, 0, "dB");
    pCeiling = addFloat("ceiling", "Ceiling", -12, 0, -0.3f, "dB");
    pLimRelease = addFloat("limrel", "Limiter release", 1, 1000, 100, "ms", 3.0f);
    pLimit = addBool("limit", "Limiter", true);
    for (auto& g : gr_) g.store(0.0f);
    prepare(48000.0);
}

std::vector<ParamGroup> Triband::paramGroups() const {
    std::vector<ParamGroup> g;
    g.push_back({"Crossover", {"xlow", "xhigh"}});
    const char* tag[kBands] = {"l", "m", "h"};
    const char* title[kBands] = {"Low band", "Mid band", "High band"};
    for (int b = 0; b < kBands; ++b) {
        const std::string t = tag[b];
        g.push_back({title[b], {t + "thresh", t + "ratio", t + "attack", t + "release", t + "knee", t + "makeup", t + "mute", t + "solo"}});
    }
    g.push_back({"Maximizer", {"maximize", "ceiling", "limrel", "limit"}});
    return g;
}

void Triband::prepare(double sr) {
    sr_ = sr;
    lim_.prepare((int)std::lround(0.004 * sr));     // 4 ms of look-ahead
    seenLo_ = seenHi_ = -1.0f;
    reset();
}

void Triband::reset() {
    for (auto& s : split_) s.reset();
    lim_.reset();
    for (float& e : env_) e = 0.0f;
    primed_ = false;
}

void Triband::process(ProcessContext& c) {
    const float xl = pXLow->get(), xh = pXHigh->get();
    if (xl != seenLo_ || xh != seenHi_) {
        seenLo_ = xl; seenHi_ = xh;
        for (auto& s : split_) s.set(crossoverLow(), crossoverHigh(), sr_);
    }
    bool anySolo = false;
    for (int b = 0; b < kBands; ++b) anySolo = anySolo || pSolo[b]->getBool();
    float thr[kBands], ratio[kBands], knee[kBands], att[kBands], rel[kBands], bandT[kBands];
    for (int b = 0; b < kBands; ++b) {
        thr[b] = pThresh[b]->get();
        ratio[b] = pRatio[b]->get();
        knee[b] = pKnee[b]->get();
        att[b] = timeCoef(pAttack[b]->get(), sr_);
        rel[b] = timeCoef(pRelease[b]->get(), sr_);
        const bool on = !pMute[b]->getBool() && (!anySolo || pSolo[b]->getBool());
        bandT[b] = on ? dbToGain(pMakeup[b]->get()) : 0.0f;
    }
    const float maxT = dbToGain(pMaximize->get()), ceilT = dbToGain(pCeiling->get()), limT = pLimit->getBool() ? 1.0f : 0.0f;
    const float limRel = timeCoef(pLimRelease->get(), sr_);
    if (!primed_) {
        for (int b = 0; b < kBands; ++b) band_[b].snap(bandT[b]);
        max_.snap(maxT); ceil_.snap(ceilT); limMix_.snap(limT);
        primed_ = true;
    }
    for (int b = 0; b < kBands; ++b) band_[b].target(bandT[b], c.numSamples);
    max_.target(maxT, c.numSamples);
    ceil_.target(ceilT, c.numSamples);
    limMix_.target(limT, c.numSamples);
    lim_.beginBlock();

    float peak[kBands] = {0.0f, 0.0f, 0.0f}, minLim = 1.0f;
    float* L = c.io[0];
    float* R = c.io[1];
    for (int i = 0; i < c.numSamples; ++i) {
        float bl[kBands], br[kBands];
        split_[0].process(L[i], bl[0], bl[1], bl[2]);
        split_[1].process(R[i], br[0], br[1], br[2]);
        float outL = 0.0f, outR = 0.0f;
        for (int b = 0; b < kBands; ++b) {
            const float inDb = levelDb(std::max(std::fabs(bl[b]), std::fabs(br[b])));
            // positive: gain taken away; negative: gain added by a ratio below 1
            const float target = clampT(inDb - Squeeze::curveDb(inDb, thr[b], ratio[b], knee[b]), -kMaxBoostDb, 80.0f);
            float& e = env_[b];
            e += (target - e) * (std::fabs(target) > std::fabs(e) ? att[b] : rel[b]);
            if (target == 0.0f && std::fabs(e) < 1.0e-6f) e = 0.0f;
            if (std::fabs(e) > std::fabs(peak[b])) peak[b] = e;
            const float g = band_[b].next() * dbGain(-e);
            outL += bl[b] * g;
            outR += br[b] * g;
        }
        const float mx = max_.next(), ceiling = ceil_.next(), lm = limMix_.next();
        outL *= mx;
        outR *= mx;
        const float g = lim_.process(outL, outR, ceiling, limRel);      // outL and outR are now the delayed samples
        if (lm >= 1.0f) {
            outL = clampT(outL * g, -ceiling, ceiling);
            outR = clampT(outR * g, -ceiling, ceiling);
            minLim = std::min(minLim, g);
        } else if (lm > 0.0f) {
            outL += (clampT(outL * g, -ceiling, ceiling) - outL) * lm;
            outR += (clampT(outR * g, -ceiling, ceiling) - outR) * lm;
            minLim = std::min(minLim, g);
        }
        L[i] = outL;
        R[i] = outR;
    }
    for (auto& s : split_) s.flushAll();
    for (int b = 0; b < kBands; ++b) gr_[b].store(peak[b], std::memory_order_relaxed);
    gr_[3].store(-gainToDb(minLim), std::memory_order_relaxed);
}

std::vector<Preset> Triband::factoryPresets() const {
    return buildPresets(*this, {
        {"Gentle three-way", {}},
        {"Mastering glue", {{"lthresh", -18}, {"lratio", 1.6f}, {"lattack", 40}, {"lrelease", 250}, {"lknee", 12}, {"lmakeup", 1},
                            {"mthresh", -16}, {"mratio", 1.5f}, {"mattack", 25}, {"mrelease", 200}, {"mknee", 12}, {"mmakeup", 1},
                            {"hthresh", -18}, {"hratio", 1.6f}, {"hattack", 10}, {"hrelease", 150}, {"hknee", 12}, {"hmakeup", 1},
                            {"maximize", 1.5f}, {"ceiling", -0.3f}, {"limrel", 150}}},
        {"Loud master", {{"lthresh", -22}, {"lratio", 2.5f}, {"lattack", 20}, {"lrelease", 150}, {"lmakeup", 2.5f},
                         {"mthresh", -20}, {"mratio", 2.2f}, {"mattack", 10}, {"mrelease", 120}, {"mmakeup", 2},
                         {"hthresh", -22}, {"hratio", 2.5f}, {"hattack", 3}, {"hrelease", 80}, {"hmakeup", 2.5f},
                         {"maximize", 6}, {"ceiling", -0.2f}, {"limrel", 80}}},
        {"De-mud", {{"xlow", 150}, {"xhigh", 800}, {"lratio", 1}, {"hratio", 1},
                    {"mthresh", -26}, {"mratio", 3}, {"mattack", 20}, {"mrelease", 180}, {"mknee", 10}, {"mmakeup", -1}}},
        {"Vocal bus", {{"xlow", 200}, {"xhigh", 4500},
                       {"lthresh", -28}, {"lratio", 2.5f}, {"lattack", 20}, {"lrelease", 200},
                       {"mthresh", -22}, {"mratio", 2.2f}, {"mattack", 8}, {"mrelease", 120}, {"mmakeup", 2},
                       {"hthresh", -26}, {"hratio", 3}, {"hattack", 2}, {"hrelease", 80}, {"hmakeup", 1}}},
        {"Drum bus", {{"xlow", 120}, {"xhigh", 5000},
                      {"lthresh", -18}, {"lratio", 4}, {"lattack", 30}, {"lrelease", 120}, {"lknee", 4}, {"lmakeup", 3},
                      {"mthresh", -20}, {"mratio", 3}, {"mattack", 15}, {"mrelease", 100}, {"mknee", 4}, {"mmakeup", 2},
                      {"hthresh", -22}, {"hratio", 2.5f}, {"hattack", 5}, {"hrelease", 80}, {"hknee", 4}, {"hmakeup", 2},
                      {"maximize", 3}, {"ceiling", -0.5f}, {"limrel", 60}}},
        {"Bass anchor", {{"xlow", 140}, {"lthresh", -26}, {"lratio", 6}, {"lattack", 15}, {"lrelease", 200}, {"lknee", 4}, {"lmakeup", 4},
                         {"mratio", 1}, {"hratio", 1}}},
        {"Open transients", {{"lthresh", -30}, {"lratio", 0.75f}, {"lattack", 1}, {"lrelease", 80},
                             {"mthresh", -30}, {"mratio", 0.7f}, {"mattack", 1}, {"mrelease", 60},
                             {"hthresh", -30}, {"hratio", 0.7f}, {"hattack", 0.5f}, {"hrelease", 50}, {"ceiling", -0.5f}}},
        {"Air lift", {{"xhigh", 7000}, {"lratio", 1}, {"mratio", 1}, {"hthresh", -30}, {"hratio", 1.5f}, {"hattack", 3}, {"hrelease", 100}, {"hmakeup", 4}}},
        {"Broadcast voice", {{"xlow", 150}, {"xhigh", 4000},
                             {"lthresh", -30}, {"lratio", 4}, {"lmakeup", 5},
                             {"mthresh", -30}, {"mratio", 3.5f}, {"mattack", 5}, {"mrelease", 100}, {"mmakeup", 5},
                             {"hthresh", -30}, {"hratio", 4}, {"hattack", 2}, {"hrelease", 80}, {"hmakeup", 4},
                             {"maximize", 4}, {"ceiling", -1}}},
        {"Limiter only", {{"lratio", 1}, {"mratio", 1}, {"hratio", 1}, {"maximize", 4}, {"ceiling", -0.3f}}},
    });
}

// ------------------------------------------------------------------ Glow

namespace {
// What the one knob does, per character: low and high lift in dB at full amount, and how fast the saturation comes in
// (it is all the way in at amount 1 / kGlowSat).
const float kGlowLowDb[3] = {4.0f, 2.5f, 1.0f};
const float kGlowHighDb[3] = {1.0f, 2.5f, 4.5f};
const float kGlowSat[3] = {1.6f, 1.3f, 1.0f};
const float kGlowMakeupDb = 16.0f;
const double kGlowLowHz = 150.0, kGlowHighHz = 5000.0;
}

Glow::Glow() {
    pAmount = addFloat("amount", "Amount", 0, 1, 0.35f, "%");
    pCharacter = addChoice("character", "Character", {"Warm", "Balanced", "Bright"}, 1);
    pOut = addFloat("out", "Output gain", -12, 12, 0, "dB");
    for (auto& g : grPub_) g.store(0.0f);
    prepare(48000.0);
}

void Glow::prepare(double sr) {
    sr_ = sr;
    look_ = (int)std::lround(0.0015 * sr);
    lim_.prepare((int)std::lround(0.002 * sr));     // 2 ms of look-ahead
    for (int ch = 0; ch < 2; ++ch) {
        ahead_[ch].prepare(look_ + 1);
        lp_[ch].lowpass(kGlowLowHz, 0.70710678, sr);
        hp_[ch].highpass(kGlowHighHz, 0.70710678, sr);
        lpDet_[ch] = lp_[ch];
        hpDet_[ch] = hp_[ch];
    }
    // the attack fits inside the look-ahead, so peaks are turned down with everything else; the release is slow
    // enough not to follow the wave itself
    const double attMs[3] = {0.5, 0.5, 0.4}, relMs[3] = {180.0, 120.0, 80.0};
    for (int b = 0; b < 3; ++b) { att_[b] = timeCoef(attMs[b], sr); rel_[b] = timeCoef(relMs[b], sr); }
    limRel_ = timeCoef(100.0, sr);
    reset();
}

void Glow::reset() {
    for (int ch = 0; ch < 2; ++ch) { lp_[ch].reset(); hp_[ch].reset(); lpDet_[ch].reset(); hpDet_[ch].reset(); os_[ch].reset(); ahead_[ch].clear(); }
    lim_.reset();
    for (float& e : env_) e = 0.0f;
    primed_ = false;
}

void Glow::process(ProcessContext& c) {
    const float a = pAmount->get();
    const int ch = clampT(pCharacter->getInt(), 0, 2);
    const float shelfT[3] = {a * kGlowLowDb[ch], 0.0f, a * kGlowHighDb[ch]};
    const float makeupT = dbToGain(kGlowMakeupDb * a), wetT = std::min(1.0f, kGlowSat[ch] * a), outT = dbToGain(pOut->get());
    // the compression gets deeper as the knob goes up; at zero the ratio is 1 and nothing happens
    const float thr = -14.0f - 14.0f * a, ratio = 1.0f + 2.5f * a, knee = 10.0f;
    if (!primed_) {
        for (int b = 0; b < 3; ++b) shelf_[b].snap(shelfT[b]);
        makeup_.snap(makeupT); wet_.snap(wetT); out_.snap(outT);
        primed_ = true;
    }
    for (int b = 0; b < 3; ++b) shelf_[b].target(shelfT[b], c.numSamples);
    makeup_.target(makeupT, c.numSamples);
    wet_.target(wetT, c.numSamples);
    out_.target(outT, c.numSamples);
    lim_.beginBlock();

    float minLim = 1.0f, peakGr[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < c.numSamples; ++i) {
        float x[2], band[2][3], det[2][3];
        for (int k = 0; k < 2; ++k) {
            const float in = c.io[k][i];
            det[k][0] = lpDet_[k].process(in);
            det[k][2] = hpDet_[k].process(in);
            det[k][1] = in - det[k][0] - det[k][2];
            ahead_[k].push(in);
            x[k] = ahead_[k].tap(look_);
            band[k][0] = lp_[k].process(x[k]);
            band[k][2] = hp_[k].process(x[k]);
            band[k][1] = x[k] - band[k][0] - band[k][2];
        }
        float gm1[3];
        for (int b = 0; b < 3; ++b) {
            const float inDb = levelDb(std::max(std::fabs(det[0][b]), std::fabs(det[1][b])));
            const float target = std::max(0.0f, inDb - Squeeze::curveDb(inDb, thr, ratio, knee));
            float& e = env_[b];
            e += (target - e) * (target > e ? att_[b] : rel_[b]);
            if (target == 0.0f && e < 1.0e-6f) e = 0.0f;
            peakGr[b] = std::max(peakGr[b], e);
            gm1[b] = dbGain(shelf_[b].next() - e) - 1.0f;
        }
        const float mk = makeup_.next(), w = wet_.next(), og = out_.next();
        float y[2];
        for (int k = 0; k < 2; ++k) {
            // the input plus what each band's gain adds or takes away: with every gain at 1 this is the input itself
            const float s = (x[k] + gm1[0] * band[k][0] + gm1[1] * band[k][1] + gm1[2] * band[k][2]) * mk;
            // Saturation at four times the rate: a tanh curve that rounds the peaks off towards full scale.
            // Only the difference it makes comes back down, scaled by the knob.
            float u[4], r[4];
            os_[k].up(s, u);
            for (int p = 0; p < 4; ++p) {
                r[p] = std::tanh(u[p]) - u[p];
                if (std::fabs(r[p]) < 1.0e-18f) r[p] = 0.0f;       // the dying tail of the filters must not linger as denormals
            }
            y[k] = os_[k].delay(s) + w * os_[k].down(r);
        }
        const float g = lim_.process(y[0], y[1], 1.0f, limRel_);
        minLim = std::min(minLim, g);
        c.io[0][i] = clampT(y[0] * g, -1.0f, 1.0f) * og;
        c.io[1][i] = clampT(y[1] * g, -1.0f, 1.0f) * og;
    }
    for (int k = 0; k < 2; ++k) { flush(lp_[k]); flush(hp_[k]); flush(lpDet_[k]); flush(hpDet_[k]); }
    grPub_[0].store(-gainToDb(minLim), std::memory_order_relaxed);
    for (int b = 0; b < 3; ++b) grPub_[b + 1].store(peakGr[b], std::memory_order_relaxed);
}

std::vector<Preset> Glow::factoryPresets() const {
    return buildPresets(*this, {
        {"Subtle polish", {{"amount", 0.2f}, {"character", 1}}},
        {"Warm and full", {{"amount", 0.45f}, {"character", 0}}},
        {"Bright and present", {{"amount", 0.45f}, {"character", 2}}},
        {"Bass weight", {{"amount", 0.65f}, {"character", 0}}},
        {"Sparkle", {{"amount", 0.3f}, {"character", 2}}},
        {"Loud and proud", {{"amount", 0.8f}, {"character", 1}, {"out", -0.5f}}},
        {"Flat out", {{"amount", 1.0f}, {"character", 1}, {"out", -1.0f}}},
    });
}

} // namespace pulp
