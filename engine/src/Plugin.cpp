#include "pulp/Plugin.h"
#include <cstdio>

namespace pulp {

float Param::toNorm(float v) const {
    if (info.max <= info.min) return 0.0f;
    float n = clampT((v - info.min) / (info.max - info.min), 0.0f, 1.0f);
    return info.skew == 1.0f ? n : std::pow(n, 1.0f / info.skew);
}
float Param::fromNorm(float n) const {
    n = clampT(n, 0.0f, 1.0f);
    if (info.skew != 1.0f) n = std::pow(n, info.skew);
    float v = info.min + n * (info.max - info.min);
    if (info.steps > 0) v = std::round(v);
    return v;
}
std::string Param::displayFor(float v) const {
    if (!info.choices.empty()) {
        const int i = clampT((int)std::lround(v - info.min), 0, (int)info.choices.size() - 1);
        return info.choices[(size_t)i];
    }
    char b[64];
    if (info.steps > 0) snprintf(b, sizeof b, "%d", (int)std::lround(v));
    else if (info.unit == "%") snprintf(b, sizeof b, "%.0f", v * 100.0f);
    else if (info.unit == "Hz" && v >= 1000.0f) { snprintf(b, sizeof b, "%.2f k", v / 1000.0f); }
    else if (std::fabs(v) >= 100.0f) snprintf(b, sizeof b, "%.0f", v);
    else if (std::fabs(v) >= 10.0f) snprintf(b, sizeof b, "%.1f", v);
    else snprintf(b, sizeof b, "%.2f", v);
    std::string s = b;
    if (!info.unit.empty()) s += (info.unit == "%" ? "%" : " " + info.unit);
    return s;
}

Param* Plugin::add(ParamInfo info) {
    params_.push_back(std::make_unique<Param>(std::move(info)));
    return params_.back().get();
}
Param* Plugin::addFloat(const char* id, const char* name, float min, float max, float def, const char* unit, float skew) {
    ParamInfo i; i.id = id; i.name = name; i.min = min; i.max = max; i.def = def; i.unit = unit; i.skew = skew;
    return add(std::move(i));
}
Param* Plugin::addChoice(const char* id, const char* name, std::vector<std::string> choices, int def) {
    ParamInfo i; i.id = id; i.name = name; i.min = 0; i.max = (float)(choices.size() - 1); i.def = (float)def;
    i.steps = (int)choices.size(); i.choices = std::move(choices);
    return add(std::move(i));
}
Param* Plugin::addBool(const char* id, const char* name, bool def) {
    return addChoice(id, name, {"Off", "On"}, def ? 1 : 0);
}
Param* Plugin::param(const std::string& id) const {
    for (auto& p : params_) if (p->info.id == id) return p.get();
    return nullptr;
}

Json Plugin::saveState() const {
    Json j = Json::object();
    j.set("type", typeId());
    Json ps = Json::object();
    for (auto& p : params_) ps.set(p->info.id, p->get());
    j.set("params", ps);
    Json ex = saveExtra();
    if (!ex.isNull()) j.set("extra", ex);
    return j;
}
void Plugin::loadState(const Json& j, const std::string& projectDir) {
    const Json& ps = j["params"];
    for (auto& p : params_) {
        const Json& v = ps[p->info.id];
        p->set(v.isNull() ? p->info.def : v.f());
    }
    loadExtra(j["extra"], projectDir);
}

void Instrument::process(ProcessContext& c) {
    beginBlock(c);
    int pos = 0;
    for (int i = 0; i < c.numEvents; ++i) {
        const NoteEvent& e = c.events[i];
        const int off = clampT(e.offset, 0, c.numSamples);
        if (off > pos) { render(c.io[0] + pos, c.io[1] + pos, off - pos, c); pos = off; }
        handleEvent(e, c);
    }
    if (pos < c.numSamples) render(c.io[0] + pos, c.io[1] + pos, c.numSamples - pos, c);
}

PluginRegistry& PluginRegistry::global() { static PluginRegistry r; return r; }
void PluginRegistry::add(PluginType t) { types_.push_back(std::move(t)); }
const PluginType* PluginRegistry::find(const std::string& id) const {
    for (auto& t : types_) if (t.id == id) return &t;
    return nullptr;
}
std::shared_ptr<Plugin> PluginRegistry::create(const std::string& id) const {
    if (auto* t = find(id)) return t->create();
    if (external_) return external_(id);
    return nullptr;
}
std::shared_ptr<Plugin> PluginRegistry::createOrMissing(const std::string& id, bool instrument) const {
    if (auto p = create(id)) return p;
    return std::make_shared<MissingPlugin>(id, instrument);
}

} // namespace pulp
