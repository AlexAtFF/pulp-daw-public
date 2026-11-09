#include "pulp/Thud.h"

namespace pulp {

namespace {
constexpr float kOut = 0.7f;
constexpr float kUnder = 1.0e-3f;           // the half-amplitude aims this far below zero, so a hit ends exactly
constexpr int kTail = 256;                  // samples the output filters are run on after the last voice
inline double curvePower(double curve) { return std::pow(10.0, clampT(curve, 0.0, 1.0)); }     // 1 = straight line, 10 = steep
}

Thud::Thud() {
    pStart = addFloat("start", "Start pitch", 30, 3000, 240, "Hz", 3.0f);
    pEnd = addFloat("end", "End pitch", 20, 500, 52, "Hz", 2.0f);
    pSweep = addFloat("sweep", "Sweep time", 1, 1000, 45, "ms", 3.0f);
    pCurve = addFloat("curve", "Sweep curve", 0, 1, 0.6f, "%");
    pDecay = addFloat("decay", "Decay", 20, 8000, 320, "ms", 3.0f);
    pClick = addFloat("click", "Click", 0, 1, 0.35f, "%");
    pClickTone = addFloat("clicktone", "Click tone", 500, 12000, 3500, "Hz", 3.0f);
    pDrive = addFloat("drive", "Drive", 0, 1, 0.1f, "%");
    pSub = addFloat("sub", "Sub", 0, 1, 0, "%");
    pTone = addFloat("tone", "Tone", 200, 20000, 20000, "Hz", 3.0f);
    pTune = add({"tune", "Tune", -24, 24, 0, 1, 49, "st", {}});
    pFine = addFloat("fine", "Fine tune", -100, 100, 0, "ct");
    pKeyTrack = addFloat("keytrack", "Key follow", 0, 1, 1, "%");
    pChoke = addBool("choke", "New hit chokes the last", true);
    pGate = addBool("gate", "Note length cuts the tail", false);
    pVelSens = addFloat("velsens", "Velocity sensitivity", 0, 1, 0.7f, "%");
    pVolume = addFloat("volume", "Level", 0, 1, 0.5f, "%");
}

std::vector<ParamGroup> Thud::paramGroups() const {
    return {
        {"Pitch", {"start", "end", "sweep", "curve", "tune", "fine", "keytrack"}},
        {"Body", {"decay", "drive", "sub", "tone"}},
        {"Click", {"click", "clicktone"}},
        {"Play", {"choke", "gate", "velsens", "volume"}},
    };
}

double Thud::sweepHz(double seconds) const {
    const double start = pStart->get(), end = pEnd->get();
    const double x = seconds / (0.001 * (double)pSweep->get());
    return end + (start - end) * (x < 1.0 ? std::pow(1.0 - x, curvePower(pCurve->get())) : 0.0);
}

void Thud::prepare(double sampleRate) {
    sr_ = sampleRate;
    for (int c = 0; c < 2; ++c) down_[c].prepare(sampleRate);
    reset();
}

void Thud::reset() {
    for (auto& v : voices_) v.active = false;
    for (int c = 0; c < 2; ++c) { down_[c].reset(); tone_[c].reset(); }
    tail_ = 0;
    ageCounter_ = 0;
    modX_ = 0.5f;
    primed_ = false;
    voicesActive_.store(0, std::memory_order_relaxed);
}

void Thud::beginBlock(ProcessContext& ctx) {
    bend_ = ctx.pitchBendSemis;
    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
    const float vol = pVolume->get();
    if (!primed_ || (n == 0 && tail_ == 0)) vol_.snap(vol);
    else vol_.target(vol, ctx.numSamples);
    primed_ = true;
}

void Thud::startVoice(const NoteEvent& e) {
    // One drum: the hit before is faded out over 4 ms while this one starts from zero, so nothing clicks.
    if (pChoke->getBool())
        for (auto& o : voices_)
            if (o.active && o.killStep == 0.0f) o.killStep = (float)(1.0 / (0.004 * sr_));
    Voice* v = bl::claimVoice(voices_, kSounding, (float)(1.0 / (0.003 * sr_)));
    v->active = true;
    v->released = false;
    v->id = e.id;
    v->age = ++ageCounter_;
    v->key = e.key;
    v->group = e.group;
    v->t = 0.0;
    v->phase = 0.0;             // a sine from its zero crossing: the hit itself starts without a step
    v->half.v = 1.0f;
    v->fine = e.fineCents;
    const float vel = e.velocity / kDefaultVelocity;
    v->velGain = std::max(0.0f, 1.0f + (vel * vel - 1.0f) * pVelSens->get());
    bl::noteGains(e.pan, v->panL, v->panR);
    v->modY = e.modY;
    v->relScale = 0.25f + 1.5f * clampT(e.release, 0.0f, 1.0f);
    v->bend = v->bendTarget = v->bendStep = 0.0f;
    if (e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) {
        v->bend = e.glideFromKey - (float)e.key;
        v->bendStep = -v->bend / e.slideSamples;
    }
    v->noise = dsp::Noise(0x2F6E2B1u);          // the same click every time
    v->clickFilter.reset();
    v->clickEnv = 1.0f;
    v->clickLeft = 512;
    v->kill = 1.0f;
    v->killStep = 0.0f;
    modX_ = e.modX;
}

void Thud::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * sr_));
    switch (e.type) {
        case NoteEvent::On: startVoice(e); break;
        case NoteEvent::Off:
            if (!pGate->getBool()) break;           // a drum hit rings out whatever the note's length
            for (auto& v : voices_) if (v.active && v.id == e.id) v.released = true;
            break;
        case NoteEvent::AllOff:
            for (auto& v : voices_) if (v.active) v.released = true;
            break;
        case NoteEvent::Kill:
            for (auto& v : voices_) if (v.active && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::KillNote:
            for (auto& v : voices_) if (v.active && v.id == e.id && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::SlideTo:
            for (auto& v : voices_)
                if (v.active && v.group == e.group && !v.released) {
                    v.bendTarget = (float)(e.key - v.key);
                    v.bendStep = (v.bendTarget - v.bend) / std::max(1.0f, e.slideSamples);
                }
            break;
    }
}

void Thud::render(float* L, float* R, int n, ProcessContext&) {
    const double startHz = pStart->get(), endHz = pEnd->get();
    const double sweepSamples = std::max(1.0, 0.001 * (double)pSweep->get() * sr_);
    const double power = curvePower(pCurve->get());
    const double tune = (double)pTune->get() + (double)pFine->get() * 0.01 + (double)bend_, keyTrack = pKeyTrack->get();
    const double decay = 0.001 * (double)pDecay->get();
    const float halfCoef = bl::t60Coef(2.0 * decay, sr_);            // half the decibels each: together they fall 60 dB in the decay time
    const float clickCoef = bl::t60Coef(0.008, sr_);
    const float click = pClick->get() * 6.0f, sub = pSub->get(), drive = pDrive->get();
    const float clickHz = pClickTone->get();
    const float smooth = bl::tauCoef(0.002, sr_);
    const double fs2 = 2.0 * sr_;

    float l2[2 * kQuantum] = {}, r2[2 * kQuantum] = {}, xl[kQuantum] = {}, xr[kQuantum] = {};
    bool any = false;
    for (auto& v : voices_) {
        if (!v.active) continue;
        any = true;
        const double semis = ((double)(v.key - kRootKey) + (double)v.bend) * keyTrack + tune + (double)v.fine * 0.01;
        const double ratio = std::exp2(semis / 12.0);
        if (v.bendStep != 0.0f) {
            v.bend += v.bendStep * (float)n;
            if ((v.bendStep > 0.0f) == (v.bend >= v.bendTarget)) { v.bend = v.bendTarget; v.bendStep = 0.0f; }
        }
        // drive: 0.3 (clean) to 60 times into tanh, made up so the peak stays where it was
        const float gT = 0.3f * std::pow(10.0f, clampT(drive + (v.modY - 0.5f), 0.0f, 1.0f) * 46.0f / 20.0f);
        if (v.t == 0.0) { v.g = gT; v.norm = 1.0f / std::tanh(gT); }
        const float coef = v.released ? bl::t60Coef(2.0 * 0.08 * (double)v.relScale, sr_) : halfCoef;
        v.clickFilter.set(clickHz, 0.9f, (float)sr_);

        for (int i = 0; i < n; ++i) {
            const double x = v.t / sweepSamples;
            const double hz = (endHz + (startHz - endHz) * (x < 1.0 ? std::pow(1.0 - x, power) : 0.0)) * ratio;
            const double dt2 = std::min(0.24, hz / fs2);
            const float h = v.half.v, a = v.velGain * v.kill;
            // the sub is the same sine before the drive, so it is always in step with the body
            float extra = sub > 0.0f ? sub * (float)std::sin(kTwoPi * v.phase) * h * h : 0.0f;
            if (v.g != gT) {            // the make-up always belongs to the gain in use
                v.g += smooth * (gT - v.g);
                if (std::fabs(gT - v.g) < 1.0e-4f * gT) v.g = gT;
                v.norm = 1.0f / std::tanh(v.g);
            }
            const float norm = v.norm;
            for (int s = 0; s < 2; ++s) {
                const float body = (float)std::sin(kTwoPi * v.phase);
                v.phase += dt2;
                if (v.phase >= 1.0) v.phase -= 1.0;
                const float o = std::tanh(v.g * body * h) * norm * h * a;
                l2[2 * i + s] += o * v.panL;
                r2[2 * i + s] += o * v.panR;
            }
            // the click stays clean as well; its filter is run on for a moment after the burst is over
            if (v.clickLeft > 0) {
                float lp, bp, hp;
                v.clickFilter.process(v.noise.next() * v.clickEnv, lp, bp, hp);
                extra += bp * click;
                v.clickEnv *= clickCoef;
                if (v.clickEnv < 1.0e-5f) { v.clickEnv = 0.0f; --v.clickLeft; }
            }
            xl[i] += extra * a * v.panL;
            xr[i] += extra * a * v.panR;

            v.t += 1.0;
            v.half.v = (v.half.v + kUnder) * coef - kUnder;
            if (v.half.v <= 0.0f) { v.active = false; break; }
            if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) { v.active = false; break; } }
        }
    }
    if (any) tail_ = kTail;
    if (tail_ <= 0) return;
    const float toneHz = clampT(pTone->get() * std::pow(2.0f, (modX_ - 0.5f) * 8.0f), 40.0f, 20000.0f);
    tone_[0].set(toneHz, 0.707f, (float)sr_);
    tone_[1].set(toneHz, 0.707f, (float)sr_);
    for (int i = 0; i < n; ++i) {
        const float vol = vol_.next() * kOut;
        L[i] += tone_[0].lowpass(down_[0].process(l2[2 * i], l2[2 * i + 1]) + xl[i]) * vol;
        R[i] += tone_[1].lowpass(down_[1].process(r2[2 * i], r2[2 * i + 1]) + xr[i]) * vol;
    }
    if (!any) {
        tail_ -= n;
        if (tail_ <= 0) { tail_ = 0; for (int c = 0; c < 2; ++c) { down_[c].reset(); tone_[c].reset(); } }
    }
    scope_.push(L, R, n);
}

std::vector<Preset> Thud::factoryPresets() const {
    return buildPresets(*this, {
        {"Tight Punch", {}},
        {"Long Boom", {{"start", 160}, {"end", 46}, {"sweep", 70}, {"curve", 0.5f}, {"decay", 2200}, {"click", 0.15f}, {"drive", 0.05f}, {"sub", 0.2f}}},
        {"Sub Drop", {{"start", 110}, {"end", 38}, {"sweep", 260}, {"curve", 0.25f}, {"decay", 1800}, {"click", 0}, {"drive", 0}, {"sub", 0.3f}}},
        {"Club Thump", {{"volume", 0.38f}, {"start", 300}, {"end", 50}, {"sweep", 60}, {"curve", 0.7f}, {"decay", 480}, {"click", 0.3f}, {"drive", 0.3f}}},
        {"Hard Driven", {{"volume", 0.30f}, {"start", 500}, {"end", 55}, {"sweep", 90}, {"curve", 0.65f}, {"decay", 700}, {"click", 0.5f}, {"drive", 0.85f}, {"tone", 9000}}},
        {"Stomp Distorted", {{"volume", 0.29f}, {"start", 700}, {"end", 60}, {"sweep", 140}, {"curve", 0.5f}, {"decay", 900}, {"click", 0.6f}, {"drive", 1.0f}, {"tone", 6000}}},
        {"Soft Felt", {{"start", 120}, {"end", 60}, {"sweep", 25}, {"curve", 0.4f}, {"decay", 260}, {"click", 0.05f}, {"drive", 0}, {"tone", 1800}}},
        {"Low Tom", {{"start", 190}, {"end", 95}, {"sweep", 120}, {"curve", 0.35f}, {"decay", 650}, {"click", 0.15f}, {"drive", 0.05f}, {"choke", 0}}},
        {"Mid Tom", {{"start", 280}, {"end", 145}, {"sweep", 100}, {"curve", 0.35f}, {"decay", 520}, {"click", 0.15f}, {"drive", 0.05f}, {"choke", 0}}},
        {"High Tom", {{"start", 400}, {"end", 210}, {"sweep", 90}, {"curve", 0.35f}, {"decay", 420}, {"click", 0.2f}, {"drive", 0.05f}, {"choke", 0}}},
        {"Laser Zap", {{"start", 2800}, {"end", 60}, {"sweep", 180}, {"curve", 0.55f}, {"decay", 400}, {"click", 0.1f}, {"drive", 0.2f}}},
        {"Deep House", {{"start", 200}, {"end", 44}, {"sweep", 50}, {"curve", 0.7f}, {"decay", 620}, {"click", 0.2f}, {"drive", 0.15f}, {"tone", 5000}}},
        {"Glide Bass", {{"start", 90}, {"end", 55}, {"sweep", 30}, {"decay", 3500}, {"click", 0.1f}, {"drive", 0.25f}, {"sub", 0.3f}, {"gate", 1}}},
        {"Rock Kick", {{"volume", 0.25f}, {"start", 330}, {"end", 62}, {"sweep", 35}, {"curve", 0.7f}, {"decay", 300}, {"click", 0.7f}, {"clicktone", 4500}, {"drive", 0.2f}}},
        {"Clicky Metal", {{"volume", 0.16f}, {"start", 420}, {"end", 58}, {"sweep", 22}, {"curve", 0.8f}, {"decay", 240}, {"click", 1.0f}, {"clicktone", 7000}, {"drive", 0.35f}}},
        {"Rubber", {{"start", 380}, {"end", 70}, {"sweep", 300}, {"curve", 0.2f}, {"decay", 500}, {"click", 0}, {"drive", 0.1f}}},
        {"Boxy", {{"start", 260}, {"end", 85}, {"sweep", 40}, {"decay", 220}, {"click", 0.25f}, {"drive", 0.2f}, {"tone", 1200}}},
        {"Distorted Tail", {{"start", 180}, {"end", 48}, {"sweep", 60}, {"decay", 2600}, {"click", 0.2f}, {"drive", 0.7f}, {"tone", 3500}, {"sub", 0.2f}}},
        {"Pillow", {{"start", 100}, {"end", 50}, {"sweep", 40}, {"curve", 0.3f}, {"decay", 380}, {"click", 0}, {"drive", 0}, {"tone", 900}}},
        {"Knocker", {{"volume", 0.32f}, {"start", 900}, {"end", 75}, {"sweep", 18}, {"curve", 0.85f}, {"decay", 180}, {"click", 0.45f}, {"drive", 0.3f}}},
        {"Short Sub", {{"volume", 0.37f}, {"start", 130}, {"end", 42}, {"sweep", 35}, {"decay", 300}, {"click", 0.05f}, {"drive", 0}, {"sub", 0.5f}}},
        {"Tunnel", {{"start", 220}, {"end", 40}, {"sweep", 400}, {"curve", 0.15f}, {"decay", 1400}, {"click", 0.2f}, {"drive", 0.45f}, {"tone", 2500}}},
        {"Falling Tom", {{"start", 600}, {"end", 110}, {"sweep", 350}, {"curve", 0.3f}, {"decay", 700}, {"click", 0.1f}, {"drive", 0.1f}, {"choke", 0}}},
    });
}

} // namespace pulp
