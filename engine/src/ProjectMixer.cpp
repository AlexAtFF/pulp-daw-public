// The mixer part of the project: routing rules, compiling the graph (processing order and
// plugin delay compensation) and saving / loading mixer tracks.
#include "pulp/Project.h"
#include <functional>

namespace pulp {

static std::shared_ptr<Param> makeSendLevel(float v = 1.0f) {
    ParamInfo i;
    i.id = "send"; i.name = "Send level"; i.min = 0; i.max = 1; i.def = 1; i.unit = "%";
    auto p = std::make_shared<Param>(i);
    p->set(v);
    return p;
}

std::string Project::mixerTrackName(int index) {
    if (index == kMasterTrack) return "Master";
    if (index == kCurrentTrack) return "Current";
    return "Insert " + std::to_string(index);
}

void Project::initMixer() {
    mixer.clear();
    mixer.resize(kNumMixerTracks);
    for (int i = 0; i < kNumMixerTracks; ++i) {
        mixer[(size_t)i].live = std::make_shared<TrackLive>();
        if (i != kMasterTrack && i != kCurrentTrack) mixer[(size_t)i].routes.push_back({kMasterTrack, makeSendLevel(), false});
    }
    edgeCache_.clear();
    channelLineCache_.clear();
    slotDryCache_.clear();
}

bool Project::canRoute(int from, int to) const {
    if (from == to || from < 0 || to < 0 || from >= kNumMixerTracks || to >= kNumMixerTracks) return false;
    if (from == kMasterTrack || from == kCurrentTrack || to == kCurrentTrack) return false;
    // would `to` (already) reach `from`? Then from -> to closes a loop.
    std::vector<char> seen((size_t)kNumMixerTracks, 0);
    std::vector<int> stack = {to};
    while (!stack.empty()) {
        const int t = stack.back();
        stack.pop_back();
        if (t == from) return false;
        if (seen[(size_t)t]) continue;
        seen[(size_t)t] = 1;
        for (auto& r : mixer[(size_t)t].routes) stack.push_back(r.to);
    }
    return true;
}

bool Project::setRoute(int from, int to, bool on, bool sidechain) {
    if (from < 0 || from >= kNumMixerTracks) return false;
    auto& routes = mixer[(size_t)from].routes;
    auto it = std::find_if(routes.begin(), routes.end(), [&](const Route& r) { return r.to == to; });
    if (!on) {
        if (it == routes.end()) return false;
        routes.erase(it);
        return true;
    }
    if (it != routes.end()) { it->sidechain = sidechain; return true; }
    if (!canRoute(from, to)) return false;
    routes.push_back({to, makeSendLevel(), sidechain});
    return true;
}

bool Project::setSlot(int track, int slot, std::shared_ptr<Plugin> plugin) {
    if (track < 0 || track >= kNumMixerTracks || slot < 0 || slot >= kNumSlots) return false;
    if (plugin) plugin->prepare(sampleRate);
    mixer[(size_t)track].slots[slot] = std::move(plugin);
    slotDryCache_.erase({track, slot});
    return true;
}

void Project::keepingAddresses(const std::function<void()>& change) {
    // what each address names now, held by pointer while things move
    struct Held { Param* param = nullptr; std::shared_ptr<void> keep; };
    auto hold = [&](const std::string& address) { Held h; h.param = findParam(address, &h.keep); return h; };
    std::vector<Held> autoHeld, linkHeld;
    for (auto& a : autos) autoHeld.push_back(hold(a.target));
    for (auto& l : links) linkHeld.push_back(hold(l.target));
    std::vector<std::vector<std::pair<std::string, Held>>> eventHeld;
    for (auto& p : patterns) {
        eventHeld.emplace_back();
        for (auto& kv : p.events) eventHeld.back().push_back({kv.first, hold(kv.first)});
    }
    // the controller plugins that links listen to
    std::vector<std::shared_ptr<Plugin>> sources;
    for (auto& l : links)
        sources.push_back(l.kind == ParamLink::Controller && l.track >= 0 && l.track < kNumMixerTracks && l.slot >= 0 && l.slot < kNumSlots
                              ? mixer[(size_t)l.track].slots[l.slot] : nullptr);

    change();

    std::map<std::string, std::string> moved;
    auto now = [&](const std::string& old, const Held& h) {
        const std::string a = h.param ? addressOf(h.param) : std::string();
        if (a.empty() || a == old) return old;
        moved[old] = a;
        return a;
    };
    for (size_t i = 0; i < autos.size(); ++i) autos[i].target = now(autos[i].target, autoHeld[i]);
    for (size_t i = 0; i < links.size(); ++i) {
        links[i].target = now(links[i].target, linkHeld[i]);
        if (!sources[i]) continue;
        for (int t = 0; t < kNumMixerTracks; ++t)
            for (int s = 0; s < kNumSlots; ++s)
                if (mixer[(size_t)t].slots[s] == sources[i]) { links[i].track = t; links[i].slot = s; }
    }
    for (size_t pi = 0; pi < patterns.size(); ++pi) {
        std::map<std::string, std::vector<AutoEvent>> out;
        for (auto& kv : eventHeld[pi]) out[now(kv.first, kv.second)] = std::move(patterns[pi].events[kv.first]);
        patterns[pi].events = std::move(out);
    }
    // a link's smoothing state goes with its parameter
    std::map<std::string, std::shared_ptr<LinkStateRt>> states;
    for (auto& kv : linkStateCache_) { auto it = moved.find(kv.first); states[it == moved.end() ? kv.first : it->second] = kv.second; }
    linkStateCache_ = std::move(states);
}

bool Project::swapMixerTracks(int a, int b) {
    auto insert = [](int t) { return t >= 1 && t <= kNumInserts; };
    if (!insert(a) || !insert(b)) return false;
    if (a == b) return true;
    auto other = [&](int t) { return t == a ? b : (t == b ? a : t); };
    keepingAddresses([&] {
        std::swap(mixer[(size_t)a], mixer[(size_t)b]);
        for (auto& t : mixer) for (auto& r : t.routes) r.to = other(r.to);
    });
    for (auto& c : channels) c.mixerTrack = other(c.mixerTrack);
    for (auto& c : clips) c.mixerTrack = other(c.mixerTrack);
    selectedMixerTrack = other(selectedMixerTrack);
    // the delay lines and send ramps kept between recompiles are filed under track numbers: they move too
    std::map<std::pair<int, int>, std::shared_ptr<EdgeRt>> edges;
    for (auto& kv : edgeCache_) edges[{other(kv.first.first), other(kv.first.second)}] = kv.second;
    edgeCache_ = std::move(edges);
    std::map<std::pair<int, int>, std::shared_ptr<PdcDelay>> dry;
    for (auto& kv : slotDryCache_) dry[{other(kv.first.first), kv.first.second}] = kv.second;
    slotDryCache_ = std::move(dry);
    return true;
}

bool Project::swapSlots(int track, int a, int b) {
    if (track < 0 || track >= kNumMixerTracks || a < 0 || b < 0 || a >= kNumSlots || b >= kNumSlots) return false;
    if (a == b) return true;
    auto& t = mixer[(size_t)track];
    keepingAddresses([&] { std::swap(t.slots[a], t.slots[b]); });
    // each slot's mix and bypass go with its effect
    const float mix = t.live->slotMix[a].get(), bypass = t.live->slotBypass[a].get();
    t.live->slotMix[a].set(t.live->slotMix[b].get()); t.live->slotBypass[a].set(t.live->slotBypass[b].get());
    t.live->slotMix[b].set(mix); t.live->slotBypass[b].set(bypass);
    std::swap(slotDryCache_[{track, a}], slotDryCache_[{track, b}]);
    for (int s : {a, b}) if (!slotDryCache_[{track, s}]) slotDryCache_.erase({track, s});
    return true;
}

int Project::firstFreeMixerTrack() const {
    std::vector<char> used((size_t)kNumMixerTracks, 0);
    for (auto& c : channels) used[(size_t)clampT(c.mixerTrack, 0, kNumMixerTracks - 1)] = 1;
    for (int i = 1; i <= kNumInserts; ++i) {
        const auto& t = mixer[(size_t)i];
        bool hasFx = false;
        for (auto& s : t.slots) hasFx = hasFx || (bool)s;
        bool fed = false;
        for (auto& o : mixer) for (auto& r : o.routes) if (r.to == i) fed = true;
        if (!used[(size_t)i] && !hasFx && !fed && t.name.empty()) return i;
    }
    return -1;
}

void Project::compileMixer(RenderState& st, const std::vector<int>& extraActive) const {
    const int n = kNumMixerTracks;
    // which tracks carry anything
    std::vector<char> active((size_t)n, 0);
    for (int t : extraActive) if (t >= 0 && t < n && t != kCurrentTrack) active[(size_t)t] = 1;     // audio clips and live inputs
    std::vector<std::vector<int>> chans((size_t)n);
    for (size_t c = 0; c < channels.size(); ++c) {
        const int t = clampT(channels[c].mixerTrack, 0, n - 1) == kCurrentTrack ? kMasterTrack : clampT(channels[c].mixerTrack, 0, n - 1);
        chans[(size_t)t].push_back((int)c);
        active[(size_t)t] = 1;
    }
    active[(size_t)kMasterTrack] = 1;
    // activity flows along routes; routing is acyclic, so a few passes settle it
    for (bool changed = true; changed;) {
        changed = false;
        for (int t = 0; t < n; ++t)
            if (active[(size_t)t])
                for (auto& r : mixer[(size_t)t].routes)
                    if (r.to >= 0 && r.to < n && !active[(size_t)r.to]) { active[(size_t)r.to] = 1; changed = true; }
    }
    const int cur = clampT(selectedMixerTrack, 0, n - 1);
    const bool useCurrent = cur != kCurrentTrack && active[(size_t)cur];
    if (useCurrent) active[(size_t)kCurrentTrack] = 1;

    // inputs of each track
    struct In { int from; std::shared_ptr<Param> level; bool sidechain; };
    std::vector<std::vector<In>> ins((size_t)n);
    for (int t = 0; t < n; ++t)
        if (active[(size_t)t])
            for (auto& r : mixer[(size_t)t].routes)
                if (r.to >= 0 && r.to < n && r.to != kCurrentTrack) ins[(size_t)r.to].push_back({t, r.level, r.sidechain});
    if (useCurrent) ins[(size_t)kCurrentTrack].push_back({cur, nullptr, false});

    // depth in the graph and latency at each track's output
    std::vector<int> depth((size_t)n, -1), outLat((size_t)n, 0), inLat((size_t)n, 0);
    std::function<void(int)> visit = [&](int t) {
        if (depth[(size_t)t] >= 0) return;
        depth[(size_t)t] = 0;       // guards against a loop slipping in through a damaged file
        int d = 0, lat = 0;
        for (auto& in : ins[(size_t)t]) {
            visit(in.from);
            d = std::max(d, depth[(size_t)in.from] + 1);
            lat = std::max(lat, outLat[(size_t)in.from]);
        }
        lat = std::min(lat, kMaxPdcSamples);
        int own = 0;
        for (auto& s : mixer[(size_t)t].slots) if (s) own += std::max(0, s->latencySamples());
        depth[(size_t)t] = d;
        inLat[(size_t)t] = lat;
        outLat[(size_t)t] = lat + own;
    };
    for (int t = 0; t < n; ++t) if (active[(size_t)t]) visit(t);

    std::vector<int> order;
    for (int t = 0; t < n; ++t) if (active[(size_t)t]) order.push_back(t);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return depth[(size_t)a] < depth[(size_t)b]; });
    std::vector<int> posOf((size_t)n, -1);
    for (size_t i = 0; i < order.size(); ++i) posOf[(size_t)order[i]] = (int)i;

    st.tracks.clear();
    st.levelStart.clear();
    int lastDepth = -1;
    for (size_t i = 0; i < order.size(); ++i) {
        const int t = order[i];
        if (depth[(size_t)t] != lastDepth) { st.levelStart.push_back((int)i); lastDepth = depth[(size_t)t]; }
        TrackRt rt;
        rt.index = t;
        rt.live = mixer[(size_t)t].live;
        for (int s = 0; s < kNumSlots; ++s) {
            rt.slots[s] = mixer[(size_t)t].slots[s];
            const int lat = rt.slots[s] ? std::min(std::max(0, rt.slots[s]->latencySamples()), kMaxPdcSamples) : 0;
            rt.slotLatency[s] = lat;
            if (lat > 0) {
                auto& d = slotDryCache_[{t, s}];
                if (!d) d = std::make_shared<PdcDelay>();
                rt.slotDry[s] = d;
            }
        }
        rt.channels = chans[(size_t)t];
        rt.channelDelay = inLat[(size_t)t];
        rt.usesPre = std::find(extraActive.begin(), extraActive.end(), t) != extraActive.end();
        rt.input = mixer[(size_t)t].input;
        rt.monitor = mixer[(size_t)t].monitor;
        rt.hwOut = mixer[(size_t)t].output;
        if (rt.channelDelay > 0 && (!rt.channels.empty() || rt.usesPre)) {
            auto& d = channelLineCache_[t];
            if (!d) d = std::make_shared<PdcDelay>();
            rt.channelLine = d;
        }
        for (auto& in : ins[(size_t)t]) {
            TrackInRt e;
            e.from = posOf[(size_t)in.from];
            e.level = in.level;
            e.sidechain = in.sidechain;
            e.delay = clampT(inLat[(size_t)t] - outLat[(size_t)in.from], 0, kMaxPdcSamples);
            auto& cached = edgeCache_[{in.from, t}];
            if (!cached) cached = std::make_shared<EdgeRt>();
            if (e.delay > 0 && !cached->delay) cached->delay = std::make_unique<PdcDelay>();
            e.rt = cached;
            rt.inputs.push_back(std::move(e));
        }
        st.tracks.push_back(std::move(rt));
    }
    st.levelStart.push_back((int)order.size());
    st.masterPos = posOf[(size_t)kMasterTrack];
    st.latency = outLat[(size_t)kMasterTrack];
}

// ------------------------------------------------------------------ persistence

Json Project::mixerToJson() const {
    Json arr = Json::array();
    TrackLive defaults;
    auto defParams = defaults.params();
    for (int i = 0; i < kNumMixerTracks; ++i) {
        const auto& t = mixer[(size_t)i];
        Json tj = Json::object();
        bool any = false;
        if (!t.name.empty()) { tj.set("name", t.name); any = true; }
        if (t.color != MixerTrack().color) { tj.set("color", (long long)t.color); any = true; }
        if (t.dock != 0) { tj.set("dock", t.dock); any = true; }
        if (t.input != 0) { tj.set("input", t.input); any = true; }
        if (t.monitor) { tj.set("monitor", true); any = true; }
        if (t.output != 0) { tj.set("output", t.output); any = true; }
        Json ps = Json::object();
        auto params = t.live->params();
        for (size_t k = 0; k < params.size(); ++k)
            if (params[k]->get() != defParams[k]->info.def) ps.set(params[k]->info.id, params[k]->get());
        if (ps.size() > 0) { tj.set("params", ps); any = true; }
        Json slots = Json::array();
        for (int s = 0; s < kNumSlots; ++s)
            if (t.slots[s]) { Json sj = Json::object(); sj.set("slot", s); sj.set("plugin", t.slots[s]->saveState()); slots.push(sj); }
        if (slots.size() > 0) { tj.set("slots", slots); any = true; }
        const bool isInsert = i != kMasterTrack && i != kCurrentTrack;
        const bool defaultRoutes = isInsert ? (t.routes.size() == 1 && t.routes[0].to == kMasterTrack && !t.routes[0].sidechain && t.routes[0].level->get() == 1.0f)
                                            : t.routes.empty();
        if (!defaultRoutes) {
            Json rs = Json::array();
            for (auto& r : t.routes) { Json rj = Json::object(); rj.set("to", r.to); rj.set("level", r.level->get()); rj.set("sidechain", r.sidechain); rs.push(rj); }
            tj.set("routes", rs);
            any = true;
        }
        if (!any) continue;
        tj.set("index", i);
        arr.push(tj);
    }
    return arr;
}

void Project::mixerFromJson(const Json& j) {
    initMixer();
    for (auto& tj : j.elements()) {
        const int i = tj["index"].i(-1);
        if (i < 0 || i >= kNumMixerTracks) continue;
        auto& t = mixer[(size_t)i];
        t.name = tj["name"].str("");
        t.color = (uint32)tj["color"].i64(t.color);
        t.dock = clampT(tj["dock"].i(0), -1, 1);
        t.input = clampT(tj["input"].i(0), 0, 3);
        t.monitor = tj["monitor"].b(false);
        t.output = clampT(tj["output"].i(0), 0, 4);
        for (Param* p : t.live->params()) {
            const Json& v = tj["params"][p->info.id];
            if (!v.isNull()) p->set(v.f());
        }
        for (auto& sj : tj["slots"].elements()) {
            const int s = sj["slot"].i(-1);
            if (s < 0 || s >= kNumSlots || sj["plugin"]["type"].str("").empty()) continue;
            auto plug = PluginRegistry::global().createOrMissing(sj["plugin"]["type"].str(""), false);
            plug->prepare(sampleRate);
            plug->loadState(sj["plugin"], projectDir);
            t.slots[s] = plug;
        }
        if (tj.has("routes")) {
            t.routes.clear();
            for (auto& rj : tj["routes"].elements()) {
                const int to = rj["to"].i(-1);
                if (to < 0 || to >= kNumMixerTracks || !canRoute(i, to)) continue;      // a damaged file cannot smuggle in a loop
                t.routes.push_back({to, makeSendLevel(clampT(rj["level"].f(1.0f), 0.0f, 1.0f)), rj["sidechain"].b(false)});
            }
        }
    }
}

} // namespace pulp
