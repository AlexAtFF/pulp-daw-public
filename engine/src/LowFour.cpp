#include "pulp/LowFour.h"

namespace pulp {

namespace {
constexpr int kChunk = 32;
constexpr float kOut = 1.2f;
constexpr float kQuiet = 1.5e-5f;
constexpr int kBassKey = 36;                // the key the macro controls are centred on, after the octave shift

// What a playing style changes: where the string is plucked, how bright the pluck is (octaves on top of
// Bite), how much pluck noise, how fast the top goes, how long the note is, stiffness, and a pitch scoop.
struct StyleDef { double position, bright, noise, loss, length, stiff, scoop; };
const StyleDef kStyles[LowFour::NumStyles] = {
    /* Finger   */ {0.22, 0.0, 0.05, 1.0, 1.0, 0.25, 0.0},
    /* Pick     */ {0.12, 1.2, 0.25, 0.8, 0.9, 0.30, 0.0},
    /* Slap     */ {0.07, 2.4, 0.35, 1.8, 0.8, 0.40, 0.0},
    /* Muted    */ {0.15, 0.4, 0.15, 6.0, 0.12, 0.25, 0.0},
    /* Fretless */ {0.30, -0.6, 0.02, 0.7, 1.3, 0.15, -0.4},
};
}

LowFour::LowFour() {
    pStyle = addChoice("style", "Style", {"Finger", "Pick", "Slap", "Muted", "Fretless"}, Finger);
    pBody = addFloat("body", "Body", 0, 1, 0.5f, "%");
    pBite = addFloat("bite", "Bite", 0, 1, 0.4f, "%");
    pSustain = addFloat("sustain", "Sustain", 0, 1, 0.5f, "%");
    pTone = addFloat("tone", "Tone", 0, 1, 0.6f, "%");
    pOctave = add({"octave", "Octave", -3, 1, -2, 1, 5, "oct", {}});
    pVolume = addFloat("volume", "Level", 0, 1, 0.5f, "%");
}

std::vector<ParamGroup> LowFour::paramGroups() const {
    return {
        {"Bass", {"style", "body", "bite", "sustain", "tone"}},
        {"Output", {"octave", "volume"}},
    };
}

void LowFour::prepare(double sampleRate) {
    sr_ = sampleRate;
    memory_.prepare(sampleRate, kVoices);
    for (int i = 0; i < kVoices; ++i) voices_[i].loop.attach(memory_.line(i), memory_.size);
    dcA_ = (float)(1.0 - std::exp(-kTwoPi * 8.0 / sampleRate));
    scoopCoef_ = (float)std::exp(-1.0 / (0.03 * sampleRate));       // per sample: the scoop is gone in about a tenth of a second
    for (int c = 0; c < 2; ++c) { up_[c].prepare(sampleRate); down_[c].prepare(sampleRate); }
    reset();
}

void LowFour::reset() {
    for (auto& v : voices_) v.active = false;
    for (int c = 0; c < 2; ++c) { shelf_[c].reset(); presence_[c].reset(); tone_[c].reset(); up_[c].reset(); down_[c].reset(); }
    seen_[0] = seen_[1] = -1.0f;
    seen_[2] = -100.0f;
    eqSeen_[0] = eqSeen_[1] = -1.0f;
    tail_ = 0;
    ageCounter_ = 0;
    primed_ = false;
    voicesActive_.store(0, std::memory_order_relaxed);
}

// Called once a quantum. Body and Bite reach the two equaliser sections over a few quanta, not in
// one jump: the sections are biquads, and theirs are the only coefficients here that could ring if thrown
// from one extreme to the other every 64 samples.
void LowFour::updateTone() {
    const float want[2] = {pBody->get(), pBite->get()};
    if (want[0] == eqSeen_[0] && want[1] == eqSeen_[1]) return;
    for (int i = 0; i < 2; ++i) {
        if (eqSeen_[i] < 0.0f || std::fabs(want[i] - eqSeen_[i]) < 1.0e-3f) eqSeen_[i] = want[i];
        else eqSeen_[i] += 0.25f * (want[i] - eqSeen_[i]);
    }
    for (int c = 0; c < 2; ++c) {
        shelf_[c].lowShelf(130.0, 0.7, -3.0 + 12.0 * (double)eqSeen_[0], sr_);
        presence_[c].peak(1600.0, 0.9, -2.0 + 9.0 * (double)eqSeen_[1], sr_);
    }
}

void LowFour::beginBlock(ProcessContext& ctx) {
    bend_ = ctx.pitchBendSemis;
    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
    // Bite turns the amplifier stage up; the make-up keeps a signal of ordinary size at the same level,
    // and the level also gives back half of what the Body shelf adds, so the knobs change the sound
    // rather than the loudness.
    const float drive = 0.8f + 2.4f * pBite->get();
    const float makeUp = 0.57f / std::tanh(0.5f * drive);
    const float vol = pVolume->get() * dbToGain(-0.5f * (-3.0f + 12.0f * pBody->get()));
    if (!primed_ || (n == 0 && tail_ == 0)) { vol_.snap(vol); drive_.snap(drive); makeUp_.snap(makeUp); }
    else { vol_.target(vol, ctx.numSamples); drive_.target(drive, ctx.numSamples); makeUp_.target(makeUp, ctx.numSamples); }
    primed_ = true;
    updateTone();
    const float now[3] = {pSustain->get(), pBite->get(), pOctave->get()};
    bool changed = false;
    for (int i = 0; i < 3; ++i) if (now[i] != seen_[i]) { seen_[i] = now[i]; changed = true; }
    if (changed) for (auto& v : voices_) v.retune = true;
}

void LowFour::tuneVoice(Voice& v) {
    const StyleDef& st = kStyles[v.style];
    const double shifted = (double)v.key + 12.0 * (double)seen_[2];
    const double semis = shifted + (double)v.fine * 0.01 + (double)v.bend + (double)v.scoop + (double)bend_;
    v.semis = semis;
    const double sustain = seen_[0];
    bl::StringSetting s;
    s.hz = keyToHz(semis);
    // Sustain: 0.35 s at the bottom of the knob, 14 s at the top; higher notes die a little sooner.
    double t60 = 0.35 * std::pow(40.0, sustain) * st.length * std::pow(2.0, -0.3 * (shifted - kBassKey) / 12.0);
    // how fast the top goes: quicker with less sustain; a note's mod Y holds it (up) or hurries it (down)
    double loss = st.loss * (5.0 + 25.0 * (1.0 - sustain)) * std::pow(2.0, -4.0 * ((double)v.modY - 0.5));
    if (v.released) {
        t60 = std::min(t60, 0.06 * (double)v.relScale);
        loss += 25.0;
    }
    s.t60 = t60;
    s.highLoss = loss;
    s.stiffness = st.stiff;
    v.loop.retune(s, sr_);
    v.retune = false;
}

void LowFour::startVoice(const NoteEvent& e) {
    for (auto& o : voices_)         // the same string again: the old note stops
        if (o.active && o.key == e.key && o.killStep == 0.0f) o.killStep = (float)(1.0 / (0.015 * sr_));
    Voice* v = bl::claimVoice(voices_, kSounding, (float)(1.0 / (0.003 * sr_)));
    const int style = clampT(pStyle->getInt(), 0, (int)NumStyles - 1);
    const StyleDef& st = kStyles[style];
    v->active = true;
    v->released = false;
    v->id = e.id;
    v->age = ++ageCounter_;
    v->key = e.key;
    v->group = e.group;
    v->style = style;
    v->fine = e.fineCents;
    v->relScale = 0.25f + 1.5f * clampT(e.release, 0.0f, 1.0f);
    v->modY = e.modY;
    v->bend = v->bendTarget = v->bendStep = 0.0f;
    if (e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) {
        v->bend = e.glideFromKey - (float)e.key;
        v->bendStep = -v->bend / e.slideSamples;
    }
    v->scoop = (float)st.scoop;
    v->loop.clear();
    v->dc = 0.0f;
    v->quiet = 0;
    v->kill = 1.0f;
    v->killStep = 0.0f;
    bl::noteGains(e.pan, v->panL, v->panR);

    tuneVoice(*v);
    const double P = v->loop.period, f0 = sr_ / P;
    const double body = pBody->get(), bite = pBite->get();
    const double shifted = (double)e.key + 12.0 * (double)pOctave->get();
    const float vel = e.velocity / kDefaultVelocity;
    // more Body plucks nearer the middle of the string, where the fundamental is strongest
    const double beta = clampT(st.position * (0.7 + 0.8 * body), 0.03, 0.5);
    v->excPos = 0;
    v->excLen = P;
    v->edge = beta * P;
    v->hi = (float)(1.0 - beta);
    v->lo = (float)-beta;
    const double fc = clampT(500.0 * std::pow(2.0, 3.5 * bite + st.bright + ((double)e.modX - 0.5) * 4.0 + 0.5 * (shifted - kBassKey) / 12.0 + ((double)vel - 1.0) * 1.5),
                             60.0, 0.45 * sr_);
    v->ea = (float)(1.0 - std::exp(-kTwoPi * fc / sr_));
    v->e1 = v->e2 = 0.0f;
    v->excGain = (float)std::min(3.0, std::sqrt(1.0 + (f0 / fc) * (f0 / fc))) * std::max(0.0f, 1.0f + (vel * vel - 1.0f) * 0.8f);
    v->noiseAmt = (float)clampT(st.noise * (0.3 + 1.4 * bite), 0.0, 0.8);
    v->excTotal = (int)P + 1 + std::min(4096, (int)(6.0f / v->ea));
    v->noise = dsp::Noise(0x51ED270Bu ^ ((uint32)e.key * 2654435761u));
}

void LowFour::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * sr_));
    switch (e.type) {
        case NoteEvent::On: startVoice(e); break;
        case NoteEvent::Off:
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

void LowFour::render(float* L, float* R, int n, ProcessContext&) {
    bool any = false;
    for (auto& v : voices_) {
        if (!v.active) continue;
        any = true;
        int pos = 0;
        while (pos < n) {
            const int m = std::min(kChunk, n - pos);
            const double semis = (double)v.key + 12.0 * (double)seen_[2] + (double)v.fine * 0.01 + (double)v.bend + (double)v.scoop + (double)bend_;
            if (v.retune || semis != v.semis) tuneVoice(v);
            if (v.bendStep != 0.0f) {
                v.bend += v.bendStep * (float)m;
                if ((v.bendStep > 0.0f) == (v.bend >= v.bendTarget)) { v.bend = v.bendTarget; v.bendStep = 0.0f; }
            }
            if (v.scoop != 0.0f) { v.scoop *= std::pow(scoopCoef_, (float)m); if (std::fabs(v.scoop) < 0.002f) v.scoop = 0.0f; }
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
    if (any) tail_ = (int)(0.1 * sr_);
    if (tail_ <= 0) return;

    // The instrument's electronics: low shelf (Body), presence (Bite), a soft amplifier stage at twice
    // the rate, and the tone control.
    const float toneHz = (float)std::min(250.0 * std::pow(2.0, 6.0 * (double)pTone->get()), 0.45 * sr_);
    tone_[0].set(toneHz, 0.7f, (float)sr_);
    tone_[1].set(toneHz, 0.7f, (float)sr_);
    float* ch[2] = {L, R};
    for (int i = 0; i < n; ++i) {
        const float dg = drive_.next(), mk = makeUp_.next(), vol = vol_.next() * kOut;
        for (int c = 0; c < 2; ++c) {
            const float x = presence_[c].process(shelf_[c].process(ch[c][i]));
            float a, b;
            up_[c].process(x, a, b);
            const float y = down_[c].process(std::tanh(dg * a) * mk, std::tanh(dg * b) * mk);
            ch[c][i] = tone_[c].lowpass(y) * vol;
        }
    }
    if (!any) {
        tail_ -= n;
        if (tail_ <= 0) {
            tail_ = 0;
            for (int c = 0; c < 2; ++c) { shelf_[c].reset(); presence_[c].reset(); tone_[c].reset(); up_[c].reset(); down_[c].reset(); }
        }
    }
    scope_.push(L, R, n);
}

std::vector<Preset> LowFour::factoryPresets() const {
    return buildPresets(*this, {
        {"Round Finger", {}},
        {"Bright Finger", {{"bite", 0.7f}, {"tone", 0.8f}}},
        {"Deep Finger", {{"body", 0.85f}, {"bite", 0.2f}, {"tone", 0.45f}}},
        {"Pick Punch", {{"style", Pick}, {"bite", 0.6f}, {"sustain", 0.45f}, {"tone", 0.7f}}},
        {"Pick Grind", {{"style", Pick}, {"body", 0.4f}, {"bite", 1.0f}, {"sustain", 0.55f}, {"tone", 0.85f}}},
        {"Pick Mellow", {{"style", Pick}, {"body", 0.6f}, {"bite", 0.25f}, {"tone", 0.5f}}},
        {"Slap Pop", {{"style", Slap}, {"bite", 0.8f}, {"sustain", 0.4f}, {"tone", 0.9f}}},
        {"Thumb Funk", {{"style", Slap}, {"body", 0.7f}, {"bite", 0.5f}, {"sustain", 0.35f}, {"tone", 0.75f}}},
        {"Palm Thump", {{"style", Muted}, {"body", 0.7f}, {"bite", 0.3f}, {"tone", 0.5f}}},
        {"Dead Note Tick", {{"style", Muted}, {"body", 0.3f}, {"bite", 0.7f}, {"sustain", 0.1f}, {"tone", 0.7f}}},
        {"Singing Fretless", {{"style", Fretless}, {"body", 0.55f}, {"bite", 0.35f}, {"sustain", 0.8f}}},
        {"Dark Fretless", {{"style", Fretless}, {"body", 0.75f}, {"bite", 0.1f}, {"sustain", 0.7f}, {"tone", 0.35f}}},
        {"Dub Weight", {{"body", 1.0f}, {"bite", 0.05f}, {"sustain", 0.6f}, {"tone", 0.25f}}},
        {"Flat String Thud", {{"body", 0.7f}, {"bite", 0.3f}, {"sustain", 0.2f}, {"tone", 0.4f}}},
        {"Long Ring", {{"bite", 0.45f}, {"sustain", 1.0f}, {"tone", 0.65f}}},
        {"Rock Eighths", {{"style", Pick}, {"body", 0.55f}, {"bite", 0.75f}, {"sustain", 0.35f}, {"tone", 0.65f}}},
        {"Growl", {{"body", 0.6f}, {"bite", 1.0f}, {"sustain", 0.6f}, {"tone", 0.7f}}},
        {"Soft Ballad", {{"body", 0.6f}, {"bite", 0.15f}, {"sustain", 0.75f}, {"tone", 0.5f}}},
        {"Upright Feel", {{"style", Fretless}, {"body", 0.8f}, {"bite", 0.3f}, {"sustain", 0.3f}, {"tone", 0.45f}}},
        {"Octave Up Pop", {{"style", Slap}, {"octave", -1}, {"bite", 0.7f}, {"sustain", 0.4f}, {"tone", 0.85f}}},
        {"Sub Floor", {{"octave", -3}, {"body", 0.9f}, {"bite", 0.1f}, {"sustain", 0.7f}, {"tone", 0.3f}}},
        {"Short Stab", {{"style", Pick}, {"bite", 0.6f}, {"sustain", 0.05f}, {"tone", 0.7f}}},
        {"Bridge Honk", {{"style", Pick}, {"body", 0.1f}, {"bite", 0.8f}, {"sustain", 0.5f}, {"tone", 0.75f}}},
    });
}

} // namespace pulp
