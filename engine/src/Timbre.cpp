// Timbre: the instrument that plays a multisample.
#include "pulp/Effects.h"
#include "pulp/Multisample.h"
#include <filesystem>

namespace pulp {

Timbre::Timbre() {
    pAttack = addFloat("attack", "Attack", 0, 5000, 0, "ms", 3.0f);
    pDecay = addFloat("decay", "Decay", 1, 10000, 600, "ms", 3.0f);
    pSustain = addFloat("sustain", "Sustain", 0, 1, 1, "%");
    pRelease = addFloat("release", "Release", 1, 10000, 350, "ms", 3.0f);
    pCutoff = addFloat("cutoff", "Cutoff", 20, 20000, 20000, "Hz", 3.0f);
    pReso = addFloat("reso", "Resonance", 0, 1, 0, "%");
    pEnvAmt = addFloat("envamt", "Filter envelope", -1, 1, 0, "");
    pFAttack = addFloat("fattack", "Filter attack", 0, 5000, 0, "ms", 3.0f);
    pFDecay = addFloat("fdecay", "Filter decay", 1, 10000, 400, "ms", 3.0f);
    pKeyTrack = addFloat("keytrack", "Filter key tracking", 0, 1, 0.3f, "%");
    pLfoRate = addFloat("lforate", "LFO speed", 0.05f, 20, 5.2f, "Hz", 2.0f);
    pLfoDelay = addFloat("lfodelay", "LFO delay", 0, 3000, 250, "ms", 2.0f);
    pVibrato = addFloat("vibrato", "Vibrato", 0, 1, 0, "st");
    pTremolo = addFloat("tremolo", "Tremolo", 0, 1, 0, "%");
    pLfoFilter = addFloat("lfofilter", "LFO to filter", 0, 1, 0, "%");
    pPitchEnv = addFloat("pitchenv", "Pitch envelope", -24, 24, 0, "st");
    pPitchTime = addFloat("pitchtime", "Pitch envelope time", 1, 2000, 60, "ms", 3.0f);
    pVelSens = addFloat("velsens", "Velocity sensitivity", 0, 1, 0.8f, "%");
    pLoop = addBool("loop", "Sustain loop", true);
    pTune = addFloat("tune", "Fine tune", -100, 100, 0, "ct");
    pTranspose = add({"transpose", "Transpose", -24, 24, 0, 1, 49, "st", {}});
    pStart = addFloat("start", "Sample start", 0, 0.9f, 0, "%");
    pVolume = addFloat("volume", "Level", 0, 2, 1, "");
}

std::vector<ParamGroup> Timbre::paramGroups() const {
    return {{"Envelope", {"attack", "decay", "sustain", "release", "velsens", "volume"}},
            {"Filter", {"cutoff", "reso", "envamt", "fattack", "fdecay", "keytrack"}},
            {"LFO", {"lforate", "lfodelay", "vibrato", "tremolo", "lfofilter"}},
            {"Pitch and sample", {"tune", "transpose", "pitchenv", "pitchtime", "loop", "start"}}};
}

std::vector<Preset> Timbre::factoryPresets() const {
    // Shapes that can be put over any instrument. The sound itself comes from the samples.
    return buildPresets(*this, {
        {"As recorded", {}},
        {"Soft and slow", {{"attack", 180}, {"release", 900}, {"cutoff", 4200}, {"velsens", 0.5f}}},
        {"Pad from anything", {{"attack", 700}, {"release", 2200}, {"cutoff", 3000}, {"vibrato", 0.12f}, {"lfodelay", 600}, {"loop", 1}}},
        {"Plucked", {{"attack", 0}, {"decay", 320}, {"sustain", 0}, {"release", 300}, {"cutoff", 5200}, {"envamt", 0.45f}, {"fdecay", 260}}},
        {"Staccato", {{"attack", 0}, {"decay", 140}, {"sustain", 0.2f}, {"release", 90}}},
        {"Dark", {{"cutoff", 1400}, {"reso", 0.15f}, {"keytrack", 0.6f}}},
        {"Bright sweep", {{"cutoff", 900}, {"envamt", 0.8f}, {"fattack", 400}, {"fdecay", 1800}, {"reso", 0.3f}}},
        {"With vibrato", {{"vibrato", 0.18f}, {"lforate", 5.6f}, {"lfodelay", 350}}},
        {"Tremolo", {{"tremolo", 0.6f}, {"lforate", 6.5f}, {"lfodelay", 0}}},
        {"Wobbling filter", {{"cutoff", 1800}, {"lfofilter", 0.6f}, {"lforate", 2.0f}, {"lfodelay", 0}, {"reso", 0.35f}}},
        {"Octave down", {{"transpose", -12}}},
        {"Octave up", {{"transpose", 12}}},
        {"Drop in", {{"pitchenv", 12}, {"pitchtime", 90}}},
        {"Rise in", {{"pitchenv", -7}, {"pitchtime", 160}}},
        {"Long tail", {{"release", 4000}}},
        {"Tight, no loop", {{"loop", 0}, {"release", 120}}},
        {"Swell", {{"attack", 1500}, {"release", 1500}, {"velsens", 0.3f}}},
        {"Lo-fi", {{"cutoff", 2600}, {"reso", 0.2f}, {"vibrato", 0.07f}, {"lforate", 0.6f}, {"lfodelay", 0}}},
        {"Gentle keys", {{"attack", 4}, {"release", 500}, {"cutoff", 6000}, {"velsens", 1.0f}}},
        {"Reverse-like swell", {{"attack", 2600}, {"decay", 100}, {"sustain", 1}, {"release", 60}}},
    });
}

void Timbre::prepare(double sampleRate) {
    sr_ = sampleRate;
    for (auto& v : voices_) v.active = false;
}

void Timbre::reset() {
    for (auto& v : voices_) v.active = false;
    voicesActive_.store(0, std::memory_order_relaxed);
}

void Timbre::setInstrument(ms::InstrumentPtr inst, bool applyItsSettings) {
    if (inst && applyItsSettings && inst->params.isObject())
        for (auto& kv : inst->params.items())
            if (Param* p = param(kv.first)) p->set(kv.second.f(p->info.def));
    inst_.set(std::move(inst));
}

bool Timbre::loadFile(const std::string& path, const std::string& projectDir, std::string* error) {
    auto inst = ms::loadInstrument(SampleCache::global().resolve(path, projectDir), error);
    if (!inst) return false;
    file_ = path;
    setInstrument(inst);
    return true;
}

bool Timbre::buildFromFiles(const std::vector<std::string>& paths, const std::string& projectDir, ms::BuildReport* report, std::string* error) {
    std::vector<ms::Note> notes;
    for (auto& p : paths) {
        ms::Note n;
        n.sample = SampleCache::global().load(p, projectDir, error);
        if (!n.sample) continue;
        n.file = p;
        notes.push_back(std::move(n));
    }
    if (notes.empty()) { if (error && error->empty()) *error = "none of the files could be read"; return false; }
    namespace fs = std::filesystem;
    const auto stem = fs::path(std::u8string(paths[0].begin(), paths[0].end())).stem().u8string();
    auto inst = ms::build(notes, std::string(stem.begin(), stem.end()), {}, report);
    if (inst->zones.empty()) { if (error) *error = "nothing usable was found in the files"; return false; }
    // these zones play the files where they are: the trimmed, looped audio is rebuilt from them when the project is opened
    file_.clear();
    setInstrument(inst);
    return true;
}

Json Timbre::saveExtra() const {
    Json j = Json::object();
    auto inst = inst_.get();
    // file references sit under the key "file" or "sample" so that saving and packing a project find them
    if (!file_.empty()) j.set("file", file_);
    else if (inst && !inst->zones.empty()) {
        // built from loose files: remember which files, and build it again when the project is opened
        Json files = Json::array();
        for (auto& z : inst->zones) files.push(z.file);
        Json b = Json::object();
        b.set("name", inst->name);
        b.set("sample", files);
        j.set("built", b);
    }
    return j;
}

void Timbre::loadExtra(const Json& j, const std::string& projectDir) {
    if (j.isNull()) return;         // a preset: it changes the settings and leaves the instrument alone
    file_.clear();
    const std::string path = j["file"].str("");
    if (!path.empty()) {
        // the parameters were restored just before this: the instrument's own suggested settings must not undo them
        auto inst = ms::loadInstrument(SampleCache::global().resolve(path, projectDir));
        file_ = path;
        setInstrument(inst, false);
        return;
    }
    if (j["built"].isObject()) {
        std::vector<ms::Note> notes;
        for (auto& f : j["built"]["sample"].elements()) {
            ms::Note n;
            n.file = f.str("");
            n.sample = SampleCache::global().load(n.file, projectDir);
            if (n.sample) notes.push_back(std::move(n));
        }
        setInstrument(notes.empty() ? nullptr : ms::InstrumentPtr(ms::build(notes, j["built"]["name"].str("Instrument"))), false);
        return;
    }
    setInstrument(nullptr, false);
}

void Timbre::beginBlock(ProcessContext& ctx) {
    cur_ = inst_.rtAcquire();
    if (cur_ != last_) {                     // the instrument was replaced: old voices point into the old one
        for (auto& v : voices_) v.active = false;
        last_ = cur_;
    }
    bend_ = ctx.pitchBendSemis;
    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
}

void Timbre::startVoice(const NoteEvent& e) {
    if (!cur_ || cur_->zones.empty()) return;
    const int key = clampT(e.key + pTranspose->getInt(), 0, 127);
    const int velocity = clampT((int)std::lround(e.velocity * 127.0f), 0, 127);
    const ms::Zone* z = cur_->pick(key, velocity, turn_++);
    if (!z || !z->sample || z->sample->frames() < 4) return;
    Voice* v = nullptr;
    for (auto& c : voices_) if (!c.active) { v = &c; break; }
    if (!v) {                                // steal the oldest
        v = &voices_[0];
        for (auto& c : voices_) if (c.age < v->age) v = &c;
    }
    v->active = true;
    v->id = e.id;
    v->key = e.key;
    v->group = e.group;
    v->zone = z;
    v->looping = z->loop && pLoop->getBool();
    const int64 frames = z->sample->frames();
    v->pos = std::floor((double)pStart->get() * (double)(v->looping ? z->loopStart : frames - 2));
    v->semis = (float)(key - z->root) - z->cents * 0.01f + e.fineCents * 0.01f + pTune->get() * 0.01f;
    v->bend = v->bendTarget = v->bendStep = 0.0f;
    if (e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) {
        v->bend = e.glideFromKey - (float)e.key;
        v->bendStep = -v->bend / e.slideSamples;
    }
    // velocity: quiet at low velocities by as much as the sensitivity says; velocity 100 is the recorded level
    const float vel = e.velocity / kDefaultVelocity;
    const float g = z->gain * pVolume->get() * (1.0f - pVelSens->get() + pVelSens->get() * vel * vel);
    v->gainL = g * (e.pan > 0.0f ? 1.0f - e.pan : 1.0f);
    v->gainR = g * (e.pan < 0.0f ? 1.0f + e.pan : 1.0f);
    v->modX = e.modX;
    v->modY = e.modY;
    v->relScale = 0.25f + 1.5f * e.release;
    v->amp.set(pAttack->get(), pDecay->get(), pSustain->get(), pRelease->get() * v->relScale, sr_);
    v->amp.value = 0.0f;
    v->amp.start();
    v->filt.set(pFAttack->get(), pFDecay->get(), 0.0f, pFDecay->get(), sr_);
    v->filt.value = 0.0f;
    v->filt.start();
    v->svfL.reset();
    v->svfR.reset();
    v->lfoPhase = 0.0;
    v->lfoFade = 0.0f;
    v->pitchEnv = pPitchEnv->get();
    v->played = 0;
    v->kill = 1.0f;
    v->killStep = 0.0f;
    v->age = ++ageCounter_;
}

void Timbre::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * sr_));
    switch (e.type) {
        case NoteEvent::On: startVoice(e); break;
        case NoteEvent::Off:
            for (auto& v : voices_) if (v.active && v.id == e.id) v.amp.release();
            break;
        case NoteEvent::AllOff:
            for (auto& v : voices_) if (v.active) v.amp.release();
            break;
        case NoteEvent::Kill:
            for (auto& v : voices_) if (v.active && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::KillNote:
            for (auto& v : voices_) if (v.active && v.id == e.id && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::SlideTo:
            for (auto& v : voices_)
                if (v.active && v.amp.stage != Adsr::Release && v.group == e.group) {
                    v.bendTarget = (float)(e.key - v.key);
                    v.bendStep = (v.bendTarget - v.bend) / std::max(1.0f, e.slideSamples);
                }
            break;
    }
}

void Timbre::renderVoice(Voice& v, float* L, float* R, int n) {
    const ms::Zone& z = *v.zone;
    const SampleData& s = *z.sample;
    const float* sl = s.left();
    const float* sr = s.right();
    const int64 frames = s.frames();

    // the LFO comes in after its delay, over the same time again
    const float delay = pLfoDelay->get() * 0.001f * (float)sr_;
    v.lfoFade = delay <= 1.0f ? 1.0f : clampT(((float)v.played - delay) / delay, 0.0f, 1.0f);
    const float lfo = (float)std::sin(kTwoPi * v.lfoPhase) * v.lfoFade;
    v.lfoPhase += (double)pLfoRate->get() * n / sr_;
    v.lfoPhase -= std::floor(v.lfoPhase);
    // the pitch envelope falls away exponentially
    const float pitchEnv = v.pitchEnv;
    v.pitchEnv *= (float)std::exp(-(double)n / (pPitchTime->get() * 0.001 * sr_ / 3.0));

    const double semis = (double)v.semis + (double)v.bend + (double)bend_ + (double)pVibrato->get() * lfo + (double)pitchEnv;
    const double inc = std::pow(2.0, semis / 12.0) * (s.sampleRate / sr_);
    if (v.bendStep != 0.0f) {
        v.bend += v.bendStep * (float)n;
        if ((v.bendStep > 0.0f) == (v.bend >= v.bendTarget)) { v.bend = v.bendTarget; v.bendStep = 0.0f; }
    }
    // filter: cutoff follows the key, the envelope, the LFO and the note's mod X; fully open it is skipped
    const float fenvPeek = v.filt.value;
    float cutoff = pCutoff->get() * std::pow(2.0f, (v.modX - 0.5f) * 10.0f + pKeyTrack->get() * (float)(v.key - 60) / 12.0f
                                                 + pEnvAmt->get() * 6.0f * fenvPeek + pLfoFilter->get() * 3.0f * lfo);
    const bool filterOn = cutoff < 19000.0f || pEnvAmt->get() != 0.0f || pLfoFilter->get() != 0.0f;
    if (filterOn) {
        const float q = 0.707f + clampT(pReso->get() + (v.modY - 0.5f) * 2.0f, 0.0f, 1.0f) * 11.0f;
        v.svfL.set(clampT(cutoff, 20.0f, 20000.0f), q, (float)sr_);
        v.svfR.g = v.svfL.g; v.svfR.k = v.svfL.k; v.svfR.a1 = v.svfL.a1; v.svfR.a2 = v.svfL.a2; v.svfR.a3 = v.svfL.a3;
    }
    const float trem = 1.0f - pTremolo->get() * 0.5f * (1.0f - lfo);
    const int64 loopLen = z.loopEnd - z.loopStart;
    auto at = [&](const float* d, int64 i) -> float { return (i >= 0 && i < frames) ? d[i] : 0.0f; };
    const float tailFade = (float)std::max(16.0, 0.003 * s.sampleRate);

    for (int i = 0; i < n; ++i) {
        if (v.looping && v.pos >= (double)z.loopEnd) v.pos -= (double)loopLen;
        if (v.pos >= (double)(frames - 1)) { v.active = false; break; }
        const int64 idx = (int64)v.pos;
        const float t = (float)(v.pos - (double)idx);
        float xl, xr;
        if (t == 0.0f) { xl = sl[idx]; xr = sr[idx]; }
        else {
            auto herm = [&](const float* d) {
                const float y0 = at(d, idx - 1), y1 = d[idx], y2 = at(d, idx + 1), y3 = at(d, idx + 2);
                const float c1 = 0.5f * (y2 - y0);
                const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                return ((c3 * t + c2) * t + c1) * t + y1;
            };
            xl = herm(sl);
            xr = sr == sl ? xl : herm(sr);
        }
        if (filterOn) { xl = v.svfL.lowpass(xl); xr = v.svfR.lowpass(xr); }
        float g = v.amp.next() * v.kill * trem;
        v.filt.next();
        // a sample that is not looped ends: its last few milliseconds are faded so the end cannot click
        if (!v.looping) { const float toEnd = (float)((double)(frames - 1) - v.pos); if (toEnd < tailFade) g *= toEnd / tailFade; }
        if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) v.active = false; }
        if (!v.amp.active()) v.active = false;
        L[i] += xl * v.gainL * g;
        R[i] += xr * v.gainR * g;
        if (!v.active) break;
        v.pos += inc;
    }
    v.played += n;
}

void Timbre::render(float* L, float* R, int n, ProcessContext&) {
    if (!cur_) return;
    for (auto& v : voices_)
        if (v.active) renderVoice(v, L, R, n);
}

} // namespace pulp
