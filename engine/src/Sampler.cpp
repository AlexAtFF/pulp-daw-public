#include "pulp/Sampler.h"

namespace pulp {

Sampler::Sampler() {
    pRoot = add({"root", "Root key", 0, 127, (float)kRootKey, 1, 128, "", {}});
    pFine = addFloat("fine", "Fine tune", -100, 100, 0, "ct");
    pKeytrack = addBool("keytrack", "Key tracking", true);
    pReverse = addBool("reverse", "Reverse", false);
    pNormalize = addBool("normalize", "Normalize", false);
    pStart = addFloat("start", "Start", 0, 1, 0, "%");
    pEnd = addFloat("end", "End", 0, 1, 1, "%");
    pEnvOn = addBool("env", "Envelope", false);
    pAttack = addFloat("attack", "Attack", 0, 5000, 0, "ms", 3.0f);
    pDecay = addFloat("decay", "Decay", 1, 5000, 300, "ms", 3.0f);
    pSustain = addFloat("sustain", "Sustain", 0, 1, 1, "%");
    pRelease = addFloat("release", "Release", 1, 5000, 100, "ms", 3.0f);
    pCutSelf = addBool("cutself", "Cut itself", false);
    pCutoff = addFloat("cutoff", "Cutoff", 20, 20000, 20000, "Hz", 3.0f);
    pReso = addFloat("reso", "Resonance", 0, 1, 0, "%");
}

void Sampler::prepare(double sampleRate) {
    sr_ = sampleRate;
    for (auto& v : voices_) v.active = false;
}

void Sampler::reset() {
    for (auto& v : voices_) v.active = false;
    voicesActive_.store(0, std::memory_order_relaxed);
}

void Sampler::setSample(SamplePtr s) {
    auto src = std::make_shared<Source>();
    src->sample = std::move(s);
    if (src->sample) {
        const float pk = src->sample->peak();
        src->normGain = pk > 1.0e-6f ? 1.0f / pk : 1.0f;
    }
    source_.set(src);
}
SamplePtr Sampler::sample() const {
    auto s = source_.get();
    return s ? s->sample : nullptr;
}
std::string Sampler::samplePath() const {
    auto s = sample();
    return s ? s->path : std::string();
}

Json Sampler::saveExtra() const {
    Json j = Json::object();
    j.set("sample", samplePath());
    return j;
}
void Sampler::loadExtra(const Json& j, const std::string& projectDir) {
    const std::string path = j["sample"].str("");
    if (path.empty()) { setSample(nullptr); return; }
    setSample(SampleCache::global().load(path, projectDir));
}

void Sampler::beginBlock(ProcessContext& ctx) {
    cur_ = source_.rtAcquire();
    if (cur_ != last_) {                     // the sample was replaced: old voices point at the old data
        for (auto& v : voices_) v.active = false;
        last_ = cur_;
    }
    bend_ = ctx.pitchBendSemis;
    const float a = pAttack->get(), d = pDecay->get(), r = pRelease->get();
    attackRate_ = a <= 0.01f ? 1.0f : (float)(1000.0 / (a * sr_));
    decayCoef_ = (float)std::exp(-1.0 / (0.001 * d * sr_ / 4.6));
    releaseCoef_ = (float)std::exp(-1.0 / (0.001 * r * sr_ / 4.6));
    sustain_ = pSustain->get();
    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
}

void Sampler::startVoice(const NoteEvent& e) {
    if (!cur_ || !cur_->sample || cur_->sample->frames() < 2) return;
    const SampleData& s = *cur_->sample;
    const float killStep = (float)(1.0 / (0.003 * sr_));
    if (pCutSelf->getBool())
        for (auto& v : voices_) if (v.active && v.killStep == 0.0f) v.killStep = killStep;

    Voice* v = nullptr;
    for (auto& c : voices_) if (!c.active) { v = &c; break; }
    if (!v) {                                // steal the oldest
        v = &voices_[0];
        for (auto& c : voices_) if (c.age < v->age) v = &c;
    }
    const int64 frames = s.frames();
    int64 first = (int64)std::floor(pStart->get() * (double)frames);
    int64 last = (int64)std::ceil(pEnd->get() * (double)frames);
    first = clampT<int64>(first, 0, frames - 1);
    last = clampT<int64>(last, first + 1, frames);

    v->active = true;
    v->id = e.id;
    v->key = e.key;
    v->group = e.group;
    v->first = first;
    v->last = last;
    v->reverse = pReverse->getBool();
    v->pos = v->reverse ? (double)(last - 1) : (double)first;
    const bool track = pKeytrack->getBool();
    v->keySemis = track ? (float)(e.key - pRoot->getInt()) : 0.0f;
    v->fineCents = e.fineCents + pFine->get();
    v->bend = v->bendTarget = v->bendStep = 0.0f;
    if (track && e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) {
        v->bend = e.glideFromKey - (float)e.key;
        v->bendStep = -v->bend / e.slideSamples;
    }
    const float vel = e.velocity / kDefaultVelocity;
    float g = vel * vel;
    if (pNormalize->getBool()) g *= cur_->normGain;
    v->gainL = g * (e.pan > 0.0f ? 1.0f - e.pan : 1.0f);
    v->gainR = g * (e.pan < 0.0f ? 1.0f + e.pan : 1.0f);
    if (pEnvOn->getBool()) { v->stage = 0; v->env = attackRate_ >= 1.0f ? 1.0f : 0.0f; if (v->env >= 1.0f) v->stage = 1; }
    else { v->stage = 4; v->env = 1.0f; }
    v->relScale = 0.25f + 1.5f * e.release;      // per-note release scales the release time, 1.0 at the default 0.5
    v->kill = 1.0f;
    v->killStep = 0.0f;
    v->age = ++ageCounter_;
    // Filter: mod X moves the cutoff up to five octaves either way, mod Y adds resonance.
    const float cutoff = pCutoff->get() * std::pow(2.0f, (e.modX - 0.5f) * 10.0f);
    v->filterOn = cutoff < 19990.0f;
    if (v->filterOn) {
        const float q = 0.707f + clampT(pReso->get() + (e.modY - 0.5f) * 2.0f, 0.0f, 1.0f) * 11.0f;
        v->filtL.reset(); v->filtR.reset();
        v->filtL.set(cutoff, q, (float)sr_);
        v->filtR = v->filtL;
    }
}

void Sampler::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * sr_));
    switch (e.type) {
        case NoteEvent::On: startVoice(e); break;
        case NoteEvent::Off:
            for (auto& v : voices_)
                if (v.active && v.id == e.id && v.stage < 3) v.stage = 3;   // one-shots (stage 4) ignore note-off
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

void Sampler::renderVoice(Voice& v, float* L, float* R, int n) {
    const SampleData& s = *cur_->sample;
    const float* sl = s.left();
    const float* sr = s.right();
    const int64 frames = s.frames();
    const double semis = (double)v.keySemis + (double)v.bend + (double)bend_ + (double)v.fineCents * 0.01;
    const double ratio = semis == 0.0 ? 1.0 : std::pow(2.0, semis / 12.0);
    const double inc = ratio * (s.sampleRate / sr_);
    if (v.bendStep != 0.0f) {                // glide in progress: advance once per render segment
        v.bend += v.bendStep * (float)n;
        if ((v.bendStep > 0.0f) == (v.bend >= v.bendTarget)) { v.bend = v.bendTarget; v.bendStep = 0.0f; }
    }
    const bool trimmedStart = v.first > 0, trimmedEnd = v.last < frames;
    const float fadeLen = (float)std::max(8.0, 0.002 * s.sampleRate);
    const bool direct = inc == 1.0 && !v.reverse && v.pos == std::floor(v.pos);
    const float relCoef = v.relScale == 1.0f ? releaseCoef_ : std::pow(releaseCoef_, 1.0f / v.relScale);

    auto at = [&](const float* d, int64 i) -> float { return (i >= 0 && i < frames) ? d[i] : 0.0f; };

    for (int i = 0; i < n; ++i) {
        if (v.reverse ? (v.pos < (double)v.first) : (v.pos >= (double)v.last)) { v.active = false; break; }
        float xl, xr;
        if (direct) {
            const int64 idx = (int64)v.pos;
            xl = sl[idx]; xr = sr[idx];
        } else {
            const int64 idx = (int64)std::floor(v.pos);
            const float t = (float)(v.pos - (double)idx);
            auto herm = [&](const float* d) {
                const float y0 = at(d, idx - 1), y1 = at(d, idx), y2 = at(d, idx + 1), y3 = at(d, idx + 2);
                const float c1 = 0.5f * (y2 - y0);
                const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                return ((c3 * t + c2) * t + c1) * t + y1;
            };
            xl = herm(sl);
            xr = sr == sl ? xl : herm(sr);
        }
        if (v.filterOn) { xl = v.filtL.lowpass(xl); xr = v.filtR.lowpass(xr); }
        float g = v.kill;
        // declick trimmed edges
        const float fromStart = v.reverse ? (float)((double)(v.last - 1) - v.pos) : (float)(v.pos - (double)v.first);
        const float toEnd = v.reverse ? (float)(v.pos - (double)v.first) : (float)((double)(v.last - 1) - v.pos);
        if ((v.reverse ? trimmedEnd : trimmedStart) && fromStart < fadeLen) g *= fromStart / fadeLen;
        if ((v.reverse ? trimmedStart : trimmedEnd) && toEnd < fadeLen) g *= toEnd / fadeLen;
        switch (v.stage) {
            case 0: v.env += attackRate_; if (v.env >= 1.0f) { v.env = 1.0f; v.stage = 1; } break;
            case 1: v.env = sustain_ + (v.env - sustain_) * decayCoef_; break;
            case 3: v.env *= relCoef; if (v.env < 1.0e-4f) v.active = false; break;
            default: break;
        }
        if (v.stage != 4) g *= v.env;
        if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) v.active = false; }
        L[i] += xl * v.gainL * g;
        R[i] += xr * v.gainR * g;
        if (!v.active) break;
        v.pos += v.reverse ? -inc : inc;
    }
}

void Sampler::render(float* L, float* R, int n, ProcessContext&) {
    if (!cur_ || !cur_->sample) return;
    for (auto& v : voices_)
        if (v.active) renderVoice(v, L, R, n);
}

} // namespace pulp
