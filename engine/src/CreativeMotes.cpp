// Motes: a granular sampler. Each held note is a stream of short grains read from one sample: where in the
// sample, how long, how often, at what pitch, in which direction and where in the stereo field are all set by
// the controls, each with its own random spread. The random numbers of a note come from its key and the
// Variation control and nothing else, so a note always sounds the same wherever it is in the song.
#include "pulp/Creative.h"

namespace pulp {

namespace {
constexpr int kWin = 1024;
struct Windows {
    float v[Motes::NumWindows][kWin + 2];
    Windows() {
        for (int s = 0; s < Motes::NumWindows; ++s)
            for (int i = 0; i <= kWin + 1; ++i) v[s][i] = i >= kWin ? 0.0f : Motes::windowAt(s, (double)i / kWin);
    }
};
const Windows& windows() { static const Windows w; return w; }
}

float Motes::windowAt(int shape, double p) {
    if (p <= 0.0 || p >= 1.0) return 0.0f;
    switch (shape) {
        case Triangle: return (float)(1.0 - std::fabs(2.0 * p - 1.0));
        case Bell: {
            const double edge = std::exp(-0.5 * (0.5 / 0.17) * (0.5 / 0.17));
            const double e = std::exp(-0.5 * ((p - 0.5) / 0.17) * ((p - 0.5) / 0.17));
            return (float)((e - edge) / (1.0 - edge));
        }
        case FlatTop:
            if (p < 0.2) return (float)(0.5 - 0.5 * std::cos(kPi * p / 0.2));
            if (p > 0.8) return (float)(0.5 - 0.5 * std::cos(kPi * (1.0 - p) / 0.2));
            return 1.0f;
        case Pluck: return (float)(std::min(1.0, p / 0.03) * std::exp(-4.5 * p) * std::min(1.0, (1.0 - p) / 0.1));
        case Swell: return windowAt(Pluck, 1.0 - p);
        default: return (float)(0.5 - 0.5 * std::cos(kTwoPi * p));
    }
}

Motes::Motes() {
    pPosition = addFloat("position", "Position", 0, 1, 0.25f, "%");
    pSpread = addFloat("spread", "Position spread", 0, 1, 0.02f, "%", 2.0f);
    pScan = addFloat("scan", "Scan speed", -2, 2, 0);
    pLoop = addBool("loop", "Loop the sample", true);
    pSize = addFloat("size", "Grain size", 5, 500, 80, "ms", 2.0f);
    pDensity = addFloat("density", "Density", 1, 200, 25, "/s", 2.0f);
    pJitter = addFloat("jitter", "Timing jitter", 0, 1, 0.3f, "%");
    pShape = addChoice("shape", "Grain shape", {"Hann", "Triangle", "Bell", "Flat top", "Pluck", "Swell"}, 0);
    pReverse = addFloat("reverse", "Reverse chance", 0, 1, 0, "%");
    pPanSpread = addFloat("panspread", "Pan spread", 0, 1, 0.3f, "%");
    pRoot = add({"root", "Root key", 0, 127, (float)kRootKey, 1, 128, "", {}});
    pKeytrack = addBool("keytrack", "Key tracking", true);
    pCoarse = addFloat("coarse", "Pitch", -24, 24, 0, "st");
    pFine = addFloat("fine", "Fine tune", -100, 100, 0, "ct");
    pPitchSpread = addFloat("pitchspread", "Pitch spread", 0, 1200, 0, "ct", 3.0f);
    pAttack = addFloat("attack", "Attack", 0, 5000, 20, "ms", 3.0f);
    pDecay = addFloat("decay", "Decay", 1, 5000, 300, "ms", 3.0f);
    pSustain = addFloat("sustain", "Sustain", 0, 1, 1, "%");
    pRelease = addFloat("release", "Release", 1, 5000, 250, "ms", 3.0f);
    pCutoff = addFloat("cutoff", "Cutoff", 20, 20000, 20000, "Hz", 3.0f);
    pReso = addFloat("reso", "Resonance", 0, 1, 0, "%");
    pLevel = addFloat("level", "Level", -36, 12, 0, "dB");
    pPoly = add({"poly", "Voices", 1, (float)kMaxPoly, 8, 1, kMaxPoly, "", {}});
    pSeed = add({"seed", "Variation", 0, 99, 0, 1, 100, "", {}});
}

std::vector<ParamGroup> Motes::paramGroups() const {
    return {{"Where", {"position", "spread", "scan", "loop"}},
            {"Grains", {"size", "density", "jitter", "shape", "reverse", "panspread"}},
            {"Pitch", {"root", "keytrack", "coarse", "fine", "pitchspread"}},
            {"Envelope", {"attack", "decay", "sustain", "release"}},
            {"Filter and output", {"cutoff", "reso", "level", "poly", "seed"}}};
}

void Motes::prepare(double sampleRate) {
    sr_ = sampleRate;
    (void)windows();
    reset();
}

void Motes::reset() {
    for (auto& v : voices_) { v.active = false; v.grains = 0; for (auto& g : v.g) g.on = false; }
    ageCounter_ = 0;
    primed_ = false;
    voicesActive_.store(0, std::memory_order_relaxed);
    grainsActive_.store(0, std::memory_order_relaxed);
}

void Motes::setSample(SamplePtr s) {
    auto src = std::make_shared<Source>();
    src->sample = std::move(s);
    source_.set(src);
}
SamplePtr Motes::sample() const {
    auto s = source_.get();
    return s ? s->sample : nullptr;
}
std::string Motes::samplePath() const {
    auto s = sample();
    return s ? s->path : std::string();
}

Json Motes::saveExtra() const {
    Json j = Json::object();
    j.set("sample", samplePath());
    return j;
}
// A state with nothing beyond the controls (a preset) leaves the sample alone.
void Motes::loadExtra(const Json& j, const std::string& projectDir) {
    if (j.isNull() || !j.has("sample")) return;
    const std::string path = j["sample"].str("");
    if (path.empty()) { setSample(nullptr); return; }
    setSample(SampleCache::global().load(path, projectDir));
}

void Motes::beginBlock(ProcessContext& ctx) {
    cur_ = source_.rtAcquire();
    if (cur_ != last_) {                    // another sample: the grains of the old one cannot go on
        for (auto& v : voices_) { v.active = false; v.grains = 0; for (auto& g : v.g) g.on = false; }
        last_ = cur_;
    }
    bend_ = ctx.pitchBendSemis;
    const float a = pAttack->get(), d = pDecay->get(), r = pRelease->get();
    attackRate_ = a <= 0.01f ? 1.0f : (float)(1000.0 / (a * sr_));
    decayCoef_ = (float)std::exp(-1.0 / (0.001 * d * sr_ / 4.6));
    releaseCoef_ = (float)std::exp(-1.0 / (0.001 * r * sr_ / 4.6));
    sustain_ = pSustain->get();
    const float lv = dbToGain(pLevel->get());
    if (!primed_) { level_.snap(lv); primed_ = true; }
    level_.target(lv, ctx.numSamples);
    int n = 0, g = 0;
    for (auto& v : voices_) if (v.active) { ++n; g += v.grains; }
    voicesActive_.store(n, std::memory_order_relaxed);
    grainsActive_.store(g, std::memory_order_relaxed);
}

void Motes::startVoice(const NoteEvent& e) {
    if (!cur_ || !cur_->sample || cur_->sample->frames() < 4) return;
    const float killStep = (float)(1.0 / (0.003 * sr_));
    // the voice limit: the oldest notes make way, each with a short fade
    const int poly = clampT(pPoly->getInt(), 1, (int)kMaxPoly);
    for (;;) {
        int count = 0;
        Voice* oldest = nullptr;
        for (auto& c : voices_)
            if (c.active && c.killStep == 0.0f) { ++count; if (!oldest || c.age < oldest->age) oldest = &c; }
        if (count < poly || !oldest) break;
        oldest->killStep = killStep;
    }
    Voice* v = nullptr;
    for (auto& c : voices_) if (!c.active) { v = &c; break; }
    if (!v) {                               // every slot still fading: take the one nearest its end
        v = &voices_[0];
        for (auto& c : voices_) if (c.kill < v->kill) v = &c;
    }
    v->active = true;
    v->id = e.id;
    v->key = e.key;
    v->group = e.group;
    v->age = ++ageCounter_;
    const float vel = e.velocity / kDefaultVelocity;
    v->gain = vel * vel;
    v->panL = e.pan > 0.0f ? 1.0f - e.pan : 1.0f;
    v->panR = e.pan < 0.0f ? 1.0f + e.pan : 1.0f;
    const bool track = pKeytrack->getBool();
    v->keySemis = track ? (float)(e.key - pRoot->getInt()) : 0.0f;
    v->fineCents = e.fineCents;
    v->bend = v->bendTarget = v->bendStep = 0.0f;
    if (track && e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) {
        v->bend = e.glideFromKey - (float)e.key;
        v->bendStep = -v->bend / e.slideSamples;
    }
    v->stage = attackRate_ >= 1.0f ? 1 : 0;
    v->env = attackRate_ >= 1.0f ? 1.0f : 0.0f;
    v->relScale = 0.25f + 1.5f * e.release;
    v->kill = 1.0f;
    v->killStep = 0.0f;
    v->modX = e.modX;
    v->modY = e.modY;
    v->scan = 0.0;
    v->next = 0.0;
    v->ended = false;
    v->rng = dsp::Noise(crt::hash32((uint32)e.key, (uint32)pSeed->getInt(), 0x6D6F7465u));
    v->filtL.reset(); v->filtR.reset();
    v->cutoff = -1.0f;
    v->grains = 0;
    for (auto& g : v->g) g.on = false;
}

void Motes::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * sr_));
    switch (e.type) {
        case NoteEvent::On: startVoice(e); break;
        case NoteEvent::Off:
            for (auto& v : voices_) if (v.active && v.id == e.id && v.stage < 3) v.stage = 3;
            break;
        case NoteEvent::AllOff:
            for (auto& v : voices_) if (v.active && v.stage < 3) v.stage = 3;
            break;
        case NoteEvent::Kill:
            for (auto& v : voices_) if (v.active && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::KillNote:
            for (auto& v : voices_) if (v.active && v.id == e.id && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::SlideTo:
            if (pKeytrack->getBool())
                for (auto& v : voices_)
                    if (v.active && v.stage != 3 && v.group == e.group) {
                        v.bendTarget = (float)(e.key - v.key);
                        v.bendStep = (v.bendTarget - v.bend) / std::max(1.0f, e.slideSamples);
                    }
            break;
    }
}

// Starts one grain `offset` samples into the stretch being rendered. Every random number is drawn whether it
// is used or not, so one control never changes what the others get.
void Motes::spawn(Voice& v, int offset, double ratio) {
    const SampleData& s = *cur_->sample;
    const double frames = (double)s.frames();
    const float rSpread = v.rng.next(), rRev = v.rng.uni(), rPitch = v.rng.next(), rPan = v.rng.next();
    const double srRatio = s.sampleRate / sr_;
    const double sizeSamples = std::max(16.0, (double)pSize->get() * 0.001 * sr_);
    // A grain is loudest at its middle, half its length from now: it is centred on the place the position will have
    // reached by then, so the loudness of the stream follows the sample in time whatever the key.
    const double scanned = v.scan + ((double)offset + 0.5 * sizeSamples) * (double)pScan->get() * srRatio;
    const double plain = (double)pPosition->get() * (frames - 1.0) + scanned;
    double centre = plain + 0.5 * (double)pSpread->get() * frames * (double)rSpread;
    const bool loop = pLoop->getBool();
    if (loop) { centre = std::fmod(centre, frames); if (centre < 0.0) centre += frames; }
    else if (plain < 0.0 || plain >= frames) { v.ended = true; return; }      // the position has run off the sample: the note ends
    else if (centre < 0.0 || centre >= frames) return;                         // only the spread threw this grain off the end
    Grain* g = nullptr;
    for (auto& c : v.g) if (!c.on) { g = &c; break; }
    if (!g) return;                         // more grains at once than there is room for: this one is left out
    const double overlap = std::max(1.0, (double)pDensity->get() * (double)pSize->get() * 0.001);
    // Grains that overlap add up like noise, so each is turned down by the square root of how many sound at once;
    // 0.355 (-9 dB) puts a single note of a full-scale sample at about -9 dBFS.
    const float amp = (float)(0.3548 / std::sqrt(overlap));
    const double detune = std::exp2((double)pPitchSpread->get() * (double)rPitch / 1200.0);
    g->on = true;
    g->shape = clampT(pShape->getInt(), 0, (int)NumWindows - 1);
    g->ratio = rRev < pReverse->get() ? -detune : detune;
    g->pos = centre - 0.5 * sizeSamples * ratio * g->ratio;     // the middle of the grain is at the position
    g->phase = -(float)offset / (float)sizeSamples;             // it begins `offset` samples from now
    g->phaseInc = (float)(1.0 / sizeSamples);
    const double angle = ((double)pPanSpread->get() * (double)rPan + 1.0) * kPi * 0.25;
    g->gl = amp * (float)(std::cos(angle) * 1.41421356);
    g->gr = amp * (float)(std::sin(angle) * 1.41421356);
}

void Motes::renderVoice(Voice& v, float* L, float* R, int n) {
    const SampleData& s = *cur_->sample;
    const float* sl = s.left();
    const float* sr = s.right();
    const int64 frames = s.frames();
    const double dframes = (double)frames;
    const double srRatio = s.sampleRate / sr_;
    const double semis = (double)v.keySemis + (double)v.bend + (double)bend_ + ((double)v.fineCents + (double)pFine->get()) * 0.01 + (double)pCoarse->get();
    const double ratio = std::exp2(semis / 12.0) * srRatio;
    if (v.bendStep != 0.0f) {
        v.bend += v.bendStep * (float)n;
        if ((v.bendStep > 0.0f) == (v.bend >= v.bendTarget)) { v.bend = v.bendTarget; v.bendStep = 0.0f; }
    }
    const bool loop = pLoop->getBool();

    // grains that begin in this stretch
    const double interval = sr_ / std::max(0.5, (double)pDensity->get());
    const double jitter = pJitter->get();
    double t = v.next;
    while (t < (double)n) {
        if (!v.ended) spawn(v, (int)t, ratio);
        t += std::max(1.0, interval * (1.0 + jitter * (double)v.rng.next()));
    }
    v.next = t - (double)n;
    v.scan += (double)n * (double)pScan->get() * srRatio;
    if (loop && std::fabs(v.scan) >= dframes) v.scan = std::fmod(v.scan, dframes);
    if (v.ended && v.stage < 3) v.stage = 3;

    float tl[kQuantum], tr[kQuantum];
    std::memset(tl, 0, sizeof(float) * (size_t)n);
    std::memset(tr, 0, sizeof(float) * (size_t)n);
    const Windows& win = windows();
    auto at = [&](const float* d, int64 i) -> float {
        if (i >= 0 && i < frames) return d[i];
        if (!loop) return 0.0f;
        i %= frames;
        return d[i < 0 ? i + frames : i];
    };
    int alive = 0;
    for (auto& g : v.g) {
        if (!g.on) continue;
        const double inc = g.ratio * ratio;
        const float* w = win.v[g.shape];
        for (int i = 0; i < n; ++i) {
            if (g.phase < 0.0f) { g.phase += g.phaseInc; if (g.phase > 0.0f) g.phase = 0.0f; continue; }   // not begun yet
            const float wp = g.phase * (float)kWin;
            const int wi = (int)wp;
            const float gain = w[wi] + (w[wi + 1] - w[wi]) * (wp - (float)wi);
            const double fl = std::floor(g.pos);
            const int64 idx = (int64)fl;
            const float f = (float)(g.pos - fl);
            float y0, y1, y2, y3, z0, z1, z2, z3;
            if (idx >= 1 && idx + 2 < frames) {
                y0 = sl[idx - 1]; y1 = sl[idx]; y2 = sl[idx + 1]; y3 = sl[idx + 2];
                z0 = sr[idx - 1]; z1 = sr[idx]; z2 = sr[idx + 1]; z3 = sr[idx + 2];
            } else {
                y0 = at(sl, idx - 1); y1 = at(sl, idx); y2 = at(sl, idx + 1); y3 = at(sl, idx + 2);
                z0 = at(sr, idx - 1); z1 = at(sr, idx); z2 = at(sr, idx + 1); z3 = at(sr, idx + 2);
            }
            const float xl = ((0.5f * (y3 - y0) + 1.5f * (y1 - y2)) * f + (y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3)) * f * f + 0.5f * (y2 - y0) * f + y1;
            const float xr = ((0.5f * (z3 - z0) + 1.5f * (z1 - z2)) * f + (z0 - 2.5f * z1 + 2.0f * z2 - 0.5f * z3)) * f * f + 0.5f * (z2 - z0) * f + z1;
            tl[i] += xl * gain * g.gl;
            tr[i] += xr * gain * g.gr;
            g.pos += inc;
            if (loop) {
                if (g.pos >= dframes) g.pos -= dframes * std::floor(g.pos / dframes);
                else if (g.pos < 0.0) g.pos -= dframes * std::floor(g.pos / dframes);
            }
            g.phase += g.phaseInc;
            if (g.phase >= 1.0f) { g.on = false; break; }
        }
        if (g.on) ++alive;
    }
    v.grains = alive;

    // the voice's filter: mod X moves the cutoff up to five octaves either way, mod Y adds resonance
    const float target = clampT(pCutoff->get() * std::pow(2.0f, (v.modX - 0.5f) * 10.0f), 20.0f, 20000.0f);
    v.cutoff = v.cutoff < 0.0f ? target : v.cutoff + (target - v.cutoff) * std::min(1.0f, (float)n / 96.0f);
    const bool filterOn = v.cutoff < 19990.0f;
    if (filterOn) {
        const float q = 0.707f + clampT(pReso->get() + (v.modY - 0.5f) * 2.0f, 0.0f, 1.0f) * 11.0f;
        v.filtL.set(v.cutoff, q, (float)sr_);
        v.filtR.g = v.filtL.g; v.filtR.k = v.filtL.k; v.filtR.a1 = v.filtL.a1; v.filtR.a2 = v.filtL.a2; v.filtR.a3 = v.filtL.a3;
    }
    const float relCoef = v.relScale == 1.0f ? releaseCoef_ : std::pow(releaseCoef_, 1.0f / v.relScale);
    for (int i = 0; i < n; ++i) {
        float xl = tl[i], xr = tr[i];
        if (filterOn) { xl = v.filtL.lowpass(xl); xr = v.filtR.lowpass(xr); }
        switch (v.stage) {
            case 0: v.env += attackRate_; if (v.env >= 1.0f) { v.env = 1.0f; v.stage = 1; } break;
            case 1: v.env = sustain_ + (v.env - sustain_) * decayCoef_; break;
            case 3: v.env *= relCoef; if (v.env < 1.0e-4f) v.active = false; break;
            default: break;
        }
        const float g = v.env * v.kill * v.gain;
        if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) v.active = false; }
        L[i] += xl * g * v.panL;
        R[i] += xr * g * v.panR;
        if (!v.active) break;
    }
    if (filterOn) { flushDenormal(v.filtL.ic1); flushDenormal(v.filtL.ic2); flushDenormal(v.filtR.ic1); flushDenormal(v.filtR.ic2); }
    if (!v.active) { v.grains = 0; for (auto& g : v.g) g.on = false; }
}

void Motes::render(float* L, float* R, int n, ProcessContext&) {
    if (cur_ && cur_->sample && cur_->sample->frames() >= 4)
        for (auto& v : voices_)
            if (v.active) renderVoice(v, L, R, n);
    for (int i = 0; i < n; ++i) { const float g = level_.next(); L[i] *= g; R[i] *= g; }
}

std::vector<Preset> Motes::factoryPresets() const {
    return buildPresets(*this, {
        {"Frozen pad", {{"position", 0.3f}, {"spread", 0.01f}, {"size", 180}, {"density", 30}, {"jitter", 0.5f}, {"panspread", 0.5f}, {"attack", 300}, {"release", 800}, {"level", -1.2f}}},
        {"Slow scan", {{"position", 0.05f}, {"spread", 0.01f}, {"scan", 0.15f}, {"size", 150}, {"density", 25}, {"jitter", 0.4f}, {"panspread", 0.4f}, {"attack", 200}, {"release", 600}, {"level", 0}}},
        {"Stretched to double length, original pitch", {{"position", 0}, {"spread", 0}, {"scan", 0.5f}, {"keytrack", 0}, {"size", 60}, {"density", 60}, {"jitter", 0.1f}, {"panspread", 0}, {"attack", 5}, {"release", 120}, {"level", 1.8f}}},
        {"Stretched to double length, keyed", {{"position", 0}, {"spread", 0}, {"scan", 0.5f}, {"size", 60}, {"density", 60}, {"jitter", 0.1f}, {"panspread", 0.1f}, {"attack", 5}, {"release", 120}, {"level", 1.6f}}},
        {"Real time at any key", {{"position", 0}, {"spread", 0}, {"scan", 1}, {"loop", 0}, {"size", 45}, {"density", 80}, {"jitter", 0.05f}, {"panspread", 0}, {"attack", 2}, {"release", 100}, {"level", 0}}},
        {"Cloud", {{"position", 0.4f}, {"spread", 0.35f}, {"size", 120}, {"density", 60}, {"jitter", 1}, {"panspread", 1}, {"pitchspread", 15}, {"attack", 400}, {"release", 1500}, {"level", -2.2f}}},
        {"Glint an octave up", {{"position", 0.3f}, {"spread", 0.1f}, {"coarse", 12}, {"size", 90}, {"density", 45}, {"jitter", 0.8f}, {"panspread", 0.8f}, {"pitchspread", 8}, {"attack", 500}, {"release", 1500}, {"cutoff", 9000}, {"level", -1.7f}}},
        {"Stutter", {{"position", 0.1f}, {"spread", 0}, {"size", 40}, {"density", 12}, {"jitter", 0}, {"shape", FlatTop}, {"panspread", 0}, {"attack", 0}, {"release", 60}, {"level", 0}}},
        {"Reverse cloud", {{"position", 0.5f}, {"spread", 0.25f}, {"reverse", 1}, {"size", 220}, {"density", 30}, {"jitter", 0.8f}, {"shape", Swell}, {"panspread", 0.9f}, {"attack", 300}, {"release", 1200}, {"level", 1.4f}}},
        {"Backwards scan", {{"position", 0.9f}, {"spread", 0.01f}, {"scan", -0.5f}, {"size", 100}, {"density", 40}, {"jitter", 0.3f}, {"panspread", 0.3f}, {"attack", 30}, {"release", 300}, {"level", -0.6f}}},
        {"Sparks", {{"position", 0.5f}, {"spread", 0.5f}, {"coarse", 12}, {"size", 12}, {"density", 90}, {"jitter", 1}, {"shape", Pluck}, {"panspread", 1}, {"pitchspread", 700}, {"attack", 10}, {"release", 400}, {"level", -3.5f}}},
        {"Low drone", {{"position", 0.3f}, {"spread", 0.05f}, {"coarse", -12}, {"size", 400}, {"density", 12}, {"jitter", 0.6f}, {"panspread", 0.5f}, {"attack", 800}, {"release", 2000}, {"cutoff", 2500}, {"level", -0.8f}}},
        {"Two octaves down", {{"position", 0.2f}, {"spread", 0.02f}, {"scan", 0.05f}, {"coarse", -24}, {"size", 500}, {"density", 8}, {"jitter", 0.5f}, {"panspread", 0.6f}, {"attack", 600}, {"release", 2500}, {"level", 0.6f}}},
        {"Plucked grains", {{"position", 0.15f}, {"spread", 0.03f}, {"size", 60}, {"density", 20}, {"jitter", 0.2f}, {"shape", Pluck}, {"panspread", 0.4f}, {"attack", 0}, {"decay", 400}, {"sustain", 0}, {"release", 300}, {"level", 1.2f}}},
        {"Rain", {{"position", 0.5f}, {"spread", 1}, {"size", 30}, {"density", 6}, {"jitter", 1}, {"shape", Pluck}, {"panspread", 1}, {"pitchspread", 400}, {"attack", 0}, {"release", 500}, {"level", -3}}},
        {"A fifth above, glassy", {{"position", 0.35f}, {"spread", 0.04f}, {"coarse", 7}, {"size", 200}, {"density", 20}, {"jitter", 0.5f}, {"shape", Bell}, {"panspread", 0.6f}, {"pitchspread", 5}, {"attack", 250}, {"release", 1200}, {"cutoff", 6000}, {"reso", 0.3f}, {"level", -2.6f}}},
        {"Smear", {{"position", 0.1f}, {"spread", 0.08f}, {"scan", 0.05f}, {"size", 300}, {"density", 40}, {"jitter", 1}, {"panspread", 0.7f}, {"pitchspread", 4}, {"attack", 400}, {"release", 1500}, {"level", -3.2f}}},
        {"Machine", {{"position", 0.25f}, {"spread", 0}, {"size", 15}, {"density", 50}, {"jitter", 0}, {"shape", Triangle}, {"panspread", 0}, {"attack", 2}, {"release", 80}, {"level", 0.5f}}},
        {"Tremble", {{"position", 0.3f}, {"spread", 0.02f}, {"size", 90}, {"density", 9}, {"jitter", 0.1f}, {"shape", Bell}, {"panspread", 0.9f}, {"attack", 20}, {"release", 400}, {"level", -3}}},
        {"Wide scan, looping", {{"position", 0}, {"spread", 0.03f}, {"scan", 1}, {"size", 100}, {"density", 40}, {"jitter", 0.5f}, {"panspread", 0.7f}, {"attack", 50}, {"release", 400}, {"level", -1.3f}}},
        {"Both ways", {{"position", 0.4f}, {"spread", 0.1f}, {"scan", 0.25f}, {"reverse", 0.5f}, {"size", 140}, {"density", 35}, {"jitter", 0.7f}, {"panspread", 0.8f}, {"attack", 100}, {"release", 800}, {"level", -3.4f}}},
        {"Dark fog", {{"position", 0.5f}, {"spread", 0.3f}, {"size", 350}, {"density", 25}, {"jitter", 1}, {"panspread", 0.9f}, {"attack", 1000}, {"release", 2500}, {"cutoff", 900}, {"reso", 0.2f}, {"level", -5.8f}}},
        {"Detuned choir", {{"position", 0.3f}, {"spread", 0.04f}, {"size", 160}, {"density", 50}, {"jitter", 0.9f}, {"panspread", 0.8f}, {"pitchspread", 35}, {"attack", 300}, {"release", 1000}, {"level", -1.8f}}},
        {"Double speed scan", {{"position", 0}, {"spread", 0}, {"scan", 2}, {"size", 50}, {"density", 70}, {"jitter", 0.1f}, {"panspread", 0.2f}, {"attack", 2}, {"release", 150}, {"level", 0}}},
    });
}

} // namespace pulp
