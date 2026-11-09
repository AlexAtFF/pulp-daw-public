// Thumb Pad: one of eleven effects, played with two controls meant for an X / Y pad.
//
// The pad position is ramped across each quantum and then smoothed (about 4 ms), and every coefficient that
// depends on it is worked out again for every sample, so a finger moving at any speed never makes steps.
// Switching the effect in or out, or changing to another effect, is a 10 ms fade; an effect that is out is
// not run at all, and starts from a clean state when it comes back.
#include "pulp/Creative.h"

namespace pulp {

namespace {
const char* const kModeNames[] = {"Low-pass sweep", "High-pass sweep", "Flanger", "Phaser", "Echo", "Wash",
                                  "Drive", "Crusher", "Ring", "Vowel", "Tape speed"};
const char* const kAxis[ThumbPad::NumModes][2] = {
    {"Cutoff", "Resonance"}, {"Cutoff", "Resonance"}, {"Delay", "Feedback"}, {"Frequency", "Feedback"},
    {"Time", "Feedback"}, {"Decay", "Brightness"}, {"Drive", "Tone"}, {"Rate", "Bits"},
    {"Frequency", "Depth"}, {"Vowel (A E I O U)", "Sharpness"}, {"Speed (centre = normal)", "Inertia"}};
// first three formants of five sung vowels (Hz) and their levels (dB)
const double kVowelHz[5][3] = {{650, 1080, 2650}, {400, 1700, 2600}, {290, 1870, 2800}, {400, 800, 2600}, {350, 600, 2700}};
const double kVowelDb[5][3] = {{0, -6, -7}, {0, -14, -12}, {0, -15, -18}, {0, -10, -12}, {0, -20, -17}};
const double kWashMs[4] = {29.7, 37.1, 41.1, 43.7};
const double kWashApMs[2] = {4.77, 3.59};

// log(cosh(x)): the integral of tanh, for the anti-aliased clipper
inline double lnCosh(double x) {
    const double a = std::fabs(x);
    return a - 0.6931471805599453 + std::log1p(std::exp(-2.0 * a));
}
}

const char* ThumbPad::axisName(int mode, int axis) { return kAxis[clampT(mode, 0, (int)NumModes - 1)][axis ? 1 : 0]; }

void ThumbPad::vowelFormants(double x, double hz[3], double gain[3]) {
    const double p = clampT(x, 0.0, 1.0) * 4.0;
    const int a = std::min(3, (int)p);
    const double t = p - a;
    for (int f = 0; f < 3; ++f) {
        hz[f] = kVowelHz[a][f] * std::pow(kVowelHz[a + 1][f] / kVowelHz[a][f], t);
        gain[f] = std::pow(10.0, (kVowelDb[a][f] + (kVowelDb[a + 1][f] - kVowelDb[a][f]) * t) / 20.0);
    }
}

ThumbPad::ThumbPad() {
    std::vector<std::string> names(kModeNames, kModeNames + NumModes);
    pMode = addChoice("mode", "Effect", names, 0);
    pX = addFloat("x", "X", 0, 1, 0.5f, "%");
    pY = addFloat("y", "Y", 0, 1, 0.5f, "%");
    pActive = addBool("active", "Active", true);
    pMix = addFloat("mix", "Dry / wet", 0, 1, 1, "%");
}

void ThumbPad::prepare(double sr) {
    sr_ = sr;
    tape_.prepare(4.5, sr);
    for (int k = 0; k < kWash; ++k) {
        washLen_[k] = std::max(8, (int)std::lround(kWashMs[k] * 0.001 * sr)) | 1;
        wash_[k].prepare(washLen_[k] + 4);
    }
    for (int s = 0; s < 2; ++s) {
        washApLen_[s] = std::max(4, (int)std::lround(kWashApMs[s] * 0.001 * sr));
        for (int ch = 0; ch < 2; ++ch) washAp_[ch][s].prepare(washApLen_[s] + 24);
    }
    smooth_ = (float)(1.0 - std::exp(-1.0 / (0.004 * sr)));
    gainStep_ = (float)(1.0 / (0.010 * sr));
    echoCoef_ = 1.0 - std::exp(-1.0 / (0.04 * sr));
    reset();
}

void ThumbPad::reset() {
    clearEffect();
    primed_ = false;
    dirty_ = false;
    gain_ = 0.0f;
    mGain_.store(0.0f, std::memory_order_relaxed);
}

void ThumbPad::clearEffect() {
    for (auto& s : svf_) s.reset();
    for (auto& f : formant_) for (auto& s : f) s.reset();
    tape_.reset();
    delaySm_ = -1.0;
    std::memset(apZ_, 0, sizeof apZ_);
    phFb_[0] = phFb_[1] = 0.0f;
    for (auto& d : wash_) d.clear();
    for (auto& ch : washAp_) for (auto& d : ch) d.clear();
    for (float& v : washLp_) v = 0.0f;
    for (int ch = 0; ch < 2; ++ch) { tone_[ch].reset(); dcBlock_[ch].reset(); adaaX_[ch] = adaaF_[ch] = 0.0; crushHeld_[ch] = 0.0f; }
    adaaOn_ = false;
    crushPhase_ = 1.0;
    ringPhase_ = 0.0;
    tapeBehind_ = 0.0;
    tapeSpeed_ = 1.0;
}

void ThumbPad::effect(int mode, float xf, float yf, float inL, float inR, float& l, float& r) {
    const double x = xf, y = yf;
    const float in[2] = {inL, inR};
    float out[2] = {inL, inR};
    switch (mode) {
        case LowPass:
        case HighPass: {
            svf_[0].set((float)(mode == LowPass ? lowPassHz(x) : highPassHz(x)), (float)filterQ(y), (float)sr_);
            svf_[1].g = svf_[0].g; svf_[1].k = svf_[0].k; svf_[1].a1 = svf_[0].a1; svf_[1].a2 = svf_[0].a2; svf_[1].a3 = svf_[0].a3;
            for (int ch = 0; ch < 2; ++ch) {
                float lp, bp, hp;
                svf_[ch].process(in[ch], lp, bp, hp);
                flushDenormal(svf_[ch].ic1); flushDenormal(svf_[ch].ic2);
                out[ch] = mode == LowPass ? lp : hp;
            }
            break;
        }
        case Flanger: {     // the input plus a copy a moment later: a comb whose teeth X moves
            const double d = std::max(2.0, flangerMs(x) * 0.001 * sr_);
            const float fb = (float)flangerFeedback(y);
            float dl, dr;
            tape_.read(d - 1.0, 1.0, dl, dr);
            float wl = inL + fb * dl, wr = inR + fb * dr;
            flushDenormal(wl); flushDenormal(wr);
            tape_.push(wl, wr);
            out[0] = 0.7071f * (inL + dl);
            out[1] = 0.7071f * (inR + dr);
            break;
        }
        case Phaser: {      // six all-pass stages: notches where they turn the phase half way round
            const double t = std::tan(kPi * std::min(phaserHz(x), 0.45 * sr_) / sr_);
            const float a = (float)((t - 1.0) / (t + 1.0));
            const float fb = (float)phaserFeedback(y);
            for (int ch = 0; ch < 2; ++ch) {
                float v = in[ch] + fb * phFb_[ch];
                for (int s = 0; s < kStages; ++s) {
                    const float o = a * v + apZ_[s][ch];
                    apZ_[s][ch] = v - a * o;
                    flushDenormal(apZ_[s][ch]);
                    v = o;
                }
                phFb_[ch] = v;
                flushDenormal(phFb_[ch]);
                out[ch] = 0.5f * (in[ch] + phFb_[ch]);
            }
            break;
        }
        case Echo: {        // the time glides like a tape echo's when X moves
            const double target = std::max(2.0, echoMs(x) * 0.001 * sr_);
            const double before = delaySm_ < 0.0 ? target : delaySm_;
            // towards the new time, but never so fast that the echo plays above double speed or below a quarter
            delaySm_ = before + clampT((target - before) * echoCoef_, -1.0, 0.75);
            if (std::fabs(target - delaySm_) < 1.0e-4) delaySm_ = target;
            const float fb = (float)echoFeedback(y);
            float dl, dr;
            tape_.read(delaySm_ - 1.0, 1.0 - (delaySm_ - before), dl, dr);
            float wl = inL + fb * dl, wr = inR + fb * dr;
            flushDenormal(wl); flushDenormal(wr);
            tape_.push(wl, wr);
            out[0] = inL + dl;
            out[1] = inR + dr;
            break;
        }
        case Wash: {        // two diffusers a side into four delay lines that feed each other
            const double t60 = washSeconds(x);
            const double dampHz = washDampHz(y);
            const float damp = dampHz >= 17900.0 ? 1.0f : (float)(1.0 - std::exp(-kTwoPi * dampHz / sr_));     // fully bright: no loss at all
            float d[2];
            for (int ch = 0; ch < 2; ++ch) {
                float v = in[ch];
                for (int s = 0; s < 2; ++s) {
                    const float z = washAp_[ch][s].tap(washApLen_[s] + ch * 17 - 1);
                    const float o = z - 0.6f * v;
                    float w = v + 0.6f * o;
                    flushDenormal(w);
                    washAp_[ch][s].push(w);
                    v = o;
                }
                d[ch] = v;
            }
            float o[kWash], sum = 0.0f;
            for (int k = 0; k < kWash; ++k) {
                const float tap = wash_[k].tap(washLen_[k] - 1);
                washLp_[k] += damp * (tap - washLp_[k]);
                flushDenormal(washLp_[k]);
                o[k] = washLp_[k] * (float)std::exp(-6.907755 * washLen_[k] / (sr_ * t60));
                sum += o[k];
            }
            sum *= 0.5f;
            for (int k = 0; k < kWash; ++k) wash_[k].push(0.5f * d[k / 2] + o[k] - sum);
            const double g0 = std::exp(-6.907755 * washLen_[0] / (sr_ * t60));
            const float level = (float)(1.6 * std::sqrt(1.0 - g0 * g0));
            out[0] = inL + level * (o[0] + o[2]);
            out[1] = inR + level * (o[1] + o[3]);
            break;
        }
        case Drive: {       // a soft clipper, anti-aliased by averaging the curve between one sample and the next
            const double g = std::pow(10.0, driveDb(x) / 20.0);
            const double norm = std::tanh(0.5) / std::tanh(0.5 * g);
            const double toneHz = driveToneHz(y);
            const float toneA = (float)(1.0 - std::exp(-kTwoPi * toneHz / sr_));
            for (int ch = 0; ch < 2; ++ch) {
                const double u = g * (double)in[ch];
                const double F = lnCosh(u);
                double v;
                if (!adaaOn_) v = std::tanh(u);
                else {
                    const double du = u - adaaX_[ch];
                    v = std::fabs(du) > 1.0e-6 ? (F - adaaF_[ch]) / du : std::tanh(0.5 * (u + adaaX_[ch]));
                }
                adaaX_[ch] = u; adaaF_[ch] = F;
                float s = (float)(v * norm);
                if (toneHz < 17900.0) {
                    tone_[ch].a = toneA;
                    s = tone_[ch].lp(s);
                    flushDenormal(tone_[ch].z);
                } else tone_[ch].z = s;
                out[ch] = s;
            }
            adaaOn_ = true;
            break;
        }
        case Crush: {       // hold each sample for a while, then fewer bits
            const double hz = crushHz(x, sr_);
            const float levels = (float)std::exp2(crushBits(y) - 1.0);
            crushPhase_ += hz >= 0.999 * sr_ ? 1.0 : hz / sr_;
            if (crushPhase_ >= 1.0) {
                crushPhase_ -= std::floor(crushPhase_);
                crushHeld_[0] = inL; crushHeld_[1] = inR;
            }
            out[0] = std::round(crushHeld_[0] * levels) / levels;
            out[1] = std::round(crushHeld_[1] * levels) / levels;
            break;
        }
        case Ring: {        // multiplied by a sine: the sound moves to either side of it
            ringPhase_ += ringHz(x) / sr_;
            if (ringPhase_ >= 1.0) ringPhase_ -= 1.0;
            const float car = (float)std::sin(kTwoPi * ringPhase_);
            const float g = (1.0f - yf) + yf * car;
            out[0] = inL * g; out[1] = inR * g;
            break;
        }
        case Vowel: {       // three band-passes on the formants of a sung vowel
            double hz[3], gain[3];
            vowelFormants(x, hz, gain);
            const float q = (float)vowelQ(y);
            float acc[2] = {0.0f, 0.0f};
            for (int f = 0; f < 3; ++f) {
                formant_[f][0].set((float)hz[f], q, (float)sr_);
                dsp::Svf& a = formant_[f][0];
                dsp::Svf& b = formant_[f][1];
                b.g = a.g; b.k = a.k; b.a1 = a.a1; b.a2 = a.a2; b.a3 = a.a3;
                for (int ch = 0; ch < 2; ++ch) {
                    float lp, bp, hp;
                    formant_[f][ch].process(in[ch], lp, bp, hp);
                    flushDenormal(formant_[f][ch].ic1); flushDenormal(formant_[f][ch].ic2);
                    acc[ch] += (float)gain[f] * bp * a.k;       // each band is 0 dB at its centre before its level
                }
            }
            out[0] = acc[0]; out[1] = acc[1];
            break;
        }
        case TapeSpeed: {   // a tape whose speed X sets; while it runs slow it falls behind, and catches up at double speed
            tape_.push(inL, inR);
            const double a = 1.0 - std::exp(-1.0 / (tapeInertiaSeconds(y) * sr_));
            tapeSpeed_ += (tapeSpeed(x) - tapeSpeed_) * a;
            const double limit = std::min(tape_.maxDelay(), 4.0 * sr_);
            const double before = tapeBehind_;
            tapeBehind_ = clampT(tapeBehind_ + 1.0 - tapeSpeed_, 0.0, limit);
            if (tapeBehind_ < 1.0e-9) tapeBehind_ = 0.0;
            const double rate = 1.0 - (tapeBehind_ - before);
            float tl, tr;
            tape_.read(tapeBehind_, rate, tl, tr);
            float g = clampT((float)(std::fabs(rate) / 0.1), 0.0f, 1.0f);       // a tape that stands still gives no sound
            g = g * g * (3.0f - 2.0f * g);
            out[0] = tl * g; out[1] = tr * g;
            break;
        }
        default: break;
    }
    l = out[0]; r = out[1];
}

void ThumbPad::process(ProcessContext& c) {
    const int n = c.numSamples;
    float* L = c.io[0];
    float* R = c.io[1];
    const float xT = pX->get(), yT = pY->get();
    const int modeT = pMode->getInt();
    const bool want = pActive->getBool();
    if (!primed_) {
        xr_ = xs_ = xT; yr_ = ys_ = yT;
        mix_.snap(pMix->get());
        mode_ = modeT;
        gain_ = want ? 1.0f : 0.0f;
        clearEffect();
        dirty_ = false;
        primed_ = true;
    }
    mix_.target(pMix->get(), n);
    if (gain_ <= 0.0f && !want) {           // out, and staying out: the input passes untouched
        if (dirty_) { clearEffect(); dirty_ = false; }
        mode_ = modeT;
        xr_ = xs_ = xT; yr_ = ys_ = yT;     // the effect will come in where the finger is
        for (int i = 0; i < n; ++i) mix_.next();
        mGain_.store(0.0f, std::memory_order_relaxed);
        return;
    }
    const float xStep = (xT - xr_) / (float)n, yStep = (yT - yr_) / (float)n;
    for (int i = 0; i < n; ++i) {
        xr_ += xStep; yr_ += yStep;
        xs_ += smooth_ * (xr_ - xs_);
        ys_ += smooth_ * (yr_ - ys_);
        const float goal = (want && mode_ == modeT) ? 1.0f : 0.0f;      // another effect was chosen: out first, then in
        if (gain_ < goal) gain_ = std::min(goal, gain_ + gainStep_);
        else if (gain_ > goal) gain_ = std::max(goal, gain_ - gainStep_);
        const float m = mix_.next();
        if (gain_ <= 0.0f) {
            if (dirty_) { clearEffect(); dirty_ = false; }
            mode_ = modeT;
            continue;
        }
        dirty_ = true;
        float wl, wr;
        effect(mode_, clampT(xs_, 0.0f, 1.0f), clampT(ys_, 0.0f, 1.0f), L[i], R[i], wl, wr);
        const float g = gain_ * m;
        L[i] = crt::mixDryWet(L[i], wl, g);
        R[i] = crt::mixDryWet(R[i], wr, g);
    }
    xr_ = xT; yr_ = yT;
    mGain_.store(gain_, std::memory_order_relaxed);
}

std::vector<Preset> ThumbPad::factoryPresets() const {
    return buildPresets(*this, {
        {"Low-pass sweep", {{"mode", LowPass}, {"x", 0.7f}, {"y", 0.45f}}},
        {"High-pass riser", {{"mode", HighPass}, {"x", 0.35f}, {"y", 0.4f}}},
        {"Jet flange", {{"mode", Flanger}, {"x", 0.45f}, {"y", 0.75f}}},
        {"Slow phase", {{"mode", Phaser}, {"x", 0.5f}, {"y", 0.6f}}},
        {"Dub echo", {{"mode", Echo}, {"x", 0.72f}, {"y", 0.65f}, {"mix", 0.7f}}},
        {"Slap echo", {{"mode", Echo}, {"x", 0.35f}, {"y", 0.25f}, {"mix", 0.6f}}},
        {"Big wash", {{"mode", Wash}, {"x", 0.8f}, {"y", 0.6f}, {"mix", 0.6f}}},
        {"Fuzz box", {{"mode", Drive}, {"x", 0.7f}, {"y", 0.5f}}},
        {"Lo-fi crusher", {{"mode", Crush}, {"x", 0.45f}, {"y", 0.35f}}},
        {"Robot ring", {{"mode", Ring}, {"x", 0.55f}, {"y", 0.9f}}},
        {"Talking vowels", {{"mode", Vowel}, {"x", 0.0f}, {"y", 0.5f}}},
        {"Tape speed", {{"mode", TapeSpeed}, {"x", 0.5f}, {"y", 0.5f}}},
    });
}

} // namespace pulp
