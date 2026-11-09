#include "pulp/Sextet.h"
#include <deque>

namespace pulp {

namespace {

constexpr int kTabBits = 12;
constexpr int kTab = 1 << kTabBits;
constexpr int kWaves = 7;
constexpr int kCtl = 64;                // control period in oversampled samples (32 output samples), counted per voice
constexpr float kModCycles = 2.0f;      // matrix amount 1 from a full-level operator swings the phase two cycles (index 4 pi)
constexpr float kFbCycles = 0.25f;      // feedback 1: half a cycle (index pi) of the mean of the operator's last two outputs
constexpr float kMaxFeedback = 4.712f;  // feedback index is never allowed past 1.5 pi

// How many harmonics each operator shape has that matter, for the alias guard.
const float kWaveWidth[kWaves] = {1.0f, 4.0f, 6.0f, 6.0f, 5.0f, 7.0f, 8.0f};

// The operator shapes: one cycle each, mean removed, peak 1, and the phase of the first rising zero
// (operators start there, so a note with an instant attack still starts from silence).
struct WaveSet {
    float t[kWaves][kTab + 1];
    uint32 start[kWaves];
    WaveSet() {
        std::vector<double> d((size_t)kTab);
        for (int w = 0; w < kWaves; ++w) {
            for (int i = 0; i < kTab; ++i) {
                const double x = (double)i / kTab, s = std::sin(kTwoPi * x);
                double v = 0.0;
                switch (w) {
                    case 0: v = s; break;                                       // sine
                    case 1: v = x < 0.5 ? s : 0.0; break;                       // half sine
                    case 2: v = std::fabs(s); break;                            // rectified sine (sounds an octave up)
                    case 3: v = x < 0.5 ? std::sin(2.0 * kTwoPi * x) : 0.0; break;   // double sine: one fast cycle, then rest
                    case 4: v = 2.0 / kPi * std::asin(s); break;                // triangle
                    case 5:                                                     // soft square: four odd harmonics
                        for (int h = 1; h <= 7; h += 2) { const double a = kPi * h / 9.0; v += std::sin(kTwoPi * h * x) / h * std::sin(a) / a; }
                        break;
                    default:                                                    // soft saw: eight harmonics
                        for (int h = 1; h <= 8; ++h) { const double a = kPi * h / 9.0; v += std::sin(kTwoPi * h * x) / h * std::sin(a) / a; }
                        break;
                }
                d[(size_t)i] = v;
            }
            double mean = 0.0, peak = 0.0;
            for (double v : d) mean += v;
            mean /= kTab;
            for (double& v : d) { v -= mean; peak = std::max(peak, std::fabs(v)); }
            for (int i = 0; i < kTab; ++i) t[w][i] = (float)(d[(size_t)i] / peak);
            t[w][kTab] = t[w][0];
            start[w] = 0;
            for (int i = 0; i < kTab; ++i)
                if (t[w][i] <= 0.0f && t[w][i + 1] > 0.0f) {
                    const double frac = (double)(-t[w][i]) / (double)(t[w][i + 1] - t[w][i]);
                    start[w] = (uint32)(((double)i + frac) / kTab * 4294967296.0);
                    break;
                }
        }
    }
};
const WaveSet& waveSet() { static const WaveSet w; return w; }

// The ready-made routings: pairs of {modulator, carrier} and the operators that reach the output.
struct Algo { const char* name; int numPairs; unsigned char pairs[5][2]; unsigned char carriers; };
const Algo kAlgos[] = {
    {"Stack of six", 5, {{6, 5}, {5, 4}, {4, 3}, {3, 2}, {2, 1}}, 0x01},
    {"Two stacks of three", 4, {{3, 2}, {2, 1}, {6, 5}, {5, 4}}, 0x09},
    {"Three pairs", 3, {{2, 1}, {4, 3}, {6, 5}}, 0x15},
    {"One carrier, five modulators", 5, {{2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}}, 0x01},
    {"All carriers (additive)", 0, {}, 0x3f},
    {"Stack of four and a pair", 4, {{4, 3}, {3, 2}, {2, 1}, {6, 5}}, 0x11},
    {"Two carriers, two modulators each", 4, {{2, 1}, {3, 1}, {5, 4}, {6, 4}}, 0x09},
    {"One modulator, five carriers", 5, {{6, 1}, {6, 2}, {6, 3}, {6, 4}, {6, 5}}, 0x1f},
    {"A pair and four carriers", 1, {{2, 1}}, 0x3d},
    {"Branched stack", 5, {{3, 2}, {2, 1}, {5, 4}, {4, 1}, {6, 1}}, 0x01},
    {"Three carriers on one stack", 5, {{6, 5}, {5, 4}, {4, 1}, {4, 2}, {4, 3}}, 0x07},
    {"A pair, a stack of three and a sine", 3, {{2, 1}, {5, 4}, {4, 3}}, 0x25},
};
constexpr int kNumAlgos = (int)(sizeof kAlgos / sizeof kAlgos[0]);

std::string opId(int i, const char* what) { return "op" + std::to_string(i + 1) + what; }

} // namespace

int Sextet::numAlgorithms() { return kNumAlgos; }
const char* Sextet::algorithmName(int a) { return a >= 1 && a <= kNumAlgos ? kAlgos[a - 1].name : "Custom matrix"; }

Sextet::Sextet() {
    waveSet();
    const std::vector<std::string> waves = {"Sine", "Half sine", "Rectified sine", "Double sine", "Triangle", "Soft square", "Soft saw"};
    std::vector<std::string> ratios = {"0.5"};
    for (int r = 1; r <= 32; ++r) ratios.push_back(std::to_string(r));
    const float defLevel[kOps] = {1.0f, 0.12f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < kOps; ++i) {
        const std::string d = "Op " + std::to_string(i + 1) + " ";
        auto id = [&](const char* w) { return opId(i, w); };
        OpParams& p = op[i];
        p.wave = addChoice(id("wave").c_str(), (d + "shape").c_str(), waves, 0);
        p.mode = addChoice(id("mode").c_str(), (d + "frequency mode").c_str(), {"Ratio", "Fixed"}, 0);
        p.coarse = addChoice(id("coarse").c_str(), (d + "ratio").c_str(), ratios, 1);
        p.fine = addFloat(id("fine").c_str(), (d + "ratio fine").c_str(), 0, 1, 0, "%");
        p.hz = addFloat(id("hz").c_str(), (d + "fixed frequency").c_str(), 0.1f, 8000, 440, "Hz", 3.0f);
        p.detune = addFloat(id("detune").c_str(), (d + "detune").c_str(), -100, 100, 0, "ct");
        p.level = addFloat(id("level").c_str(), (d + "level").c_str(), 0, 1, defLevel[i], "%", 2.0f);
        p.pan = addFloat(id("pan").c_str(), (d + "pan").c_str(), -1, 1, 0, "");
        p.vel = addFloat(id("vel").c_str(), (d + "velocity sensitivity").c_str(), 0, 1, 0.7f, "%");
        p.kscale = addFloat(id("kscale").c_str(), (d + "level key scaling").c_str(), -1, 1, 0, "");
        p.attack = addFloat(id("attack").c_str(), (d + "attack").c_str(), 0, 10000, 1, "ms", 3.0f);
        p.decay = addFloat(id("decay").c_str(), (d + "decay").c_str(), 1, 20000, 600, "ms", 3.0f);
        p.sustain = addFloat(id("sustain").c_str(), (d + "sustain").c_str(), 0, 1, 0.7f, "%");
        p.release = addFloat(id("release").c_str(), (d + "release").c_str(), 1, 20000, 200, "ms", 3.0f);
        p.rks = addFloat(id("rks").c_str(), (d + "rate key scaling").c_str(), 0, 1, 0, "%");
        p.fb = addFloat(id("fb").c_str(), (d + "feedback").c_str(), 0, 1, 0, "%");
        p.lfo = addFloat(id("lfo").c_str(), (d + "LFO to level").c_str(), 0, 1, 0, "%");
    }
    std::vector<std::string> algos = {"Custom matrix"};
    for (auto& a : kAlgos) algos.push_back(a.name);
    pAlgo = addChoice("algo", "Algorithm", algos, 3);
    for (int i = 0; i < kOps; ++i)
        for (int j = 0; j < kOps; ++j) {
            if (i == j) { matrix[i][j] = op[i].fb; continue; }
            const std::string id = "m" + std::to_string(i + 1) + std::to_string(j + 1);
            const std::string name = "Op " + std::to_string(j + 1) + " into Op " + std::to_string(i + 1);
            matrix[i][j] = addFloat(id.c_str(), name.c_str(), 0, 1, (i == 0 && j == 1) ? 1.0f : 0.0f, "%", 2.0f);
        }
    for (int i = 0; i < kOps; ++i) {
        const std::string id = "out" + std::to_string(i + 1), name = "Op " + std::to_string(i + 1) + " to output";
        op[i].out = addFloat(id.c_str(), name.c_str(), 0, 1, i == 0 ? 1.0f : 0.0f, "%");
    }
    pModDepth = addFloat("moddepth", "Modulation depth", 0, 2, 1, "");
    pGuard = addBool("guard", "Alias guard", true);
    pPEnvAmt = addFloat("penvamt", "Pitch envelope amount", -48, 48, 0, "st");
    pPEnvTime = addFloat("penvtime", "Pitch envelope time", 1, 5000, 100, "ms", 3.0f);
    pGlide = addFloat("glide", "Glide", 0, 2000, 0, "ms", 3.0f);
    const float defRate[2] = {5.0f, 2.0f};
    for (int k = 0; k < 2; ++k) {
        const std::string n = "lfo" + std::to_string(k + 1), d = "LFO " + std::to_string(k + 1) + " ";
        pLfoWave[k] = addChoice((n + "wave").c_str(), (d + "shape").c_str(), parts::lfoNames(), 0);
        pLfoRate[k] = addFloat((n + "rate").c_str(), (d + "rate").c_str(), 0.02f, 40, defRate[k], "Hz", 3.0f);
        pLfoDelay[k] = addFloat((n + "delay").c_str(), (d + "delay").c_str(), 0, 5000, 0, "ms", 3.0f);
        pLfoPitch[k] = addFloat((n + "pitch").c_str(), (d + "to pitch").c_str(), 0, 1200, 0, "ct", 3.0f);
        pLfoAmp[k] = addFloat((n + "amp").c_str(), (d + "to operator levels").c_str(), 0, 1, 0, "%");
    }
    pFilter = addChoice("filter", "Filter", {"Off", "Low pass", "High pass", "Band pass"}, 0);
    pCutoff = addFloat("cutoff", "Filter cutoff", 20, 20000, 20000, "Hz", 3.0f);
    pReso = addFloat("reso", "Filter resonance", 0, 1, 0.1f, "%");
    ParamInfo vi;
    vi.id = "voices"; vi.name = "Polyphony"; vi.min = 1; vi.max = (float)kMaxPoly; vi.def = 16; vi.steps = kMaxPoly;
    pVoices = add(vi);
    pVolume = addFloat("volume", "Level", 0, 1, 0.5f, "%");
}

std::vector<ParamGroup> Sextet::paramGroups() const {
    std::vector<ParamGroup> g;
    for (int i = 0; i < kOps; ++i) {
        ParamGroup pg;
        pg.title = "Operator " + std::to_string(i + 1);
        for (const char* w : {"wave", "mode", "coarse", "fine", "hz", "detune", "level", "pan", "vel", "kscale",
                              "attack", "decay", "sustain", "release", "rks", "fb", "lfo"})
            pg.ids.push_back(opId(i, w));
        g.push_back(pg);
    }
    ParamGroup m;
    m.title = "Matrix";
    m.ids.push_back("algo");
    for (int i = 0; i < kOps; ++i)
        for (int j = 0; j < kOps; ++j)
            if (i != j) m.ids.push_back("m" + std::to_string(i + 1) + std::to_string(j + 1));
    for (int i = 0; i < kOps; ++i) m.ids.push_back("out" + std::to_string(i + 1));
    g.push_back(m);
    g.push_back({"Pitch and LFO", {"penvamt", "penvtime", "glide",
                                   "lfo1wave", "lfo1rate", "lfo1delay", "lfo1pitch", "lfo1amp",
                                   "lfo2wave", "lfo2rate", "lfo2delay", "lfo2pitch", "lfo2amp"}});
    g.push_back({"Global", {"moddepth", "guard", "filter", "cutoff", "reso", "voices", "volume"}});
    return g;
}

void Sextet::applyAlgorithm(int algorithm) {
    if (algorithm < 1 || algorithm > kNumAlgos) return;
    const Algo& a = kAlgos[algorithm - 1];
    for (int i = 0; i < kOps; ++i) {
        for (int j = 0; j < kOps; ++j) if (i != j) matrix[i][j]->set(0.0f);
        op[i].out->set((a.carriers >> i) & 1 ? 1.0f : 0.0f);
    }
    for (int k = 0; k < a.numPairs; ++k) matrix[a.pairs[k][1] - 1][a.pairs[k][0] - 1]->set(1.0f);
    pAlgo->set(0.0f);
}

void Sextet::prepare(double sampleRate) {
    sr_ = sampleRate;
    dec_[0].prepare();
    dec_[1].prepare();
    dcCoef_ = (float)(1.0 - std::exp(-kTwoPi * 10.0 / sampleRate));
    reset();
}

void Sextet::reset() {
    for (auto& v : voices_) v.active = false;
    voicesActive_.store(0, std::memory_order_relaxed);
    dec_[0].reset();
    dec_[1].reset();
    post_.reset();
    dcL_ = dcR_ = 0.0f;
    lastKey_ = -1.0f;
    primed_ = false;
}

void Sextet::beginBlock(ProcessContext& ctx) {
    hostBend_ = ctx.pitchBendSemis;
    Snap& S = snap_;
    const int algo = clampT(pAlgo->getInt(), 0, kNumAlgos);
    float outSum = 0.0f;
    for (int i = 0; i < kOps; ++i) {
        const OpParams& p = op[i];
        OpSnap& o = S.op[i];
        o.wave = clampT(p.wave->getInt(), 0, kWaves - 1);
        o.fixed = p.mode->getInt() == 1;
        const int c = p.coarse->getInt();
        const double det = std::pow(2.0, (double)p.detune->get() / 1200.0);
        o.ratio = (float)(o.fixed ? det : (c == 0 ? 0.5 : (double)c) * (1.0 + (double)p.fine->get()) * det);
        o.hz = p.hz->get();
        o.level = p.level->get();
        o.pan = p.pan->get();
        o.vel = p.vel->get();
        o.kscale = p.kscale->get();
        o.ams = p.lfo->get();
        o.attack = p.attack->get();
        o.decay = p.decay->get();
        o.sustain = p.sustain->get();
        o.release = p.release->get();
        o.rks = p.rks->get();
        o.out = algo == 0 ? p.out->get() : (float)((kAlgos[algo - 1].carriers >> i) & 1);
        outSum += o.out * o.level;
        for (int j = 0; j < kOps; ++j) S.mat[i][j] = (i == j || algo == 0) ? matrix[i][j]->get() : 0.0f;
    }
    if (algo > 0) {
        const Algo& a = kAlgos[algo - 1];
        for (int k = 0; k < a.numPairs; ++k) S.mat[a.pairs[k][1] - 1][a.pairs[k][0] - 1] = 1.0f;
    }
    for (int j = 0; j < kOps; ++j) {
        S.op[j].modulates = false;
        for (int i = 0; i < kOps; ++i) if (i != j && S.mat[i][j] != 0.0f) S.op[j].modulates = true;
    }
    // Processing order: an operator runs once everything that modulates it has run, so a modulator's
    // output reaches its target in the same sample. Where the routing loops, the link that closes the
    // loop is one (oversampled) sample late.
    bool done[kOps] = {};
    for (int k = 0; k < kOps; ++k) {
        int pick = -1;
        for (int i = kOps - 1; i >= 0 && pick < 0; --i) {
            if (done[i]) continue;
            bool ready = true;
            for (int j = 0; j < kOps; ++j) if (j != i && !done[j] && S.mat[i][j] != 0.0f) ready = false;
            if (ready) pick = i;
        }
        for (int i = kOps - 1; i >= 0 && pick < 0; --i) if (!done[i]) pick = i;
        S.order[k] = pick;
        done[pick] = true;
    }
    // Several carriers together are no louder than one.
    S.norm = 1.0f / std::max(1.0f, outSum);
    S.modDepth = pModDepth->get();
    S.guard = pGuard->getBool();
    S.penvAmt = pPEnvAmt->get();
    S.penvRate = (float)(4.6 / (0.001 * (double)pPEnvTime->get() * sr_));
    S.glideMs = pGlide->get();
    for (int k = 0; k < 2; ++k) {
        S.lfo[k].wave = pLfoWave[k]->getInt();
        S.lfo[k].rate = pLfoRate[k]->get();
        S.lfo[k].delay = pLfoDelay[k]->get();
        S.lfo[k].pitch = pLfoPitch[k]->get();
        S.lfo[k].amp = pLfoAmp[k]->get();
    }
    S.voices = clampT(pVoices->getInt(), 1, kMaxPoly);

    const float vol = pVolume->get() * 0.7f;
    postMode_ = pFilter->getInt();
    if (postMode_ == 0) post_.reset();
    const float gT = parts::svfG(pCutoff->get(), sr_), kT = parts::svfK(pReso->get());
    if (!primed_) { volume_.snap(vol); postG_ = gT; postK_ = kT; primed_ = true; }
    volume_.target(vol, kQuantum);
    postGStep_ = (gT - postG_) / (float)kQuantum;
    postKStep_ = (kT - postK_) / (float)kQuantum;

    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
}

void Sextet::startVoice(const NoteEvent& e) {
    const Snap& S = snap_;
    const WaveSet& W = waveSet();
    const float killStep = (float)(1.0 / (0.003 * 2.0 * sr_));
    // Polyphony: fade out the oldest sounding voices until there is room for this one.
    for (;;) {
        int living = 0;
        Voice* oldest = nullptr;
        for (auto& c : voices_)
            if (c.active && c.killStep == 0.0f) { ++living; if (!oldest || c.age < oldest->age) oldest = &c; }
        if (living < S.voices || !oldest) break;
        oldest->killStep = killStep;
    }
    Voice* v = nullptr;
    for (auto& c : voices_) if (!c.active) { v = &c; break; }
    if (!v) {       // every slot is busy: take the one that has faded furthest
        for (auto& c : voices_) if (c.killStep > 0.0f && (!v || c.kill < v->kill)) v = &c;
        if (!v) { v = &voices_[0]; for (auto& c : voices_) if (c.age < v->age) v = &c; }
    }
    v->active = true;
    v->released = false;
    v->id = e.id;
    v->age = ++ageCounter_;
    v->key = e.key;
    v->group = e.group;
    const float vel = e.velocity / kDefaultVelocity;
    v->vel2 = vel * vel;
    v->pan = e.pan;
    v->fine = e.fineCents;
    v->modX = e.modX;
    v->modY = e.modY;
    v->relScale = 0.25f + 1.5f * e.release;
    v->bend = v->bendTarget = v->bendStep = 0.0f;
    if (e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) {
        v->bend = e.glideFromKey - (float)e.key;
        v->bendStep = -v->bend / e.slideSamples;
    } else if (S.glideMs > 0.0f && lastKey_ >= 0.0f && lastKey_ != (float)e.key) {
        v->bend = lastKey_ - (float)e.key;
        v->bendStep = -v->bend / std::max(1.0f, (float)(S.glideMs * 0.001 * sr_));
    }
    lastKey_ = (float)e.key;
    v->kill = 1.0f;
    v->killStep = 0.0f;
    v->time = 0.0;
    v->ctlLeft = 0;
    v->rng = dsp::Noise(0x51F15EEDu + e.id * 2654435761u);
    for (int k = 0; k < 2; ++k) { v->lfoPhase[k] = 0.0f; v->lfoRnd[k] = v->rng.next(); }
    const double osr = 2.0 * sr_;
    for (int i = 0; i < kOps; ++i) {
        const OpSnap& o = S.op[i];
        v->phase[i] = W.start[o.wave];
        v->y[i] = v->yPrev[i] = 0.0f;
        const float ks = o.rks > 0.0f ? std::pow(2.0f, -o.rks * (float)(e.key - kRootKey) / 12.0f) : 1.0f;
        v->env[i].set(o.attack * ks, o.decay * ks, o.sustain, o.release * ks * v->relScale, osr);
        v->env[i].start();
    }
    control(*v, true);
}

void Sextet::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * 2.0 * sr_));
    auto release = [](Voice& v) { v.released = true; for (auto& en : v.env) en.release(); };
    switch (e.type) {
        case NoteEvent::On: startVoice(e); break;
        case NoteEvent::Off:
            for (auto& v : voices_) if (v.active && v.id == e.id) release(v);
            break;
        case NoteEvent::AllOff:
            for (auto& v : voices_) if (v.active) release(v);
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

// Works out where every ramp of a voice should be one control period from now (or, for a new
// voice, where it starts). Control points are counted from the note's own first sample, so a note
// sounds the same wherever it falls in the engine's quantum.
void Sextet::control(Voice& v, bool snap) {
    const Snap& S = snap_;
    const double osr = 2.0 * sr_;
    const int half = kCtl / 2;
    if (!snap) {
        v.time += half;
        if (v.bendStep != 0.0f) {
            v.bend += v.bendStep * (float)half;
            if ((v.bendStep > 0.0f) == (v.bend >= v.bendTarget)) { v.bend = v.bendTarget; v.bendStep = 0.0f; }
        }
    }
    const float tMs = (float)(v.time * 1000.0 / sr_);
    float cents = 0.0f, trem[2];
    for (int k = 0; k < 2; ++k) {
        const LfoSnap& l = S.lfo[k];
        if (!snap) {
            v.lfoPhase[k] += l.rate * (float)half / (float)sr_;
            if (v.lfoPhase[k] >= 1.0f) { v.lfoPhase[k] -= std::floor(v.lfoPhase[k]); v.lfoRnd[k] = v.rng.next(); }
        }
        const float fade = clampT((tMs - l.delay) / (0.5f * l.delay + 10.0f), 0.0f, 1.0f);
        const float x = parts::lfoShape(l.wave, v.lfoPhase[k], v.lfoRnd[k]);
        cents += x * fade * l.pitch;
        trem[k] = l.amp * fade * 0.5f * (1.0f - x);     // how far this LFO pulls a fully sensitive operator down
    }
    const float pe = S.penvAmt != 0.0f ? S.penvAmt * std::exp(-(float)v.time * S.penvRate) : 0.0f;
    const double semis = (double)v.key + (double)v.bend + (double)hostBend_ + (double)(v.fine + cents) * 0.01 + (double)pe;
    const double base = keyToHz(semis);
    const float rampK = 1.0f / (float)kCtl;
    auto ramp = [&](float& cur, float& step, float target) {
        if (snap) { cur = target; step = 0.0f; }
        else step = (target - cur) * rampK;
    };

    float f[kOps], peak[kOps];
    for (int i = 0; i < kOps; ++i) {
        const OpSnap& o = S.op[i];
        double hz = o.fixed ? (double)o.hz * o.ratio : base * o.ratio;
        // an operator that would run past the oversampled Nyquist is faded out instead
        const float nyq = clampT((float)((0.48 * osr - hz) / (0.04 * osr)), 0.0f, 1.0f);
        hz = std::min(hz, 0.49 * osr);
        f[i] = (float)hz;
        const uint32 incT = (uint32)(hz / osr * 4294967296.0);
        if (snap) { v.inc[i] = incT; v.incStep[i] = 0; }
        else v.incStep[i] = (std::int32_t)(((int64)incT - (int64)v.inc[i]) / kCtl);
        const float velGain = std::max(0.0f, 1.0f + (v.vel2 - 1.0f) * o.vel);
        const float ks = o.kscale == 0.0f ? 1.0f : dbToGain(clampT(o.kscale * (float)(v.key - kRootKey), -60.0f, 12.0f));
        const float a = o.level * velGain * ks * (1.0f - o.ams * trem[0]) * (1.0f - o.ams * trem[1]) * nyq;
        ramp(v.amp[i], v.ampStep[i], a);
        const parts::Env& en = v.env[i];
        peak[i] = a * (en.stage == parts::Env::Attack ? 1.0f : en.value);
        float pl, pr;
        parts::panGains(o.pan + v.pan, pl, pr);
        ramp(v.gl[i], v.glStep[i], o.out * S.norm * pl);
        ramp(v.gr[i], v.grStep[i], o.out * S.norm * pr);
    }

    // Modulation depths. Mod X scales every modulation, mod Y every feedback; both are 1 at the centre.
    const float depth = S.modDepth * 2.0f * v.modX, fbScale = 2.0f * v.modY;
    float g[kOps], fbG[kOps];
    for (int i = 0; i < kOps; ++i) {
        g[i] = 1.0f;
        const float beta = S.mat[i][i] * kFbCycles * 2.0f * (float)kTwoPi * fbScale * peak[i];
        fbG[i] = beta > kMaxFeedback ? kMaxFeedback / beta : 1.0f;
    }
    if (S.guard) {
        // Alias guard. How far up every operator's spectrum reaches is estimated from the modulation it
        // receives; where that would pass what the oversampling can carry, the incoming index is scaled
        // back. Low and middle notes are untouched; high notes lose the partials that could only have aliased.
        float width[kOps] = {};     // an operator not reached yet (the far side of a loop) counts as nothing
        for (int k = 0; k < kOps; ++k) {
            const int i = S.order[k];
            const float wf = kWaveWidth[S.op[i].wave] * f[i];
            if (peak[i] <= 0.0f || wf <= 0.0f) { width[i] = 0.0f; continue; }
            // a carrier may reach into the band the decimator removes; a modulator must stay under Nyquist
            const float limit = (float)(S.op[i].modulates ? 0.95 * sr_ : 1.4 * sr_);
            float beta = S.mat[i][i] * kFbCycles * 2.0f * (float)kTwoPi * fbScale * peak[i] * fbG[i];
            if (beta > 0.0f) {
                const float room = limit / wf;      // how many harmonics of this operator fit
                float cap = room <= 1.0f ? 0.0f : std::max(0.0f, 1.0f - 2.2f * std::pow(room, -0.72f));
                if (room > 200.0f) cap = std::max(cap, 0.95f + (kMaxFeedback - 0.95f) * clampT((room - 200.0f) / 600.0f, 0.0f, 1.0f));
                if (beta > cap) { fbG[i] *= cap / beta; beta = cap; }
            }
            const float own = wf * (1.0f + 3.0f * beta / std::sqrt(1.02f - std::min(beta, 1.0f)));
            // Side bands of a modulation of index I matter (down to about -60 dB) out to I + 3 * cbrt(I) + 1
            // times the modulator's width. With every index scaled by u * u * u the top of this operator's
            // spectrum is c + a * u^3 + b * u, which is solved for the largest u that fits.
            float a = 0.0f, b = 0.0f, c = own;
            for (int j = 0; j < kOps; ++j) {
                if (j == i || width[j] <= 0.0f) continue;
                const float index = kWaveWidth[S.op[i].wave] * S.mat[i][j] * kModCycles * (float)kTwoPi * depth * peak[j];
                if (index <= 1.0e-3f) continue;
                a += index * width[j];
                b += 3.0f * std::cbrt(index) * width[j];
                c += width[j];
            }
            float u = 1.0f;
            if (a + b + c > limit) {
                if (c >= limit) u = 0.0f;
                else
                    for (int it = 0; it < 6; ++it)      // Newton from above on a rising, convex curve: never overshoots
                        u = clampT(u - ((a * u * u + b) * u + c - limit) / (3.0f * a * u * u + b), 0.0f, 1.0f);
            }
            g[i] = u * u * u;
            width[i] = c + g[i] * a + u * b;
        }
    }
    for (int i = 0; i < kOps; ++i)
        for (int j = 0; j < kOps; ++j)
            ramp(v.con[i][j], v.conStep[i][j],
                 i == j ? S.mat[i][i] * kFbCycles * fbScale * fbG[i] : S.mat[i][j] * kModCycles * depth * g[i]);
}

void Sextet::renderVoice(Voice& v, float* osL, float* osR, int n) {
    const Snap& S = snap_;
    const WaveSet& W = waveSet();
    int pos = 0;
    while (pos < n && v.active) {
        if (v.ctlLeft == 0) { control(v, false); v.ctlLeft = kCtl; }
        const int m = std::min(n - pos, v.ctlLeft);

        // Which operators have anything to do in this stretch, in processing order.
        int act[kOps], nAct = 0;
        bool on[kOps];
        for (int k = 0; k < kOps; ++k) {
            const int i = S.order[k];
            on[i] = v.env[i].stage != parts::Env::Idle && (v.amp[i] != 0.0f || v.ampStep[i] != 0.0f);
            if (on[i]) { act[nAct++] = i; continue; }
            // a resting operator keeps its phase and its ramps moving, so it comes back in step
            v.y[i] = v.yPrev[i] = 0.0f;
            v.phase[i] += v.inc[i] * (uint32)m + (uint32)v.incStep[i] * (uint32)(m * (m - 1) / 2);
            v.inc[i] += (uint32)v.incStep[i] * (uint32)m;
            v.amp[i] += v.ampStep[i] * (float)m;
            v.gl[i] += v.glStep[i] * (float)m;
            v.gr[i] += v.grStep[i] * (float)m;
        }
        int src[kOps][kOps], nSrc[kOps];
        bool fb[kOps];
        for (int i = 0; i < kOps; ++i) {
            int a = -1;
            for (int k = 0; k < nAct; ++k) if (act[k] == i) a = k;
            if (a >= 0) { nSrc[a] = 0; fb[a] = false; }
            for (int j = 0; j < kOps; ++j) {
                const bool live = a >= 0 && on[j] && (v.con[i][j] != 0.0f || v.conStep[i][j] != 0.0f);
                if (!live) { v.con[i][j] += v.conStep[i][j] * (float)m; continue; }
                if (j == i) fb[a] = true;
                else src[a][nSrc[a]++] = j;
            }
        }

        float* oL = osL + pos;
        float* oR = osR + pos;
        for (int s = 0; s < m; ++s) {
            float l = 0.0f, r = 0.0f;
            for (int a = 0; a < nAct; ++a) {
                const int i = act[a];
                float* con = v.con[i];
                const float* cst = v.conStep[i];
                float mod = 0.0f;
                for (int c = 0; c < nSrc[a]; ++c) {
                    const int j = src[a][c];
                    con[j] += cst[j];
                    mod += con[j] * v.y[j];
                }
                if (fb[a]) { con[i] += cst[i]; mod += con[i] * (v.y[i] + v.yPrev[i]); }
                const float ev = v.env[i].next();
                v.amp[i] += v.ampStep[i];
                const uint32 p = v.phase[i] + (uint32)(int64)((double)mod * 4294967296.0);
                v.phase[i] += v.inc[i];
                v.inc[i] += (uint32)v.incStep[i];
                const float* t = W.t[S.op[i].wave];
                const uint32 idx = p >> (32 - kTabBits);
                const float fr = (float)(p & ((1u << (32 - kTabBits)) - 1u)) * (1.0f / (float)(1u << (32 - kTabBits)));
                const float y = (t[idx] + (t[idx + 1] - t[idx]) * fr) * ev * v.amp[i];
                v.yPrev[i] = v.y[i];
                v.y[i] = y;
                v.gl[i] += v.glStep[i];
                v.gr[i] += v.grStep[i];
                l += y * v.gl[i];
                r += y * v.gr[i];
            }
            const float kg = v.kill;
            if (v.killStep > 0.0f) v.kill = std::max(0.0f, v.kill - v.killStep);
            oL[s] += l * kg;
            oR[s] += r * kg;
        }
        v.ctlLeft -= m;
        pos += m;

        // The voice is over once nothing that reaches the output is still sounding.
        bool sounding = false;
        for (int i = 0; i < kOps; ++i) {
            const bool out = v.gl[i] != 0.0f || v.gr[i] != 0.0f || v.glStep[i] != 0.0f || v.grStep[i] != 0.0f;
            if (out && v.env[i].stage != parts::Env::Idle && (v.amp[i] != 0.0f || v.ampStep[i] != 0.0f)) sounding = true;
        }
        if (!sounding || v.kill <= 0.0f) v.active = false;
    }
}

void Sextet::render(float* L, float* R, int n, ProcessContext&) {
    float osL[2 * kQuantum], osR[2 * kQuantum];
    const int n2 = 2 * n;
    std::memset(osL, 0, sizeof(float) * (size_t)n2);
    std::memset(osR, 0, sizeof(float) * (size_t)n2);
    for (auto& v : voices_) if (v.active) renderVoice(v, osL, osR, n2);

    const float cl = postMode_ == 1 ? 1.0f : 0.0f, ch = postMode_ == 2 ? 1.0f : 0.0f;
    for (int s = 0; s < n; ++s) {
        float l = dec_[0].process(osL[2 * s], osL[2 * s + 1]);
        float r = dec_[1].process(osR[2 * s], osR[2 * s + 1]);
        postG_ += postGStep_;
        postK_ += postKStep_;
        if (postMode_ != 0) post_.process(postG_, postK_, cl, postMode_ == 3 ? postK_ : 0.0f, ch, l, r);
        dcL_ += dcCoef_ * (l - dcL_);       // the output carries no DC
        dcR_ += dcCoef_ * (r - dcR_);
        const float vol = volume_.next();
        L[s] += (l - dcL_) * vol;
        R[s] += (r - dcR_) * vol;
    }
    dec_[0].flush();
    dec_[1].flush();
    post_.flush();
    flushDenormal(dcL_);
    flushDenormal(dcR_);
    scope_.push(L, R, n);
}

// ------------------------------------------------------------------ presets

namespace {

const char* intern(const std::string& s) {
    static std::deque<std::string> pool;
    for (auto& p : pool) if (p == s) return p.c_str();
    pool.push_back(s);
    return pool.back().c_str();
}

// Builds one preset: every operator is silent until op() gives it a job.
struct Patch {
    PresetDef d;
    Patch(const char* name, int algorithm) {
        d.name = name;
        set("algo", (float)algorithm);
        for (int i = 1; i <= 6; ++i) o(i, "level", 0.0f);
    }
    Patch& set(const char* id, float v) { d.values.push_back({id, v}); return *this; }
    Patch& o(int n, const char* what, float v) { d.values.push_back({intern(opId(n - 1, what)), v}); return *this; }
    // operator n: frequency ratio, level, attack / decay (ms), sustain, release (ms)
    Patch& op(int n, float ratio, float level, float a, float dc, float s, float r) {
        const float coarse = ratio < 1.0f ? 0.0f : std::min(32.0f, std::floor(ratio));
        const float base = coarse == 0.0f ? 0.5f : coarse;
        o(n, "coarse", coarse).o(n, "fine", clampT(ratio / base - 1.0f, 0.0f, 1.0f));
        return o(n, "level", level).o(n, "attack", a).o(n, "decay", dc).o(n, "sustain", s).o(n, "release", r);
    }
    Patch& fixed(int n, float hz) { return o(n, "mode", 1.0f).o(n, "hz", hz); }
    Patch& lfo(int k, float wave, float rate, float delay, float pitch, float amp) {
        const std::string n = "lfo" + std::to_string(k);
        return set(intern(n + "wave"), wave).set(intern(n + "rate"), rate).set(intern(n + "delay"), delay)
              .set(intern(n + "pitch"), pitch).set(intern(n + "amp"), amp);
    }
    operator PresetDef() const { return d; }
};

// Algorithms by number (see kAlgos): 1 stack of six, 2 two stacks of three, 3 three pairs,
// 4 one carrier five modulators, 5 all carriers, 6 stack of four and a pair, 7 two carriers with two
// modulators each, 8 one modulator five carriers, 9 a pair and four carriers, 10 branched stack,
// 11 three carriers on one stack, 12 a pair, a stack of three and a sine.
// A modulator's level times 12.6 is its modulation index in radians.
std::vector<PresetDef> buildDefs() {
    std::vector<PresetDef> p;
    p.push_back({"Default", {}});

    // ---- electric pianos
    p.push_back(Patch("Glass Tines", 3)
        .op(1, 1, 1.0f, 0, 2400, 0, 350).o(1, "vel", 0.6f).o(1, "rks", 0.25f)
        .op(2, 14, 0.055f, 0, 160, 0, 120).o(2, "vel", 1.0f).o(2, "kscale", -0.35f)
        .op(3, 1, 0.9f, 0, 2800, 0, 400).o(3, "detune", 4).o(3, "pan", -0.25f).o(3, "rks", 0.25f)
        .op(4, 1, 0.09f, 0, 1500, 0, 300).o(4, "vel", 0.9f)
        .op(5, 1, 0.6f, 0, 2600, 0, 400).o(5, "detune", -5).o(5, "pan", 0.25f).o(5, "rks", 0.25f)
        .op(6, 1, 0.045f, 0, 900, 0, 300).o(6, "vel", 0.8f).o(6, "fb", 0.15f));
    p.push_back(Patch("Velvet Keys", 3)
        .op(1, 1, 1.0f, 0, 3200, 0, 500).o(1, "vel", 0.5f).o(1, "lfo", 0.6f)
        .op(2, 1, 0.065f, 0, 2000, 0, 400).o(2, "vel", 0.9f)
        .op(3, 1, 0.55f, 0, 3000, 0, 500).o(3, "detune", 6).o(3, "pan", 0.3f).o(3, "lfo", 1)
        .op(4, 1, 0.035f, 0, 1500, 0, 400)
        .op(5, 1, 0.55f, 0, 3000, 0, 500).o(5, "detune", -6).o(5, "pan", -0.3f).o(5, "lfo", 1)
        .op(6, 2, 0.02f, 0, 800, 0, 300)
        .lfo(1, 0, 4.6f, 0, 0, 0.35f));
    p.push_back(Patch("Bark Piano", 7)
        .op(1, 1, 1.0f, 0, 1800, 0, 250).o(1, "vel", 0.6f)
        .op(2, 1, 0.13f, 0, 700, 0, 200).o(2, "vel", 1)
        .op(3, 7, 0.05f, 0, 90, 0, 80).o(3, "vel", 1)
        .op(4, 1, 0.8f, 0, 2000, 0, 250).o(4, "detune", 5)
        .op(5, 1, 0.1f, 0, 900, 0, 200).o(5, "vel", 1).o(5, "fb", 0.2f)
        .op(6, 3, 0.04f, 0, 200, 0, 100).o(6, "vel", 1));

    // ---- bells
    p.push_back(Patch("Chapel Bell", 3)
        .op(1, 1, 1.0f, 0, 6000, 0, 3500)
        .op(2, 3.5f, 0.2f, 0, 5000, 0, 3000).o(2, "vel", 0.7f)
        .op(3, 1, 0.8f, 0, 5500, 0, 3500).o(3, "detune", 7)
        .op(4, 3.5f, 0.15f, 0, 4000, 0, 3000).o(4, "detune", -6).o(4, "vel", 0.7f)
        .op(5, 2, 0.3f, 0, 900, 0, 900)
        .op(6, 7.1f, 0.16f, 0, 700, 0, 600));
    p.push_back(Patch("Bronze Gong", 7)
        .op(1, 1, 1.0f, 2, 8000, 0, 4000).o(1, "lfo", 0.5f)
        .op(2, 1.41f, 0.22f, 0, 6000, 0, 4000)
        .op(3, 2.37f, 0.1f, 0, 2500, 0, 2000)
        .op(4, 0.5f, 0.7f, 2, 9000, 0, 5000).o(4, "lfo", 0.5f)
        .op(5, 0.71f, 0.15f, 0, 7000, 0, 4000)
        .op(6, 3.3f, 0.05f, 0, 1500, 0, 1500)
        .lfo(2, 0, 3.1f, 0, 0, 0.3f));
    p.push_back(Patch("Crystal Glock", 3)
        .op(1, 1, 1.0f, 0, 1300, 0, 1000).o(1, "rks", 0.3f)
        .op(2, 4, 0.07f, 0, 350, 0, 300).o(2, "vel", 1)
        .op(3, 4, 0.3f, 0, 600, 0, 500)
        .op(4, 13, 0.04f, 0, 120, 0, 100).o(4, "vel", 1));

    // ---- basses
    p.push_back(Patch("Solid Bass", 3)
        .op(1, 1, 1.0f, 0, 900, 0.6f, 110)
        .op(2, 1, 0.17f, 0, 260, 0.25f, 100).o(2, "fb", 0.3f).o(2, "vel", 1)
        .op(3, 0.5f, 0.6f, 0, 1200, 0.7f, 110)
        .op(4, 1, 0.05f, 0, 400, 0.2f, 100));
    p.push_back(Patch("Rubber Bass", 3)
        .op(1, 1, 1.0f, 0, 700, 0.5f, 90)
        .op(2, 1, 0.3f, 0, 110, 0.04f, 80).o(2, "vel", 1)
        .op(3, 2, 0.2f, 0, 300, 0.1f, 90)
        .op(4, 1, 0.1f, 0, 150, 0, 80));
    p.push_back(Patch("Wire Bass", 7)
        .op(1, 1, 1.0f, 0, 1100, 0.35f, 100)
        .op(2, 3, 0.11f, 0, 140, 0, 80).o(2, "vel", 1)
        .op(3, 1, 0.14f, 0, 500, 0.2f, 100).o(3, "fb", 0.35f)
        .op(4, 0.5f, 0.7f, 0, 1300, 0.6f, 100)
        .op(5, 1, 0.05f, 0, 300, 0.1f, 100));
    p.push_back(Patch("Growl Bass", 2)
        .op(1, 1, 1.0f, 0, 600, 0.8f, 120)
        .op(2, 1, 0.2f, 0, 900, 0.6f, 120).o(2, "fb", 0.2f).o(2, "lfo", 0.6f)
        .op(3, 2, 0.18f, 0, 1500, 0.5f, 120).o(3, "lfo", 1)
        .op(4, 0.5f, 0.7f, 0, 600, 0.9f, 120)
        .op(5, 0.5f, 0.1f, 0, 600, 0.6f, 120)
        .lfo(1, 0, 5.5f, 0, 0, 0.5f));

    // ---- brass
    p.push_back(Patch("Brass Section", 3)
        .op(1, 1, 1.0f, 35, 500, 0.85f, 160).o(1, "vel", 0.5f)
        .op(2, 1, 0.17f, 70, 700, 0.75f, 200).o(2, "fb", 0.45f).o(2, "vel", 0.9f)
        .op(3, 1, 0.9f, 40, 500, 0.85f, 160).o(3, "detune", 7).o(3, "pan", -0.3f)
        .op(4, 1, 0.15f, 80, 700, 0.7f, 200).o(4, "fb", 0.4f).o(4, "vel", 0.9f)
        .op(5, 1, 0.7f, 45, 500, 0.85f, 160).o(5, "detune", -6).o(5, "pan", 0.3f)
        .op(6, 2, 0.08f, 60, 700, 0.6f, 200).o(6, "vel", 1)
        .lfo(1, 0, 5.0f, 500, 5, 0));
    p.push_back(Patch("Soft Horn", 3)
        .op(1, 1, 1.0f, 70, 400, 0.9f, 220)
        .op(2, 1, 0.085f, 110, 600, 0.65f, 250).o(2, "fb", 0.2f).o(2, "vel", 1)
        .lfo(1, 0, 5.2f, 450, 10, 0));

    // ---- organs
    p.push_back(Patch("Drawbar Organ", 5)
        .op(1, 0.5f, 0.7f, 3, 200, 1, 50).o(1, "vel", 0).o(1, "lfo", 0.5f)
        .op(2, 1, 1.0f, 3, 200, 1, 50).o(2, "vel", 0).o(2, "lfo", 0.5f)
        .op(3, 1.5f, 0.5f, 3, 200, 1, 50).o(3, "vel", 0).o(3, "lfo", 0.5f)
        .op(4, 2, 0.65f, 3, 200, 1, 50).o(4, "vel", 0).o(4, "lfo", 0.5f)
        .op(5, 3, 0.35f, 3, 200, 1, 50).o(5, "vel", 0).o(5, "lfo", 0.5f)
        .op(6, 4, 0.25f, 3, 200, 1, 50).o(6, "vel", 0).o(6, "lfo", 0.5f)
        .lfo(1, 0, 6.6f, 0, 4, 0.12f).set("volume", 0.6f));
    p.push_back(Patch("Click Organ", 5)
        .op(1, 1, 1.0f, 2, 200, 1, 45).o(1, "vel", 0)
        .op(2, 2, 0.6f, 2, 200, 1, 45).o(2, "vel", 0)
        .op(3, 3, 0.4f, 2, 200, 1, 45).o(3, "vel", 0)
        .op(4, 0.5f, 0.5f, 2, 200, 1, 45).o(4, "vel", 0)
        .op(5, 4, 0.5f, 0, 220, 0, 100).o(5, "vel", 0.3f)
        .op(6, 6, 0.35f, 0, 90, 0, 60).o(6, "vel", 0.3f).set("volume", 0.65f));
    p.push_back(Patch("Pipe Flutes", 5)
        .op(1, 1, 1.0f, 45, 300, 0.95f, 180).o(1, "vel", 0.3f)
        .op(2, 2, 0.45f, 60, 300, 0.95f, 180).o(2, "vel", 0.3f)
        .op(3, 4, 0.2f, 80, 300, 0.9f, 180).o(3, "vel", 0.3f)
        .op(4, 1, 0.5f, 55, 300, 0.95f, 180).o(4, "detune", 9).o(4, "pan", 0.4f).o(4, "vel", 0.3f)
        .op(5, 2, 0.2f, 70, 300, 0.95f, 180).o(5, "detune", -8).o(5, "pan", -0.4f).o(5, "vel", 0.3f)
        .op(6, 8, 0.06f, 30, 400, 0.3f, 150).o(6, "vel", 0.3f));

    // ---- mallets
    p.push_back(Patch("Rosewood Marimba", 3)
        .op(1, 1, 1.0f, 0, 750, 0, 600).o(1, "rks", 0.4f)
        .op(2, 4, 0.085f, 0, 80, 0, 80).o(2, "vel", 1)
        .op(3, 0.5f, 0.25f, 0, 500, 0, 400));
    p.push_back(Patch("Vibes Shimmer", 3)
        .op(1, 1, 1.0f, 0, 4000, 0, 2500).o(1, "lfo", 1)
        .op(2, 4, 0.045f, 0, 600, 0, 500).o(2, "vel", 1)
        .op(3, 1, 0.6f, 0, 3800, 0, 2500).o(3, "detune", 4).o(3, "pan", 0.3f).o(3, "lfo", 1)
        .op(4, 10, 0.03f, 0, 160, 0, 150)
        .op(5, 1, 0.4f, 0, 3800, 0, 2500).o(5, "detune", -4).o(5, "pan", -0.3f).o(5, "lfo", 1)
        .lfo(1, 0, 5.6f, 0, 0, 0.55f));
    p.push_back(Patch("Thumb Kalimba", 7)
        .op(1, 1, 1.0f, 0, 1000, 0, 700)
        .op(2, 5.2f, 0.06f, 0, 55, 0, 60).o(2, "vel", 1)
        .op(3, 3, 0.035f, 0, 300, 0, 250)
        .op(4, 2, 0.15f, 0, 400, 0, 350));
    p.push_back(Patch("Steel Pan", 7)
        .op(1, 1, 1.0f, 0, 1300, 0, 900)
        .op(2, 2, 0.11f, 0, 500, 0, 400).o(2, "vel", 1)
        .op(3, 4, 0.03f, 0, 200, 0, 200)
        .op(4, 2, 0.35f, 0, 500, 0, 400)
        .op(5, 1, 0.08f, 0, 300, 0, 300)
        .op(6, 6, 0.02f, 0, 100, 0, 100));
    p.push_back(Patch("Log Drum", 3)
        .op(1, 1, 1.0f, 0, 600, 0, 400)
        .op(2, 1, 0.22f, 0, 70, 0, 70).o(2, "vel", 1)
        .op(3, 0.5f, 0.5f, 0, 400, 0, 300)
        .op(4, 1.5f, 0.05f, 0, 100, 0, 100)
        .set("penvamt", 3).set("penvtime", 30));

    // ---- plucks
    p.push_back(Patch("Nylon Pluck", 7)
        .op(1, 1, 1.0f, 0, 1700, 0, 500).o(1, "rks", 0.5f)
        .op(2, 1, 0.13f, 0, 280, 0.08f, 300).o(2, "fb", 0.12f).o(2, "vel", 1)
        .op(3, 3, 0.035f, 0, 90, 0, 90).o(3, "vel", 1)
        .op(4, 2, 0.2f, 0, 900, 0, 400)
        .op(5, 2, 0.05f, 0, 200, 0, 200));
    p.push_back(Patch("Silk Harp", 3)
        .op(1, 1, 1.0f, 0, 2600, 0, 1400).o(1, "pan", -0.35f)
        .op(2, 2, 0.075f, 0, 450, 0, 400).o(2, "vel", 1)
        .op(3, 1, 0.85f, 0, 2600, 0, 1400).o(3, "detune", 5).o(3, "pan", 0.35f)
        .op(4, 2, 0.06f, 0, 600, 0, 500)
        .op(5, 4, 0.1f, 0, 700, 0, 600)
        .op(6, 1, 0.05f, 0, 200, 0, 200));
    p.push_back(Patch("Funk Clav", 3)
        .op(1, 1, 1.0f, 0, 1000, 0.15f, 70).o(1, "wave", 1)
        .op(2, 3, 0.19f, 0, 420, 0.25f, 70).o(2, "fb", 0.3f).o(2, "vel", 1)
        .op(3, 2, 0.5f, 0, 700, 0.1f, 70)
        .op(4, 5, 0.11f, 0, 220, 0.1f, 70).o(4, "vel", 1));

    // ---- pads
    p.push_back(Patch("Aurora Pad", 3)
        .op(1, 1, 1.0f, 700, 2000, 0.8f, 1800).o(1, "detune", -8).o(1, "pan", -0.5f)
        .op(2, 1, 0.07f, 1800, 3000, 0.6f, 1500).o(2, "lfo", 1)
        .op(3, 1, 1.0f, 800, 2000, 0.8f, 1800).o(3, "detune", 8).o(3, "pan", 0.5f)
        .op(4, 2, 0.055f, 2200, 3000, 0.7f, 1500).o(4, "lfo", 1)
        .op(5, 2, 0.3f, 1200, 2000, 0.8f, 1800)
        .op(6, 3, 0.04f, 2500, 3000, 0.7f, 1500).o(6, "lfo", 1)
        .lfo(1, 0, 0.25f, 0, 5, 0).lfo(2, 1, 0.13f, 0, 0, 0.6f).set("volume", 0.58f));
    p.push_back(Patch("Glass Choir", 7)
        .op(1, 1, 1.0f, 450, 1500, 0.85f, 1200).o(1, "detune", -5).o(1, "pan", -0.3f)
        .op(2, 3, 0.05f, 900, 2000, 0.7f, 1200)
        .op(3, 5, 0.025f, 300, 1200, 0.3f, 1000)
        .op(4, 1, 0.9f, 500, 1500, 0.85f, 1200).o(4, "detune", 5).o(4, "pan", 0.3f)
        .op(5, 3, 0.045f, 1100, 2000, 0.7f, 1200)
        .op(6, 1, 0.05f, 600, 2000, 0.7f, 1200).o(6, "fb", 0.3f)
        .lfo(1, 0, 4.8f, 600, 7, 0));
    p.push_back(Patch("Warm Strings", 5)
        .op(1, 1, 1.0f, 280, 800, 0.9f, 700).o(1, "fb", 0.5f).o(1, "detune", -9).o(1, "pan", -0.6f).o(1, "vel", 0.4f)
        .op(2, 1, 1.0f, 280, 800, 0.9f, 700).o(2, "fb", 0.5f).o(2, "detune", 9).o(2, "pan", 0.6f).o(2, "vel", 0.4f)
        .op(3, 1, 0.8f, 300, 800, 0.9f, 700).o(3, "fb", 0.45f).o(3, "detune", 2).o(3, "vel", 0.4f)
        .op(4, 2, 0.3f, 320, 800, 0.9f, 700).o(4, "fb", 0.35f).o(4, "detune", -4).o(4, "pan", 0.3f).o(4, "vel", 0.4f)
        .op(5, 2, 0.25f, 320, 800, 0.9f, 700).o(5, "fb", 0.3f).o(5, "detune", 5).o(5, "pan", -0.3f).o(5, "vel", 0.4f)
        .op(6, 0.5f, 0.35f, 300, 800, 0.9f, 700).o(6, "fb", 0.2f).o(6, "vel", 0.4f)
        .lfo(1, 0, 5.3f, 400, 8, 0).set("volume", 0.6f));

    // ---- leads
    p.push_back(Patch("Laser Lead", 2)
        .op(1, 1, 1.0f, 1, 300, 0.85f, 120)
        .op(2, 1, 0.2f, 1, 400, 0.8f, 120).o(2, "fb", 0.45f)
        .op(3, 2, 0.1f, 1, 600, 0.5f, 120)
        .set("penvamt", 12).set("penvtime", 45)
        .lfo(1, 0, 5.8f, 300, 9, 0));
    p.push_back(Patch("Hollow Lead", 3)
        .op(1, 1, 1.0f, 8, 300, 0.9f, 150)
        .op(2, 2, 0.13f, 20, 400, 0.8f, 150).o(2, "vel", 0.9f)
        .op(3, 1, 0.4f, 8, 300, 0.9f, 150).o(3, "detune", 6)
        .op(4, 2, 0.09f, 20, 400, 0.8f, 150)
        .lfo(1, 0, 5.5f, 350, 10, 0));
    p.push_back(Patch("Night Reed", 3)
        .op(1, 1, 1.0f, 25, 300, 0.9f, 140)
        .op(2, 1, 0.12f, 40, 500, 0.75f, 140).o(2, "fb", 0.5f).o(2, "vel", 1)
        .op(3, 2, 0.25f, 30, 300, 0.8f, 140)
        .op(4, 3, 0.06f, 50, 500, 0.6f, 140).o(4, "vel", 1)
        .lfo(1, 0, 5.0f, 400, 8, 0.1f).o(1, "lfo", 0.5f));

    // ---- effects
    p.push_back(Patch("Metal Rain", 4)
        .op(1, 1, 1.0f, 0, 900, 0, 700)
        .op(2, 1, 0.1f, 0, 300, 0, 300).fixed(2, 1373)
        .op(3, 1, 0.08f, 0, 200, 0, 200).fixed(3, 2741)
        .op(4, 3.14f, 0.07f, 0, 500, 0, 400)
        .op(5, 7.3f, 0.05f, 0, 150, 0, 150)
        .op(6, 1, 0.1f, 0, 700, 0, 500).fixed(6, 311)
        .set("penvamt", -7).set("penvtime", 300)
        .lfo(1, 5, 11, 0, 300, 0));
    p.push_back(Patch("Alien Signal", 1)
        .op(1, 1, 1.0f, 5, 500, 0.8f, 600)
        .op(2, 2, 0.15f, 5, 500, 0.8f, 600).o(2, "lfo", 1)
        .op(3, 3, 0.15f, 5, 500, 0.8f, 600).o(3, "lfo", 1)
        .op(4, 1, 0.2f, 5, 500, 0.9f, 600).o(4, "lfo", 1)
        .op(5, 5, 0.1f, 5, 500, 0.8f, 600)
        .op(6, 0.5f, 0.3f, 5, 500, 0.8f, 600).o(6, "fb", 0.6f)
        .lfo(1, 4, 7, 0, 700, 0).lfo(2, 5, 3, 0, 0, 0.8f));
    p.push_back(Patch("Deep Space", 12)
        .op(1, 0.5f, 1.0f, 1500, 2000, 1, 3000).o(1, "lfo", 0.6f)
        .op(2, 1, 0.1f, 500, 2000, 1, 3000).fixed(2, 0.31f)
        .op(3, 1, 0.8f, 2000, 2000, 1, 3000)
        .op(4, 1.5f, 0.08f, 3000, 2000, 1, 3000)
        .op(5, 1, 0.3f, 500, 2000, 1, 3000).fixed(5, 0.17f)
        .op(6, 3, 0.15f, 2500, 2000, 1, 3000).o(6, "detune", 12).o(6, "lfo", 1)
        .lfo(2, 0, 0.09f, 0, 0, 0.5f).set("volume", 0.75f));
    return p;
}

} // namespace

std::vector<Preset> Sextet::factoryPresets() const {
    static const std::vector<PresetDef> defs = buildDefs();
    return buildPresets(*this, defs);
}

} // namespace pulp
