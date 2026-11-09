#include "pulp/Trio.h"

namespace pulp {

Trio::Trio() {
    const std::vector<std::string> shapes = {"Sine", "Triangle", "Square", "Saw", "Soft saw", "Noise"};
    const int defShape[3] = {BlepOsc::Saw, BlepOsc::Saw, BlepOsc::Square};
    const float defCoarse[3] = {0, 0, -12}, defFine[3] = {0, 8, 0}, defLevel[3] = {0.8f, 0.5f, 0.35f};
    for (int i = 0; i < 3; ++i) {
        const std::string n = "o" + std::to_string(i + 1), d = "Osc " + std::to_string(i + 1) + " ";
        osc_[i].shape = addChoice((n + "shape").c_str(), (d + "shape").c_str(), shapes, defShape[i]);
        osc_[i].coarse = add({n + "coarse", d + "coarse", -24, 24, defCoarse[i], 1, 49, "st", {}});
        osc_[i].fine = addFloat((n + "fine").c_str(), (d + "fine").c_str(), -100, 100, defFine[i], "ct");
        osc_[i].level = addFloat((n + "level").c_str(), (d + "level").c_str(), 0, 1, defLevel[i], "%");
        osc_[i].pan = addFloat((n + "pan").c_str(), (d + "pan").c_str(), -1, 1, 0, "");
        osc_[i].phase = addFloat((n + "phase").c_str(), (d + "phase").c_str(), 0, 1, 0, "%");
        osc_[i].spread = addFloat((n + "spread").c_str(), (d + "stereo detune").c_str(), 0, 50, 0, "ct");
    }
    pAttack = addFloat("attack", "Attack", 0, 5000, 2, "ms", 3.0f);
    pDecay = addFloat("decay", "Decay", 1, 5000, 400, "ms", 3.0f);
    pSustain = addFloat("sustain", "Sustain", 0, 1, 0.7f, "%");
    pRelease = addFloat("release", "Release", 1, 5000, 180, "ms", 3.0f);
    pCutoff = addFloat("cutoff", "Cutoff", 20, 20000, 20000, "Hz", 3.0f);
    pReso = addFloat("reso", "Resonance", 0, 1, 0.1f, "%");
    pEnvAmt = addFloat("envamt", "Filter env amount", -1, 1, 0, "");
    pKeyTrack = addFloat("keytrack", "Filter key tracking", 0, 1, 0.3f, "%");
    pFAttack = addFloat("fattack", "Filter attack", 0, 5000, 1, "ms", 3.0f);
    pFDecay = addFloat("fdecay", "Filter decay", 1, 5000, 300, "ms", 3.0f);
    pFSustain = addFloat("fsustain", "Filter sustain", 0, 1, 0.2f, "%");
    pFRelease = addFloat("frelease", "Filter release", 1, 5000, 200, "ms", 3.0f);
    pAm = addBool("am", "Osc 3 ring-modulates 1 and 2", false);
    pVolume = addFloat("volume", "Level", 0, 1, 0.5f, "%");
    pVelSens = addFloat("velsens", "Velocity sensitivity", 0, 1, 1, "%");
}

std::vector<ParamGroup> Trio::paramGroups() const {
    std::vector<ParamGroup> g;
    for (int i = 1; i <= 3; ++i) {
        const std::string n = "o" + std::to_string(i);
        g.push_back({"Oscillator " + std::to_string(i), {n + "shape", n + "coarse", n + "fine", n + "level", n + "pan", n + "phase", n + "spread"}});
    }
    g.push_back({"Amplitude", {"attack", "decay", "sustain", "release", "volume", "velsens", "am"}});
    g.push_back({"Filter", {"cutoff", "reso", "envamt", "keytrack", "fattack", "fdecay", "fsustain", "frelease"}});
    return g;
}

void Trio::prepare(double sampleRate) {
    sr_ = sampleRate;
    for (auto& v : voices_) v.active = false;
}

void Trio::reset() {
    for (auto& v : voices_) v.active = false;
    voicesActive_.store(0, std::memory_order_relaxed);
}

void Trio::beginBlock(ProcessContext& ctx) {
    bend_ = ctx.pitchBendSemis;
    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
}

void Trio::startVoice(const NoteEvent& e) {
    Voice* v = nullptr;
    for (auto& c : voices_) if (!c.active) { v = &c; break; }
    if (!v) {
        v = &voices_[0];
        for (auto& c : voices_) if (c.age < v->age) v = &c;
    }
    v->active = true;
    v->id = e.id;
    v->age = ++ageCounter_;
    v->key = e.key;
    v->group = e.group;
    const float vel = e.velocity / kDefaultVelocity;
    v->velGain = 1.0f + (vel * vel - 1.0f) * pVelSens->get();
    v->panL = e.pan > 0.0f ? 1.0f - e.pan : 1.0f;
    v->panR = e.pan < 0.0f ? 1.0f + e.pan : 1.0f;
    v->fine = e.fineCents;
    v->modX = e.modX;
    v->modY = e.modY;
    v->relScale = 0.25f + 1.5f * e.release;
    v->bend = v->bendTarget = v->bendStep = 0.0f;
    if (e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) {
        v->bend = e.glideFromKey - (float)e.key;
        v->bendStep = -v->bend / e.slideSamples;
    }
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 2; ++c) {
            v->osc[i][c].phase = osc_[i].phase->get();
            v->osc[i][c].noise = 0x1234567u + (uint32)(i * 7919 + c * 104729) + e.id * 2654435761u;
        }
    v->amp.set(pAttack->get(), pDecay->get(), pSustain->get(), pRelease->get() * v->relScale, sr_);
    v->filt.set(pFAttack->get(), pFDecay->get(), pFSustain->get(), pFRelease->get() * v->relScale, sr_);
    v->amp.value = 0.0f;
    v->filt.value = 0.0f;
    v->amp.start();
    v->filt.start();
    v->svf[0].reset();
    v->svf[1].reset();
    v->kill = 1.0f;
    v->killStep = 0.0f;
}

void Trio::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * sr_));
    switch (e.type) {
        case NoteEvent::On: startVoice(e); break;
        case NoteEvent::Off:
            for (auto& v : voices_) if (v.active && v.id == e.id) { v.amp.release(); v.filt.release(); }
            break;
        case NoteEvent::AllOff:
            for (auto& v : voices_) if (v.active) { v.amp.release(); v.filt.release(); }
            break;
        case NoteEvent::Kill:
            for (auto& v : voices_) if (v.active && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::KillNote:
            for (auto& v : voices_) if (v.active && v.id == e.id && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::SlideTo:
            for (auto& v : voices_)
                if (v.active && v.group == e.group && v.amp.stage != Adsr::Release) {
                    v.bendTarget = (float)(e.key - v.key);
                    v.bendStep = (v.bendTarget - v.bend) / std::max(1.0f, e.slideSamples);
                }
            break;
    }
}

void Trio::render(float* L, float* R, int n, ProcessContext&) {
    int shape[3];
    float level[3], gl[3], gr[3], coarse[3], fine[3], spread[3];
    for (int i = 0; i < 3; ++i) {
        shape[i] = osc_[i].shape->getInt();
        level[i] = osc_[i].level->get();
        const float p = osc_[i].pan->get();
        gl[i] = level[i] * (p > 0.0f ? 1.0f - p : 1.0f);
        gr[i] = level[i] * (p < 0.0f ? 1.0f + p : 1.0f);
        coarse[i] = osc_[i].coarse->get();
        fine[i] = osc_[i].fine->get();
        spread[i] = osc_[i].spread->get();
    }
    const bool am = pAm->getBool();
    // Keep the level sensible whatever the oscillator mix: three full oscillators are no louder than one.
    const float vol = pVolume->get() * 0.45f / std::max(1.0f, am ? level[0] + level[1] : level[0] + level[1] + level[2]);
    const float cutoff = pCutoff->get(), reso = pReso->get(), envAmt = pEnvAmt->get(), keyTrack = pKeyTrack->get();

    for (auto& v : voices_) {
        if (!v.active) continue;
        const double base = (double)v.key + (double)v.bend + (double)bend_ + (double)v.fine * 0.01;
        if (v.bendStep != 0.0f) {
            v.bend += v.bendStep * (float)n;
            if ((v.bendStep > 0.0f) == (v.bend >= v.bendTarget)) { v.bend = v.bendTarget; v.bendStep = 0.0f; }
        }
        double dt[3][2];
        for (int i = 0; i < 3; ++i) {
            const double semis = base + coarse[i] + fine[i] * 0.01;
            const double sp = spread[i] * 0.005;       // half the spread each way, in semitones
            dt[i][0] = std::min(0.45, keyToHz(semis - sp) / sr_);
            dt[i][1] = std::min(0.45, keyToHz(semis + sp) / sr_);
        }
        // filter coefficients once per segment
        const float fenv = v.filt.value;
        const float octaves = envAmt * fenv * 8.0f + (v.modX - 0.5f) * 10.0f + keyTrack * (float)(v.key - kRootKey) / 12.0f;
        const float fc = clampT(cutoff * std::pow(2.0f, octaves), 20.0f, (float)(sr_ * 0.45));
        const float q = 0.707f + clampT(reso + (v.modY - 0.5f) * 2.0f, 0.0f, 1.0f) * 11.0f;
        v.svf[0].set(fc, q, (float)sr_);
        v.svf[1].g = v.svf[0].g; v.svf[1].k = v.svf[0].k; v.svf[1].a1 = v.svf[0].a1; v.svf[1].a2 = v.svf[0].a2; v.svf[1].a3 = v.svf[0].a3;
        const float gain = vol * v.velGain;

        for (int s = 0; s < n; ++s) {
            float l, r;
            if (am) {
                const float m0 = v.osc[2][0].next(shape[2], dt[2][0]) * level[2], m1 = v.osc[2][1].next(shape[2], dt[2][1]) * level[2];
                l = (v.osc[0][0].next(shape[0], dt[0][0]) * gl[0] + v.osc[1][0].next(shape[1], dt[1][0]) * gl[1]) * m0;
                r = (v.osc[0][1].next(shape[0], dt[0][1]) * gr[0] + v.osc[1][1].next(shape[1], dt[1][1]) * gr[1]) * m1;
            } else {
                l = v.osc[0][0].next(shape[0], dt[0][0]) * gl[0] + v.osc[1][0].next(shape[1], dt[1][0]) * gl[1] + v.osc[2][0].next(shape[2], dt[2][0]) * gl[2];
                r = v.osc[0][1].next(shape[0], dt[0][1]) * gr[0] + v.osc[1][1].next(shape[1], dt[1][1]) * gr[1] + v.osc[2][1].next(shape[2], dt[2][1]) * gr[2];
            }
            l = v.svf[0].lowpass(l);
            r = v.svf[1].lowpass(r);
            v.filt.next();
            float a = v.amp.next() * gain * v.kill;
            if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) { v.active = false; break; } }
            L[s] += l * a * v.panL;
            R[s] += r * a * v.panR;
            if (!v.amp.active()) { v.active = false; break; }
        }
    }
}

// ------------------------------------------------------------------ presets

namespace {
struct PV { const char* id; float v; };
struct PresetDef { const char* name; std::vector<PV> values; };
}

std::vector<Preset> Trio::factoryPresets() const {
    // Shapes: 0 sine, 1 triangle, 2 square, 3 saw, 4 soft saw, 5 noise.
    static const std::vector<PresetDef> defs = {
        {"Default", {}},
        {"Warm Saw Pad", {{"o1shape", 3}, {"o2shape", 3}, {"o2fine", 12}, {"o3shape", 3}, {"o3coarse", 0}, {"o3fine", -12}, {"o3level", 0.5f}, {"o1spread", 14}, {"o2spread", 18}, {"attack", 380}, {"decay", 1200}, {"sustain", 0.8f}, {"release", 900}, {"cutoff", 2400}, {"reso", 0.15f}}},
        {"Soft Keys", {{"o1shape", 1}, {"o2shape", 0}, {"o2coarse", 12}, {"o2fine", 0}, {"o2level", 0.25f}, {"o3level", 0}, {"attack", 3}, {"decay", 900}, {"sustain", 0.25f}, {"release", 350}, {"cutoff", 5200}}},
        {"Square Lead", {{"o1shape", 2}, {"o2shape", 2}, {"o2fine", 6}, {"o2level", 0.6f}, {"o3shape", 2}, {"o3coarse", -12}, {"o3level", 0.3f}, {"attack", 2}, {"decay", 250}, {"sustain", 0.75f}, {"release", 120}, {"cutoff", 6500}, {"reso", 0.2f}}},
        {"Sub Bass", {{"o1shape", 0}, {"o1coarse", -12}, {"o2shape", 1}, {"o2coarse", -12}, {"o2fine", 0}, {"o2level", 0.3f}, {"o3level", 0}, {"attack", 2}, {"decay", 300}, {"sustain", 0.9f}, {"release", 90}, {"keytrack", 0}}},
        {"Reese Bass", {{"o1shape", 3}, {"o1coarse", -12}, {"o2shape", 3}, {"o2coarse", -12}, {"o2fine", 22}, {"o2level", 0.8f}, {"o3shape", 3}, {"o3coarse", -12}, {"o3fine", -22}, {"o3level", 0.8f}, {"cutoff", 900}, {"reso", 0.25f}, {"sustain", 1}, {"release", 140}}},
        {"Pluck", {{"o1shape", 3}, {"o2shape", 2}, {"o2fine", 5}, {"o2level", 0.4f}, {"o3level", 0}, {"attack", 1}, {"decay", 260}, {"sustain", 0}, {"release", 220}, {"cutoff", 700}, {"reso", 0.3f}, {"envamt", 0.6f}, {"fdecay", 180}, {"fsustain", 0}}},
        {"Acid-ish", {{"o1shape", 3}, {"o2level", 0}, {"o3level", 0}, {"attack", 1}, {"decay", 200}, {"sustain", 0.5f}, {"release", 80}, {"cutoff", 420}, {"reso", 0.75f}, {"envamt", 0.5f}, {"fdecay", 160}, {"fsustain", 0.05f}}},
        {"Hollow Organ", {{"o1shape", 2}, {"o2shape", 0}, {"o2coarse", 12}, {"o2fine", 0}, {"o2level", 0.5f}, {"o3shape", 0}, {"o3coarse", 19}, {"o3level", 0.3f}, {"attack", 4}, {"decay", 100}, {"sustain", 1}, {"release", 60}, {"cutoff", 9000}}},
        {"Bell Tone", {{"o1shape", 0}, {"o2shape", 0}, {"o2coarse", 19}, {"o2fine", 4}, {"o2level", 0.4f}, {"o3shape", 0}, {"o3coarse", 24}, {"o3fine", 9}, {"o3level", 0.25f}, {"attack", 1}, {"decay", 1800}, {"sustain", 0}, {"release", 1500}}},
        {"Wide Strings", {{"o1shape", 3}, {"o2shape", 3}, {"o2fine", 9}, {"o2level", 0.8f}, {"o3shape", 3}, {"o3coarse", 0}, {"o3fine", -9}, {"o3level", 0.8f}, {"o1spread", 20}, {"o2spread", 26}, {"o3spread", 30}, {"attack", 250}, {"decay", 800}, {"sustain", 0.85f}, {"release", 600}, {"cutoff", 3800}}},
        {"Ring Mod Clang", {{"o1shape", 0}, {"o2shape", 1}, {"o2coarse", 7}, {"o2fine", 0}, {"o3shape", 0}, {"o3coarse", 14}, {"o3level", 1}, {"am", 1}, {"attack", 1}, {"decay", 900}, {"sustain", 0.1f}, {"release", 500}}},
        {"Noise Sweep", {{"o1shape", 5}, {"o2level", 0}, {"o3level", 0}, {"attack", 900}, {"decay", 1500}, {"sustain", 0.6f}, {"release", 1200}, {"cutoff", 300}, {"reso", 0.5f}, {"envamt", 0.8f}, {"fattack", 1800}, {"fsustain", 1}, {"keytrack", 0}}},
        {"Chip Lead", {{"o1shape", 2}, {"o2level", 0}, {"o3shape", 2}, {"o3coarse", 12}, {"o3level", 0.25f}, {"attack", 0}, {"decay", 60}, {"sustain", 0.8f}, {"release", 30}}},
        {"Soft Flute", {{"o1shape", 0}, {"o2shape", 1}, {"o2coarse", 12}, {"o2fine", 0}, {"o2level", 0.12f}, {"o3shape", 5}, {"o3coarse", 0}, {"o3level", 0.04f}, {"attack", 60}, {"decay", 300}, {"sustain", 0.85f}, {"release", 160}, {"cutoff", 4200}}},
        {"Fifth Stab", {{"o1shape", 3}, {"o2shape", 3}, {"o2coarse", 7}, {"o2fine", 0}, {"o2level", 0.7f}, {"o3shape", 2}, {"o3coarse", -12}, {"attack", 1}, {"decay", 320}, {"sustain", 0.1f}, {"release", 240}, {"cutoff", 2600}, {"envamt", 0.35f}}},
        {"Dark Drone", {{"o1shape", 3}, {"o1coarse", -12}, {"o2shape", 2}, {"o2coarse", -24}, {"o2level", 0.6f}, {"o3shape", 0}, {"o3coarse", -5}, {"o3level", 0.4f}, {"o1spread", 10}, {"attack", 1500}, {"sustain", 1}, {"release", 2500}, {"cutoff", 520}, {"reso", 0.35f}}},
        {"Glass Pad", {{"o1shape", 1}, {"o2shape", 0}, {"o2coarse", 12}, {"o2fine", 5}, {"o2level", 0.5f}, {"o3shape", 0}, {"o3coarse", 24}, {"o3level", 0.2f}, {"o2spread", 12}, {"attack", 500}, {"decay", 2000}, {"sustain", 0.6f}, {"release", 1400}}},
        {"Octave Bass", {{"o1shape", 2}, {"o1coarse", -12}, {"o2shape", 3}, {"o2coarse", 0}, {"o2fine", 0}, {"o2level", 0.35f}, {"o3shape", 0}, {"o3coarse", -24}, {"o3level", 0.6f}, {"attack", 1}, {"decay", 220}, {"sustain", 0.6f}, {"release", 90}, {"cutoff", 1500}, {"envamt", 0.3f}}},
        {"Detuned Lead", {{"o1shape", 3}, {"o2shape", 3}, {"o2fine", 18}, {"o2level", 0.8f}, {"o3shape", 3}, {"o3coarse", 0}, {"o3fine", -18}, {"o3level", 0.8f}, {"attack", 3}, {"decay", 400}, {"sustain", 0.8f}, {"release", 200}, {"cutoff", 8000}, {"reso", 0.12f}}},
        {"Tri Mallet", {{"o1shape", 1}, {"o2shape", 0}, {"o2coarse", 24}, {"o2fine", 0}, {"o2level", 0.2f}, {"o3level", 0}, {"attack", 0}, {"decay", 420}, {"sustain", 0}, {"release", 380}}},
        {"Brass-ish", {{"o1shape", 3}, {"o2shape", 3}, {"o2fine", 4}, {"o2level", 0.7f}, {"o3shape", 2}, {"o3coarse", 0}, {"o3level", 0.2f}, {"attack", 45}, {"decay", 300}, {"sustain", 0.8f}, {"release", 150}, {"cutoff", 900}, {"envamt", 0.45f}, {"fattack", 60}, {"fsustain", 0.6f}}},
    };
    std::vector<Preset> out;
    for (auto& d : defs) {
        Json ps = Json::object();
        for (auto& p : params_) ps.set(p->info.id, p->info.def);
        for (auto& pv : d.values) ps.set(pv.id, pv.v);
        Json st = Json::object();
        st.set("type", typeId());
        st.set("params", ps);
        out.push_back({d.name, st});
    }
    return out;
}

} // namespace pulp
