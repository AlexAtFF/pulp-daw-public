// Jet (flanger), Swirl (phaser), Flutter (tremolo / auto-pan), Waver (vibrato) and Rotor (rotary speaker).
#include "pulp/Space.h"

namespace pulp {

namespace space {

const char* const kModSyncNames[kNumModSync] = {"Off", "8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/4 dotted", "1/4",
                                                "1/4 triplet", "1/8 dotted", "1/8", "1/8 triplet", "1/16", "1/16 triplet", "1/32"};
const int kModSyncTicks[kNumModSync] = {0, 30720, 15360, 7680, 3840, 1920, 1440, 960, 640, 720, 480, 320, 240, 160, 120};

std::vector<std::string> modSyncChoices() {
    return std::vector<std::string>(kModSyncNames, kModSyncNames + kNumModSync);
}

void LfoClock::begin(const ProcessContext& c, double sr, double freeHz, double cycleTicks) {
    const int n = c.numSamples;
    const double tempo = std::max(1.0, c.tempo);
    const double tps = tempo * kPPQ / (60.0 * sr);                  // ticks per sample
    const bool synced = cycleTicks > 0.0;
    const double cpt = synced ? 1.0 / cycleTicks : freeHz * 60.0 / (tempo * kPPQ);     // cycles per tick
    inc = cpt * tps;
    rateHz = inc * sr;
    double want;
    if (c.playing) {
        if (synced) offset_ = 0.0;
        else if (primed_ && wasPlaying_ && !wasSynced_ && cpt != lastCpt_ && std::fabs(c.tickPos - nextTick_) < 1.0e-3)
            offset_ = end_ - c.tickPos * cpt;       // the rate (or the tempo under it) moved: carry on from the same phase
        want = c.tickPos * cpt + offset_;
    } else {
        want = primed_ ? end_ : 0.0;
    }
    want -= std::floor(want);
    double slip = 0.0;
    if (primed_) {
        slip = end_ - want;
        slip -= std::floor(slip + 0.5);
        if (std::fabs(slip) < 1.0e-9) slip = 0.0;
    }
    phase0 = want;
    slip0 = slip;
    slipStep = slip == 0.0 ? 0.0 : (slip * std::exp(-(double)n / (0.012 * sr)) - slip) / (double)n;
    const double e = at(n);
    end_ = e - std::floor(e);
    nextTick_ = c.tickPos + (double)n * tps;
    lastCpt_ = cpt;
    wasPlaying_ = c.playing;
    wasSynced_ = synced;
    primed_ = true;
}

} // namespace space

namespace {
inline double frac(double x) { return x - std::floor(x); }
inline double syncTicks(const Param* p) { return space::kModSyncTicks[clampT(p->getInt(), 0, space::kNumModSync - 1)]; }
}

// ------------------------------------------------------------------ Jet

Jet::Jet() {
    pDelay = addFloat("delay", "Delay", 0.1f, 10, 1.0f, "ms", 2.0f);
    pDepth = addFloat("depth", "Sweep depth", 0, 10, 3.0f, "ms", 2.0f);
    pRate = addFloat("rate", "Rate (sync off)", 0.01f, 20, 0.25f, "Hz", 3.0f);
    pSync = addChoice("sync", "Rate sync", space::modSyncChoices(), 0);
    pShape = addChoice("shape", "Sweep shape", {"Sine", "Triangle"}, 1);
    pFeedback = addFloat("feedback", "Feedback", -0.9f, 0.9f, 0.5f, "%");
    pPhase = addFloat("phase", "Stereo phase", 0, 180, 90, "deg");
    pMix = addFloat("mix", "Dry / wet", 0, 1, 0.5f, "%");
}

void Jet::prepare(double sr) {
    sr_ = sr;
    for (auto& d : d_) d.prepare((int)(0.021 * sr) + 16);
    reset();
}

void Jet::reset() {
    for (auto& d : d_) d.clear();
    lfo_.reset();
    primed_ = false;
}

double Jet::tailSeconds() const {
    const double longest = (pDelay->get() + pDepth->get()) * 0.001, fb = std::fabs(pFeedback->get());
    return std::min(4.0, longest * (1.0 + (fb > 0.001 ? std::log(0.001) / std::log(fb) : 0.0)));
}

void Jet::process(ProcessContext& c) {
    const int n = c.numSamples;
    lfo_.begin(c, sr_, pRate->get(), syncTicks(pSync));
    const float baseT = std::max(2.0f, (float)(pDelay->get() * 0.001 * sr_)), depthT = (float)(pDepth->get() * 0.001 * sr_);
    const float fbT = pFeedback->get(), mixT = pMix->get();
    if (!primed_) { base_.snap(baseT); depth_.snap(depthT); fb_.snap(fbT); mix_.snap(mixT); primed_ = true; }
    base_.target(baseT, n); depth_.target(depthT, n); fb_.target(fbT, n); mix_.target(mixT, n);
    const double off = pPhase->get() / 360.0;
    const bool sine = pShape->getInt() == 0;
    const float maxD = (float)(d_[0].mask - 6);
    for (int i = 0; i < n; ++i) {
        const double ph = lfo_.at(i);
        const float base = base_.next(), depth = depth_.next(), fb = fb_.next();
        float dg, wg;
        space::mixGains(mix_.next(), dg, wg);
        for (int ch = 0; ch < 2; ++ch) {
            const double p = frac(ph + (ch ? off : 0.0));
            const float l = sine ? (float)(0.5 - 0.5 * std::cos(kTwoPi * p)) : (float)(1.0 - std::fabs(2.0 * p - 1.0));
            const float d = std::min(base + depth * l, maxD);
            const float in = c.io[ch][i];
            const float wet = d_[ch].tapCubic(d - 1.0f);        // the sample written d samples ago
            float v = in + fb * wet;
            flushDenormal(v);
            d_[ch].write(v);
            // Positive feedback: the wet signal is raised by (1 + fb), which is what taking the dry signal from the
            // feedback node does in a hardware flanger. Dry + wet is then (1 + z^-D) / (1 - fb z^-D): the notches stay
            // complete and the peaks between them grow. Negative feedback resonates on the notch frequencies instead.
            c.io[ch][i] = in * dg + wet * (fb > 0.0f ? 1.0f + fb : 1.0f) * wg;
        }
    }
}

std::vector<Preset> Jet::factoryPresets() const {
    return buildPresets(*this, {
        {"Slow jet", {}},
        {"Metal drain", {{"delay", 0.6f}, {"depth", 5.0f}, {"rate", 0.12f}, {"feedback", 0.7f}, {"shape", 0}}},
        {"Hollow tube", {{"delay", 1.5f}, {"depth", 4.0f}, {"rate", 0.3f}, {"feedback", -0.8f}, {"phase", 60}}},
        {"Fast warble", {{"delay", 2.0f}, {"depth", 1.2f}, {"rate", 5.5f}, {"feedback", 0.2f}, {"shape", 0}, {"phase", 120}}},
        {"One-bar sweep", {{"delay", 0.4f}, {"depth", 6.0f}, {"sync", 4}, {"feedback", 0.6f}, {"phase", 0}}},
        {"Wide and gentle", {{"delay", 3.0f}, {"depth", 2.0f}, {"rate", 0.18f}, {"feedback", 0.25f}, {"phase", 180}, {"mix", 0.35f}}},
        {"Fixed comb", {{"delay", 2.5f}, {"depth", 0.0f}, {"feedback", 0.7f}}},
    });
}

std::vector<ParamGroup> Jet::paramGroups() const {
    return {{"Sweep", {"delay", "depth", "shape", "feedback"}},
            {"Rate", {"rate", "sync", "phase"}},
            {"Output", {"mix"}}};
}

// ------------------------------------------------------------------ Swirl

Swirl::Swirl() {
    pStages = addChoice("stages", "Stages", {"2", "4", "6", "8", "10", "12"}, 1);
    pLow = addFloat("low", "Sweep from", 50, 5000, 300, "Hz", 3.0f);
    pHigh = addFloat("high", "Sweep to", 100, 12000, 3000, "Hz", 3.0f);
    pRate = addFloat("rate", "Rate (sync off)", 0.01f, 20, 0.4f, "Hz", 3.0f);
    pSync = addChoice("sync", "Rate sync", space::modSyncChoices(), 0);
    pDepth = addFloat("depth", "Depth", 0, 1, 1, "%");
    pFeedback = addFloat("feedback", "Feedback", -0.9f, 0.9f, 0.3f, "%");
    pPhase = addFloat("phase", "Stereo phase", 0, 180, 45, "deg");
    pMix = addFloat("mix", "Dry / wet", 0, 1, 0.5f, "%");
}

void Swirl::reset() {
    std::memset(z_, 0, sizeof z_);
    lfo_.reset();
    stagesSeen_ = 0;
    primed_ = false;
}

double Swirl::tailSeconds() const {
    const double fb = std::fabs(pFeedback->get()), mid = std::sqrt((double)pLow->get() * std::max(pHigh->get(), pLow->get()));
    const double loop = 2.0 * (pStages->getInt() + 1) / (kPi * mid);      // group delay of the chain at its centre
    return std::min(2.0, loop * (1.0 + (fb > 0.001 ? std::log(0.001) / std::log(fb) : 0.0)));
}

void Swirl::process(ProcessContext& c) {
    const int n = c.numSamples;
    lfo_.begin(c, sr_, pRate->get(), syncTicks(pSync));
    const int stages = 2 * (clampT(pStages->getInt(), 0, 5) + 1);
    if (stages != stagesSeen_) {        // stages that were not running start from rest
        for (int ch = 0; ch < 2; ++ch) for (int s = std::min(stages, stagesSeen_); s < kMaxStages; ++s) z_[ch][s] = 0.0f;
        stagesSeen_ = stages;
    }
    const float lo = std::log(pLow->get()), hi = std::log(std::max(pHigh->get(), pLow->get()));
    const float midT = 0.5f * (lo + hi), halfT = 0.5f * (hi - lo);
    const float depthT = pDepth->get(), fbT = pFeedback->get(), mixT = pMix->get();
    if (!primed_) { lnMid_.snap(midT); lnHalf_.snap(halfT); depth_.snap(depthT); fb_.snap(fbT); mix_.snap(mixT); primed_ = true; }
    lnMid_.target(midT, n); lnHalf_.target(halfT, n); depth_.target(depthT, n); fb_.target(fbT, n); mix_.target(mixT, n);
    const double off = pPhase->get() / 360.0;
    const float fMax = (float)(0.45 * sr_);
    const double w = kPi / sr_;
    for (int i = 0; i < n; ++i) {
        const double ph = lfo_.at(i);
        const float mid = lnMid_.next(), sweep = lnHalf_.next() * depth_.next(), fb = fb_.next();
        float dg, wg;
        space::mixGains(mix_.next(), dg, wg);
        for (int ch = 0; ch < 2; ++ch) {
            const float s = (float)std::sin(kTwoPi * (ph + (ch ? off : 0.0)));
            const float hz = clampT(std::exp(mid + sweep * s), 20.0f, fMax);
            const float t = (float)std::tan(w * hz), a = (t - 1.0f) / (t + 1.0f);
            const float in = c.io[ch][i];
            float* z = z_[ch];
            // Feedback without a sample of delay in the loop. The chain's output is linear in its input,
            // out = gain * in + rest (rest = what the states alone would put out), so in = x + fb * out solves directly.
            float gain = 1.0f, rest = 0.0f;
            for (int k = 0; k < stages; ++k) { rest = a * rest + z[k]; gain *= a; }
            float x = (in + fb * rest) / (1.0f - fb * gain);
            for (int k = 0; k < stages; ++k) {      // first-order all-pass: (a + z^-1) / (1 + a z^-1)
                const float y = a * x + z[k];
                z[k] = x - a * y;
                x = y;
            }
            flushDenormal(x);
            // The wet signal is raised by (1 + fb): the same as taking the dry signal from the feedback node, as a
            // hardware phaser does. Dry + wet is then (1 + A) / (1 - fb A): the notches stay complete at any feedback;
            // positive feedback grows peaks between them, negative feedback narrows them.
            c.io[ch][i] = in * dg + x * (1.0f + fb) * wg;
        }
    }
    for (auto& ch : z_) for (float& v : ch) flushDenormal(v);
}

std::vector<Preset> Swirl::factoryPresets() const {
    return buildPresets(*this, {
        {"Four-stage classic", {}},
        {"Slow twelve", {{"stages", 5}, {"low", 150}, {"high", 4000}, {"rate", 0.08f}, {"feedback", 0.45f}, {"phase", 90}}},
        {"Resonant sweep", {{"stages", 2}, {"low", 250}, {"high", 2500}, {"rate", 0.25f}, {"feedback", 0.7f}}},
        {"Shallow shimmer", {{"stages", 0}, {"low", 600}, {"high", 3000}, {"rate", 1.2f}, {"depth", 0.5f}, {"feedback", 0.0f}, {"mix", 0.4f}}},
        {"Quarter-note pulse", {{"stages", 3}, {"sync", 7}, {"low", 300}, {"high", 5000}, {"feedback", 0.6f}, {"phase", 0}}},
        {"Narrow notches", {{"stages", 1}, {"low", 200}, {"high", 1800}, {"rate", 0.5f}, {"feedback", -0.75f}, {"phase", 180}}},
        {"Still notches", {{"stages", 3}, {"low", 400}, {"high", 3200}, {"depth", 0.0f}, {"feedback", 0.5f}}},
    });
}

std::vector<ParamGroup> Swirl::paramGroups() const {
    return {{"Stages", {"stages", "low", "high", "feedback"}},
            {"Sweep", {"rate", "sync", "depth", "phase"}},
            {"Output", {"mix"}}};
}

// ------------------------------------------------------------------ Flutter

Flutter::Flutter() {
    pShape = addChoice("shape", "Shape", {"Sine", "Triangle", "Square", "Saw up", "Saw down"}, 0);
    pRate = addFloat("rate", "Rate (sync off)", 0.05f, 30, 5.0f, "Hz", 3.0f);
    pSync = addChoice("sync", "Rate sync", space::modSyncChoices(), 0);
    pDepth = addFloat("depth", "Depth", 0, 1, 0.6f, "%");
    pPhase = addFloat("phase", "Stereo phase (180 = auto-pan)", 0, 180, 0, "deg");
    pSmooth = addFloat("smooth", "Edge smoothing", 0, 50, 4, "ms", 2.0f);
}

void Flutter::reset() {
    lfo_.reset();
    primed_ = false;
}

float Flutter::shapeAt(int shape, double phase, double width) {
    const double p = frac(phase), w = clampT(width, 0.0, 0.5);
    auto edge = [](double x) { return 0.5 - 0.5 * std::cos(kPi * clampT(x, 0.0, 1.0)); };       // 0 to 1, smoothly
    auto sawUp = [&](double q) { return w <= 0.0 || q < 1.0 - w ? q / (1.0 - w) : 1.0 - edge((q - (1.0 - w)) / w); };
    switch (shape) {
        case Triangle: return (float)std::fabs(2.0 * p - 1.0);
        case Square:
            if (w <= 0.0) return p < 0.5 ? 1.0f : 0.0f;
            if (p >= 1.0 - 0.5 * w) return (float)edge((p - 1.0 + 0.5 * w) / w);
            if (p < 0.5 * w) return (float)edge((p + 0.5 * w) / w);
            if (p < 0.5 - 0.5 * w) return 1.0f;
            if (p < 0.5 + 0.5 * w) return (float)(1.0 - edge((p - 0.5 + 0.5 * w) / w));
            return 0.0f;
        case SawUp: return (float)sawUp(p);
        case SawDown: return (float)sawUp(p > 0.0 ? 1.0 - p : 0.0);
        default: return (float)(0.5 + 0.5 * std::cos(kTwoPi * p));
    }
}

void Flutter::process(ProcessContext& c) {
    const int n = c.numSamples;
    lfo_.begin(c, sr_, pRate->get(), syncTicks(pSync));
    const int shape = clampT(pShape->getInt(), 0, (int)NumShapes - 1);
    const double width = pSmooth->get() * 0.001 * lfo_.rateHz;
    const double off = pPhase->get() / 360.0;
    const float depthT = pDepth->get();
    if (!primed_) { depth_.snap(depthT); primed_ = true; }
    depth_.target(depthT, n);
    for (int i = 0; i < n; ++i) {
        const double ph = lfo_.at(i);
        const float d = depth_.next();
        const float gl = 1.0f - d * (1.0f - shapeAt(shape, ph, width));
        const float gr = off == 0.0 ? gl : 1.0f - d * (1.0f - shapeAt(shape, ph + off, width));
        c.io[0][i] *= gl;
        c.io[1][i] *= gr;
    }
}

std::vector<Preset> Flutter::factoryPresets() const {
    return buildPresets(*this, {
        {"Gentle tremolo", {}},
        {"Slow auto-pan", {{"rate", 0.4f}, {"depth", 0.9f}, {"phase", 180}}},
        {"Sixteenth chop", {{"shape", Square}, {"sync", 12}, {"depth", 1.0f}, {"smooth", 3}}},
        {"Quarter-note pump", {{"shape", SawUp}, {"sync", 7}, {"depth", 0.85f}, {"smooth", 12}}},
        {"Eighth-note gate", {{"shape", SawDown}, {"sync", 10}, {"depth", 1.0f}, {"smooth", 6}}},
        {"Wide triangle pan", {{"shape", Triangle}, {"sync", 4}, {"depth", 1.0f}, {"phase", 180}}},
        {"Fast shiver", {{"rate", 11.0f}, {"depth", 0.35f}, {"phase", 90}}},
    });
}

// ------------------------------------------------------------------ Waver

namespace {
constexpr double kWaverMinRate = 0.5, kWaverMaxCents = 100.0;
}

Waver::Waver() {
    pRate = addFloat("rate", "Rate (sync off)", (float)kWaverMinRate, 14, 5.5f, "Hz", 2.0f);
    pSync = addChoice("sync", "Rate sync", space::modSyncChoices(), 0);
    pDepth = addFloat("depth", "Depth", 0, (float)kWaverMaxCents, 20, "ct", 2.0f);
    pPhase = addFloat("phase", "Stereo phase", 0, 180, 0, "deg");
    pOnset = addFloat("onset", "Delay after silence", 0, 1000, 0, "ms", 2.0f);
    pFadeIn = addFloat("fadein", "Fade-in", 0, 2000, 0, "ms", 2.0f);
}

void Waver::prepare(double sr) {
    sr_ = sr;
    // the deepest, slowest setting swings the delay this far each way; the centre sits just above it
    const double swing = (std::pow(2.0, kWaverMaxCents / 1200.0) - 1.0) * sr / (kTwoPi * kWaverMinRate);
    latency_ = (int)std::ceil(swing) + 4;
    for (auto& d : d_) d.prepare(2 * latency_ + 16);
    reset();
}

void Waver::reset() {
    for (auto& d : d_) d.clear();
    lfo_.reset();
    env_ = 0.0f;
    quiet_ = 1 << 28;
    sounding_ = 0;
    primed_ = false;
}

void Waver::process(ProcessContext& c) {
    const int n = c.numSamples;
    lfo_.begin(c, sr_, pRate->get(), syncTicks(pSync));
    const double rate = std::max(lfo_.rateHz, 1.0e-3);
    const double ratio = std::pow(2.0, (double)pDepth->get() / 1200.0) - 1.0;       // peak change of speed
    const float ampT = (float)std::min(ratio * sr_ / (kTwoPi * rate), (double)(latency_ - 3));
    if (!primed_) { amp_.snap(ampT); primed_ = true; }
    amp_.target(ampT, n);
    const double off = pPhase->get() / 360.0;
    const float onset = (float)(pOnset->get() * 0.001 * sr_), fadeSet = (float)(pFadeIn->get() * 0.001 * sr_);
    const bool gated = onset > 0.0f || fadeSet > 0.0f;
    const float fade = std::max(fadeSet, (float)(0.02 * sr_));
    const float rel = (float)std::exp(-1.0 / (0.02 * sr_));
    const int hold = (int)(0.05 * sr_);
    const float centre = (float)latency_;
    for (int i = 0; i < n; ++i) {
        const float inL = c.io[0][i], inR = c.io[1][i];
        d_[0].write(inL);
        d_[1].write(inR);
        float scale = 1.0f;
        if (gated) {        // the vibrato waits for a note, then comes in, as a player's would
            env_ = std::max(std::max(std::fabs(inL), std::fabs(inR)), env_ * rel);
            if (env_ < 1.0e-3f) { if (quiet_ < (1 << 28)) ++quiet_; } else quiet_ = 0;
            if (quiet_ > hold) sounding_ = 0;
            else if (sounding_ < (1 << 28)) ++sounding_;
            scale = clampT(((float)sounding_ - onset - centre) / fade, 0.0f, 1.0f);
        }
        const float a = amp_.next() * scale;
        const double ph = lfo_.at(i);
        c.io[0][i] = d_[0].tapCubic(centre + a * (float)std::sin(kTwoPi * ph));
        c.io[1][i] = d_[1].tapCubic(centre + a * (float)std::sin(kTwoPi * (ph + off)));
    }
    flushDenormal(env_);
}

std::vector<Preset> Waver::factoryPresets() const {
    return buildPresets(*this, {
        {"String vibrato", {{"rate", 5.5f}, {"depth", 18}, {"onset", 250}, {"fadein", 400}}},
        {"Singer's vibrato", {{"rate", 5.0f}, {"depth", 30}, {"onset", 400}, {"fadein", 600}}},
        {"Tape wow", {{"rate", 0.6f}, {"depth", 22}}},
        {"Tape flutter", {{"rate", 11.0f}, {"depth", 6}}},
        {"Nervous", {{"rate", 8.0f}, {"depth", 55}}},
        {"Stereo drift", {{"rate", 0.8f}, {"depth", 14}, {"phase", 180}}},
        {"Eighth-note wobble", {{"sync", 10}, {"depth", 35}}},
    });
}

// ------------------------------------------------------------------ Rotor

Rotor::Rotor() {
    pHorn = addChoice("horn", "Horn speed", {"Stop", "Slow", "Fast"}, Slow);
    pDrum = addChoice("drum", "Drum speed", {"Stop", "Slow", "Fast"}, Slow);
    pHornSlow = addFloat("hornslow", "Horn slow rate", 0.2f, 2, 0.8f, "Hz");
    pHornFast = addFloat("hornfast", "Horn fast rate", 3, 10, 6.8f, "Hz");
    pDrumSlow = addFloat("drumslow", "Drum slow rate", 0.2f, 2, 0.67f, "Hz");
    pDrumFast = addFloat("drumfast", "Drum fast rate", 3, 10, 5.8f, "Hz");
    pInertia = addFloat("inertia", "Inertia", 0.25f, 4, 1, "x", 2.0f);
    pDoppler = addFloat("doppler", "Doppler", 0, 1, 0.8f, "%");
    pAmDepth = addFloat("amdepth", "Amplitude depth", 0, 1, 0.6f, "%");
    pSpread = addFloat("spread", "Microphone spread", 0, 180, 100, "deg");
    pDistance = addFloat("distance", "Microphone distance", 0, 1, 0.35f, "%");
    pDrive = addFloat("drive", "Drive", 0, 1, 0.25f, "%");
    pBalance = addFloat("balance", "Drum / horn balance", -1, 1, 0);
    pMix = addFloat("mix", "Dry / wet", 0, 1, 1, "%");
    speedPub_[0].store(0.0f);
    speedPub_[1].store(0.0f);
    // The oversampling filter: low-pass at the original Nyquist frequency, Blackman window (the scheme Grit uses).
    const double fc = 0.5 / kOver;
    double sum = 0.0;
    for (int k = 0; k < kTaps; ++k) {
        const double m = k - (kTaps - 1) / 2.0;
        const double sinc = m == 0.0 ? 2.0 * fc : std::sin(kTwoPi * fc * m) / (kPi * m);
        const double w = 0.42 - 0.5 * std::cos(kTwoPi * k / (kTaps - 1)) + 0.08 * std::cos(2.0 * kTwoPi * k / (kTaps - 1));
        fir_[k] = (float)(sinc * w);
        sum += fir_[k];
    }
    for (float& v : fir_) v = (float)(v / sum);
}

void Rotor::prepare(double sr) {
    sr_ = sr;
    centre_ = (int)std::lround(0.0012 * sr);
    latency_ = kOverLatency + centre_;
    horn_.prepare(2 * centre_ + 16);
    drum_.prepare(2 * centre_ + 16);
    for (auto& d : dry_) d.prepare(latency_ + 8);
    for (int i = 0; i < 2; ++i) { lowX_[i].lowpass(800.0, 0.7071, sr); highX_[i].highpass(800.0, 0.7071, sr); }    // fourth-order crossover
    hornPeak_.peak(1900.0, 1.0, 3.0, sr);       // the honk of a horn driver
    hornCut_.lowpass(7500.0, 0.7071, sr);
    for (auto& t : hornTone_) t.setHz(1800.0, sr);
    for (auto& t : reflTone_) t.setHz(2500.0, sr);
    reset();
}

void Rotor::reset() {
    std::memset(up_, 0, sizeof up_);
    std::memset(down_, 0, sizeof down_);
    upPos_ = downPos_ = 0;
    for (int i = 0; i < 2; ++i) { lowX_[i].reset(); highX_[i].reset(); hornTone_[i].reset(); reflTone_[i].reset(); dry_[i].clear(); }
    hornPeak_.reset();
    hornCut_.reset();
    horn_.clear();
    drum_.clear();
    angle_[0] = angle_[1] = 0.0;
    primed_ = false;
}

float Rotor::meter(int index) const {
    return index == 0 || index == 1 ? speedPub_[index].load(std::memory_order_relaxed) : 0.0f;
}

void Rotor::process(ProcessContext& c) {
    const int n = c.numSamples;
    // ---- where the rotors are heading, and how quickly they get there ----
    const int hornSel = pHorn->getInt(), drumSel = pDrum->getInt();
    const float target[2] = {hornSel == Stop ? 0.0f : (hornSel == Slow ? pHornSlow->get() : pHornFast->get()),
                             drumSel == Stop ? 0.0f : (drumSel == Slow ? pDrumSlow->get() : pDrumFast->get())};
    // The light horn is up to speed in about a second; the heavy drum takes several.
    static const double tauUp[2] = {0.45, 2.2}, tauDown[2] = {0.75, 1.8};
    const double inertia = pInertia->get();
    float coef[2];
    for (int r = 0; r < 2; ++r) {
        if (!primed_) speed_[r] = target[r];
        const double tau = (target[r] > speed_[r] ? tauUp[r] : tauDown[r]) * inertia;
        coef[r] = (float)(1.0 - std::exp(-1.0 / (tau * sr_)));
    }
    // ---- amplifier, rotors, microphones ----
    const float g = 0.25f * std::pow(10.0f, pDrive->get() * 1.8f);          // -12 dB to +24 dB into the valve stage
    const float doppler = pDoppler->get(), am = pAmDepth->get(), dist = pDistance->get(), bal = pBalance->get();
    const float driveT = g, makeupT = g <= 1.0f ? 1.0f / g : 1.0f / std::sqrt(g);
    const float swingHT = (float)(doppler * 0.00055 * sr_), swingDT = (float)(doppler * 0.0002 * sr_);
    const float amHT = am * 0.85f * lerp(1.0f, 0.45f, dist), amDT = am * 0.4f * lerp(1.0f, 0.5f, dist);
    const float reflT = 0.5f * dist, bleedT = 0.3f * dist;
    const float hornGT = bal < 0.0f ? 1.0f + bal : 1.0f, drumGT = bal > 0.0f ? 1.0f - bal : 1.0f, mixT = pMix->get();
    if (!primed_) {
        drive_.snap(driveT); makeup_.snap(makeupT); swingH_.snap(swingHT); swingD_.snap(swingDT); amH_.snap(amHT); amD_.snap(amDT);
        refl_.snap(reflT); bleed_.snap(bleedT); hornG_.snap(hornGT); drumG_.snap(drumGT); mix_.snap(mixT);
        primed_ = true;
    }
    drive_.target(driveT, n); makeup_.target(makeupT, n); swingH_.target(swingHT, n); swingD_.target(swingDT, n);
    amH_.target(amHT, n); amD_.target(amDT, n); refl_.target(reflT, n); bleed_.target(bleedT, n);
    hornG_.target(hornGT, n); drumG_.target(drumGT, n); mix_.target(mixT, n);
    const double micHalf = pSpread->get() / 720.0;          // each microphone sits half the spread from the middle, in turns
    const float centre = (float)centre_;
    const double invSr = 1.0 / sr_;

    for (int i = 0; i < n; ++i) {
        const float inL = c.io[0][i], inR = c.io[1][i];
        // the cabinet has one amplifier: mono in. Four times oversampled so the valve stage does not alias.
        const float drive = drive_.next();
        up_[upPos_] = 0.5f * (inL + inR);
        for (int p = 0; p < kOver; ++p) {
            float u = 0.0f;
            int idx = upPos_;
            for (int j = p; j < kTaps; j += kOver) { u += fir_[j] * up_[idx]; idx = idx == 0 ? kUpHist - 1 : idx - 1; }
            down_[(downPos_ + p) % kTaps] = std::tanh(u * (float)kOver * drive);
        }
        float y = 0.0f;
        int idx = downPos_ % kTaps;
        for (int j = 0; j < kTaps; ++j) { y += fir_[j] * down_[idx]; idx = idx == 0 ? kTaps - 1 : idx - 1; }
        upPos_ = (upPos_ + 1) % kUpHist;
        downPos_ = (downPos_ + kOver) % kTaps;
        y *= makeup_.next();
        // crossover: drum below 800 Hz, horn above
        const float lo = lowX_[1].process(lowX_[0].process(y));
        const float hi = hornCut_.process(hornPeak_.process(highX_[1].process(highX_[0].process(y))));
        horn_.write(hi);
        drum_.write(lo);
        // the rotors turn in opposite directions
        for (int r = 0; r < 2; ++r) speed_[r] += (target[r] - speed_[r]) * coef[r];
        angle_[0] += speed_[0] * invSr;
        angle_[1] -= speed_[1] * invSr;
        if (angle_[0] >= 1.0) angle_[0] -= 1.0;
        if (angle_[1] < 0.0) angle_[1] += 1.0;
        const float swingH = swingH_.next(), swingD = swingD_.next(), amH = amH_.next(), amD = amD_.next(), refl = refl_.next();
        const float hornG = hornG_.next(), drumG = drumG_.next();
        float mic[2];
        for (int m = 0; m < 2; ++m) {
            const double at = m ? micHalf : -micHalf;
            // horn: nearer and louder and brighter when its mouth faces the microphone
            const double aH = kTwoPi * (angle_[0] - at);
            const float cH = (float)std::cos(aH), away = 0.5f - 0.5f * cH;
            float h = horn_.tapCubic(centre - swingH * cH);
            h += (hornTone_[m].lp(h) - h) * (amH * 0.75f * away);
            h *= 1.0f - amH * away;
            // one reflection from the cabinet wall behind: later, duller, turning the other way round
            const float cR = (float)std::cos(aH + 2.4);
            const float rf = reflTone_[m].lp(horn_.tapCubic(centre + 0.35f * swingH * cR)) * refl;
            // drum: a gentler version of the same
            const float cD = (float)std::cos(kTwoPi * (angle_[1] - 0.6 * at));
            const float d = drum_.tapCubic(centre - swingD * cD) * (1.0f - amD * (0.5f - 0.5f * cD));
            mic[m] = hornG * (h + rf) + drumG * d;
        }
        // far microphones hear more of each other's side
        const float bleed = bleed_.next();
        const float wl = mic[0] + bleed * (mic[1] - mic[0]), wr = mic[1] + bleed * (mic[0] - mic[1]);
        dry_[0].push(inL);
        dry_[1].push(inR);
        float dg, wg;
        space::mixGains(mix_.next(), dg, wg);
        c.io[0][i] = dry_[0].tap(latency_) * dg + wl * wg;
        c.io[1][i] = dry_[1].tap(latency_) * dg + wr * wg;
    }
    for (int r = 0; r < 2; ++r) {
        if (std::fabs(speed_[r] - target[r]) < 1.0e-5f) speed_[r] = target[r];
        speedPub_[r].store(speed_[r], std::memory_order_relaxed);
        flushDenormal(hornTone_[r].z);
        flushDenormal(reflTone_[r].z);
        space::settle(lowX_[r]);
        space::settle(highX_[r]);
    }
    space::settle(hornPeak_);
    space::settle(hornCut_);
}

std::vector<Preset> Rotor::factoryPresets() const {
    return buildPresets(*this, {
        {"Chorale", {}},
        {"Tremolo", {{"horn", Fast}, {"drum", Fast}}},
        {"Brake", {{"horn", Stop}, {"drum", Stop}}},
        {"Overdriven tremolo", {{"horn", Fast}, {"drum", Fast}, {"drive", 0.75f}, {"amdepth", 0.7f}}},
        {"Close and wide", {{"spread", 170}, {"distance", 0.05f}, {"amdepth", 0.8f}}},
        {"Across the room", {{"horn", Fast}, {"drum", Fast}, {"spread", 60}, {"distance", 0.9f}, {"amdepth", 0.5f}}},
        {"Horn spins, drum rests", {{"horn", Fast}, {"drum", Stop}}},
        {"Heavy rotors", {{"horn", Fast}, {"drum", Fast}, {"inertia", 2.5f}, {"hornfast", 6.0f}, {"drumfast", 5.0f}}},
        {"Pitch wobble cabinet", {{"horn", Fast}, {"drum", Fast}, {"doppler", 1.0f}, {"amdepth", 0.2f}, {"drive", 0.1f}}},
    });
}

std::vector<ParamGroup> Rotor::paramGroups() const {
    return {{"Speed", {"horn", "drum", "hornslow", "hornfast", "drumslow", "drumfast", "inertia"}},
            {"Rotors", {"doppler", "amdepth", "balance"}},
            {"Microphones", {"spread", "distance"}},
            {"Amplifier", {"drive", "mix"}}};
}

} // namespace pulp
