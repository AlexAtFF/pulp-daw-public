#include "pulp/Clatter.h"

namespace pulp {

namespace {
constexpr float kOut = 0.8f;
// Six partials that share no common fundamental: the metal of the hats, and a wider set for the cymbal.
const double kHatRatios[6] = {1.0, 1.4471, 1.6170, 1.9265, 2.5028, 2.6637};
const double kCymbalRatios[6] = {1.0, 1.3420, 1.7312, 2.1753, 2.6818, 3.1473};
// Kit mode, one octave of keys from C: the drum, and how far it is shifted.
struct KitSlot { int model, offset; };
const KitSlot kKitMap[12] = {
    {Clatter::Kick, 0}, {Clatter::Rimshot, 0}, {Clatter::Snare, 0}, {Clatter::Clap, 0}, {Clatter::Tom, -5}, {Clatter::Tom, 2},
    {Clatter::ClosedHat, 0}, {Clatter::Clave, 0}, {Clatter::Shaker, 0}, {Clatter::Cowbell, 0}, {Clatter::OpenHat, 0}, {Clatter::Cymbal, 0},
};
// Level trims that bring every model to about the same peak at the default settings.
const float kTrim[Clatter::Kit] = {1.19f, 0.73f, 0.82f, 0.49f, 0.59f, 1.0f, 0.6f, 0.74f, 0.48f, 1.0f, 0.44f};
}

int Clatter::kitModel(int key) { return kKitMap[((key % 12) + 12) % 12].model; }
int Clatter::kitOffset(int key) { return kKitMap[((key % 12) + 12) % 12].offset; }

Clatter::Clatter() {
    pModel = addChoice("model", "Model", {"Kick", "Snare", "Clap", "Closed hat", "Open hat", "Tom", "Cymbal", "Rimshot", "Cowbell", "Clave", "Shaker", "Kit (key picks the drum)"}, Kick);
    pPitch = addFloat("pitch", "Pitch", -24, 24, 0, "st");
    pTone = addFloat("tone", "Tone", 0, 1, 0.5f, "%");
    pNoise = addFloat("noise", "Noise", 0, 1, 0.3f, "%");
    pDecay = addFloat("decay", "Decay", 0, 1, 0.45f, "%");
    pSnap = addFloat("snap", "Snap", 0, 1, 0.5f, "%");
    pColour = addFloat("colour", "Colour", 0, 1, 0.3f, "%");
    pVelSens = addFloat("velsens", "Velocity sensitivity", 0, 1, 0.8f, "%");
    pVolume = addFloat("volume", "Level", 0, 1, 0.5f, "%");
}

std::vector<ParamGroup> Clatter::paramGroups() const {
    return {
        {"Drum", {"model", "pitch", "decay"}},
        {"Character", {"tone", "noise", "snap", "colour"}},
        {"Output", {"velsens", "volume"}},
    };
}

void Clatter::prepare(double sampleRate) {
    sr_ = sampleRate;
    reset();
}

void Clatter::reset() {
    for (auto& v : voices_) v.active = false;
    ageCounter_ = 0;
    primed_ = false;
    voicesActive_.store(0, std::memory_order_relaxed);
}

void Clatter::beginBlock(ProcessContext& ctx) {
    bend_ = ctx.pitchBendSemis;
    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
    const float vol = pVolume->get();
    if (!primed_ || n == 0) vol_.snap(vol);
    else vol_.target(vol, ctx.numSamples);
    primed_ = true;
}

// Fills in a voice for one model. tone, noise, decay, snap and colour are 0..1; semis is the transposition.
void Clatter::setup(Voice& v, int model, double semis, float tone, float noise, float decay, float snap, float colour) {
    const double r = std::exp2(semis / 12.0), sr = sr_;
    const float fsr = (float)sr;
    auto len = [&](double lo, double hi) { return lo * std::pow(hi / lo, (double)decay); };     // seconds to fall 60 dB
    auto coef = [&](double seconds) { return bl::t60Coef(seconds, sr); };
    auto hz = [&](double f) { return std::min(0.45, f / sr); };
    v.envN.v = 1.0f;
    switch (model) {
        case Kick: {
            // a sine that drops from several times its pitch; Snap is how far, Colour adds harmonics
            v.nSine = 1;
            v.dtS[0] = hz(52.0 * r);
            v.pitchEnv = 1.0f + 7.0f * snap;
            v.pitchCoef = coef(0.06);
            v.h2 = 0.35f * colour;
            v.h3 = 0.2f * colour;
            v.lvlS[0] = 0.9f / (1.0f + 0.45f * colour);
            v.envS[0].v = 1.0f;
            v.coefS[0] = coef(len(0.12, 1.6));
            v.lvlNoise = 0.15f * snap + 0.6f * noise;           // the beater: a short low-passed burst
            v.mode1 = Low;
            v.f1.set(1500.0f * std::pow(2.0f, 3.0f * tone), 0.7f, fsr);
            v.coefN = coef(0.012);
            break;
        }
        case Tom: {
            v.nSine = 2;
            v.dtS[0] = hz(110.0 * r);
            v.dtS[1] = hz(110.0 * r * 1.58);
            v.pitchEnv = 0.25f + 1.2f * snap;
            v.pitchCoef = coef(0.08);
            v.h2 = 0.2f * colour;
            v.lvlS[0] = 0.8f;
            v.lvlS[1] = 0.12f + 0.3f * colour;
            v.envS[0].v = v.envS[1].v = 1.0f;
            const double t = len(0.15, 1.2);
            v.coefS[0] = coef(t);
            v.coefS[1] = coef(0.5 * t);
            v.lvlNoise = 0.1f * snap + 0.5f * noise;
            v.mode1 = Low;
            v.f1.set(800.0f * std::pow(2.0f, 3.0f * tone), 0.7f, fsr);
            v.coefN = coef(0.05);
            break;
        }
        case Snare: {
            // two shell modes and the wires: noise through a broad band-pass, with a sharper burst on top for Snap
            v.nSine = 2;
            const double fa = 185.0 * r;
            v.dtS[0] = hz(fa);
            v.dtS[1] = hz(fa * (1.5 + 0.7 * (double)colour));
            v.pitchEnv = 0.6f * snap;
            v.pitchCoef = coef(0.03);
            v.lvlS[0] = 0.6f * (1.0f - 0.5f * noise);
            v.lvlS[1] = 0.35f * (1.0f - 0.5f * noise);
            v.envS[0].v = v.envS[1].v = 1.0f;
            const double t = len(0.08, 0.35);
            v.coefS[0] = coef(t);
            v.coefS[1] = coef(0.7 * t);
            v.lvlNoise = 0.5f + 3.0f * noise;
            v.mode1 = Band;             // a broad band: wires are not white all the way up
            v.f1.set((float)(1800.0 * std::sqrt(r)) * std::pow(2.0f, 2.0f * tone), 0.55f, fsr);
            v.coefN = coef(len(0.1, 0.6));
            v.snapEnv = 2.0f * snap;
            v.snapCoef = coef(0.012);
            break;
        }
        case Clap: {
            // band-passed noise in three quick bursts and then a tail; Colour spreads the bursts,
            // Snap sharpens them, Noise is how much of it is tail
            v.lvlNoise = 3.0f;
            v.mode1 = Band;
            v.f1.set((float)(1100.0 * r) * std::pow(2.0f, (tone - 0.5f) * 2.0f), 1.6f, fsr);
            const double gap = 0.006 + 0.008 * (double)colour;
            v.burstGap = std::max(8, (int)(gap * sr));
            v.burstCount = v.burstGap;
            v.burstsLeft = 3;
            v.burstEnv = 1.0f;
            v.burstCoef = coef(gap * (2.2 - 1.6 * (double)snap));
            v.burstLvl = 1.0f - 0.3f * noise;
            v.lvl1 = 0.55f + 0.45f * noise;
            v.coefN = coef(len(0.08, 0.7));
            break;
        }
        case ClosedHat:
        case OpenHat: {
            // six squares that share no fundamental, and noise, through a high-pass
            v.nSq = 6;
            const double spread = 0.6 + 0.8 * (double)colour;
            for (int i = 0; i < 6; ++i) v.dtQ[i] = hz(320.0 * r * (1.0 + (kHatRatios[i] - 1.0) * spread));
            v.lvlMetal = (1.0f - noise) / 3.0f;
            v.lvlNoise = noise;
            v.mode1 = High;
            v.f1.set((float)(3500.0 * std::sqrt(r)) * std::pow(2.0f, 1.8f * tone), 1.3f, fsr);
            v.coefN = coef(model == ClosedHat ? len(0.02, 0.25) : len(0.25, 2.0));
            v.snapEnv = 1.5f * snap;
            v.snapCoef = coef(0.006);
            break;
        }
        case Cymbal: {
            // the same idea with a wider set of partials and two bands: a body that goes quickly and a top that lasts
            v.nSq = 6;
            const double spread = 0.6 + 0.8 * (double)colour;
            for (int i = 0; i < 6; ++i) v.dtQ[i] = hz(420.0 * r * (1.0 + (kCymbalRatios[i] - 1.0) * spread));
            v.lvlMetal = (1.0f - 0.7f * noise) / 3.0f;
            v.lvlNoise = 0.3f + 0.7f * noise;
            v.mode1 = Band;
            const float size = (float)std::pow(r, 0.75);            // a smaller cymbal is higher all over
            v.f1.set(std::min(14000.0f, 4500.0f * size * std::pow(2.0f, (tone - 0.5f) * 1.5f)), 0.9f, fsr);
            const double t = len(0.5, 6.0);
            v.coefN = coef(0.35 * t);
            v.use2 = true;
            v.mode2 = High;
            v.f2.set(std::min(16000.0f, 6500.0f * size * std::pow(2.0f, (tone - 0.5f) * 1.2f)), 0.8f, fsr);
            v.envN2.v = 1.0f;
            v.coefN2 = coef(t);
            v.lvl2 = 1.0f;
            v.snapEnv = 2.0f * snap;
            v.snapCoef = coef(0.02);
            break;
        }
        case Rimshot: {
            v.nSine = 2;
            const double fa = 470.0 * r;
            v.dtS[0] = hz(fa);
            v.dtS[1] = hz(fa * (2.6 + 1.6 * (double)colour));
            v.pitchEnv = 0.15f * snap;
            v.pitchCoef = coef(0.01);
            v.lvlS[0] = 0.65f - 0.35f * tone;           // Tone leans it from the low mode to the high one
            v.lvlS[1] = 0.25f + 0.45f * tone;
            v.envS[0].v = v.envS[1].v = 1.0f;
            const double t = len(0.015, 0.12);
            v.coefS[0] = coef(t);
            v.coefS[1] = coef(0.6 * t);
            v.lvlNoise = (0.25f + 0.9f * noise + 0.5f * snap) * (0.5f + tone);
            v.mode1 = High;
            v.f1.set(1500.0f * std::pow(2.0f, 1.5f * tone), 0.8f, fsr);
            v.coefN = coef(0.008);
            break;
        }
        case Cowbell: {
            // two squares a fifth or so apart through a band-pass; a quick strike, then a slower ring
            v.nSq = 2;
            const double fa = 545.0 * r, fb = fa * (1.35 + 0.3 * (double)colour);
            v.dtQ[0] = hz(fa);
            v.dtQ[1] = hz(fb);
            v.lvlMetal = 0.5f;
            v.lvlNoise = 0.5f * noise;
            v.mode1 = Band;
            v.f1.set((float)fb * std::pow(2.0f, (tone - 0.5f) * 2.0f), 2.2f, fsr);
            v.coefN = coef(len(0.08, 1.0));
            v.lvl1 = 0.5f;
            v.snapEnv = 1.0f + 3.0f * snap;
            v.snapCoef = coef(0.03);
            break;
        }
        case Clave: {
            // one high sine; Colour brings in a lower second mode (a wood block), Tone an overtone
            const double fa = 2450.0 * r;
            v.h2 = std::max(0.0f, tone - 0.3f) * 0.6f;
            v.nSine = colour > 0.02f ? 2 : 1;
            v.dtS[0] = hz(fa);
            v.dtS[1] = hz(fa * 0.42);
            v.lvlS[0] = 0.8f;
            v.lvlS[1] = 0.6f * colour;
            v.envS[0].v = 1.0f;
            v.envS[1].v = v.nSine > 1 ? 1.0f : 0.0f;
            const double t = len(0.02, 0.2);
            v.coefS[0] = coef(t);
            v.coefS[1] = coef(1.5 * t);
            v.lvlNoise = 0.3f * snap + 0.6f * noise;
            v.mode1 = Band;
            v.f1.set((float)fa * std::pow(2.0f, tone - 0.5f), 2.0f, fsr);
            v.coefN = coef(0.004);
            break;
        }
        case Shaker:
        default: {
            // band-passed noise that swells in and dies away; Noise makes it grainy, Snap shortens the swell
            v.lvlNoise = 2.0f;
            v.grain = noise;
            v.grainCoef = coef(0.0015);
            v.grainRate = (float)(4000.0 * (1.05 - (double)noise) / sr);
            v.grainEnv = 1.0f;
            v.mode1 = Band;
            v.f1.set((float)(5500.0 * std::sqrt(r)) * std::pow(2.0f, (tone - 0.5f) * 1.6f), 0.7f + 2.0f * colour, fsr);
            v.att = 0.0f;
            v.attStep = (float)(1.0 / ((0.001 + 0.025 * (1.0 - 0.9 * (double)snap)) * sr));
            v.coefN = coef(len(0.04, 0.5));
            break;
        }
    }
}

void Clatter::startVoice(const NoteEvent& e) {
    int model = clampT(pModel->getInt(), 0, (int)NumModels - 1);
    double keySemis = (double)(e.key - kRootKey);
    if (model == Kit) {
        model = kitModel(e.key);
        keySemis = 12.0 * (double)(e.key / 12 - kRootKey / 12) + (double)kitOffset(e.key);
    }
    if (model == ClosedHat)         // the hat closing stops the open one
        for (auto& o : voices_)
            if (o.active && o.model == OpenHat && o.killStep == 0.0f) o.killStep = (float)(1.0 / (0.008 * sr_));
    Voice* v = bl::claimVoice(voices_, kSounding, (float)(1.0 / (0.003 * sr_)));
    *v = Voice();
    v->active = true;
    v->id = e.id;
    v->age = ++ageCounter_;
    v->model = model;
    const float tone = clampT(pTone->get() + (e.modX - 0.5f), 0.0f, 1.0f), colour = clampT(pColour->get() + (e.modY - 0.5f), 0.0f, 1.0f);
    setup(*v, model, keySemis + (double)pPitch->get() + (double)e.fineCents * 0.01, tone, pNoise->get(), pDecay->get(), pSnap->get(), colour);
    v->noise = dsp::Noise(0x6D2B79F5u ^ (e.id * 2654435761u) ^ ((uint32)model * 97u));
    const float vel = e.velocity / kDefaultVelocity;
    v->gain = std::max(0.0f, 1.0f + (vel * vel - 1.0f) * pVelSens->get()) * kTrim[model];
    bl::noteGains(e.pan, v->panL, v->panR);
}

void Clatter::handleEvent(const NoteEvent& e, ProcessContext&) {
    switch (e.type) {
        case NoteEvent::On: startVoice(e); break;
        case NoteEvent::Off: break;                 // a drum rings out whatever the note's length
        case NoteEvent::SlideTo: break;             // and does not bend
        case NoteEvent::AllOff:
            for (auto& v : voices_) if (v.active && v.killStep == 0.0f) v.killStep = (float)(1.0 / (0.03 * sr_));
            break;
        case NoteEvent::Kill:
            for (auto& v : voices_) if (v.active) v.killStep = std::max(v.killStep, (float)(1.0 / (0.003 * sr_)));
            break;
        case NoteEvent::KillNote:
            for (auto& v : voices_) if (v.active && v.id == e.id) v.killStep = std::max(v.killStep, (float)(1.0 / (0.003 * sr_)));
            break;
    }
}

void Clatter::render(float* L, float* R, int n, ProcessContext&) {
    const double bendRatio = std::exp2((double)bend_ / 12.0);
    bool any = false;
    for (auto& v : voices_) {
        if (!v.active) continue;
        any = true;
        for (int i = 0; i < n; ++i) {
            float out = 0.0f;
            if (v.nSine > 0) {
                const double pm = (1.0 + (double)v.pitchEnv) * bendRatio;
                v.pitchEnv *= v.pitchCoef;
                if (v.pitchEnv < 1.0e-6f) v.pitchEnv = 0.0f;
                const double p0 = kTwoPi * v.phS[0];
                float s = (float)std::sin(p0);
                if (v.h2 != 0.0f) s += v.h2 * (float)std::sin(2.0 * p0) + v.h3 * (float)std::sin(3.0 * p0);
                out = s * v.lvlS[0] * v.envS[0].next(v.coefS[0]);
                v.phS[0] += std::min(0.45, v.dtS[0] * pm);
                if (v.phS[0] >= 1.0) v.phS[0] -= 1.0;
                if (v.nSine > 1) {
                    out += (float)std::sin(kTwoPi * v.phS[1]) * v.lvlS[1] * v.envS[1].next(v.coefS[1]);
                    v.phS[1] += std::min(0.45, v.dtS[1] * pm);
                    if (v.phS[1] >= 1.0) v.phS[1] -= 1.0;
                }
            }
            float src = 0.0f;
            if (v.nSq > 0) {
                double sum = 0.0;
                for (int k = 0; k < v.nSq; ++k) {
                    const double t = v.phQ[k], d = std::min(0.45, v.dtQ[k] * bendRatio);
                    double s = t < 0.5 ? 1.0 : -1.0;
                    s += bl::polyBlep(t, d);
                    double t2 = t + 0.5; if (t2 >= 1.0) t2 -= 1.0;
                    s -= bl::polyBlep(t2, d);
                    sum += s;
                    v.phQ[k] = t + d >= 1.0 ? t + d - 1.0 : t + d;
                }
                src = (float)sum * v.lvlMetal;
            }
            if (v.lvlNoise > 0.0f) {
                float nz = v.noise.next();
                if (v.grain > 0.0f) {
                    if (v.noise.uni() < v.grainRate) v.grainEnv = 1.0f;
                    nz *= (1.0f - v.grain) + v.grain * 3.0f * v.grainEnv;
                    v.grainEnv *= v.grainCoef;
                }
                src += nz * v.lvlNoise;
            }
            float lp, bp, hp;
            v.f1.process(src, lp, bp, hp);
            const float y1 = v.mode1 == Low ? lp : (v.mode1 == Band ? bp : hp);
            float e;
            if (v.burstsLeft > 0) {
                e = v.burstEnv * v.burstLvl;
                v.burstEnv *= v.burstCoef;
                if (--v.burstCount <= 0) { v.burstCount = v.burstGap; --v.burstsLeft; v.burstEnv = 1.0f; }
            } else if (v.att < 1.0f) {
                e = v.envN.v * v.lvl1 * v.att;
                v.att += v.attStep;
            } else {
                e = v.envN.next(v.coefN) * v.lvl1;
            }
            e *= 1.0f + v.snapEnv;
            v.snapEnv *= v.snapCoef;
            if (v.snapEnv < 1.0e-6f) v.snapEnv = 0.0f;
            out += y1 * e;
            if (v.use2) {
                v.f2.process(src, lp, bp, hp);
                out += (v.mode2 == Low ? lp : (v.mode2 == Band ? bp : hp)) * v.envN2.next(v.coefN2) * v.lvl2;
            }
            const float a = v.gain * v.kill;
            L[i] += out * a * v.panL;
            R[i] += out * a * v.panR;
            if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) { v.active = false; break; } }
            if (v.burstsLeft == 0 && v.att >= 1.0f && v.envN.v <= 0.0f && v.envS[0].v <= 0.0f && v.envS[1].v <= 0.0f && v.envN2.v <= 0.0f) { v.active = false; break; }
        }
    }
    if (!any) return;
    for (int i = 0; i < n; ++i) {
        const float vol = vol_.next() * kOut;
        L[i] *= vol;
        R[i] *= vol;
    }
    scope_.push(L, R, n);
}

std::vector<Preset> Clatter::factoryPresets() const {
    return buildPresets(*this, {
        {"Round Kick", {{"model", Kick}, {"tone", 0.4f}, {"noise", 0.15f}, {"decay", 0.45f}, {"snap", 0.5f}, {"colour", 0.1f}}},
        {"Punch Kick", {{"model", Kick}, {"pitch", 3}, {"tone", 0.7f}, {"noise", 0.35f}, {"decay", 0.3f}, {"snap", 0.8f}, {"colour", 0.35f}}},
        {"Long Low Kick", {{"model", Kick}, {"pitch", -4}, {"tone", 0.3f}, {"noise", 0.05f}, {"decay", 0.85f}, {"snap", 0.35f}, {"colour", 0}}},
        {"Crack Snare", {{"volume", 0.31f}, {"model", Snare}, {"tone", 0.7f}, {"noise", 0.6f}, {"decay", 0.45f}, {"snap", 0.8f}, {"colour", 0.5f}}},
        {"Fat Snare", {{"model", Snare}, {"pitch", -4}, {"tone", 0.4f}, {"noise", 0.45f}, {"decay", 0.6f}, {"snap", 0.4f}, {"colour", 0.3f}}},
        {"Thin Snare", {{"volume", 0.28f}, {"model", Snare}, {"pitch", 5}, {"tone", 0.85f}, {"noise", 0.8f}, {"decay", 0.3f}, {"snap", 0.6f}, {"colour", 0.7f}}},
        {"Tight Clap", {{"model", Clap}, {"tone", 0.55f}, {"noise", 0.2f}, {"decay", 0.25f}, {"snap", 0.8f}, {"colour", 0.3f}}},
        {"Wide Clap", {{"volume", 0.38f}, {"model", Clap}, {"tone", 0.45f}, {"noise", 0.8f}, {"decay", 0.6f}, {"snap", 0.4f}, {"colour", 0.8f}}},
        {"Tick Hat", {{"model", ClosedHat}, {"tone", 0.7f}, {"noise", 0.2f}, {"decay", 0.2f}, {"snap", 0.6f}, {"colour", 0.5f}}},
        {"Sizzle Hat", {{"volume", 0.64f}, {"model", ClosedHat}, {"tone", 0.5f}, {"noise", 0.7f}, {"decay", 0.5f}, {"snap", 0.3f}, {"colour", 0.7f}}},
        {"Open Wash", {{"volume", 0.66f}, {"model", OpenHat}, {"tone", 0.6f}, {"noise", 0.4f}, {"decay", 0.55f}, {"snap", 0.3f}, {"colour", 0.5f}}},
        {"Floor Tom", {{"model", Tom}, {"pitch", -7}, {"tone", 0.4f}, {"noise", 0.2f}, {"decay", 0.6f}, {"snap", 0.4f}, {"colour", 0.3f}}},
        {"Rack Tom", {{"model", Tom}, {"pitch", 5}, {"tone", 0.55f}, {"noise", 0.15f}, {"decay", 0.4f}, {"snap", 0.5f}, {"colour", 0.2f}}},
        {"Crash Splash", {{"model", Cymbal}, {"tone", 0.6f}, {"noise", 0.6f}, {"decay", 0.5f}, {"snap", 0.7f}, {"colour", 0.5f}}},
        {"Ride Ping", {{"volume", 0.32f}, {"model", Cymbal}, {"pitch", 4}, {"tone", 0.4f}, {"noise", 0.15f}, {"decay", 0.7f}, {"snap", 0.9f}, {"colour", 0.15f}}},
        {"Rim Click", {{"model", Rimshot}, {"tone", 0.6f}, {"noise", 0.3f}, {"decay", 0.4f}, {"snap", 0.7f}, {"colour", 0.5f}}},
        {"Low Bell", {{"model", Cowbell}, {"pitch", -5}, {"tone", 0.45f}, {"noise", 0.1f}, {"decay", 0.5f}, {"snap", 0.5f}, {"colour", 0.5f}}},
        {"High Bell", {{"model", Cowbell}, {"pitch", 7}, {"tone", 0.6f}, {"noise", 0}, {"decay", 0.3f}, {"snap", 0.7f}, {"colour", 0.3f}}},
        {"Wood Clave", {{"model", Clave}, {"tone", 0.6f}, {"noise", 0.1f}, {"decay", 0.35f}, {"snap", 0.5f}, {"colour", 0}}},
        {"Wood Block", {{"model", Clave}, {"pitch", -12}, {"tone", 0.5f}, {"noise", 0.1f}, {"decay", 0.5f}, {"snap", 0.6f}, {"colour", 0.8f}}},
        {"Soft Shaker", {{"model", Shaker}, {"tone", 0.5f}, {"noise", 0.1f}, {"decay", 0.4f}, {"snap", 0.2f}, {"colour", 0.3f}}},
        {"Grain Shaker", {{"volume", 0.33f}, {"model", Shaker}, {"tone", 0.6f}, {"noise", 0.8f}, {"decay", 0.5f}, {"snap", 0.6f}, {"colour", 0.6f}}},
        {"Machine Kit", {{"model", Kit}, {"tone", 0.55f}, {"noise", 0.4f}, {"decay", 0.45f}, {"snap", 0.6f}, {"colour", 0.4f}}},
        {"Dusty Kit", {{"model", Kit}, {"pitch", -2}, {"tone", 0.3f}, {"noise", 0.6f}, {"decay", 0.55f}, {"snap", 0.35f}, {"colour", 0.6f}}},
        {"Tight Kit", {{"model", Kit}, {"pitch", 2}, {"tone", 0.7f}, {"noise", 0.3f}, {"decay", 0.2f}, {"snap", 0.8f}, {"colour", 0.3f}}},
    });
}

} // namespace pulp
