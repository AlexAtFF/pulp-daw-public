// Talker: a channel vocoder. The track's own sound is the modulator; the carrier is the sidechain input,
// or a built-in saw chord, pulse or noise when nothing is routed there.
#include "pulp/Vocal.h"
#include <complex>

namespace pulp {
using namespace vocal;

namespace {
constexpr float kMakeup = 8.0f;             // the product of two band levels is small; this brings it back up (18 dB)
constexpr float kSibilanceLevel = 0.6f;     // level of the noise that stands in for the carrier on sibilants

// One fourth-order Butterworth band-pass between f1 and f2 as two two-pole sections (band outputs chained).
// norm scales the second section's band output to unity gain at the centre; quad scales its low output to
// the same size, a quarter turn behind, which gives an envelope without ripple.
void designBand(double f1, double f2, double sr, float g[2], float k[2], float& norm, float& quad) {
    f2 = clampT(f2, 30.0, 0.47 * sr);
    f1 = clampT(f1, 10.0, f2 / 1.02);
    const double o1 = std::tan(kPi * f1 / sr), o2 = std::tan(kPi * f2 / sr), bw = o2 - o1, w2 = o1 * o2;
    const std::complex<double> p(-0.70710678118654752, 0.70710678118654752);
    const std::complex<double> pb = p * bw, disc = std::sqrt(pb * pb - 4.0 * w2);
    const std::complex<double> s[2] = {0.5 * (pb + disc), 0.5 * (pb - disc)};
    double w[2];
    for (int i = 0; i < 2; ++i) {
        w[i] = std::abs(s[i]);
        g[i] = (float)w[i];
        k[i] = (float)clampT(-2.0 * s[i].real() / w[i], 0.01, 4.0);
    }
    norm = (float)(bw * bw / (w[0] * w[1]));
    quad = (float)(std::sqrt(w2) / w[1]);
}
inline double blep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}
inline void flush(float& v) { if (std::fabs(v) < 1.0e-18f) v = 0.0f; }
}

Talker::Talker() {
    ParamInfo b;
    b.id = "bands"; b.name = "Bands"; b.min = 8; b.max = kMaxBands; b.def = 20; b.steps = kMaxBands - 8 + 1;
    pBands = add(b);
    pLow = addFloat("low", "Lowest band", 50, 500, 100, "Hz", 2.0f);
    pHigh = addFloat("high", "Highest band", 2000, 16000, 8000, "Hz", 2.0f);
    pWidth = addFloat("width", "Band width", 0.3f, 2.0f, 1.0f, "x");
    pFormant = addFloat("formant", "Formant shift", -12, 12, 0, "st");
    pAttack = addFloat("attack", "Envelope attack", 0.1f, 200, 2, "ms", 3.0f);
    pRelease = addFloat("release", "Envelope release", 2, 1000, 40, "ms", 3.0f);
    pSibil = addFloat("sibil", "Unvoiced noise", 0, 1, 0.5f, "%");
    pAir = addFloat("air", "Air (dry highs)", 0, 1, 0, "%");
    pAirFreq = addFloat("airfreq", "Air from", 2000, 12000, 6000, "Hz", 2.0f);
    pSide = addBool("side", "Carrier from sidechain", true);
    pCarrier = addChoice("carrier", "Built-in carrier", {"Saw", "Pulse", "Noise"}, Saw);
    pChord = addChoice("chord", "Built-in chord", {"Single note", "Octaves", "Fifth", "Major", "Minor", "Suspended"}, 2);
    ParamInfo k;
    k.id = "pitch"; k.name = "Built-in pitch"; k.min = 24; k.max = 84; k.def = 45; k.steps = 61; k.unit = "key";
    pPitch = add(k);
    pOut = addFloat("out", "Output level", -24, 24, 0, "dB");
    pMix = addFloat("mix", "Dry / wet", 0, 1, 1, "%");
}

std::vector<ParamGroup> Talker::paramGroups() const {
    return {{"Bands", {"bands", "low", "high", "width", "formant"}},
            {"Envelope", {"attack", "release"}},
            {"Consonants", {"sibil", "air", "airfreq"}},
            {"Carrier", {"side", "carrier", "chord", "pitch"}},
            {"Output", {"out", "mix"}}};
}

void Talker::prepare(double sr) {
    sr_ = sr;
    hfDet_.set(3500.0f, 0.707f, (float)sr);
    lfDet_.set(1200.0f, 0.707f, (float)sr);
    airSeen_ = -1.0f;
    reset();
}

void Talker::reset() {
    for (auto& b : band_) {
        std::memset(b.anS, 0, sizeof b.anS);
        std::memset(b.syS, 0, sizeof b.syS);
        b.env = 0.0f;
        b.idle = true;
    }
    for (int i = 0; i < kMaxOsc; ++i) phase_[i] = std::fmod(0.137 * (i + 1), 1.0);
    carNoise_[0] = dsp::Noise(0x51F15EEDu);
    carNoise_[1] = dsp::Noise(0x0BADC0DEu);
    sibNoise_ = dsp::Noise(0x7E57AB1Eu);
    hfDet_.reset(); lfDet_.reset();
    for (auto& a : air_) a.reset();
    hfP_ = lfP_ = unv_ = 0.0f;
    bands_ = 0;
    pendingBands_ = 0;
    primed_ = false;
}

void Talker::bandEdges(int band, double& lo, double& hi) const {
    const int n = clampT((int)std::lround(pBands->get()), 8, (int)kMaxBands);
    const double l = clampT((double)pLow->get(), 20.0, 0.2 * sr_), h = clampT((double)pHigh->get(), l * 2.0, 0.45 * sr_);
    const double oct = std::log2(h / l) / n, hw = 0.5 * oct * pWidth->get();
    const double fc = l * std::pow(2.0, (band + 0.5) * oct);
    lo = fc * std::pow(2.0, -hw);
    hi = fc * std::pow(2.0, hw);
}

void Talker::layout(int bands, float lo, float hi, float width, float formant) {
    seen_[0] = lo; seen_[1] = hi; seen_[2] = width; seen_[3] = formant;
    const double l = clampT((double)lo, 20.0, 0.2 * sr_), h = clampT((double)hi, l * 2.0, 0.45 * sr_);
    const double oct = std::log2(h / l) / bands, hw = 0.5 * oct * width;
    const double shift = std::pow(2.0, (double)formant / 12.0);
    const double up = std::pow(2.0, hw), down = 1.0 / up;
    for (int b = 0; b < bands; ++b) {
        Band& B = band_[b];
        const double fc = l * std::pow(2.0, (b + 0.5) * oct);
        float g[2], k[2];
        designBand(fc * down, fc * up, sr_, g, k, B.anNorm, B.anQuad);
        for (int s = 0; s < 2; ++s) {
            Sec& S = B.an[s];
            S.g = g[s]; S.k = k[s];
            S.a1 = 1.0f / (1.0f + S.g * (S.g + S.k)); S.a2 = S.g * S.a1; S.a3 = S.g * S.a2;
        }
        const double fs = fc * shift;
        B.synthOn = fs * up <= 0.47 * sr_ && fs * down >= 15.0;
        float unused = 1.0f;
        designBand(fs * down, fs * up, sr_, g, k, B.syNorm, unused);
        for (int s = 0; s < 2; ++s) {
            Sec& S = B.sy[s];
            S.g = g[s]; S.k = k[s];
            S.a1 = 1.0f / (1.0f + S.g * (S.g + S.k)); S.a2 = S.g * S.a1; S.a3 = S.g * S.a2;
        }
    }
}

// The built-in carrier: a chord of band-limited saws or narrow pulses (one oscillator of each note per side,
// a few cents apart), or noise.
void Talker::renderCarrier(float* l, float* r, int n) {
    const int type = pCarrier->getInt();
    if (type == NoiseCarrier) {
        // loud: each band takes only a sliver of white noise, where a saw puts whole harmonics
        for (int i = 0; i < n; ++i) { l[i] = 2.4f * carNoise_[0].next(); r[i] = 2.4f * carNoise_[1].next(); }
        return;
    }
    static const int kChords[6][4] = {{0, -1, -1, -1}, {0, 12, -1, -1}, {0, 7, 12, -1}, {0, 4, 7, 12}, {0, 3, 7, 12}, {0, 5, 7, 12}};
    const int* iv = kChords[clampT(pChord->getInt(), 0, 5)];
    int notes = 0;
    while (notes < 4 && iv[notes] >= 0) ++notes;
    const double key = (double)std::lround(pPitch->get());
    const float amp = 0.7f / std::sqrt((float)notes);
    const double width = 0.15;
    for (int i = 0; i < n; ++i) { l[i] = 0.0f; r[i] = 0.0f; }
    for (int j = 0; j < notes; ++j) {
        const double hz = keyToHz(key + iv[j]);
        for (int side = 0; side < 2; ++side) {
            const double dt = std::min(0.45, hz * (side ? 1.0017 : 0.9983) / sr_);        // three cents either way
            double ph = phase_[j * 2 + side];
            float* dst = side ? r : l;
            for (int i = 0; i < n; ++i) {
                double v;
                if (type == Saw) v = 2.0 * ph - 1.0 - blep(ph, dt);
                else {
                    double t2 = ph + (1.0 - width);
                    if (t2 >= 1.0) t2 -= 1.0;
                    v = (ph < width ? 1.0 : -1.0) + blep(ph, dt) - blep(t2, dt) - (2.0 * width - 1.0);
                }
                dst[i] += amp * (float)v;
                ph += dt;
                if (ph >= 1.0) ph -= 1.0;
            }
            phase_[j * 2 + side] = ph;
        }
    }
}

void Talker::process(ProcessContext& c) {
    const int n = c.numSamples;
    float* L = c.io[0];
    float* R = c.io[1];
    const int wantBands = clampT((int)std::lround(pBands->get()), 8, (int)kMaxBands);
    const float lo = pLow->get(), hi = pHigh->get(), width = pWidth->get(), formant = pFormant->get();
    const float outGain = dbToGain(pOut->get()) * kMakeup, mix = pMix->get(), airLevel = pAir->get();
    if (!primed_) {
        bands_ = wantBands;
        layout(bands_, lo, hi, width, formant);
        out_.snap(outGain); mix_.snap(mix); airGain_.snap(airLevel); fade_.snap(1.0f);
        primed_ = true;
    } else if (pendingBands_ != 0 && fade_.left == 0 && fade_.cur == 0.0f) {
        // the number of bands changed and the old bank has faded out: the new bank starts from rest and fades in
        bands_ = wantBands;
        pendingBands_ = 0;
        for (auto& b : band_) { std::memset(b.anS, 0, sizeof b.anS); std::memset(b.syS, 0, sizeof b.syS); b.env = 0.0f; b.idle = true; }
        layout(bands_, lo, hi, width, formant);
        fade_.target(1.0f, 4 * n);
    } else if (lo != seen_[0] || hi != seen_[1] || width != seen_[2] || formant != seen_[3]) {
        layout(bands_, lo, hi, width, formant);
    }
    if (wantBands != bands_ && pendingBands_ == 0) { pendingBands_ = wantBands; fade_.target(0.0f, 4 * n); }     // about 5 ms each way
    out_.target(outGain, n);
    mix_.target(mix, n);
    airGain_.target(airLevel, n);

    // modulator, and how unvoiced it is (share of its energy above 3.5 kHz against below 1.2 kHz)
    float mod[kQuantum], u[kQuantum], car[2][kQuantum], wet[2][kQuantum], env[kQuantum];
    const float sibil = pSibil->get();
    const float cP = (float)(1.0 - std::exp(-1.0 / (0.004 * sr_)));
    const float cUp = (float)(1.0 - std::exp(-1.0 / (0.003 * sr_))), cDown = (float)(1.0 - std::exp(-1.0 / (0.015 * sr_)));
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(L[i])) L[i] = 0.0f;      // a broken sample upstream must not get stuck in the filters
        if (!std::isfinite(R[i])) R[i] = 0.0f;
        const float m = 0.5f * (L[i] + R[i]);
        mod[i] = m;
        float lp, bp, hp;
        hfDet_.process(m, lp, bp, hp);
        hfP_ += (hp * hp - hfP_) * cP;
        lfDet_.process(m, lp, bp, hp);
        lfP_ += (lp * lp - lfP_) * cP;
        const float tot = hfP_ + lfP_;
        const float target = tot > 1.0e-10f ? clampT((hfP_ / tot - 0.4f) / 0.3f, 0.0f, 1.0f) : 0.0f;
        unv_ += (target - unv_) * (target > unv_ ? cUp : cDown);
        u[i] = unv_ * sibil;
        wet[0][i] = 0.0f;
        wet[1][i] = 0.0f;
    }
    flush(hfDet_.ic1); flush(hfDet_.ic2); flush(lfDet_.ic1); flush(lfDet_.ic2);
    flush(hfP_); flush(lfP_); flush(unv_);

    // carrier
    const bool useSide = pSide->getBool() && c.sidechain[0] != nullptr && c.sidechain[1] != nullptr;
    if (useSide) {
        for (int i = 0; i < n; ++i) {
            car[0][i] = std::isfinite(c.sidechain[0][i]) ? c.sidechain[0][i] : 0.0f;
            car[1][i] = std::isfinite(c.sidechain[1][i]) ? c.sidechain[1][i] : 0.0f;
        }
    } else {
        renderCarrier(car[0], car[1], n);
    }
    for (int i = 0; i < n; ++i) {
        if (u[i] <= 0.0f) continue;
        const float nz = kSibilanceLevel * sibNoise_.next() * u[i];
        car[0][i] = car[0][i] * (1.0f - u[i]) + nz;
        car[1][i] = car[1][i] * (1.0f - u[i]) + nz;
    }
    const bool monoCar = std::memcmp(car[0], car[1], sizeof(float) * (size_t)n) == 0;

    const float att = (float)(1.0 - std::exp(-1.0 / (0.001 * pAttack->get() * sr_)));
    const float rel = (float)(1.0 - std::exp(-1.0 / (0.001 * pRelease->get() * sr_)));
    for (int b = 0; b < bands_; ++b) {
        Band& B = band_[b];
        {   // analysis: band-pass, envelope from the band output and its quarter-turn partner, attack and release
            const Sec s0 = B.an[0], s1 = B.an[1];
            float a0 = B.anS[0][0], a1 = B.anS[0][1], b0 = B.anS[1][0], b1 = B.anS[1][1], e = B.env;
            const float norm = B.anNorm, quad = B.anNorm * B.anQuad;
            float peak = 0.0f;
            for (int i = 0; i < n; ++i) {
                float v3 = mod[i] - a1;
                float v1 = s0.a1 * a0 + s0.a2 * v3, v2 = a1 + s0.a2 * a0 + s0.a3 * v3;
                a0 = 2.0f * v1 - a0; a1 = 2.0f * v2 - a1;
                v3 = v1 - b1;
                v1 = s1.a1 * b0 + s1.a2 * v3; v2 = b1 + s1.a2 * b0 + s1.a3 * v3;
                b0 = 2.0f * v1 - b0; b1 = 2.0f * v2 - b1;
                const float y = v1 * norm, q = v2 * quad;
                const float inst = std::sqrt(y * y + q * q);
                e += (inst - e) * (inst > e ? att : rel);
                env[i] = e;
                peak = std::max(peak, e);
            }
            flush(a0); flush(a1); flush(b0); flush(b1);
            if (peak < 1.0e-7f) e = 0.0f;
            B.anS[0][0] = a0; B.anS[0][1] = a1; B.anS[1][0] = b0; B.anS[1][1] = b1; B.env = e;
            if (peak < 1.0e-7f || !B.synthOn) {
                if (!B.idle) { std::memset(B.syS, 0, sizeof B.syS); B.idle = true; }
                continue;
            }
        }
        B.idle = false;
        const Sec s0 = B.sy[0], s1 = B.sy[1];
        const float norm = B.syNorm;
        for (int ch = 0; ch < (monoCar ? 1 : 2); ++ch) {
            float a0 = B.syS[ch][0][0], a1 = B.syS[ch][0][1], b0 = B.syS[ch][1][0], b1 = B.syS[ch][1][1];
            const float* x = car[ch];
            float* y = wet[ch];
            for (int i = 0; i < n; ++i) {
                float v3 = x[i] - a1;
                float v1 = s0.a1 * a0 + s0.a2 * v3, v2 = a1 + s0.a2 * a0 + s0.a3 * v3;
                a0 = 2.0f * v1 - a0; a1 = 2.0f * v2 - a1;
                v3 = v1 - b1;
                v1 = s1.a1 * b0 + s1.a2 * v3; v2 = b1 + s1.a2 * b0 + s1.a3 * v3;
                b0 = 2.0f * v1 - b0; b1 = 2.0f * v2 - b1;
                y[i] += v1 * norm * env[i];
            }
            flush(a0); flush(a1); flush(b0); flush(b1);
            B.syS[ch][0][0] = a0; B.syS[ch][0][1] = a1; B.syS[ch][1][0] = b0; B.syS[ch][1][1] = b1;
        }
        if (monoCar) std::memcpy(B.syS[1], B.syS[0], sizeof B.syS[0]);
    }
    if (monoCar) std::memcpy(wet[1], wet[0], sizeof(float) * (size_t)n);

    // air: the dry highs, for consonants that the bands smear
    const float airFreq = pAirFreq->get();
    const bool airOn = airLevel > 0.0f || airGain_.cur > 0.0f;
    if (airOn && airFreq != airSeen_) {
        airSeen_ = airFreq;
        for (auto& a : air_) a.set(airFreq, 0.707f, (float)sr_);
    }
    if (!airOn) for (auto& a : air_) a.reset();

    for (int i = 0; i < n; ++i) {
        const float g = out_.next() * fade_.next(), m = mix_.next(), ag = airGain_.next();
        float wl = wet[0][i] * g, wr = wet[1][i] * g;
        if (airOn) {
            float lp, bp, hp;
            air_[0].process(L[i], lp, bp, hp); wl += hp * ag;
            air_[1].process(R[i], lp, bp, hp); wr += hp * ag;
        }
        L[i] += m * (wl - L[i]);
        R[i] += m * (wr - R[i]);
    }
    if (airOn) for (auto& a : air_) { flush(a.ic1); flush(a.ic2); }

    mSide_.store(useSide ? 1.0f : 0.0f, std::memory_order_relaxed);
    mUnvoiced_.store(unv_, std::memory_order_relaxed);
    mBands_.store((float)bands_, std::memory_order_relaxed);
    scope_.push(L, R, n);
}

std::vector<Preset> Talker::factoryPresets() const {
    return buildPresets(*this, {
        {"Clear speech, 20 bands", {}},
        {"Old machine, 8 bands", {{"bands", 8}, {"low", 150}, {"high", 5000}, {"attack", 5}, {"release", 80}, {"sibil", 0.2f}}},
        {"Fine detail, 40 bands", {{"bands", 40}, {"low", 80}, {"high", 12000}, {"attack", 1}, {"release", 25}, {"sibil", 0.6f}, {"air", 0.15f}}},
        {"Buzzing pulse voice", {{"side", 0}, {"carrier", Pulse}, {"chord", 0}, {"pitch", 40}, {"bands", 24}, {"release", 30}}},
        {"Whispering noise", {{"side", 0}, {"carrier", NoiseCarrier}, {"bands", 28}, {"sibil", 0.0f}, {"high", 10000}}},
        {"Minor chord choir", {{"side", 0}, {"carrier", Saw}, {"chord", 4}, {"pitch", 48}, {"bands", 26}, {"attack", 8}, {"release", 120}}},
        {"Deep giant", {{"formant", -6}, {"bands", 22}, {"release", 60}}},
        {"Small creature", {{"formant", 7}, {"bands", 22}, {"high", 10000}}},
        {"Hollow tubes", {{"bands", 14}, {"width", 0.4f}, {"release", 70}, {"sibil", 0.3f}}},
        {"Slow pad follower", {{"bands", 18}, {"attack", 60}, {"release", 400}, {"sibil", 0.0f}, {"width", 1.4f}}},
        {"Half and half with air", {{"mix", 0.5f}, {"air", 0.3f}, {"airfreq", 5000}}},
    });
}

} // namespace pulp
