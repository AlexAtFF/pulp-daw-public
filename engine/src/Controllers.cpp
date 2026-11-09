#include "pulp/Controllers.h"

namespace pulp {

// ------------------------------------------------------------------ Wobble (LFO)

Wobble::Wobble() {
    pShape = addChoice("shape", "Shape", {"Sine", "Triangle", "Square", "Saw up", "Saw down", "Random steps"}, 0);
    pSync = addBool("sync", "Tempo sync", true);
    pRate = addFloat("rate", "Speed (free)", 0.02f, 30.0f, 1.0f, "Hz", 3.0f);
    pSteps = addFloat("steps", "Length (synced)", 0.25f, 128.0f, 16.0f, "steps", 3.0f);
    pBase = addFloat("base", "Base", 0.0f, 1.0f, 0.5f, "%");
    pDepth = addFloat("depth", "Depth", -1.0f, 1.0f, 0.5f, "");
    pPhase = addFloat("phase", "Phase", 0.0f, 1.0f, 0.0f, "%");
}

float Wobble::wave(int shape, double phase) {
    const double ph = phase - std::floor(phase);
    switch (shape) {
        case 1: return (float)(ph < 0.25 ? 4.0 * ph : (ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0));      // starts at the middle, rising, like the sine
        case 2: return ph < 0.5 ? 1.0f : -1.0f;
        case 3: return (float)(2.0 * ph - 1.0);
        case 4: return (float)(1.0 - 2.0 * ph);
        case 5: {       // a new level every cycle, the same every time the song is played
            uint32 h = (uint32)(long long)std::floor(phase) * 2654435761u + 0x9E3779B9u;
            h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            return (float)((double)(h & 0xFFFFFF) / 8388607.5 - 1.0);
        }
        default: return (float)std::sin(kTwoPi * ph);
    }
}

void Wobble::process(ProcessContext& c) {
    // While the song plays the phase comes from the song position, so an export matches what was heard.
    const bool sync = pSync->getBool();
    double cycles;
    if (c.playing) {
        cycles = sync ? c.tickPos / ((double)pSteps->get() * kTicksPerStep) : c.tickPos * 60.0 / (c.tempo * kPPQ) * (double)pRate->get();
    } else {
        const double hz = sync ? c.tempo / 60.0 * 4.0 / (double)pSteps->get() : (double)pRate->get();
        free_ += hz * c.numSamples / sr_;
        cycles = free_;
    }
    const float v = pBase->get() + 0.5f * pDepth->get() * wave(pShape->getInt(), cycles + (double)pPhase->get());
    out_.store(clampT(v, 0.0f, 1.0f), std::memory_order_relaxed);
}

std::vector<Preset> Wobble::factoryPresets() const {
    return buildPresets(*this, {
        {"One bar sine", {{"shape", 0}, {"sync", 1}, {"steps", 16}, {"base", 0.5f}, {"depth", 0.6f}}},
        {"Quarter-note pump", {{"shape", 4}, {"sync", 1}, {"steps", 4}, {"base", 0.5f}, {"depth", 1.0f}}},
        {"Eighth-note gate", {{"shape", 2}, {"sync", 1}, {"steps", 2}, {"base", 0.5f}, {"depth", 1.0f}}},
        {"Slow drift", {{"shape", 1}, {"sync", 0}, {"rate", 0.1f}, {"base", 0.5f}, {"depth", 0.4f}}},
        {"Sample and hold", {{"shape", 5}, {"sync", 1}, {"steps", 2}, {"base", 0.5f}, {"depth", 0.9f}}},
    });
}

// ------------------------------------------------------------------ Follower (envelope follower)

Follower::Follower() {
    pBase = addFloat("base", "Base", 0.0f, 1.0f, 0.0f, "%");
    pAmount = addFloat("amount", "Amount", -2.0f, 2.0f, 1.0f, "");
    pAttack = addFloat("attack", "Attack", 0.1f, 200.0f, 2.0f, "ms", 3.0f);
    pRelease = addFloat("release", "Release", 5.0f, 2000.0f, 150.0f, "ms", 3.0f);
    pSource = addChoice("source", "Listens to", {"This track", "Sidechain input"}, 0);
    pMute = addBool("mute", "Mute the sound", false);
}

void Follower::process(ProcessContext& c) {
    const bool side = pSource->getInt() == 1 && c.sidechain[0] != nullptr;
    const float* l = side ? c.sidechain[0] : c.io[0];
    const float* r = side ? c.sidechain[1] : c.io[1];
    const float att = (float)(1.0 - std::exp(-1.0 / (pAttack->get() * 0.001 * sr_)));
    const float rel = (float)(1.0 - std::exp(-1.0 / (pRelease->get() * 0.001 * sr_)));
    float e = env_;
    for (int i = 0; i < c.numSamples; ++i) {
        const float x = std::max(std::fabs(l[i]), std::fabs(r[i]));
        e += (x > e ? att : rel) * (x - e);
    }
    flushDenormal(e);
    env_ = e;
    out_.store(clampT(pBase->get() + pAmount->get() * e, 0.0f, 1.0f), std::memory_order_relaxed);
    if (pMute->getBool()) {     // a trigger track that should be felt, not heard
        std::memset(c.io[0], 0, sizeof(float) * (size_t)c.numSamples);
        std::memset(c.io[1], 0, sizeof(float) * (size_t)c.numSamples);
    }
}

std::vector<Preset> Follower::factoryPresets() const {
    return buildPresets(*this, {
        {"Follow the level", {{"base", 0.0f}, {"amount", 1.0f}, {"attack", 2.0f}, {"release", 150.0f}}},
        {"Ducking (link a volume to this)", {{"base", 1.0f}, {"amount", -1.4f}, {"attack", 1.0f}, {"release", 180.0f}}},
        {"Ducking from the sidechain", {{"base", 1.0f}, {"amount", -1.4f}, {"attack", 1.0f}, {"release", 180.0f}, {"source", 1}}},
        {"Slow swell", {{"base", 0.0f}, {"amount", 1.5f}, {"attack", 80.0f}, {"release", 900.0f}}},
        {"Silent trigger", {{"base", 1.0f}, {"amount", -1.4f}, {"attack", 1.0f}, {"release", 180.0f}, {"mute", 1}}},
    });
}

// ------------------------------------------------------------------ Pad XY

PadXy::PadXy() {
    pX = addFloat("x", "X", 0.0f, 1.0f, 0.5f, "%");
    pY = addFloat("y", "Y", 0.0f, 1.0f, 0.5f, "%");
    pSmooth = addFloat("smooth", "Smoothing", 0.0f, 500.0f, 30.0f, "ms", 2.0f);
    out_[0].store(0.5f);
    out_[1].store(0.5f);
}

void PadXy::process(ProcessContext& c) {
    const float tx = pX->get(), ty = pY->get();
    const float ms = pSmooth->get();
    if (!primed_ || ms <= 0.01f) { cur_[0] = tx; cur_[1] = ty; primed_ = true; }
    else {
        const float k = (float)(1.0 - std::exp(-(double)c.numSamples / (ms * 0.001 * sr_)));
        cur_[0] += k * (tx - cur_[0]);
        cur_[1] += k * (ty - cur_[1]);
    }
    out_[0].store(cur_[0], std::memory_order_relaxed);
    out_[1].store(cur_[1], std::memory_order_relaxed);
}

std::vector<Preset> PadXy::factoryPresets() const {
    return buildPresets(*this, {
        {"Centre", {{"x", 0.5f}, {"y", 0.5f}, {"smooth", 30.0f}}},
        {"Slow glide", {{"x", 0.5f}, {"y", 0.5f}, {"smooth", 300.0f}}},
        {"Instant", {{"x", 0.5f}, {"y", 0.5f}, {"smooth", 0.0f}}},
    });
}

// ------------------------------------------------------------------ Formula

FormulaCtl::FormulaCtl() {
    pA = addFloat("a", "A", 0.0f, 1.0f, 0.5f, "%");
    pB = addFloat("b", "B", 0.0f, 1.0f, 0.5f, "%");
    pC = addFloat("c", "C", 0.0f, 1.0f, 0.5f, "%");
    setFormula("a + b * (sine(beat / 4) - 0.5)");
}

bool FormulaCtl::setFormula(const std::string& text, std::string* error) {
    auto f = Formula::compile(text, variables(), error);
    if (!f) return false;
    text_ = text;
    compiled_.set(f);
    return true;
}

void FormulaCtl::process(ProcessContext& c) {
    const Formula* f = compiled_.rtAcquire();
    if (!f) return;
    double seconds, beats;
    if (c.playing) { beats = c.tickPos / kPPQ; seconds = beats * 60.0 / c.tempo; free_ = seconds; }
    else { free_ += c.numSamples / sr_; seconds = free_; beats = seconds * c.tempo / 60.0; }
    const double vars[6] = {pA->get(), pB->get(), pC->get(), seconds, beats, c.tempo};
    out_.store((float)clampT(f->eval(vars), 0.0, 1.0), std::memory_order_relaxed);
}

Json FormulaCtl::saveExtra() const {
    Json j = Json::object();
    j.set("formula", text_);
    return j;
}

void FormulaCtl::loadExtra(const Json& j, const std::string&) {
    const std::string t = j["formula"].str("");
    if (!t.empty()) setFormula(t);
}

std::vector<Preset> FormulaCtl::factoryPresets() const {
    // presets carry their formula in the extra state
    struct Def { const char* name; const char* formula; float a, b, c; };
    const Def defs[] = {
        {"Sine around A, depth B", "a + b * (sine(beat / 4) - 0.5)", 0.5f, 0.5f, 0.5f},
        {"Rising every bar", "frac(beat / 4)", 0.5f, 0.5f, 0.5f},
        {"Sixteenth-note chop", "if(frac(beat * 4) < a, 1, 0)", 0.5f, 0.5f, 0.5f},
        {"Two sines beating", "0.5 + 0.25 * sin(2 * pi * beat / 4) + 0.25 * sin(2 * pi * beat / 3)", 0.5f, 0.5f, 0.5f},
        {"Fade in over C x 16 bars", "clamp(beat / (64 * max(c, 0.01)), 0, 1)", 0.5f, 0.5f, 0.25f},
    };
    std::vector<Preset> out;
    for (auto& d : defs) {
        Json st = Json::object();
        st.set("type", typeId());
        Json ps = Json::object();
        ps.set("a", d.a); ps.set("b", d.b); ps.set("c", d.c);
        st.set("params", ps);
        Json ex = Json::object();
        ex.set("formula", d.formula);
        st.set("extra", ex);
        out.push_back({d.name, st});
    }
    return out;
}

} // namespace pulp
