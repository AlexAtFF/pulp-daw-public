#include "pulp/Pads.h"
#include "pulp/Stretch.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace pulp {

namespace {

// Four-point interpolated read, zero outside the data.
inline float readAt(const float* d, int64 frames, double pos) {
    const int64 idx = (int64)std::floor(pos);
    const float t = (float)(pos - (double)idx);
    auto at = [&](int64 i) -> float { return (i >= 0 && i < frames) ? d[i] : 0.0f; };
    if (t == 0.0f) return at(idx);
    const float y0 = at(idx - 1), y1 = at(idx), y2 = at(idx + 1), y3 = at(idx + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string baseName(const std::string& path) {
    size_t a = path.find_last_of("/\\:");
    std::string n = a == std::string::npos ? path : path.substr(a + 1);
    const size_t dot = n.find_last_of('.');
    return dot == std::string::npos ? n : n.substr(0, dot);
}

} // namespace

// =================================================================== Sixteen

PadKit::PadKit() {
    for (int i = 0; i < kPads; ++i) {
        const std::string p = "p" + std::to_string(i + 1) + "_", n = "Pad " + std::to_string(i + 1) + " ";
        pLevel[i] = addFloat((p + "level").c_str(), (n + "level").c_str(), -36, 12, 0, "dB");
        pPan[i] = addFloat((p + "pan").c_str(), (n + "pan").c_str(), -1, 1, 0, "");
        pTune[i] = addFloat((p + "tune").c_str(), (n + "tune").c_str(), -24, 24, 0, "st");
        pLength[i] = addFloat((p + "length").c_str(), (n + "length").c_str(), 1, 100, 100, "%", 2.0f);
        std::vector<std::string> groups{"None"};
        for (int g = 1; g <= kGroups; ++g) groups.push_back("Group " + std::to_string(g));
        pGroup[i] = addChoice((p + "group").c_str(), (n + "cut group").c_str(), groups, 0);
        hit_[i].store(0.0f);
    }
    kit_.set(std::make_shared<Kit>());
}

std::vector<ParamGroup> PadKit::paramGroups() const {
    std::vector<ParamGroup> out;
    for (int i = 0; i < kPads; ++i) {
        const std::string p = "p" + std::to_string(i + 1) + "_";
        out.push_back({"Pad " + std::to_string(i + 1), {p + "level", p + "pan", p + "tune", p + "length", p + "group"}});
    }
    return out;
}

float PadKit::meter(int pad) const { return pad >= 0 && pad < kPads ? hit_[pad].load(std::memory_order_relaxed) : 0.0f; }

void PadKit::prepare(double sampleRate) {
    sr_ = sampleRate;
    for (auto& v : voices_) v.active = false;
}

void PadKit::reset() {
    for (auto& v : voices_) v.active = false;
    for (auto& h : hit_) h.store(0.0f, std::memory_order_relaxed);
    voicesActive_.store(0, std::memory_order_relaxed);
}

std::shared_ptr<PadKit::Kit> PadKit::copyKit() const {
    auto cur = kit_.get();
    return cur ? std::make_shared<Kit>(*cur) : std::make_shared<Kit>();
}

bool PadKit::setLayer(int pad, int layer, SamplePtr s) {
    if (pad < 0 || pad >= kPads || layer < 0 || layer >= kLayers) return false;
    auto k = copyKit();
    auto& layers = k->pads[pad].layers;
    if (!s) {
        if (layer >= (int)layers.size()) return false;
        layers.erase(layers.begin() + layer);
    } else if (layer < (int)layers.size()) layers[(size_t)layer] = std::move(s);
    else layers.push_back(std::move(s));
    if (layers.size() == 1 && k->pads[pad].name.empty()) k->pads[pad].name = baseName(layers[0]->path);
    if (layers.empty()) k->pads[pad].name.clear();
    kit_.set(k);
    return true;
}

int PadKit::addLayer(int pad, SamplePtr s) {
    const int n = layerCount(pad);
    if (!s || pad < 0 || pad >= kPads || n >= kLayers) return -1;
    return setLayer(pad, n, std::move(s)) ? n : -1;
}

void PadKit::clearPad(int pad) {
    if (pad < 0 || pad >= kPads) return;
    auto k = copyKit();
    k->pads[pad] = Pad();
    kit_.set(k);
}

void PadKit::clearAll() { kit_.set(std::make_shared<Kit>()); }

int PadKit::layerCount(int pad) const {
    auto k = kit_.get();
    return k && pad >= 0 && pad < kPads ? (int)k->pads[pad].layers.size() : 0;
}

SamplePtr PadKit::layer(int pad, int layer) const {
    auto k = kit_.get();
    if (!k || pad < 0 || pad >= kPads || layer < 0 || layer >= (int)k->pads[pad].layers.size()) return nullptr;
    return k->pads[pad].layers[(size_t)layer];
}

std::string PadKit::padName(int pad) const {
    auto k = kit_.get();
    return k && pad >= 0 && pad < kPads ? k->pads[pad].name : std::string();
}

void PadKit::setPadName(int pad, const std::string& name) {
    if (pad < 0 || pad >= kPads) return;
    auto k = copyKit();
    k->pads[pad].name = name;
    kit_.set(k);
}

// Where a drummer expects each sound: 1 kick, 2 snare, 3 clap, 4 closed hat, 5 open hat, 6 to 8 toms,
// 9 rimshot, 10 crash, 11 ride, 12 cowbell, 13 percussion, 14 shaker, 15 a second kick, 16 anything else.
int PadKit::padForName(const std::string& fileName) {
    const std::string n = lower(baseName(fileName));
    auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
    if (has("kick") || has("bass drum") || has("bassdrum") || has("bd")) return (has("long") || has("sub") || has("2")) ? 14 : 0;
    if (has("rim") || has("sidestick") || has("side stick")) return 8;
    if (has("snare") || has("sd")) return 1;
    if (has("clap")) return 2;
    if (has("hat") || has("hh")) return (has("open") || has("pedal")) ? 4 : 3;
    if (has("tom")) return has("low") || has("floor") ? 5 : (has("high") || has("hi") ? 7 : 6);
    if (has("crash") || has("cymbal") || has("china") || has("splash")) return 9;
    if (has("ride")) return 10;
    if (has("cowbell") || has("bell")) return 11;
    if (has("conga") || has("bongo") || has("clave") || has("wood") || has("perc") || has("block")) return 12;
    if (has("shaker") || has("maraca") || has("tamb") || has("cabasa")) return 13;
    return -1;
}

int PadKit::loadKit(const std::vector<std::string>& files, const std::string& projectDir) {
    auto k = std::make_shared<Kit>();
    std::vector<SamplePtr> spare;
    int filled = 0;
    for (auto& f : files) {
        auto s = SampleCache::global().load(f, projectDir);
        if (!s) continue;
        const int pad = padForName(f);
        if (pad >= 0 && k->pads[pad].layers.empty()) { k->pads[pad].layers.push_back(s); k->pads[pad].name = baseName(f); ++filled; }
        else spare.push_back(s);
    }
    // sounds with no obvious place, and second helpings, take the pads that are still empty, from the top
    for (auto& s : spare) {
        int pad = -1;
        for (int i = kPads - 1; i >= 12 && pad < 0; --i) if (k->pads[i].layers.empty()) pad = i;
        for (int i = 0; i < kPads && pad < 0; ++i) if (k->pads[i].layers.empty()) pad = i;
        if (pad < 0) break;
        k->pads[pad].layers.push_back(s);
        k->pads[pad].name = baseName(s->path);
        ++filled;
    }
    kit_.set(k);
    // hats cut each other, as on a real kit
    pGroup[3]->set(k->pads[3].layers.empty() || k->pads[4].layers.empty() ? 0.0f : 1.0f);
    pGroup[4]->set(pGroup[3]->get());
    return filled;
}

Json PadKit::saveExtra() const {
    auto k = kit_.get();
    Json pads = Json::array();
    for (int i = 0; i < kPads; ++i) {
        Json p = Json::object();
        p.set("name", k ? k->pads[i].name : std::string());
        Json layers = Json::array();
        if (k) for (auto& s : k->pads[i].layers) { Json l = Json::object(); l.set("sample", s->path); layers.push(l); }
        p.set("layers", layers);
        pads.push(p);
    }
    Json j = Json::object();
    j.set("pads", pads);
    return j;
}

void PadKit::loadExtra(const Json& j, const std::string& projectDir) {
    if (j.isNull()) return;         // a preset that only moves knobs keeps the kit
    auto k = std::make_shared<Kit>();
    int i = 0;
    for (auto& p : j["pads"].elements()) {
        if (i >= kPads) break;
        for (auto& l : p["layers"].elements()) {
            if ((int)k->pads[i].layers.size() >= kLayers) break;
            if (auto s = SampleCache::global().load(l["sample"].str(""), projectDir)) k->pads[i].layers.push_back(s);
        }
        k->pads[i].name = p["name"].str("");
        ++i;
    }
    kit_.set(k);
}

std::vector<Preset> PadKit::factoryPresets() const {
    struct P { int pad; const char* name; const char* file; };
    struct K { const char* name; std::vector<P> pads; float tune, length; };
    const std::vector<P> k808 = {{0, "Kick", "808 Kit/808 Kick.wav"}, {1, "Snare", "808 Kit/808 Snare.wav"}, {2, "Clap", "808 Kit/808 Clap.wav"},
        {3, "Closed hat", "808 Kit/808 Hat Closed.wav"}, {4, "Open hat", "808 Kit/808 Hat Open.wav"}, {5, "Low tom", "808 Kit/808 Tom Low.wav"},
        {6, "Mid tom", "808 Kit/808 Tom Mid.wav"}, {7, "High tom", "808 Kit/808 Tom High.wav"}, {8, "Rimshot", "808 Kit/808 Rimshot.wav"},
        {9, "Cymbal", "808 Kit/808 Cymbal.wav"}, {11, "Cowbell", "808 Kit/808 Cowbell.wav"}, {12, "Conga", "808 Kit/808 Conga.wav"},
        {13, "Maracas", "808 Kit/808 Maracas.wav"}, {14, "Long kick", "808 Kit/808 Kick Long.wav"}, {15, "Clave", "808 Kit/808 Clave.wav"}};
    const std::vector<P> k909 = {{0, "Kick", "909 Kit/909 Kick.wav"}, {1, "Snare", "909 Kit/909 Snare.wav"}, {2, "Clap", "909 Kit/909 Clap.wav"},
        {3, "Closed hat", "909 Kit/909 Hat Closed.wav"}, {4, "Open hat", "909 Kit/909 Hat Open.wav"}, {5, "Low tom", "909 Kit/909 Tom Low.wav"},
        {6, "Mid tom", "909 Kit/909 Tom Mid.wav"}, {7, "High tom", "909 Kit/909 Tom High.wav"}, {8, "Rimshot", "909 Kit/909 Rimshot.wav"},
        {9, "Crash", "909 Kit/909 Crash.wav"}, {10, "Ride", "909 Kit/909 Ride.wav"}};
    const std::vector<P> kAc = {{0, "Kick", "Acoustic Kit/kick.wav"}, {1, "Snare", "Acoustic Kit/snare.wav"}, {2, "Clap", "Acoustic Kit/clap.wav"},
        {3, "Closed hat", "Acoustic Kit/hihat_closed.wav"}, {4, "Open hat", "Acoustic Kit/hihat_open.wav"}, {5, "Low tom", "Acoustic Kit/tom_low.wav"},
        {6, "Mid tom", "Acoustic Kit/tom_mid.wav"}, {7, "High tom", "Acoustic Kit/tom_high.wav"}, {8, "Sidestick", "Acoustic Kit/sidestick.wav"},
        {9, "Crash", "Acoustic Kit/crash.wav"}, {10, "Ride", "Acoustic Kit/ride.wav"}, {11, "Cowbell", "Acoustic Kit/cowbell.wav"},
        {12, "Woodblock", "Acoustic Kit/woodblock.wav"}, {13, "Shaker", "Acoustic Kit/shaker.wav"}, {14, "Pedal hat", "Acoustic Kit/hihat_pedal.wav"},
        {15, "Tambourine", "Acoustic Kit/tambourine.wav"}};
    std::vector<P> hybrid = {{0, "808 kick", "808 Kit/808 Kick.wav"}, {1, "909 snare", "909 Kit/909 Snare.wav"}, {2, "909 clap", "909 Kit/909 Clap.wav"},
        {3, "909 closed hat", "909 Kit/909 Hat Closed.wav"}, {4, "909 open hat", "909 Kit/909 Hat Open.wav"}, {5, "808 low tom", "808 Kit/808 Tom Low.wav"},
        {6, "808 mid tom", "808 Kit/808 Tom Mid.wav"}, {7, "808 high tom", "808 Kit/808 Tom High.wav"}, {8, "808 rimshot", "808 Kit/808 Rimshot.wav"},
        {9, "909 crash", "909 Kit/909 Crash.wav"}, {10, "909 ride", "909 Kit/909 Ride.wav"}, {11, "808 cowbell", "808 Kit/808 Cowbell.wav"},
        {12, "808 conga", "808 Kit/808 Conga.wav"}, {13, "808 maracas", "808 Kit/808 Maracas.wav"}, {14, "808 long kick", "808 Kit/808 Kick Long.wav"},
        {15, "909 kick", "909 Kit/909 Kick.wav"}};
    const std::vector<K> kits = {
        {"808 kit", k808, 0, 100}, {"909 kit", k909, 0, 100}, {"Acoustic kit", kAc, 0, 100}, {"808 and 909 together", hybrid, 0, 100},
        {"808 kit, tuned down a third", k808, -4, 100}, {"909 kit, tight and short", k909, 0, 35}, {"Acoustic kit, damped", kAc, 0, 30},
        {"808 kit, pitched up and snappy", k808, 5, 45}, {"909 kit, slowed and heavy", k909, -7, 100}, {"Acoustic kit, tuned low", kAc, -5, 100},
    };
    std::vector<Preset> out;
    for (auto& kit : kits) {
        Json params = Json::object();
        for (int i = 0; i < kPads; ++i) {
            const std::string p = "p" + std::to_string(i + 1) + "_";
            params.set(p + "level", 0.0);
            params.set(p + "pan", 0.0);
            // cymbals and hats keep their length and tuning in the altered kits: only drums are retuned
            const bool metal = i == 3 || i == 4 || i == 9 || i == 10;
            params.set(p + "tune", metal ? 0.0 : (double)kit.tune);
            params.set(p + "length", metal ? 100.0 : (double)kit.length);
            params.set(p + "group", (i == 3 || i == 4) ? 1.0 : 0.0);
        }
        Json pads = Json::array();
        for (int i = 0; i < kPads; ++i) {
            Json p = Json::object();
            Json layers = Json::array();
            std::string name;
            for (auto& e : kit.pads) if (e.pad == i) {
                Json l = Json::object();
                l.set("sample", std::string("factory:Drums/") + e.file);
                layers.push(l);
                name = e.name;
            }
            p.set("name", name);
            p.set("layers", layers);
            pads.push(p);
        }
        Json extra = Json::object();
        extra.set("pads", pads);
        Json state = Json::object();
        state.set("type", "pads");
        state.set("params", params);
        state.set("extra", extra);
        out.push_back({kit.name, state});
    }
    return out;
}

void PadKit::beginBlock(ProcessContext& ctx) {
    cur_ = kit_.rtAcquire();
    if (cur_ != last_) {                 // the kit changed: old voices point at samples that may be gone
        for (auto& v : voices_) v.active = false;
        last_ = cur_;
    }
    bend_ = ctx.pitchBendSemis;
    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
    for (auto& h : hit_) { const float x = h.load(std::memory_order_relaxed); if (x > 0.0f) h.store(x < 0.01f ? 0.0f : x * 0.985f, std::memory_order_relaxed); }
}

void PadKit::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * sr_));
    switch (e.type) {
        case NoteEvent::On: {
            const int pad = e.key - kBaseKey;
            if (!cur_ || pad < 0 || pad >= kPads) break;
            const auto& layers = cur_->pads[pad].layers;
            if (layers.empty()) break;
            // the harder the hit, the later the layer
            const int li = clampT((int)(e.velocity * (float)layers.size()), 0, (int)layers.size() - 1);
            const SampleData* s = layers[(size_t)li].get();
            if (!s || s->frames() < 2) break;
            const int group = pGroup[pad]->getInt();
            if (group > 0)
                for (auto& v : voices_) if (v.active && v.killStep == 0.0f && pGroup[v.pad]->getInt() == group) v.killStep = killStep;
            Voice* v = nullptr;
            for (auto& c : voices_) if (!c.active) { v = &c; break; }
            if (!v) { v = &voices_[0]; for (auto& c : voices_) if (c.age < v->age) v = &c; }
            v->active = true;
            v->id = e.id;
            v->pad = pad;
            v->s = s;
            v->pos = 0.0;
            v->last = std::max<int64>(2, (int64)std::ceil((double)s->frames() * (double)pLength[pad]->get() * 0.01));
            const float vel = e.velocity / kDefaultVelocity;
            const float g = vel * vel * dbToGain(pLevel[pad]->get());
            const float pan = clampT(pPan[pad]->get() + e.pan, -1.0f, 1.0f);
            v->gainL = g * (pan > 0.0f ? 1.0f - pan : 1.0f);
            v->gainR = g * (pan < 0.0f ? 1.0f + pan : 1.0f);
            v->inc = std::pow(2.0, ((double)pTune[pad]->get() + (double)e.fineCents * 0.01) / 12.0) * (s->sampleRate / sr_);
            v->kill = 1.0f;
            v->killStep = 0.0f;
            v->age = ++ageCounter_;
            hit_[pad].store(clampT(e.velocity, 0.05f, 1.0f), std::memory_order_relaxed);
            break;
        }
        case NoteEvent::Kill:
            for (auto& v : voices_) if (v.active && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::KillNote:
            for (auto& v : voices_) if (v.active && v.id == e.id && v.killStep == 0.0f) v.killStep = killStep;
            break;
        default: break;     // drums ring out: note-off and slides mean nothing to them
    }
}

void PadKit::render(float* L, float* R, int n, ProcessContext&) {
    if (!cur_) return;
    const double bend = bend_ == 0.0f ? 1.0 : std::pow(2.0, (double)bend_ / 12.0);
    for (auto& v : voices_) {
        if (!v.active) continue;
        const float* sl = v.s->left();
        const float* sr = v.s->right();
        const int64 frames = v.s->frames();
        const double inc = v.inc * bend;
        const bool cut = v.last < frames;
        const double fade = std::max(8.0, 0.004 * v.s->sampleRate);
        for (int i = 0; i < n; ++i) {
            if (v.pos >= (double)v.last) { v.active = false; break; }
            float xl = readAt(sl, frames, v.pos);
            float xr = sr == sl ? xl : readAt(sr, frames, v.pos);
            float g = v.kill;
            if (cut) { const double toEnd = (double)v.last - v.pos; if (toEnd < fade) g *= (float)(toEnd / fade); }
            if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) v.active = false; }
            L[i] += xl * v.gainL * g;
            R[i] += xr * v.gainR * g;
            if (!v.active) break;
            v.pos += inc;
        }
    }
}

// =================================================================== Chop

Chop::Chop() {
    pSens = addFloat("sens", "Sensitivity", 0, 100, 50, "%");
    pMode = addBool("mode", "Play on to the end", false);
    pAttack = addFloat("attack", "Fade in", 0, 50, 0.5f, "ms", 2.0f);
    pFade = addFloat("fade", "Fade out", 1, 500, 8, "ms", 2.0f);
    pPitch = addFloat("pitch", "Pitch", -24, 24, 0, "st");
    pFine = addFloat("fine", "Fine tune", -100, 100, 0, "ct");
    pReverse = addBool("reverse", "Reverse", false);
    pGate = addBool("gate", "Stop when the key is released", false);
    source_.set(std::make_shared<Source>());
}

std::vector<ParamGroup> Chop::paramGroups() const {
    return {{"Slicing", {"sens", "mode"}}, {"Playing", {"attack", "fade", "pitch", "fine", "reverse", "gate"}}};
}

std::vector<Preset> Chop::factoryPresets() const {
    auto defs = [&](std::vector<std::pair<const char*, std::vector<std::pair<const char*, float>>>> list) {
        std::vector<Preset> out;
        for (auto& d : list) {
            Json params = Json::object();
            for (auto& p : params_) params.set(p->info.id, (double)p->info.def);
            for (auto& kv : d.second) params.set(kv.first, (double)kv.second);
            params.set("sens", (double)pSens->get());       // presets change how slices play, never where they are
            Json state = Json::object();
            state.set("type", "slicer");
            state.set("params", params);
            out.push_back({d.first, state});
        }
        return out;
    };
    return defs({
        {"Straight slices", {}},
        {"Tight and gated", {{"gate", 1}, {"fade", 4}}},
        {"Long tails (each key plays on to the end)", {{"mode", 1}}},
        {"Soft edges", {{"attack", 6}, {"fade", 60}}},
        {"Backwards", {{"reverse", 1}, {"fade", 20}}},
        {"Octave down", {{"pitch", -12}}},
        {"Octave up", {{"pitch", 12}}},
        {"A fifth down, heavy", {{"pitch", -7}, {"fade", 30}}},
        {"Stutter (very short)", {{"gate", 1}, {"fade", 1}, {"attack", 0}}},
        {"Swells", {{"attack", 40}, {"fade", 120}, {"mode", 1}}},
    });
}

void Chop::prepare(double sampleRate) {
    sr_ = sampleRate;
    for (auto& v : voices_) v.active = false;
}

void Chop::reset() {
    for (auto& v : voices_) v.active = false;
    voicesActive_.store(0, std::memory_order_relaxed);
}

SamplePtr Chop::sample() const { auto s = source_.get(); return s ? s->sample : nullptr; }
std::vector<int64> Chop::slices() const { auto s = source_.get(); return s ? s->starts : std::vector<int64>(); }
double Chop::beats() const { auto s = source_.get(); return s ? s->beats : 4.0; }

void Chop::setBeats(double beats) {
    auto cur = source_.get();
    auto next = std::make_shared<Source>(cur ? *cur : Source());
    next->beats = clampT(beats, 0.25, 256.0);
    source_.set(next);
}

void Chop::setSlices(std::vector<int64> starts) {
    auto cur = source_.get();
    auto next = std::make_shared<Source>(cur ? *cur : Source());
    const int64 frames = next->sample ? next->sample->frames() : 0;
    std::sort(starts.begin(), starts.end());
    next->starts.clear();
    next->starts.push_back(0);
    for (int64 s : starts)
        if (s > next->starts.back() && s < frames && (int)next->starts.size() < kMaxSlices) next->starts.push_back(s);
    source_.set(next);
}

void Chop::sliceEvenly(int count) {
    auto s = sample();
    if (!s) return;
    count = clampT(count, 1, kMaxSlices);
    std::vector<int64> starts;
    for (int i = 0; i < count; ++i) starts.push_back(s->frames() * i / count);
    setSlices(starts);
}

void Chop::detect() {
    auto s = sample();
    if (s) setSlices(findHits(*s, pSens->get() * 0.01f));
}

void Chop::setSample(SamplePtr s) {
    auto next = std::make_shared<Source>();
    next->sample = s;
    next->starts = {0};
    if (s && s->frames() > 0) {
        next->starts = findHits(*s, pSens->get() * 0.01f);
        // how many beats long is it? The detected tempo if it is clear, otherwise the whole number of bars
        // that puts the tempo nearest 120.
        const double seconds = (double)s->frames() / s->sampleRate;
        double conf = 0.0;
        const double bpm = seconds > 1.0 ? stretch::detectTempo(*s, &conf) : 0.0;
        double beats = 0.0;
        if (bpm > 40.0 && conf >= 0.2) beats = std::round(seconds * bpm / 60.0);
        if (beats < 1.0) {
            double best = 1e9;
            for (double b : {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0}) {
                const double d = std::abs(std::log2((b * 60.0 / seconds) / 120.0));
                if (d < best) { best = d; beats = b; }
            }
        }
        next->beats = beats;
    }
    source_.set(next);
}

// Hits are found where the level jumps: the energy of short frames (and of their high frequencies, which
// catches hats over a ringing bass) is compared with the loudest of the two frames before.
std::vector<int64> Chop::findHits(const SampleData& s, float sensitivity, int maxSlices) {
    std::vector<int64> out{0};
    const int64 frames = s.frames();
    const int hop = std::max(32, (int)std::lround(s.sampleRate * 0.0058));
    const int count = (int)(frames / hop);
    if (count < 4) return out;
    std::vector<float> full((size_t)count), high((size_t)count);
    const float* l = s.left();
    const float* r = s.right();
    float prev = 0.0f, peakE = 0.0f;
    for (int k = 0; k < count; ++k) {
        double e = 0.0, h = 0.0;
        for (int i = 0; i < hop; ++i) {
            const float x = 0.5f * (l[(size_t)k * hop + i] + r[(size_t)k * hop + i]);
            e += (double)x * x;
            h += (double)(x - prev) * (x - prev);
            prev = x;
        }
        full[(size_t)k] = (float)(e / hop);
        high[(size_t)k] = (float)(h / hop);
        peakE = std::max(peakE, full[(size_t)k]);
    }
    if (peakE <= 0.0f) return out;
    const float eps = peakE * 1.0e-5f;
    std::vector<float> onset((size_t)count, 0.0f);
    for (int k = 2; k < count; ++k) {
        const float before = std::max(full[(size_t)k - 1], full[(size_t)k - 2]), beforeH = std::max(high[(size_t)k - 1], high[(size_t)k - 2]);
        const float a = std::log((full[(size_t)k] + eps) / (before + eps)), b = std::log((high[(size_t)k] + eps) / (beforeH + eps));
        onset[(size_t)k] = std::max(0.0f, a) + 0.5f * std::max(0.0f, b);
    }
    // sensitivity 0 wants a jump of about 13 dB, sensitivity 1 about 1.5 dB
    const float threshold = 3.0f * std::pow(0.115f, clampT(sensitivity, 0.0f, 1.0f));
    const int minGap = std::max(1, (int)std::lround(0.05 * s.sampleRate / hop));
    struct Hit { int frame; float strength; };
    std::vector<Hit> hits;
    for (int k = 2; k < count - 1; ++k) {
        if (onset[(size_t)k] < threshold || full[(size_t)k] < peakE * 1.0e-4f) continue;
        bool isPeak = true;
        for (int d = -2; d <= 2 && isPeak; ++d)
            if (d != 0 && k + d >= 0 && k + d < count && onset[(size_t)(k + d)] > onset[(size_t)k]) isPeak = false;
        if (!isPeak) continue;
        if (!hits.empty() && k - hits.back().frame < minGap) { if (onset[(size_t)k] > hits.back().strength) hits.back() = {k, onset[(size_t)k]}; continue; }
        hits.push_back({k, onset[(size_t)k]});
    }
    if ((int)hits.size() > maxSlices - 1) {
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.strength > b.strength; });
        hits.resize((size_t)(maxSlices - 1));
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.frame < b.frame; });
    }
    const int64 back = std::max<int64>(1, (int64)(0.001 * s.sampleRate));
    for (auto& h : hits) {
        // the exact place: where the sound first rises clearly above what was there just before (a hat often
        // lands on the ringing tail of a kick, so both the level and its sample-to-sample change are compared
        // with the stretch before), then a millisecond earlier, then back to the nearest zero crossing so the
        // slice starts without a click
        const int64 a = std::max<int64>(1, (int64)(h.frame - 1) * hop), b = std::min<int64>(frames, (int64)(h.frame + 1) * hop);
        auto mono = [&](int64 i) { return 0.5f * (l[i] + r[i]); };
        float before = 0.0f, beforeD = 0.0f, top = 0.0f, topD = 0.0f;
        for (int64 i = std::max<int64>(1, a - hop); i < a; ++i) { before = std::max(before, std::abs(mono(i))); beforeD = std::max(beforeD, std::abs(mono(i) - mono(i - 1))); }
        for (int64 i = a; i < b; ++i) { top = std::max(top, std::abs(mono(i))); topD = std::max(topD, std::abs(mono(i) - mono(i - 1))); }
        int64 at = a;
        for (int64 i = a; i < b; ++i) {
            const bool louder = std::abs(mono(i)) > std::max(before * 1.5f, top * 0.1f) && top > before * 1.6f;
            const bool sharper = std::abs(mono(i) - mono(i - 1)) > std::max(beforeD * 2.0f, topD * 0.15f) && topD > beforeD * 2.2f;
            if (louder || sharper) { at = i; break; }
        }
        at = std::max<int64>(0, at - back);
        for (int64 i = at; i > std::max<int64>(0, at - back); --i)
            if ((l[i] + r[i] >= 0.0f) != (l[i - 1] + r[i - 1] >= 0.0f)) { at = i; break; }
        if (at > out.back() + back * 20) out.push_back(at);       // nothing closer than 20 ms to the slice before
    }
    return out;
}

Json Chop::saveExtra() const {
    auto src = source_.get();
    Json j = Json::object();
    j.set("sample", src && src->sample ? src->sample->path : std::string());
    Json starts = Json::array();
    if (src) for (int64 s : src->starts) starts.push(Json((double)s));
    j.set("slices", starts);
    j.set("beats", src ? src->beats : 4.0);
    return j;
}

void Chop::loadExtra(const Json& j, const std::string& projectDir) {
    if (j.isNull()) return;         // presets leave the loop and its slices alone
    const std::string path = j["sample"].str("");
    auto next = std::make_shared<Source>();
    if (!path.empty()) next->sample = SampleCache::global().load(path, projectDir);
    next->starts = {0};
    next->beats = j["beats"].isNull() ? 4.0 : j["beats"].num();
    source_.set(next);
    std::vector<int64> starts;
    for (auto& e : j["slices"].elements()) starts.push_back((int64)e.num());
    if (next->sample) setSlices(starts);
}

void Chop::beginBlock(ProcessContext& ctx) {
    cur_ = source_.rtAcquire();
    if (cur_ != last_) {
        for (auto& v : voices_) v.active = false;
        last_ = cur_;
    }
    bend_ = ctx.pitchBendSemis;
    int n = 0;
    for (auto& v : voices_) n += v.active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
}

void Chop::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * sr_));
    switch (e.type) {
        case NoteEvent::On: {
            if (!cur_ || !cur_->sample) break;
            const int slice = e.key - kBaseKey;
            if (slice < 0 || slice >= (int)cur_->starts.size()) break;
            const int64 frames = cur_->sample->frames();
            // playing on to the end is one voice at a time: a new key takes over from the last
            if (pMode->getInt() == 1) for (auto& c : voices_) if (c.active && c.killStep == 0.0f) c.killStep = killStep;
            Voice* v = nullptr;
            for (auto& c : voices_) if (!c.active) { v = &c; break; }
            if (!v) { v = &voices_[0]; for (auto& c : voices_) if (c.age < v->age) v = &c; }
            v->active = true;
            v->releasing = false;
            v->id = e.id;
            v->group = e.group;
            v->first = cur_->starts[(size_t)slice];
            v->last = (pMode->getInt() == 0 && slice + 1 < (int)cur_->starts.size()) ? cur_->starts[(size_t)slice + 1] : frames;
            v->reverse = pReverse->getBool();
            v->pos = v->reverse ? (double)(v->last - 1) : (double)v->first;
            v->inc = std::pow(2.0, ((double)pPitch->get() + ((double)pFine->get() + (double)e.fineCents) * 0.01) / 12.0) * (cur_->sample->sampleRate / sr_);
            const float vel = e.velocity / kDefaultVelocity;
            const float g = vel * vel;
            v->gainL = g * (e.pan > 0.0f ? 1.0f - e.pan : 1.0f);
            v->gainR = g * (e.pan < 0.0f ? 1.0f + e.pan : 1.0f);
            v->env = pAttack->get() <= 0.01f ? 1.0f : 0.0f;
            v->kill = 1.0f;
            v->killStep = 0.0f;
            v->age = ++ageCounter_;
            break;
        }
        case NoteEvent::Off:
            if (pGate->getBool()) for (auto& v : voices_) if (v.active && v.id == e.id) v.releasing = true;
            break;
        case NoteEvent::AllOff:
            if (pGate->getBool()) for (auto& v : voices_) if (v.active) v.releasing = true;
            break;
        case NoteEvent::Kill:
            for (auto& v : voices_) if (v.active && v.killStep == 0.0f) v.killStep = killStep;
            break;
        case NoteEvent::KillNote:
            for (auto& v : voices_) if (v.active && v.id == e.id && v.killStep == 0.0f) v.killStep = killStep;
            break;
        default: break;
    }
}

void Chop::render(float* L, float* R, int n, ProcessContext&) {
    if (!cur_ || !cur_->sample) return;
    const SampleData& s = *cur_->sample;
    const float* sl = s.left();
    const float* sr = s.right();
    const int64 frames = s.frames();
    const double bend = bend_ == 0.0f ? 1.0 : std::pow(2.0, (double)bend_ / 12.0);
    const float attackStep = pAttack->get() <= 0.01f ? 1.0f : (float)(1000.0 / ((double)pAttack->get() * sr_));
    const float releaseCoef = (float)std::exp(-1.0 / (0.001 * (double)pFade->get() * sr_ / 4.6));
    const double fade = std::max(8.0, 0.001 * (double)pFade->get() * s.sampleRate);
    for (auto& v : voices_) {
        if (!v.active) continue;
        const double inc = v.inc * bend;
        for (int i = 0; i < n; ++i) {
            if (v.reverse ? (v.pos < (double)v.first) : (v.pos >= (double)v.last)) { v.active = false; break; }
            const float xl = readAt(sl, frames, v.pos);
            const float xr = sr == sl ? xl : readAt(sr, frames, v.pos);
            float g = v.kill;
            // a slice fades out before its end (the next slice starts there); at the very end of the recording too
            const double toEnd = v.reverse ? v.pos - (double)v.first : (double)v.last - v.pos;
            if (toEnd < fade) g *= (float)(toEnd / fade);
            if (v.releasing) { v.env *= releaseCoef; if (v.env < 1.0e-4f) v.active = false; }
            else if (v.env < 1.0f) v.env = std::min(1.0f, v.env + attackStep);
            g *= v.env;
            if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) v.active = false; }
            L[i] += xl * v.gainL * g;
            R[i] += xr * v.gainR * g;
            if (!v.active) break;
            v.pos += v.reverse ? -inc : inc;
        }
    }
}

} // namespace pulp
