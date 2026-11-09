// Quarter Inch (tape), Glass (tube stages), Splat (fuzz) and Coarse (bit depth and sample rate reduction).
#include "pulp/Tone.h"

namespace pulp {

using tonefx::dbGain;
using tonefx::osTables;

// ------------------------------------------------------------------ Quarter Inch

namespace {
const double kWowHz[2] = {0.55, 0.93}, kWowWeight[2] = {0.7, 0.3};
const double kFlutterHz[3] = {7.1, 11.7, 19.3}, kFlutterWeight[3] = {0.5, 0.3, 0.2};
}

QuarterInch::QuarterInch() {
    pDrive = addFloat("drive", "Drive", 0.0f, 24.0f, 6.0f, "dB");
    pHyst = addFloat("hyst", "Hysteresis", 0.0f, 1.0f, 0.3f, "%");
    pSpeed = addChoice("speed", "Tape speed", {"3.75 ips", "7.5 ips", "15 ips", "30 ips"}, 2);
    pBump = addFloat("bump", "Head bump", 0.0f, 6.0f, 2.0f, "dB");
    pWow = addFloat("wow", "Wow", 0.0f, 1.0f, 0.0f, "%");
    pFlutter = addFloat("flutter", "Flutter", 0.0f, 1.0f, 0.0f, "%");
    pHiss = addFloat("hiss", "Hiss (lowest = off)", -96.0f, -36.0f, -96.0f, "dB");
    pOut = addFloat("out", "Output", -24.0f, 12.0f, 0.0f, "dB");
    pMix = addFloat("mix", "Dry / wet", 0.0f, 1.0f, 1.0f, "%");
    for (auto& ch : ch_) ch.os.setTables(osTables(4, 1));
    prepare(48000.0);
}

void QuarterInch::speedCorners(int speed, double& highHz, double& bumpHz, double& lowHz) {
    static const double high[kSpeeds] = {5500.0, 10000.0, 16000.0, 22000.0};
    static const double bump[kSpeeds] = {45.0, 60.0, 80.0, 110.0};
    static const double low[kSpeeds] = {18.0, 22.0, 28.0, 36.0};
    speed = clampT(speed, 0, kSpeeds - 1);
    highHz = high[speed]; bumpHz = bump[speed]; lowHz = low[speed];
}

void QuarterInch::prepare(double sr) {
    sr_ = sr;
    base_ = (int)std::lround(kDelaySeconds * sr);
    int size = 1;
    while (size < 2 * base_ + 8) size <<= 1;
    for (int ch = 0; ch < 2; ++ch) {
        wet_[ch].assign((size_t)size, 0.0f);
        dry_[ch].assign((size_t)(tonefx::kOsLatency + base_), 0.0f);
    }
    mask_ = size - 1;
    dsp::Biquad q;
    q.highShelf(kEmphasisHz, 0.7071, kEmphasisDb, sr);
    preC_.snap(q);
    q.highShelf(kEmphasisHz, 0.7071, -kEmphasisDb, sr);
    deC_.snap(q);
    reset();
}

void QuarterInch::reset() {
    for (int ch = 0; ch < 2; ++ch) {
        Chan& k = ch_[ch];
        k.os.reset();
        k.sat.reset(0.0, 1.0);
        k.p[0] = k.p[1] = k.p[2] = 0.0;
        k.uPrev = 0.0;
        k.pre.reset(); k.de.reset(); k.bump.reset(); k.lp.reset();
        k.hp.reset(); k.hissTone.reset();
        std::fill(wet_[ch].begin(), wet_[ch].end(), 0.0f);
        std::fill(dry_[ch].begin(), dry_[ch].end(), 0.0f);
    }
    w_ = 0;
    dryPos_ = 0;
    // the transport starts from the same place every time, so a render is always the same
    dsp::Noise seed(0x51EDu);
    for (double& ph : phase_) ph = seed.uni();
    hissL_ = dsp::Noise(0x7A9Eu);
    hissR_ = dsp::Noise(0x1C3Bu);
    bumpSeen_ = -100.0f;
    primed_ = false;
}

void QuarterInch::process(ProcessContext& c) {
    const int n = c.numSamples;
    const float driveT = dbGain(pDrive->get()), hystT = pHyst->get(), wowT = pWow->get(), flutterT = pFlutter->get();
    const float hissT = pHiss->get() <= pHiss->info.min + 0.01f ? 0.0f : dbGain(pHiss->get());
    const float outT = dbGain(pOut->get()), mixT = pMix->get();
    const bool first = !primed_;
    if (first) { drive_.snap(driveT); hyst_.snap(hystT); wow_.snap(wowT); flutter_.snap(flutterT); hiss_.snap(hissT); out_.snap(outT); mix_.snap(mixT); }
    drive_.target(driveT, n);
    hyst_.target(hystT, n);
    wow_.target(wowT, n);
    flutter_.target(flutterT, n);
    hiss_.target(hissT, n);
    out_.target(outT, n);
    mix_.target(mixT, n);

    // The corners that belong to the tape speed glide to a new speed instead of jumping.
    double want[3];
    speedCorners(pSpeed->getInt(), want[0], want[1], want[2]);
    want[0] = std::min(want[0], 0.45 * sr_);
    bool moved = first;
    for (int k = 0; k < 3; ++k) {
        if (first) corner_[k] = want[k];
        else if (corner_[k] != want[k]) {
            corner_[k] *= std::pow(want[k] / corner_[k], 0.08);
            if (std::fabs(corner_[k] / want[k] - 1.0) < 0.002) corner_[k] = want[k];
            moved = true;
        }
    }
    const float bumpDb = pBump->get();
    if (moved || bumpDb != bumpSeen_) {
        bumpSeen_ = bumpDb;
        dsp::Biquad q;
        q.peak(corner_[1], 1.0, bumpDb, sr_);
        if (first) bumpC_.snap(q); else bumpC_.glide(q, n);
        q.lowpass(corner_[0], 0.62, sr_);
        if (first) lpC_.snap(q); else lpC_.glide(q, n);
        for (auto& ch : ch_) { ch.hp.setHz(corner_[2], sr_); ch.hissTone.setHz(1500.0, sr_); }
    }
    primed_ = true;

    const bool hissOn = hissT > 0.0f || hiss_.cur > 0.0f;
    const bool wobble = wowT > 0.0f || flutterT > 0.0f || wow_.cur > 0.0f || flutter_.cur > 0.0f;
    const int latency = tonefx::kOsLatency + base_;
    double phInc[5];
    for (int k = 0; k < 2; ++k) phInc[k] = kWowHz[k] / sr_;
    for (int k = 0; k < 3; ++k) phInc[2 + k] = kFlutterHz[k] / sr_;

    for (int i = 0; i < n; ++i) {
        const double g = drive_.next(), h = hyst_.next();
        const float wow = wow_.next(), flutter = flutter_.next(), hiss = hiss_.next(), outG = out_.next(), mix = mix_.next();
        const double comp = 1.0 / std::sqrt(g);
        bumpC_.step();
        lpC_.step();
        // how far the tape is ahead of or behind its place, in samples; the same for both channels
        double dev = 0.0;
        if (wobble) {
            double a = 0.0, b = 0.0;
            for (int k = 0; k < 2; ++k) a += kWowWeight[k] / (kTwoPi * kWowHz[k]) * std::sin(kTwoPi * phase_[k]);
            for (int k = 0; k < 3; ++k) b += kFlutterWeight[k] / (kTwoPi * kFlutterHz[k]) * std::sin(kTwoPi * phase_[2 + k]);
            dev = sr_ * (kMaxWow * (double)wow * a + kMaxFlutter * (double)flutter * b);
        }
        for (int k = 0; k < 5; ++k) { phase_[k] += phInc[k]; if (phase_[k] >= 1.0) phase_[k] -= 1.0; }

        for (int ch = 0; ch < 2; ++ch) {
            Chan& k = ch_[ch];
            const float x = c.io[ch][i];
            // record: treble lifted before the tape and taken off again after it, so the top saturates first
            float f[4];
            k.os.up((float)k.pre.run(preC_, x), f);
            for (int p = 0; p < 4; ++p) {
                const double u = g * f[p];
                if (u == k.uPrev) {
                    // nothing is moving: what is left on the tape fades, so silence in ends as silence out
                    for (double& pk : k.p) { pk = u + (pk - u) * 0.999; if (std::fabs(pk - u) < 1.0e-20) pk = u; }
                }
                k.uPrev = u;
                f[p] = (float)k.sat.run(hysteresis(u, h, k.p), 1.0);
            }
            double y = (double)k.os.down(f) * comp;
            y = k.de.run(deC_, y);
            y = k.bump.run(bumpC_, y);
            y = k.lp.run(lpC_, y);
            y = k.hp.hp(y);
            // playback: the transport's speed error is a delay that moves
            wet_[ch][(size_t)w_] = (float)y;
            float wet;
            if (dev == 0.0) wet = wet_[ch][(size_t)((w_ - base_) & mask_)];
            else {
                const double delay = (double)base_ + dev;
                const int di = (int)delay;
                const float t = (float)(delay - (double)di) + 1.0f;
                const float a = wet_[ch][(size_t)((w_ - di + 1) & mask_)], b = wet_[ch][(size_t)((w_ - di) & mask_)];
                const float cc = wet_[ch][(size_t)((w_ - di - 1) & mask_)], d = wet_[ch][(size_t)((w_ - di - 2) & mask_)];
                wet = a * ((t - 1.0f) * (t - 2.0f) * (t - 3.0f) * (-1.0f / 6.0f)) + b * (t * (t - 2.0f) * (t - 3.0f) * 0.5f)
                    + cc * (t * (t - 1.0f) * (t - 3.0f) * -0.5f) + d * (t * (t - 1.0f) * (t - 2.0f) * (1.0f / 6.0f));
            }
            if (hissOn) {
                const float white = (ch == 0 ? hissL_ : hissR_).next();
                wet += hiss * (white - 0.7f * (float)k.hissTone.lp(white));
            }
            float& slot = dry_[ch][(size_t)dryPos_];
            const float dry = slot;
            slot = x;
            c.io[ch][i] = dry + (wet * outG - dry) * mix;
        }
        w_ = (w_ + 1) & mask_;
        dryPos_ = (dryPos_ + 1) % latency;
    }
    for (auto& k : ch_) { k.pre.flush(); k.de.flush(); k.bump.flush(); k.lp.flush(); k.hp.flush(); k.hissTone.flush(); }
}

std::vector<Preset> QuarterInch::factoryPresets() const {
    return buildPresets(*this, {
        {"Studio machine", {}},
        {"Fast and clean", {{"drive", 3}, {"hyst", 0.15f}, {"speed", 3}, {"bump", 1.5f}}},
        {"Pushed mixdown", {{"drive", 12}, {"hyst", 0.35f}, {"speed", 2}, {"bump", 2.5f}, {"out", -2}}},
        {"Worn cassette", {{"drive", 9}, {"hyst", 0.6f}, {"speed", 0}, {"bump", 3}, {"wow", 0.5f}, {"flutter", 0.6f}, {"hiss", -58}}},
        {"Slow reel warmth", {{"drive", 8}, {"hyst", 0.4f}, {"speed", 1}, {"bump", 4}, {"wow", 0.12f}, {"flutter", 0.15f}}},
        {"Seasick wobble", {{"drive", 4}, {"speed", 1}, {"wow", 1.0f}, {"flutter", 0.3f}}},
        {"Flutter shimmer", {{"drive", 5}, {"speed", 2}, {"flutter", 1.0f}, {"mix", 0.5f}}},
        {"Saturated to the hilt", {{"drive", 22}, {"hyst", 0.5f}, {"speed", 2}, {"bump", 3}, {"out", -5}}},
        {"Parallel glue", {{"drive", 14}, {"hyst", 0.3f}, {"speed", 2}, {"mix", 0.4f}}},
    });
}

std::vector<ParamGroup> QuarterInch::paramGroups() const {
    return {
        {"Tape", {"drive", "hyst", "speed", "bump"}},
        {"Transport", {"wow", "flutter", "hiss"}},
        {"Output", {"out", "mix"}},
    };
}

// ------------------------------------------------------------------ Glass

Glass::Glass() {
    pStages = addChoice("stages", "Stages", {"One", "Two", "Three"}, 1);
    pDrive = addFloat("drive", "Drive", 0.0f, 36.0f, 12.0f, "dB");
    pBias = addFloat("bias", "Bias (even harmonics)", 0.0f, 1.0f, 0.35f, "%");
    pTone = addFloat("tone", "Tone", -1.0f, 1.0f, 0.0f);
    pSag = addFloat("sag", "Sag", 0.0f, 1.0f, 0.2f, "%");
    pOut = addFloat("out", "Output", -24.0f, 12.0f, -3.0f, "dB");
    pMix = addFloat("mix", "Dry / wet", 0.0f, 1.0f, 1.0f, "%");
    for (int s = 1; s <= kMaxStages; ++s) tab_[s] = osTables(4, s);
    for (auto& ch : ch_) ch.os.setTables(tab_[2]);
}

void Glass::prepare(double sr) {
    sr_ = sr;
    sagAtt_ = (float)(1.0 - std::exp(-1.0 / (0.001 * kSagAttackMs * sr)));
    sagRel_ = (float)(1.0 - std::exp(-1.0 / (0.001 * kSagReleaseMs * sr)));
    for (auto& ch : ch_) {
        for (auto& cp : ch.couple) cp.setHz(15.0, sr * 4.0);
        ch.dc.setHz(8.0, sr);
    }
    reset();
}

void Glass::reset() {
    for (auto& ch : ch_) {
        ch.os.reset();
        for (auto& s : ch.st) s.reset(0.0, kKneeNeg);
        for (auto& cp : ch.couple) cp.reset();
        ch.tone.reset();
        ch.dc.reset();
    }
    dly_.reset();
    dip_.reset();
    sagEnv_ = 0.0f;
    toneSeen_ = -100.0f;
    primed_ = false;
}

void Glass::process(ProcessContext& c) {
    const int n = c.numSamples;
    const float driveT = pDrive->get(), biasT = pBias->get() * 0.9f, sagT = pSag->get();
    const float outT = dbGain(pOut->get()), mixT = pMix->get();
    const bool first = !primed_;
    if (first) { drive_.snap(driveT); bias_.snap(biasT); sag_.snap(sagT); out_.snap(outT); mix_.snap(mixT); }
    drive_.target(driveT, n);
    bias_.target(biasT, n);
    sag_.target(sagT, n);
    out_.target(outT, n);
    mix_.target(mixT, n);
    if (dip_.begin(pStages->getInt(), n)) {
        // a different number of stages is a different length of time spent in them: the way down makes up for it
        for (auto& ch : ch_) {
            ch.os.setTables(tab_[dip_.active + 1]);
            for (auto& s : ch.st) s.reset(biasT, kKneeNeg);
            for (auto& cp : ch.couple) cp.reset();
        }
    }
    const int stages = dip_.active + 1;
    const float toneNow = pTone->get();
    if (toneNow != toneSeen_) {
        toneSeen_ = toneNow;
        dsp::Biquad q;
        q.highShelf(2000.0, 0.7071, (double)toneNow * 9.0, sr_);
        if (first) toneC_.snap(q); else toneC_.glide(q, n);
    }
    primed_ = true;
    const double polarity = (stages & 1) ? -1.0 : 1.0;      // every stage turns the signal over
    float sagGain = 1.0f;

    for (int i = 0; i < n; ++i) {
        const float x[2] = {c.io[0][i], c.io[1][i]};
        const double driveDb = drive_.next(), b = bias_.next();
        const float sag = sag_.next(), outG = out_.next(), mix = mix_.next(), dipG = dip_.gain.next();
        const double total = std::exp(driveDb * 0.11512925464970229);
        const double gs = std::exp(driveDb * 0.11512925464970229 / (double)stages);
        const double comp = 1.0 / std::sqrt(total);
        // sag: the harder the stages are driven, the further the supply drops, slowly
        const float lvl = std::max(std::fabs(x[0]), std::fabs(x[1])) * (float)total;
        sagEnv_ += (lvl > sagEnv_ ? sagAtt_ : sagRel_) * (lvl - sagEnv_);
        sagGain = 1.0f - (float)kSagDepth * sag * (sagEnv_ / (1.0f + sagEnv_));
        const double rest = tonefx::Sig::f(b, kKneeNeg), invSlope = 1.0 / tonefx::Sig::slope(b, kKneeNeg);
        toneC_.step();
        float dry[2];
        dly_.push(x[0], x[1], dry[0], dry[1]);
        for (int ch = 0; ch < 2; ++ch) {
            Chan& k = ch_[ch];
            float f[4];
            k.os.up(x[ch] * sagGain, f);
            for (int p = 0; p < 4; ++p) {
                double v = f[p];
                for (int s = 0; s < stages; ++s) {
                    v = -(k.st[s].run(b + gs * v, kKneeNeg) - rest) * invSlope;
                    v = k.couple[s].hp(v);
                }
                f[p] = (float)(v * polarity);      // turned the right way up before the way down, so a change of stages does not flip what is already in its filter
            }
            double y = (double)k.os.down(f) * comp;
            y = k.tone.run(toneC_, y);
            y = k.dc.hp(y);
            c.io[ch][i] = dry[ch] + ((float)y * dipG * outG - dry[ch]) * mix;
        }
    }
    for (auto& k : ch_) {
        for (auto& cp : k.couple) cp.flush();
        k.tone.flush();
        k.dc.flush();
    }
    flushDenormal(sagEnv_);
    sagPub_.store(-20.0f * std::log10(std::max(sagGain, 1.0e-6f)), std::memory_order_relaxed);
}

std::vector<Preset> Glass::factoryPresets() const {
    return buildPresets(*this, {
        {"Two warm stages", {}},
        {"Single stage sheen", {{"stages", 0}, {"drive", 8}, {"bias", 0.25f}, {"sag", 0.0f}, {"out", -2}}},
        {"Even harmonic bloom", {{"stages", 0}, {"drive", 14}, {"bias", 0.9f}, {"tone", -0.2f}, {"out", -3}}},
        {"Three stage thick", {{"stages", 2}, {"drive", 20}, {"bias", 0.4f}, {"tone", -0.3f}, {"sag", 0.4f}, {"out", -4}}},
        {"Sagging supply", {{"stages", 1}, {"drive", 22}, {"bias", 0.3f}, {"sag", 1.0f}, {"out", -3}}},
        {"Bright edge", {{"stages", 1}, {"drive", 16}, {"bias", 0.2f}, {"tone", 0.6f}, {"sag", 0.1f}, {"out", -5}}},
        {"Dark and heavy", {{"stages", 2}, {"drive", 30}, {"bias", 0.5f}, {"tone", -0.8f}, {"sag", 0.5f}, {"out", -5}}},
        {"Parallel warmth", {{"stages", 1}, {"drive", 18}, {"bias", 0.5f}, {"mix", 0.35f}}},
    });
}

// ------------------------------------------------------------------ Splat

double Splat::Shape::f(double x) const {
    x += off;
    if (x >= 0.0) return clampT(x - tp, 0.0, hi);
    const double u = s * x;
    if (s > 0.0) return clampT(u + tn, -lo, 0.0);
    return clampT(u - tp, 0.0, hi);
}

namespace {
// Antiderivative of clamp(w, L, H) in w.
inline double clampInt(double w, double L, double H) { return w < L ? L * w - 0.5 * L * L : (w > H ? H * w - 0.5 * H * H : 0.5 * w * w); }
}

namespace {
// Integral of the curve without its bias, from 0 to x.
inline double fuzzInt(const Splat::Shape& k, double x) {
    if (x >= 0.0) return clampInt(x - k.tp, 0.0, k.hi) - clampInt(-k.tp, 0.0, k.hi);
    if (k.s > 1.0e-9) return (clampInt(k.s * x + k.tn, -k.lo, 0.0) - clampInt(k.tn, -k.lo, 0.0)) / k.s;
    if (k.s < -1.0e-9) return (clampInt(k.s * x - k.tp, 0.0, k.hi) - clampInt(-k.tp, 0.0, k.hi)) / k.s;
    return 0.0;
}
}

double Splat::Shape::F(double x) const { return off == 0.0 ? fuzzInt(*this, x) : fuzzInt(*this, x + off) - fuzzInt(*this, off); }

Splat::Shape Splat::shapeFor(float sputter, float octave, float asym, double level) {
    Shape k;
    k.off = kBiasShare * (double)asym * level;
    k.s = 1.0 - 2.0 * (double)octave;
    k.tp = 0.8 * (double)sputter;
    k.tn = 1.3 * (double)sputter;
    k.hi = 1.0;
    k.lo = 1.0 - 0.6 * (double)asym;
    return k;
}

Splat::Splat() {
    pFuzz = addFloat("fuzz", "Fuzz", 0.0f, 48.0f, 30.0f, "dB");
    pAsym = addFloat("asym", "Asymmetry", 0.0f, 1.0f, 0.5f, "%");
    pSputter = addFloat("sputter", "Sputter", 0.0f, 1.0f, 0.0f, "%");
    pOctave = addFloat("octave", "Octave up", 0.0f, 1.0f, 0.0f, "%");
    pTone = addFloat("tone", "Tone", 0.0f, 1.0f, 0.5f, "%");
    pLevel = addFloat("level", "Level", -36.0f, 6.0f, -12.0f, "dB");
    pMix = addFloat("mix", "Dry / wet", 0.0f, 1.0f, 1.0f, "%");
    for (auto& ch : ch_) ch.os.setTables(osTables(4, 1));
}

void Splat::prepare(double sr) {
    sr_ = sr;
    for (auto& ch : ch_) { ch.dc.setHz(25.0, sr); ch.lo.setHz(kToneLowHz, sr); ch.hi.setHz(kToneHighHz, sr); }
    envRel_ = 1.0 - std::exp(-1.0 / (0.001 * kBiasReleaseMs * sr));
    reset();
}

void Splat::reset() {
    for (auto& ch : ch_) { ch.os.reset(); ch.x0 = 0.0; ch.dc.reset(); ch.lo.reset(); ch.hi.reset(); }
    env_ = 0.0;
    dly_.reset();
    primed_ = false;
}

void Splat::process(ProcessContext& c) {
    const int n = c.numSamples;
    const float fuzzT = dbGain(pFuzz->get()), levelT = dbGain(pLevel->get());
    if (!primed_) {
        fuzz_.snap(fuzzT); asym_.snap(pAsym->get()); sputter_.snap(pSputter->get()); octave_.snap(pOctave->get());
        tone_.snap(pTone->get()); level_.snap(levelT); mix_.snap(pMix->get());
        primed_ = true;
    }
    fuzz_.target(fuzzT, n);
    asym_.target(pAsym->get(), n);
    sputter_.target(pSputter->get(), n);
    octave_.target(pOctave->get(), n);
    tone_.target(pTone->get(), n);
    level_.target(levelT, n);
    mix_.target(pMix->get(), n);

    for (int i = 0; i < n; ++i) {
        const double g = fuzz_.next();
        const float tone = tone_.next(), level = level_.next(), mix = mix_.next();
        const float x[2] = {c.io[0][i], c.io[1][i]};
        // how big the signal is after the gain: up at once, down slowly
        const double size = g * (double)std::max(std::fabs(x[0]), std::fabs(x[1]));
        env_ = size > env_ ? size : env_ - envRel_ * env_;
        const Shape sh = shapeFor(sputter_.next(), octave_.next(), asym_.next(), env_);
        float dry[2];
        dly_.push(x[0], x[1], dry[0], dry[1]);
        for (int ch = 0; ch < 2; ++ch) {
            Chan& k = ch_[ch];
            float f[4];
            k.os.up(x[ch], f);
            for (int p = 0; p < 4; ++p) {
                // the average of the curve between the last sample and this one: exact, because the curve is all straight lines
                const double x1 = g * f[p], x0 = k.x0, dx = x1 - x0;
                f[p] = (float)(std::fabs(dx) < 1.0e-6 * (1.0 + std::fabs(x1)) ? sh.f(0.5 * (x0 + x1)) : (sh.F(x1) - sh.F(x0)) / dx);
                k.x0 = x1;
            }
            double y = k.dc.hp((double)k.os.down(f));
            const double lo = k.lo.lp(y), hi = k.hi.hp(y);
            y = lo + (hi - lo) * (double)tone;
            c.io[ch][i] = dry[ch] + ((float)y * level - dry[ch]) * mix;
        }
    }
    for (auto& k : ch_) { k.dc.flush(); k.lo.flush(); k.hi.flush(); }
    if (env_ < 1.0e-6) env_ = 0.0;      // 120 dB under the clipping level there is no bias left to speak of
}

std::vector<Preset> Splat::factoryPresets() const {
    return buildPresets(*this, {
        {"Square wall", {}},
        {"Round and woolly", {{"fuzz", 24}, {"asym", 0.2f}, {"tone", 0.2f}, {"level", -10}}},
        {"Dying battery", {{"fuzz", 30}, {"asym", 0.8f}, {"sputter", 0.55f}, {"tone", 0.45f}, {"level", -10}}},
        {"Octave screamer", {{"fuzz", 34}, {"asym", 0.3f}, {"octave", 1.0f}, {"tone", 0.7f}, {"level", -13}}},
        {"Half octave rasp", {{"fuzz", 28}, {"octave", 0.5f}, {"asym", 0.6f}, {"tone", 0.55f}, {"level", -12}}},
        {"Spitting gate", {{"fuzz", 40}, {"sputter", 0.9f}, {"asym", 1.0f}, {"tone", 0.6f}, {"level", -12}}},
        {"Thin buzz", {{"fuzz", 44}, {"asym", 0.4f}, {"tone", 1.0f}, {"level", -14}}},
        {"Under the dry signal", {{"fuzz", 36}, {"asym", 0.5f}, {"tone", 0.35f}, {"level", -16}, {"mix", 0.4f}}},
    });
}

// ------------------------------------------------------------------ Coarse

Coarse::Coarse() {
    pBits = addFloat("bits", "Bits (24 = off)", 1.0f, kBitsOff, 8.0f, "bit");
    pDither = addBool("dither", "Dither", false);
    pRate = addFloat("rate", "Sample rate (highest = off)", 250.0f, kRateOff, kRateOff, "Hz", 3.0f);
    pMode = addChoice("mode", "Resampling", {"Hold", "Hold, filtered first", "Interpolated", "Filtered both ways"}, 0);
    pMix = addFloat("mix", "Dry / wet", 0.0f, 1.0f, 1.0f, "%");
}

void Coarse::prepare(double sr) { sr_ = sr; reset(); }

void Coarse::reset() {
    for (auto& ch : ch_) {
        for (auto& s : ch.aa) s.reset();
        for (auto& s : ch.rc) s.reset();
        ch.held = ch.prev = 0.0;
    }
    dip_.reset();
    dither_ = dsp::Noise(0x2F6E2B1u);
    phase_ = 1.0;
    rateSeen_ = -1.0f;
    primed_ = false;
}

void Coarse::process(ProcessContext& c) {
    const int n = c.numSamples;
    const float bits = pBits->get(), rate = pRate->get();
    const float stepT = bits >= kBitsOff - 0.001f ? 0.0f : (float)stepFor(bits);
    const float ratioT = rate >= kRateOff - 0.5f ? 1.0f : std::min(1.0f, rate / (float)sr_);
    const bool first = !primed_;
    if (first) { step_.snap(stepT); ratio_.snap(ratioT); mix_.snap(pMix->get()); }
    step_.target(stepT, n);
    ratio_.target(ratioT, n);
    mix_.target(pMix->get(), n);
    if (dip_.begin(pMode->getInt(), n) && !first) {
        for (auto& ch : ch_) { for (auto& s : ch.aa) s.reset(); for (auto& s : ch.rc) s.reset(); }
    }
    const int mode = dip_.active;
    if (rate != rateSeen_) {
        // fourth-order low-pass just under half the reduced rate, used before the hold and after it
        rateSeen_ = rate;
        const double fc = std::min(0.45 * (double)rate, 0.45 * sr_);
        dsp::Biquad q;
        q.lowpass(fc, 0.5412, sr_);
        if (first) lpC_[0].snap(q); else lpC_[0].glide(q, n);
        q.lowpass(fc, 1.3066, sr_);
        if (first) lpC_[1].snap(q); else lpC_[1].glide(q, n);
    }
    primed_ = true;
    const bool dither = pDither->getBool();
    const bool filterIn = mode == HoldFiltered || mode == Filtered;

    for (int i = 0; i < n; ++i) {
        const double ratio = ratio_.next(), q = step_.next();
        const float mix = mix_.next(), dipG = dip_.gain.next();
        lpC_[0].step();
        lpC_[1].step();
        const bool reduce = ratio < 1.0;
        bool capture = false;
        if (reduce) {
            phase_ += ratio;
            if (phase_ >= 1.0) { phase_ -= 1.0; capture = true; }
        } else phase_ = 1.0;
        for (int ch = 0; ch < 2; ++ch) {
            Chan& k = ch_[ch];
            const float x = c.io[ch][i];
            double y = x;
            if (reduce) {
                if (filterIn) y = k.aa[1].run(lpC_[1], k.aa[0].run(lpC_[0], y));
                if (capture) { k.prev = k.held; k.held = y; }
                y = mode == Interpolated ? k.prev + (k.held - k.prev) * phase_ : k.held;
                if (mode == Filtered) y = k.rc[1].run(lpC_[1], k.rc[0].run(lpC_[0], y));
            } else { k.prev = k.held = y; }
            if (q > 0.0 && y != 0.0) {
                // digital silence stays silent: the dither is only added to a signal
                const double d = dither ? (double)dither_.uni() - (double)dither_.uni() : 0.0;
                y = q * std::floor(y / q + 0.5 + d);
            }
            // fully wet is exactly the crushed value (a held sample stays the same number), fully dry exactly the input
            const float amount = dipG * mix;
            c.io[ch][i] = amount >= 1.0f ? (float)y : x + ((float)y - x) * amount;
        }
    }
    for (auto& k : ch_) { for (auto& s : k.aa) s.flush(); for (auto& s : k.rc) s.flush(); }
}

std::vector<Preset> Coarse::factoryPresets() const {
    return buildPresets(*this, {
        {"Eight bits", {}},
        {"Twelve bit sampler", {{"bits", 12}, {"rate", 26000}, {"mode", 3}}},
        {"Old game console", {{"bits", 4}, {"rate", 11000}, {"mode", 0}}},
        {"Telephone line", {{"bits", 8}, {"rate", 8000}, {"mode", 1}}},
        {"Ringing images", {{"bits", 24}, {"rate", 6000}, {"mode", 0}}},
        {"Soft and grainy", {{"bits", 10}, {"dither", 1}, {"rate", 16000}, {"mode", 2}}},
        {"Two bit rubble", {{"bits", 2}, {"rate", 48000}, {"mix", 0.6f}}},
        {"Dithered six bits", {{"bits", 6}, {"dither", 1}}},
    });
}

} // namespace pulp
