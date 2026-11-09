#include "pulp/Twine.h"

namespace pulp {

namespace {
constexpr int kChunk = 32;                  // samples between retunes while a string is bending
constexpr float kOut = 0.8f;
constexpr float kQuiet = 1.5e-5f;           // a string below this for two periods is finished
const float kBodyHz[3] = {105.0f, 205.0f, 410.0f}, kBodyQ[3] = {5.0f, 7.0f, 6.0f};
}

Twine::Twine() {
    pDecay = addFloat("decay", "Decay", 0.1f, 20.0f, 4.0f, "s", 3.0f);
    pDecayTrack = addFloat("decaytrack", "Decay key follow", 0, 1, 0, "%");
    pDamping = addFloat("damping", "Damping", 0, 1, 0.4f, "%");
    pBright = addFloat("brightness", "Brightness", 0, 1, 0.6f, "%");
    pPosition = addFloat("position", "Pick position", 0.03f, 0.5f, 0.18f, "%");
    pStiff = addFloat("stiffness", "Stiffness", 0, 1, 0, "%");
    pNoise = addFloat("noise", "Pick noise", 0, 1, 0.15f, "%");
    pMute = addFloat("mute", "Palm mute", 0, 1, 0, "%");
    pRelease = addFloat("release", "Release", 10, 5000, 200, "ms", 3.0f);
    pRing = addBool("ring", "Let ring", false);
    pBody = addFloat("body", "Body", 0, 1, 0.25f, "%");
    pBodySize = addFloat("bodysize", "Body size", 0, 1, 0.5f, "%");
    pSpread = addFloat("spread", "Stereo spread", 0, 1, 0.3f, "%");
    pVelTone = addFloat("veltone", "Velocity to tone", 0, 1, 0.5f, "%");
    pTune = add({"tune", "Tune", -24, 24, 0, 1, 49, "st", {}});
    pFine = addFloat("fine", "Fine tune", -100, 100, 0, "ct");
    pVolume = addFloat("volume", "Level", 0, 1, 0.5f, "%");
}

std::vector<ParamGroup> Twine::paramGroups() const {
    return {
        {"String", {"decay", "decaytrack", "damping", "stiffness", "mute", "release", "ring"}},
        {"Pluck", {"brightness", "position", "noise", "veltone"}},
        {"Body", {"body", "bodysize", "spread"}},
        {"Output", {"tune", "fine", "volume"}},
    };
}

void Twine::prepare(double sampleRate) {
    sr_ = sampleRate;
    memory_.prepare(sampleRate, kVoices);
    for (int i = 0; i < kVoices; ++i) voices_[i].loop.attach(memory_.line(i), memory_.size);
    dcA_ = (float)(1.0 - std::exp(-kTwoPi * 8.0 / sampleRate));
    reset();
}

void Twine::reset() {
    for (auto& v : voices_) v.active = false;
    for (auto& b : body_) { b[0].reset(); b[1].reset(); }
    for (float& s : seen_) s = -1.0f;
    bodySize_ = -1.0f;
    tail_ = 0;
    ageCounter_ = 0;
    primed_ = false;
    voicesActive_.store(0, std::memory_order_relaxed);
}

// Called once a quantum. Three broad resonances, an air mode and two plate modes; a bigger body puts
// them lower. They are state-variable filters, which stay stable however fast they are retuned, and the
// size itself is slewed so that a jump of the control is a short glide.
void Twine::updateBody() {
    const float want = pBodySize->get();
    if (bodySize_ == want) return;
    if (bodySize_ < 0.0f || std::fabs(want - bodySize_) < 1.0e-3f) bodySize_ = want;
    else bodySize_ += 0.25f * (want - bodySize_);
    const float scale = std::pow(2.0f, 1.0f - 2.0f * bodySize_);
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 2; ++c) body_[i][c].set(kBodyHz[i] * scale, kBodyQ[i], (float)sr_);
}

void Twine::beginBlock(ProcessContext& ctx) {
    bend_ = ctx.pitchBendSemis;
    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
    const float vol = pVolume->get(), body = pBody->get();
    if (!primed_ || (n == 0 && tail_ == 0)) { vol_.snap(vol); bodyMix_.snap(body); }
    else { vol_.target(vol, ctx.numSamples); bodyMix_.target(body, ctx.numSamples); }
    primed_ = true;
    updateBody();
    // any change to what shapes the strings means every string is worked out again
    const float now[8] = {pDecay->get(), pDecayTrack->get(), pDamping->get(), pStiff->get(), pMute->get(), pRelease->get(), pTune->get(), pFine->get()};
    bool changed = false;
    for (int i = 0; i < 8; ++i) if (now[i] != seen_[i]) { seen_[i] = now[i]; changed = true; }
    if (changed) for (auto& v : voices_) v.retune = true;
}

void Twine::tuneVoice(Voice& v) {
    const double semis = (double)v.key + (double)seen_[6] + ((double)seen_[7] + (double)v.fine) * 0.01 + (double)v.bend + (double)bend_;
    v.semis = semis;
    bl::StringSetting s;
    s.hz = keyToHz(semis);
    const double mute = seen_[4];
    double t60 = (double)seen_[0] * std::pow(2.0, -(double)seen_[1] * (double)(v.key - kRootKey) / 12.0) * std::pow(0.02, mute);
    // damping 0..1 is how long a partial at 2 kHz lasts, from 40 s down to 30 ms; a note's mod Y eases it
    const double damping = clampT((double)seen_[2] - ((double)v.modY - 0.5), 0.0, 1.0);
    double loss = bl::kLn1000 / (40.0 * std::pow(0.03 / 40.0, damping)) + 60.0 * mute;
    if (v.released) {           // a finger comes down on the string
        t60 = std::min(t60, 0.001 * (double)seen_[5] * (double)v.relScale);
        loss += 15.0;
    }
    s.t60 = t60;
    s.highLoss = loss;
    s.stiffness = seen_[3];
    v.loop.retune(s, sr_);
    v.retune = false;
}

void Twine::startVoice(const NoteEvent& e) {
    // the same string plucked again: what was ringing on it stops
    for (auto& o : voices_)
        if (o.active && o.key == e.key && o.killStep == 0.0f) o.killStep = (float)(1.0 / (0.015 * sr_));
    Voice* v = bl::claimVoice(voices_, kSounding, (float)(1.0 / (0.003 * sr_)));
    v->active = true;
    v->released = false;
    v->id = e.id;
    v->age = ++ageCounter_;
    v->key = e.key;
    v->group = e.group;
    v->fine = e.fineCents;
    v->relScale = 0.25f + 1.5f * clampT(e.release, 0.0f, 1.0f);
    v->modY = e.modY;
    v->bend = v->bendTarget = v->bendStep = 0.0f;
    if (e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) {
        v->bend = e.glideFromKey - (float)e.key;
        v->bendStep = -v->bend / e.slideSamples;
    }
    v->loop.clear();
    v->dc = 0.0f;
    v->quiet = 0;
    v->kill = 1.0f;
    v->killStep = 0.0f;
    // every string has its own place across the stereo field (a note panned hard stays hard)
    const double spot = std::fmod((double)e.key * 0.6180339887498949, 1.0) * 2.0 - 1.0;
    bl::noteGains(clampT(e.pan + pSpread->get() * 0.8f * (float)spot * (1.0f - std::fabs(e.pan)), -1.0f, 1.0f), v->panL, v->panR);

    tuneVoice(*v);
    // The pluck is one period of a pulse as wide as the pick position (the shape a string is pulled
    // into), with its mean taken off, mixed with a burst of noise, and softened by a two-pole low-pass
    // that a harder hit opens.
    const double P = v->loop.period, f0 = sr_ / P;
    const double beta = clampT((double)pPosition->get(), 0.03, 0.5);
    const float vel = e.velocity / kDefaultVelocity;
    v->excPos = 0;
    v->excLen = P;
    v->edge = beta * P;
    v->hi = (float)(1.0 - beta);
    v->lo = (float)-beta;
    const double fc = clampT(150.0 * std::pow(2.0, 7.0 * (double)pBright->get() + ((double)e.modX - 0.5) * 4.0 + 0.35 * (double)(e.key - kRootKey) / 12.0
                                                   + ((double)vel - 1.0) * 3.0 * (double)pVelTone->get()), 80.0, 0.45 * sr_);
    v->ea = (float)(1.0 - std::exp(-kTwoPi * fc / sr_));
    v->e1 = v->e2 = 0.0f;
    v->excGain = (float)std::min(3.0, std::sqrt(1.0 + (f0 / fc) * (f0 / fc))) * std::max(0.0f, 1.0f + (vel * vel - 1.0f) * 0.8f);
    v->noiseAmt = pNoise->get();
    v->excTotal = (int)P + 1 + std::min(4096, (int)(6.0f / v->ea));
    v->noise = dsp::Noise(0x9E3779B9u ^ ((uint32)e.key * 2654435761u));
}

void Twine::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * sr_));
    switch (e.type) {
        case NoteEvent::On: startVoice(e); break;
        case NoteEvent::Off:
            if (pRing->getBool()) break;
            for (auto& v : voices_) if (v.active && v.id == e.id && !v.released) { v.released = true; v.retune = true; }
            break;
        case NoteEvent::AllOff:
            for (auto& v : voices_) if (v.active && !v.released) { v.released = true; v.retune = true; }
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

void Twine::render(float* L, float* R, int n, ProcessContext&) {
    bool any = false;
    for (auto& v : voices_) {
        if (!v.active) continue;
        any = true;
        int pos = 0;
        while (pos < n) {
            const int m = std::min(kChunk, n - pos);
            const double semis = (double)v.key + (double)seen_[6] + ((double)seen_[7] + (double)v.fine) * 0.01 + (double)v.bend + (double)bend_;
            if (v.retune || semis != v.semis) tuneVoice(v);
            if (v.bendStep != 0.0f) {
                v.bend += v.bendStep * (float)m;
                if ((v.bendStep > 0.0f) == (v.bend >= v.bendTarget)) { v.bend = v.bendTarget; v.bendStep = 0.0f; }
            }
            float peak = 0.0f;
            for (int i = 0; i < m; ++i) {
                float exc = 0.0f;
                if (v.excPos < v.excTotal) {
                    const double k = (double)v.excPos++;
                    float raw = 0.0f;
                    if (k < v.excLen) {
                        const float pulse = k + 1.0 <= v.edge ? v.hi : (k >= v.edge ? v.lo : v.hi * (float)(v.edge - k) + v.lo * (float)(k + 1.0 - v.edge));
                        const float window = 0.5f + 0.5f * (float)std::cos(kPi * k / v.excLen);
                        raw = (pulse * (1.0f - v.noiseAmt) + v.noise.next() * window * v.noiseAmt * 1.2f) * (float)std::min(1.0, v.excLen - k);
                    }
                    v.e1 += v.ea * (raw - v.e1);
                    v.e2 += v.ea * (v.e1 - v.e2);
                    exc = v.e2 * v.excGain;
                }
                const float y = v.loop.tick(exc);
                v.dc += dcA_ * (y - v.dc);
                const float o = y - v.dc;
                peak = std::max(peak, std::fabs(o));
                const float a = v.kill;
                if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) { v.active = false; break; } }
                L[pos + i] += o * a * v.panL;
                R[pos + i] += o * a * v.panR;
            }
            if (!v.active) break;
            if (v.excPos >= v.excTotal && peak < kQuiet) {
                v.quiet += m;
                if (v.quiet > 2 * (int)v.loop.period + 64) { v.active = false; break; }
            } else v.quiet = 0;
            pos += m;
        }
    }
    if (any) tail_ = (int)(0.5 * sr_);
    if (tail_ <= 0) return;
    // band-pass outputs scaled to unity at their peaks
    const float g0 = 1.0f / kBodyQ[0], g1 = 0.8f / kBodyQ[1], g2 = 0.6f / kBodyQ[2];
    for (int i = 0; i < n; ++i) {
        const float mix = bodyMix_.next() * 1.5f, vol = vol_.next() * kOut;
        float* ch[2] = {L, R};
        for (int c = 0; c < 2; ++c) {
            const float x = ch[c][i];
            float lp, b0, b1, b2, hp;
            body_[0][c].process(x, lp, b0, hp);
            body_[1][c].process(x, lp, b1, hp);
            body_[2][c].process(x, lp, b2, hp);
            ch[c][i] = (x + mix * (g0 * b0 + g1 * b1 + g2 * b2)) * vol;
        }
    }
    if (!any) {
        tail_ -= n;
        if (tail_ <= 0) { tail_ = 0; for (auto& b : body_) { b[0].reset(); b[1].reset(); } }
    }
    scope_.push(L, R, n);
}

std::vector<Preset> Twine::factoryPresets() const {
    return buildPresets(*this, {
        {"Steel String", {{"decay", 5}, {"damping", 0.35f}, {"brightness", 0.65f}, {"position", 0.16f}, {"body", 0.3f}}},
        {"Nylon Guitar", {{"decay", 3.2f}, {"damping", 0.5f}, {"brightness", 0.45f}, {"position", 0.22f}, {"noise", 0.08f}, {"body", 0.45f}, {"bodysize", 0.55f}}},
        {"Concert Harp", {{"volume", 0.71f}, {"decay", 6}, {"decaytrack", 0.25f}, {"damping", 0.3f}, {"brightness", 0.5f}, {"position", 0.35f}, {"noise", 0.05f}, {"ring", 1}, {"body", 0.2f}, {"spread", 0.8f}}},
        {"Finger Bass", {{"volume", 0.69f}, {"tune", -24}, {"decay", 3}, {"damping", 0.55f}, {"brightness", 0.35f}, {"position", 0.28f}, {"noise", 0.05f}, {"body", 0.15f}, {"spread", 0}}},
        {"Picked Bass", {{"volume", 0.65f}, {"tune", -24}, {"decay", 2.5f}, {"damping", 0.45f}, {"brightness", 0.6f}, {"position", 0.12f}, {"noise", 0.25f}, {"body", 0.1f}, {"spread", 0}}},
        {"Koto", {{"decay", 3.5f}, {"damping", 0.45f}, {"brightness", 0.85f}, {"position", 0.06f}, {"noise", 0.2f}, {"stiffness", 0.25f}, {"body", 0.35f}, {"bodysize", 0.3f}}},
        {"Palm Mute", {{"volume", 0.63f}, {"mute", 0.65f}, {"brightness", 0.6f}, {"position", 0.12f}, {"noise", 0.2f}, {"body", 0.2f}}},
        {"Tight Mute Pluck", {{"mute", 0.9f}, {"brightness", 0.8f}, {"position", 0.2f}, {"body", 0.1f}}},
        {"Glass Bell", {{"stiffness", 0.85f}, {"decay", 7}, {"damping", 0.15f}, {"brightness", 0.8f}, {"position", 0.3f}, {"noise", 0}, {"ring", 1}, {"body", 0}, {"spread", 0.6f}}},
        {"Tubular Chime", {{"stiffness", 1}, {"decay", 10}, {"damping", 0.1f}, {"brightness", 0.6f}, {"position", 0.45f}, {"noise", 0}, {"ring", 1}, {"body", 0}}},
        {"Banjo Snap", {{"decay", 1.2f}, {"decaytrack", 0.3f}, {"damping", 0.6f}, {"brightness", 0.95f}, {"position", 0.05f}, {"noise", 0.3f}, {"body", 0.5f}, {"bodysize", 0.15f}}},
        {"Bright Mandolin", {{"tune", 12}, {"decay", 1.8f}, {"damping", 0.4f}, {"brightness", 0.9f}, {"position", 0.1f}, {"body", 0.4f}, {"bodysize", 0.2f}}},
        {"Hammered Dulcimer", {{"decay", 4.5f}, {"damping", 0.25f}, {"brightness", 0.7f}, {"position", 0.12f}, {"noise", 0.05f}, {"stiffness", 0.1f}, {"ring", 1}, {"spread", 0.9f}}},
        {"Quill Harpsichord", {{"decay", 3}, {"damping", 0.3f}, {"brightness", 1}, {"position", 0.08f}, {"noise", 0.1f}, {"release", 60}, {"body", 0.25f}}},
        {"Muted Keys", {{"decay", 1.5f}, {"damping", 0.5f}, {"brightness", 0.9f}, {"position", 0.04f}, {"mute", 0.25f}, {"noise", 0.1f}, {"release", 40}, {"body", 0.1f}}},
        {"Lute", {{"volume", 0.66f}, {"decay", 2.4f}, {"damping", 0.6f}, {"brightness", 0.4f}, {"position", 0.3f}, {"body", 0.55f}, {"bodysize", 0.6f}}},
        {"Rubber Band", {{"volume", 0.82f}, {"tune", -12}, {"decay", 0.8f}, {"damping", 0.8f}, {"brightness", 0.5f}, {"position", 0.4f}, {"mute", 0.2f}, {"body", 0.3f}}},
        {"Thumb Piano", {{"stiffness", 0.55f}, {"decay", 2.5f}, {"damping", 0.35f}, {"brightness", 0.5f}, {"position", 0.5f}, {"noise", 0}, {"body", 0.5f}, {"bodysize", 0.2f}}},
        {"Wire Zither", {{"decay", 8}, {"damping", 0.12f}, {"brightness", 0.9f}, {"position", 0.1f}, {"ring", 1}, {"spread", 1}, {"body", 0.15f}}},
        {"Deep Piano String", {{"tune", -12}, {"decay", 9}, {"decaytrack", 0.3f}, {"damping", 0.3f}, {"brightness", 0.55f}, {"position", 0.12f}, {"stiffness", 0.2f}, {"noise", 0}, {"body", 0.2f}, {"bodysize", 0.9f}}},
        {"Pizzicato", {{"decay", 0.9f}, {"decaytrack", 0.2f}, {"damping", 0.7f}, {"brightness", 0.5f}, {"position", 0.25f}, {"mute", 0.15f}, {"body", 0.5f}}},
        {"Ukulele", {{"decay", 1.6f}, {"decaytrack", 0.2f}, {"damping", 0.55f}, {"brightness", 0.6f}, {"position", 0.25f}, {"body", 0.5f}, {"bodysize", 0.2f}}},
        {"Dark Soft Pluck", {{"volume", 0.78f}, {"tune", -24}, {"decay", 4}, {"damping", 0.75f}, {"brightness", 0.25f}, {"position", 0.4f}, {"noise", 0}, {"body", 0.1f}}},
        {"Music Box", {{"tune", 12}, {"stiffness", 0.7f}, {"decay", 3}, {"damping", 0.3f}, {"brightness", 0.7f}, {"position", 0.2f}, {"noise", 0}, {"ring", 1}, {"body", 0.25f}, {"bodysize", 0.05f}}},
    });
}

} // namespace pulp
