#include "pulp/Squelch.h"

namespace pulp {

namespace {
constexpr double kEnvOctaves = 7.0;         // envelope amount at full: how far the envelope opens the filter
constexpr double kEnvBias = 0.2;            // ... and how much of that it starts below the cutoff setting
constexpr double kAccentOctaves = 4.0;      // extra opening from the accent charge at full accent
constexpr double kMaxFeedback = 17.2;       // the ladder starts to sing by itself at 17
constexpr int kChunk = 8;                   // samples between filter coefficient targets
constexpr float kOut = 1.25f;
// ln(cosh(x)), the antiderivative of tanh, without overflow.
inline double lnCosh(double x) { const double a = std::fabs(x); return a + std::log1p(std::exp(-2.0 * a)) - 0.6931471805599453; }
}

Squelch::Squelch() {
    pWave = addChoice("wave", "Waveform", {"Saw", "Square"}, 0);
    pTune = add({"tune", "Tune", -36, 24, -24, 1, 61, "st", {}});
    pFine = addFloat("fine", "Fine tune", -100, 100, 0, "ct");
    pCutoff = addFloat("cutoff", "Cutoff", 30, 12000, 450, "Hz", 3.0f);
    pReso = addFloat("reso", "Resonance", 0, 1, 0.7f, "%");
    pEnvMod = addFloat("envmod", "Envelope amount", 0, 1, 0.55f, "%");
    pDecay = addFloat("decay", "Decay", 30, 3000, 350, "ms", 3.0f);
    pAccent = addFloat("accent", "Accent", 0, 1, 0.6f, "%");
    pSlide = addFloat("slide", "Slide time", 5, 500, 60, "ms", 3.0f);
    pDrive = addFloat("drive", "Drive", 0, 1, 0.2f, "%");
    pAmpDecay = addFloat("ampdecay", "Amp decay", 50, 8000, 3000, "ms", 3.0f);
    pVolume = addFloat("volume", "Level", 0, 1, 0.5f, "%");
}

std::vector<ParamGroup> Squelch::paramGroups() const {
    return {
        {"Oscillator", {"wave", "tune", "fine"}},
        {"Filter", {"cutoff", "reso", "envmod", "decay"}},
        {"Accent and slide", {"accent", "slide"}},
        {"Output", {"drive", "ampdecay", "volume"}},
    };
}

float Squelch::meter(int index) const {
    switch (index) {
        case 0: return meterCutoff_.load(std::memory_order_relaxed);
        case 1: return meterAccent_.load(std::memory_order_relaxed);
        case 2: return meterEnv_.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void Squelch::prepare(double sampleRate) {
    sr_ = sampleRate;
    fs2_ = 2.0 * sampleRate;
    down_.prepare(sampleRate);
    pre_.prepare(sampleRate);
    // The resonance loop is AC coupled, so the bottom octaves keep their weight when the filter screams.
    const double gh = std::tan(kPi * 60.0 / fs2_);
    hpG_ = gh / (1.0 + gh);
    dcA_ = 1.0 - std::exp(-kTwoPi * 12.0 / fs2_);
    reset();
}

void Squelch::goIdle() {
    active_ = false;
    gate_ = false;
    ampStage_ = Idle;
    amp_ = 0.0f;
    fenv_ = 0.0f;
    fenvRising_ = false;
    kill_ = 1.0f;
    killDir_ = 0;
    numHeld_ = 0;
    s1_ = s2_ = s3_ = s4_ = sh_ = dc_ = 0.0;
    g_ = 0.0;
    phase_ = 0.0;
    driveX_ = driveF_ = 0.0;
    pre_.reset();
    down_.reset();
}

void Squelch::reset() {
    goIdle();
    accCap_ = 0.0f;
    clock_ = 0;
    releasedAt_ = -1;
    primed_ = false;
    voices_.store(0, std::memory_order_relaxed);
    meterCutoff_.store(0.0f, std::memory_order_relaxed);
    meterAccent_.store(0.0f, std::memory_order_relaxed);
    meterEnv_.store(0.0f, std::memory_order_relaxed);
}

void Squelch::beginBlock(ProcessContext& ctx) {
    bend_ = ctx.pitchBendSemis;
    const float v = pVolume->get();
    if (!primed_ || !active_) vol_.snap(v);
    else vol_.target(v, ctx.numSamples);
    primed_ = true;
    voices_.store(active_ ? 1 : 0, std::memory_order_relaxed);
}

void Squelch::hold(uint32 id) {
    if (numHeld_ == 8) { for (int i = 1; i < 8; ++i) held_[i - 1] = held_[i]; numHeld_ = 7; }
    held_[numHeld_++] = id;
}

bool Squelch::letGo(uint32 id) {
    int w = 0;
    for (int i = 0; i < numHeld_; ++i) if (held_[i] != id) held_[w++] = held_[i];
    numHeld_ = w;
    return numHeld_ == 0;
}

void Squelch::gateOff() {
    if (!gate_) return;
    gate_ = false;
    releasedAt_ = clock_;
    if (ampStage_ != Idle) ampStage_ = Release;
}

// legato: the note before is still held, so the pitch slides and no envelope starts again.
void Squelch::trigger(const NoteEvent& e, bool legato) {
    accented_ = e.velocity > kAccentVelocity;
    const float v = std::min(e.velocity, kAccentVelocity) / kDefaultVelocity;
    velTarget_ = std::max(0.0f, 1.0f + (v * v - 1.0f) * 0.5f);
    modX_ = e.modX;
    modY_ = e.modY;
    bl::noteGains(e.pan, panLT_, panRT_);
    group_ = e.group;
    currentId_ = e.id;
    hold(e.id);
    relCoef_ = (float)std::exp(-4.6 / (0.001 * (4.0 + 28.0 * clampT(e.release, 0.0f, 1.0f)) * sr_));
    keyTarget_ = (double)e.key + (double)e.fineCents * 0.01;
    if (legato) {
        gate_ = true;
        if (ampStage_ == Release || ampStage_ == Idle) ampStage_ = Decay;
        return;
    }
    if (!active_) {             // from silence: nothing left over from the note before
        velGain_ = velTarget_;
        accGain_ = accented_ ? 1.0f + 0.9f * pAccent->get() : 1.0f;
        panL_ = panLT_;
        panR_ = panRT_;
        vol_.snap(pVolume->get());
    }
    key_ = e.glideFromKey >= 0.0f ? (double)e.glideFromKey : keyTarget_;
    ampStage_ = Attack;
    fenvRising_ = true;
    active_ = true;
    gate_ = true;
    killDir_ = kill_ < 1.0f ? 1 : 0;
}

void Squelch::handleEvent(const NoteEvent& e, ProcessContext&) {
    switch (e.type) {
        case NoteEvent::On: {
            // A note slides when the one before is still held, or when it is a portamento note that
            // starts on the very sample the one before let go.
            const bool tied = active_ && killDir_ >= 0 && (gate_ || (e.glideFromKey >= 0.0f && releasedAt_ == clock_));
            trigger(e, tied);
            break;
        }
        case NoteEvent::Off:
            if (letGo(e.id)) gateOff();
            break;
        case NoteEvent::AllOff:
            numHeld_ = 0;
            gateOff();
            break;
        case NoteEvent::Kill:
            numHeld_ = 0;
            gate_ = false;
            if (active_) killDir_ = -1;
            break;
        case NoteEvent::KillNote:
            letGo(e.id);
            if (active_ && e.id == currentId_) { numHeld_ = 0; gate_ = false; killDir_ = -1; }
            break;
        case NoteEvent::SlideTo:
            if (active_ && gate_ && e.group == group_) keyTarget_ = (double)e.key;
            break;
    }
}

void Squelch::render(float* L, float* R, int n, ProcessContext&) {
    clock_ += n;
    const float reso = pReso->get(), accent = pAccent->get();
    const double resoNow = clampT((double)reso + ((double)modY_ - 0.5) * 2.0, 0.0, 1.0);
    // The accent charges a capacitor through the resonance control: more resonance, slower and bigger "wow".
    const float accA = bl::tauCoef(0.004 + 0.06 * resoNow, sr_);
    if (!active_) {
        if (accCap_ > 1.0e-6f) for (int i = 0; i < n; ++i) accCap_ -= accA * accCap_;
        else accCap_ = 0.0f;
        meterAccent_.store(accCap_, std::memory_order_relaxed);
        meterEnv_.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const bool square = pWave->getInt() == 1;
    const double cutoff = pCutoff->get(), envMod = pEnvMod->get();
    const double offs = (double)pTune->get() + (double)pFine->get() * 0.01 + (double)bend_;
    const float decayMs = accented_ ? std::min(pDecay->get(), 200.0f) : pDecay->get();
    const float fDecay = (float)std::exp(-4.6 / (0.001 * (double)decayMs * sr_));
    const float fAttack = (float)(1.0 / (0.003 * sr_));
    const float aAttack = (float)(1.0 / (0.0015 * sr_));
    const float aDecay = (float)std::exp(-4.6 / (0.001 * (double)pAmpDecay->get() * sr_));
    const float smooth = bl::tauCoef(0.003, sr_);
    const float glide = (float)(1.0 - std::exp(-3.0 / (0.001 * (double)pSlide->get() * sr_)));
    const float killStep = (float)(1.0 / (0.003 * sr_));
    const float accGainT = accented_ ? 1.0f + 0.9f * accent : 1.0f;
    const double kTarget = kMaxFeedback * (1.0 - std::pow(1.0 - resoNow, 1.6));     // most of the travel below the singing point
    const double driveAmt = pDrive->get();
    const double driveT = 0.5 * std::pow(10.0, driveAmt * 28.0 / 20.0);
    const double driveNormT = (1.0 - 0.45 * driveAmt) / std::tanh(driveT);       // roughly even loudness as it is turned up
    const double fbScale = 1.0 - hpG_;
    double fcShown = cutoff;

    int pos = 0;
    while (pos < n && active_) {
        const int m = std::min(kChunk, n - pos);
        // where the filter should be by the end of this chunk
        const double octs = envMod * kEnvOctaves * ((double)fenv_ - kEnvBias) + (double)accent * kAccentOctaves * (double)accCap_
                          + ((double)modX_ - 0.5) * 8.0;
        const double fc = clampT(cutoff * std::exp2(octs), 20.0, std::min(20000.0, 0.21 * fs2_));
        fcShown = fc;
        // the ladder peaks at 1/sqrt(2) of its corner; this puts the peak on fc
        const double gT = 1.4142135623730951 * std::tan(kPi * fc / fs2_);
        if (g_ <= 0.0) { g_ = gT; k_ = kTarget; drive_ = driveT; driveNorm_ = driveNormT; }    // first chunk after silence
        const double inv = 1.0 / (2.0 * m);
        const double gStep = (gT - g_) * inv, kStep = (kTarget - k_) * inv;
        const double dStep = (driveT - drive_) * inv, dnStep = (driveNormT - driveNorm_) * inv;
        dt_ = std::min(0.45, keyToHz(key_ + offs) / fs2_);

        for (int i = 0; i < m; ++i) {
            // ---- per-sample control: envelopes, glide, declick ----
            switch (ampStage_) {
                case Attack: amp_ += aAttack; if (amp_ >= 1.0f) { amp_ = 1.0f; ampStage_ = Decay; } break;
                case Decay: amp_ = (amp_ + bl::Fall::kUnder) * aDecay - bl::Fall::kUnder; break;
                case Release: amp_ = (amp_ + bl::Fall::kUnder) * relCoef_ - bl::Fall::kUnder; break;
                default: break;
            }
            if (amp_ <= 0.0f && ampStage_ != Attack) { goIdle(); break; }
            if (fenvRising_) { fenv_ += fAttack; if (fenv_ >= 1.0f) { fenv_ = 1.0f; fenvRising_ = false; } }
            else { fenv_ *= fDecay; if (fenv_ < 1.0e-9f) fenv_ = 0.0f; }
            accCap_ += accA * ((accented_ ? fenv_ : 0.0f) - accCap_);
            flushDenormal(accCap_);
            accGain_ += smooth * (accGainT - accGain_);
            velGain_ += smooth * (velTarget_ - velGain_);
            panL_ += smooth * (panLT_ - panL_);
            panR_ += smooth * (panRT_ - panR_);
            if (key_ != keyTarget_) {
                key_ += (double)glide * (keyTarget_ - key_);
                if (std::fabs(keyTarget_ - key_) < 1.0e-4) key_ = keyTarget_;
                dt_ = std::min(0.45, keyToHz(key_ + offs) / fs2_);
            }
            if (killDir_ < 0) { kill_ -= killStep; if (kill_ <= 0.0f) { goIdle(); break; } }
            else if (killDir_ > 0) { kill_ += killStep; if (kill_ >= 1.0f) { kill_ = 1.0f; killDir_ = 0; } }
            const double level = (double)(amp_ * velGain_ * accGain_ * kill_);

            // ---- twice per sample: oscillator, ladder, drive ----
            float pair[2];
            for (int sub = 0; sub < 2; ++sub) {
                g_ += gStep; k_ += kStep; drive_ += dStep; driveNorm_ += dnStep;
                const double t = phase_;
                double osc;
                if (square) {
                    osc = t < 0.5 ? 1.0 : -1.0;
                    osc += bl::polyBlep(t, dt_);
                    double t2 = t + 0.5; if (t2 >= 1.0) t2 -= 1.0;
                    osc -= bl::polyBlep(t2, dt_);
                    osc *= 0.6;
                } else {
                    osc = 1.0 - 2.0 * t + bl::polyBlep(t, dt_);
                }
                phase_ += dt_;
                if (phase_ >= 1.0) phase_ -= 1.0;
                // Nothing above the audible band goes into the ladder: its saturation would fold it back down.
                // The feedback takes the pass band down by 1 + k; most of that is given back here, ahead of
                // the filter, so a resonant sound keeps its body.
                const double x = (double)pre_.process((float)osc) * 0.4 * (1.0 + 0.7 * k_);

                // Diode ladder, solved without a unit delay. With trapezoidal integrators each stage is
                // y_i = G * (its neighbours) + S_i; working up from the last stage gives y4 = gamma * u + sigma.
                const double ig = 1.0 / (1.0 + g_), G = g_ * ig, hG = 0.5 * G;
                const double d3 = 1.0 / (1.0 - hG * hG), a3 = hG * d3;
                const double d2 = 1.0 / (1.0 - hG * a3), a2 = hG * d2;
                const double d1 = 1.0 / (1.0 - G * a2), a1 = G * d1;
                const double b4 = s4_ * ig;
                const double b3 = (hG * b4 + s3_ * ig) * d3;
                const double b2 = (hG * b3 + s2_ * ig) * d2;
                const double b1 = (G * b2 + s1_ * ig) * d1;
                const double gamma = hG * a3 * a2 * a1;
                const double sigma = hG * (a3 * (a2 * b1 + b2) + b3) + b4;
                // u = x - k * tanh(z), where z is the output after the feedback path's high-pass. The
                // saturation sits in the loop, so the resonance can sing but cannot grow without limit:
                // whatever happens |u| <= |x| + k. z is found with four Newton steps from the linear answer
                // (z + B tanh z is monotonic, so there is exactly one).
                const double B = fbScale * gamma * k_;
                const double z0 = fbScale * (gamma * x + sigma - sh_);
                double z = z0 / (1.0 + B);
                for (int it = 0; it < 4; ++it) {
                    const double th = std::tanh(z);
                    z -= (z + B * th - z0) / (1.0 + B * (1.0 - th * th));
                }
                const double u = x - k_ * std::tanh(z);
                const double y1 = a1 * u + b1, y2 = a2 * y1 + b2, y3 = a3 * y2 + b3, y4 = hG * y3 + b4;
                s1_ = 2.0 * y1 - s1_; s2_ = 2.0 * y2 - s2_; s3_ = 2.0 * y3 - s3_; s4_ = 2.0 * y4 - s4_;
                sh_ += 2.0 * (y4 - sh_) * hpG_;

                // Drive: tanh, taken as the mean of the curve between this input and the last one
                // (the difference of its antiderivative), which removes most of what would alias.
                // The saturating feedback rectifies a little, so the ladder's output carries an offset
                // that moves with the sound: it is taken out before the envelope can turn it into a thump.
                dc_ += dcA_ * (y4 - dc_);
                const double in = drive_ * (y4 - dc_) * level, F = lnCosh(in), step = in - driveX_;
                const double shaped = std::fabs(step) > 1.0e-5 ? (F - driveF_) / step : std::tanh(0.5 * (in + driveX_));
                driveX_ = in;
                driveF_ = F;
                pair[sub] = (float)(shaped * driveNorm_);
            }
            const float o = down_.process(pair[0], pair[1]) * vol_.next() * kOut;
            L[pos + i] += o * panL_;
            R[pos + i] += o * panR_;
        }
        pos += m;
    }
    meterCutoff_.store((float)fcShown, std::memory_order_relaxed);
    meterAccent_.store(accCap_, std::memory_order_relaxed);
    meterEnv_.store(fenv_, std::memory_order_relaxed);
    scope_.push(L, R, n);
}

std::vector<Preset> Squelch::factoryPresets() const {
    return buildPresets(*this, {
        {"Classic Squelch", {}},
        {"Rubber Saw", {{"cutoff", 300}, {"reso", 0.55f}, {"envmod", 0.45f}, {"decay", 220}, {"drive", 0.1f}}},
        {"Hollow Square", {{"volume", 0.70f}, {"wave", 1}, {"cutoff", 380}, {"reso", 0.6f}, {"envmod", 0.5f}, {"decay", 300}, {"drive", 0.15f}}},
        {"Screamer", {{"volume", 0.39f}, {"cutoff", 700}, {"reso", 0.97f}, {"envmod", 0.7f}, {"decay", 500}, {"accent", 0.9f}, {"drive", 0.55f}}},
        {"Deep Sub Line", {{"volume", 0.70f}, {"wave", 1}, {"tune", -36}, {"cutoff", 160}, {"reso", 0.3f}, {"envmod", 0.3f}, {"decay", 180}, {"drive", 0.05f}}},
        {"Wet Resonance", {{"cutoff", 240}, {"reso", 0.9f}, {"envmod", 0.62f}, {"decay", 700}, {"drive", 0.0f}}},
        {"Driven Lead", {{"volume", 0.39f}, {"tune", -12}, {"cutoff", 900}, {"reso", 0.75f}, {"envmod", 0.5f}, {"decay", 400}, {"drive", 0.8f}}},
        {"Soft Bubble", {{"volume", 0.70f}, {"wave", 1}, {"cutoff", 200}, {"reso", 0.8f}, {"envmod", 0.35f}, {"decay", 120}, {"accent", 0.4f}, {"drive", 0.0f}}},
        {"Long Sweep", {{"cutoff", 150}, {"reso", 0.85f}, {"envmod", 0.8f}, {"decay", 2400}, {"drive", 0.25f}}},
        {"Snappy Pluck", {{"volume", 0.69f}, {"cutoff", 260}, {"reso", 0.45f}, {"envmod", 0.75f}, {"decay", 60}, {"ampdecay", 400}, {"drive", 0.1f}}},
        {"Dark Roller", {{"cutoff", 110}, {"reso", 0.65f}, {"envmod", 0.25f}, {"decay", 260}, {"accent", 0.8f}, {"drive", 0.3f}}},
        {"Overdriven Square", {{"wave", 1}, {"cutoff", 520}, {"reso", 0.7f}, {"envmod", 0.55f}, {"decay", 320}, {"drive", 1.0f}}},
        {"Liquid", {{"cutoff", 420}, {"reso", 0.93f}, {"envmod", 0.4f}, {"decay", 900}, {"slide", 140}, {"drive", 0.05f}}},
        {"Accent Wow", {{"cutoff", 200}, {"reso", 0.88f}, {"envmod", 0.3f}, {"decay", 400}, {"accent", 1.0f}, {"drive", 0.2f}}},
        {"Slow Slide", {{"cutoff", 500}, {"reso", 0.6f}, {"envmod", 0.4f}, {"decay", 600}, {"slide", 320}, {"drive", 0.15f}}},
        {"Tight Stab", {{"volume", 0.68f}, {"wave", 1}, {"cutoff", 800}, {"reso", 0.35f}, {"envmod", 0.6f}, {"decay", 90}, {"ampdecay", 220}, {"drive", 0.35f}}},
        {"Fuzz Pipe", {{"wave", 1}, {"tune", -12}, {"cutoff", 1400}, {"reso", 0.5f}, {"envmod", 0.2f}, {"decay", 500}, {"drive", 0.9f}}},
        {"High Chirp", {{"volume", 0.36f}, {"tune", 0}, {"cutoff", 1200}, {"reso", 0.95f}, {"envmod", 0.65f}, {"decay", 150}, {"drive", 0.1f}}},
        {"Mellow Round", {{"volume", 0.73f}, {"cutoff", 320}, {"reso", 0.15f}, {"envmod", 0.2f}, {"decay", 500}, {"accent", 0.3f}, {"drive", 0.0f}}},
        {"Zap Bass", {{"cutoff", 90}, {"reso", 0.8f}, {"envmod", 1.0f}, {"decay", 45}, {"ampdecay", 600}, {"drive", 0.3f}}},
        {"Open Saw", {{"cutoff", 4500}, {"reso", 0.2f}, {"envmod", 0.1f}, {"decay", 800}, {"drive", 0.25f}}},
        {"Growl", {{"tune", -36}, {"cutoff", 350}, {"reso", 0.82f}, {"envmod", 0.5f}, {"decay", 450}, {"accent", 0.75f}, {"drive", 0.7f}}},
    });
}

} // namespace pulp
