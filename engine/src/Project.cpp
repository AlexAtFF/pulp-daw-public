#include "pulp/Project.h"
#include "pulp/Audio.h"
#include "pulp/Sampler.h"
#include "pulp/Stretch.h"
#include <tuple>
#include <filesystem>

namespace pulp {

void sortNotes(std::vector<Note>& notes) {
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.key < b.key;
    });
}

Project::Project() : global(std::make_shared<GlobalLive>()) {
    tracks.resize(kNumPlaylistTracks);
    initMixer();
    addPattern();
}

Channel& Project::addChannel(std::shared_ptr<Plugin> instrument, const std::string& name) {
    Channel c;
    c.id = nextChannelId_++;
    c.name = name;
    instrument->prepare(sampleRate);
    c.live = std::make_shared<ChannelLive>(std::move(instrument));
    channels.push_back(std::move(c));
    return channels.back();
}

Channel& Project::addSamplerChannel(const std::string& samplePath, const std::string& name) {
    auto s = std::make_shared<Sampler>();
    if (!samplePath.empty()) s->setSample(SampleCache::global().load(samplePath, projectDir));
    return addChannel(s, name);
}

void Project::removeChannel(int index) {
    if (index < 0 || index >= (int)channels.size()) return;
    const int id = channels[(size_t)index].id;
    for (auto& p : patterns) p.notes.erase(id);
    for (auto& c : channels) c.layerChildren.erase(std::remove(c.layerChildren.begin(), c.layerChildren.end(), id), c.layerChildren.end());
    channels.erase(channels.begin() + index);
}

void AudioItem::minMax(int64 from, int64 to, float& mn, float& mx) const {
    mn = mx = 0.0f;
    const int64 n = frames();
    from = std::max<int64>(0, from);
    to = std::min(n, std::max(to, from + 1));
    if (from >= to) return;
    if (sample) {
        const float* d = sample->left();
        const int64 stride = std::max<int64>(1, (to - from) / 24);
        for (int64 i = from; i < to; i += stride) { mn = std::min(mn, d[i]); mx = std::max(mx, d[i]); }
    } else if (stream) {
        const int64 b0 = from / AudioStream::kOverviewBin, b1 = std::min<int64>((int64)stream->ovMin.size() - 1, (to - 1) / AudioStream::kOverviewBin);
        for (int64 b = b0; b <= b1; ++b) { mn = std::min(mn, stream->ovMin[(size_t)b]); mx = std::max(mx, stream->ovMax[(size_t)b]); }
    }
}

// Long files are played from disk, short ones are loaded whole.
bool Project::loadAudioItem(AudioItem& a, std::string* error) const {
    a.sample = nullptr;
    a.stream = nullptr;
    const std::string full = SampleCache::global().resolve(a.path, projectDir);
    WavInfo info;
    if (readWavInfo(full, info) && info.frames() > AudioStream::streamAbove()) {
        a.stream = AudioStream::open(full, error);
        return a.stream != nullptr;
    }
    a.sample = SampleCache::global().load(a.path, projectDir, error);
    return a.sample != nullptr;
}

int Project::addAudio(const std::string& path, std::string* error) {
    for (auto& a : audios) if (a.path == path && a.valid()) return a.id;
    AudioItem a;
    a.path = path;
    if (!loadAudioItem(a, error)) return 0;
    a.id = nextAudioId_++;
    const auto slash = path.find_last_of("/\\:");
    a.name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto dot = a.name.find_last_of('.');
    if (dot != std::string::npos) a.name.erase(dot);
    audios.push_back(std::move(a));
    return audios.back().id;
}

int Project::addAudio(SamplePtr sample, const std::string& name) {
    if (!sample) return 0;
    AudioItem a;
    a.id = nextAudioId_++;
    a.name = name;
    a.path = sample->path;
    a.sample = std::move(sample);
    audios.push_back(std::move(a));
    return audios.back().id;
}

const AudioItem* Project::audioById(int id) const {
    for (auto& a : audios) if (a.id == id) return &a;
    return nullptr;
}

Clip& Project::addAudioClip(int audioId, int track, int64 startTick, int mixerTrack) {
    Clip c;
    c.type = Clip::AudioClip;
    c.track = clampT(track, 0, kNumPlaylistTracks - 1);
    c.start = std::max<int64>(0, startTick);
    c.audioId = audioId;
    c.mixerTrack = clampT(mixerTrack, 0, kNumInserts);
    if (const AudioItem* a = audioById(audioId)) c.length = std::max<int64>(1, secondsToTicks(a->seconds()));
    clips.push_back(c);
    return clips.back();
}

// ------------------------------------------------------------------ automation

static std::vector<std::string> splitAddress(const std::string& a) {
    std::vector<std::string> parts;
    size_t at = 0;
    for (int n = 0; n < 3; ++n) {
        const size_t c = a.find(':', at);
        if (c == std::string::npos) break;
        parts.push_back(a.substr(at, c - at));
        at = c + 1;
    }
    parts.push_back(a.substr(at));
    return parts;
}

Param* Project::findParam(const std::string& address, std::shared_ptr<void>* keep) const {
    auto found = [&](Param* p, std::shared_ptr<void> owner) { if (keep) *keep = std::move(owner); return p; };
    if (address == "tempo") return found(&global->tempo, global);
    if (address == "mastervol") return found(&global->masterVolume, global);
    if (address == "masterpitch") return found(&global->masterPitch, global);
    const auto parts = splitAddress(address);
    if (parts.size() < 3) return nullptr;
    const std::string& kind = parts[0];
    if (kind == "ch" || kind == "chp") {
        const int ci = channelIndex(std::atoi(parts[1].c_str()));
        if (ci < 0) return nullptr;
        const auto& live = channels[(size_t)ci].live;
        if (kind == "ch") { Param* p = live->stripParam(parts[2]); return p ? found(p, live) : nullptr; }
        Param* p = live->plugin->param(parts[2]);
        return p ? found(p, live->plugin) : nullptr;
    }
    const int track = std::atoi(parts[1].c_str());
    if (track < 0 || track >= kNumMixerTracks) return nullptr;
    const MixerTrack& mt = mixer[(size_t)track];
    if (kind == "mix") { Param* p = mt.live->param(parts[2]); return p ? found(p, mt.live) : nullptr; }
    if (kind == "send") {
        const Route* r = mt.routeTo(std::atoi(parts[2].c_str()));
        return r ? found(r->level.get(), r->level) : nullptr;
    }
    if (kind == "fx" && parts.size() == 4) {
        const int slot = std::atoi(parts[2].c_str());
        if (slot < 0 || slot >= kNumSlots || !mt.slots[slot]) return nullptr;
        Param* p = mt.slots[slot]->param(parts[3]);
        return p ? found(p, mt.slots[slot]) : nullptr;
    }
    return nullptr;
}

std::string Project::addressOf(const Param* p) const {
    if (!p) return {};
    if (p == &global->tempo) return "tempo";
    if (p == &global->masterVolume) return "mastervol";
    if (p == &global->masterPitch) return "masterpitch";
    for (auto& c : channels) {
        for (Param* q : c.live->stripParams()) if (q == p) return "ch:" + std::to_string(c.id) + ":" + p->info.id;
        for (auto& q : c.live->plugin->params()) if (q.get() == p) return "chp:" + std::to_string(c.id) + ":" + p->info.id;
    }
    for (int t = 0; t < kNumMixerTracks; ++t) {
        const MixerTrack& mt = mixer[(size_t)t];
        for (Param* q : mt.live->params()) if (q == p) return "mix:" + std::to_string(t) + ":" + p->info.id;
        for (int s = 0; s < kNumSlots; ++s)
            if (mt.slots[s]) for (auto& q : mt.slots[s]->params()) if (q.get() == p) return "fx:" + std::to_string(t) + ":" + std::to_string(s) + ":" + p->info.id;
        for (auto& r : mt.routes) if (r.level.get() == p) return "send:" + std::to_string(t) + ":" + std::to_string(r.to);
    }
    return {};
}

std::string Project::paramLabel(const std::string& address) const {
    Param* p = findParam(address);
    if (!p) return address + " (missing)";
    if (address == "tempo" || address == "mastervol" || address == "masterpitch") return p->info.name;
    const auto parts = splitAddress(address);
    const std::string& kind = parts[0];
    if (kind == "ch" || kind == "chp") {
        const auto& c = channels[(size_t)channelIndex(std::atoi(parts[1].c_str()))];
        return c.name + " - " + p->info.name;
    }
    const int track = std::atoi(parts[1].c_str());
    const std::string tname = mixer[(size_t)track].name.empty() ? mixerTrackName(track) : mixer[(size_t)track].name;
    if (kind == "send") return tname + " - send to " + mixerTrackName(std::atoi(parts[2].c_str()));
    if (kind == "fx") return tname + " - " + mixer[(size_t)track].slots[std::atoi(parts[2].c_str())]->displayName() + " - " + p->info.name;
    return tname + " - " + p->info.name;
}

AutoClip* Project::autoById(int id) { for (auto& a : autos) if (a.id == id) return &a; return nullptr; }
const AutoClip* Project::autoById(int id) const { for (auto& a : autos) if (a.id == id) return &a; return nullptr; }

Clip& Project::addAutomationClip(const std::string& target, int track, int64 startTick, int64 length) {
    AutoClip a;
    a.id = nextAutoId_++;
    a.target = target;
    a.name = paramLabel(target);
    const Param* p = findParam(target);
    const float v = p ? p->getNorm() : 0.5f;
    length = std::max<int64>(kTicksPerStep, length);
    a.points.push_back({0, v, 0.0f, ShapeCurve});
    a.points.push_back({length, v, 0.0f, ShapeCurve});
    autos.push_back(a);
    Clip c;
    c.type = Clip::AutomationClip;
    c.track = clampT(track, 0, kNumPlaylistTracks - 1);
    c.start = std::max<int64>(0, startTick);
    c.length = length;
    c.autoId = a.id;
    clips.push_back(c);
    return clips.back();
}

int Project::makeAutoClipUnique(int clipIndex) {
    if (clipIndex < 0 || clipIndex >= (int)clips.size()) return 0;
    const AutoClip* src = autoById(clips[(size_t)clipIndex].autoId);
    if (!src) return 0;
    AutoClip copy = *src;
    copy.id = nextAutoId_++;
    copy.name += " #2";
    autos.push_back(copy);
    clips[(size_t)clipIndex].autoId = copy.id;
    return copy.id;
}

ParamLink* Project::linkFor(const std::string& target) {
    for (auto& l : links) if (l.target == target) return &l;
    return nullptr;
}
void Project::setLink(const ParamLink& link) {
    if (ParamLink* l = linkFor(link.target)) *l = link;
    else links.push_back(link);
    linkStateCache_.erase(link.target);
}
bool Project::removeLink(const std::string& target) {
    const size_t n = links.size();
    links.erase(std::remove_if(links.begin(), links.end(), [&](const ParamLink& l) { return l.target == target; }), links.end());
    linkStateCache_.erase(target);
    return links.size() != n;
}

bool Project::isAutomated(const std::string& address) const {
    for (auto& l : links) if (l.target == address) return true;
    for (auto& a : autos) if (a.target == address) for (auto& c : clips) if (c.type == Clip::AutomationClip && c.autoId == a.id) return true;
    for (auto& p : patterns) { auto it = p.events.find(address); if (it != p.events.end() && !it->second.empty()) return true; }
    return false;
}

void Project::compileAutomation(RenderState& st) const {
    std::map<std::string, size_t> index;
    auto target = [&](const std::string& address) -> AutoTargetRt* {
        auto it = index.find(address);
        if (it != index.end()) return &st.autoTargets[it->second];
        AutoTargetRt t;
        t.param = findParam(address, &t.keep);
        if (!t.param) return nullptr;
        index[address] = st.autoTargets.size();
        st.autoTargets.push_back(std::move(t));
        return &st.autoTargets.back();
    };
    std::map<const void*, std::shared_ptr<const AutoSourceRt>> sources;
    auto eventSource = [&](const std::vector<AutoEvent>& ev) {
        auto& s = sources[&ev];
        if (!s) { auto n = std::make_shared<AutoSourceRt>(); n->isEvents = true; n->events = ev; s = n; }
        return s;
    };
    if (!songMode) {
        // pattern mode: the selected pattern's events loop with it
        const Pattern& p = patterns[(size_t)clampT(currentPattern, 0, (int)patterns.size() - 1)];
        for (auto& kv : p.events) {
            if (kv.second.empty()) continue;
            if (AutoTargetRt* t = target(kv.first)) t->segments.push_back({0, patternLengthTicks(p), 0, eventSource(kv.second)});
        }
    } else {
        for (auto& c : clips) {
            if (c.muted || c.length <= 0 || !trackAudible(c.track)) continue;
            if (c.type == Clip::AutomationClip) {
                const AutoClip* a = autoById(c.autoId);
                if (!a || a->points.empty()) continue;
                AutoTargetRt* t = target(a->target);
                if (!t) continue;
                auto& s = sources[a];
                if (!s) { auto n = std::make_shared<AutoSourceRt>(); n->points = a->points; n->lfo = a->lfo; n->min = a->min; n->max = a->max; s = n; }
                t->segments.push_back({c.start, c.length, c.offset, s});
            } else if (c.type == Clip::PatternClip) {
                const Pattern* p = patternById(c.patternId);
                if (!p) continue;
                for (auto& kv : p->events) {
                    if (kv.second.empty()) continue;
                    if (AutoTargetRt* t = target(kv.first)) t->segments.push_back({c.start, c.length, c.offset, eventSource(kv.second)});
                }
            }
        }
    }
    for (auto& t : st.autoTargets)
        std::stable_sort(t.segments.begin(), t.segments.end(), [](const AutoSegmentRt& a, const AutoSegmentRt& b) { return a.start < b.start; });

    for (auto& l : links) {
        LinkRt r;
        r.param = findParam(l.target, &r.keep);
        if (!r.param) continue;
        r.kind = l.kind;
        r.midiChannel = l.midiChannel;
        r.cc = clampT(l.cc, 0, 127);
        if (l.kind == ParamLink::Controller) {
            if (l.track < 0 || l.track >= kNumMixerTracks || l.slot < 0 || l.slot >= kNumSlots) continue;
            r.controller = mixer[(size_t)l.track].slots[l.slot];
            if (!r.controller || l.output < 0 || l.output >= r.controller->numControlOutputs()) continue;
            r.output = l.output;
        }
        r.min = l.min; r.max = l.max; r.smoothMs = l.smoothMs;
        if (!l.formula.empty()) r.formula = Formula::compile(l.formula, {"x"});
        auto& state = linkStateCache_[l.target];
        if (!state) state = std::make_shared<LinkStateRt>();
        r.state = state;
        st.links.push_back(std::move(r));
    }
}

// ------------------------------------------------------------------ stretching

namespace {
// Processed audio is kept between compiles (and between undo steps, which rebuild the project):
// stretching a long clip again on every edit would freeze the interface.
struct StretchCache {
    struct Key {
        uint64 uid; int mode; long long ratio; int pitch;
        bool operator<(const Key& o) const { return std::tie(uid, mode, ratio, pitch) < std::tie(o.uid, o.mode, o.ratio, o.pitch); }
    };
    struct Entry { SamplePtr data; uint64 used = 0; };
    std::mutex m;
    std::map<Key, Entry> map;
    uint64 clock = 0;
    SamplePtr get(const AudioItem& a, int mode, double time, double pitchSemis) {
        const uint64 uid = a.sample ? a.sample->uid.v : a.stream->uid.v;
        const Key key{uid, mode, std::llround(time * 1.0e6), (int)std::lround(pitchSemis * 100.0)};
        {
            std::lock_guard<std::mutex> g(m);
            auto it = map.find(key);
            if (it != map.end()) { it->second.used = ++clock; return it->second.data; }
        }
        SamplePtr src = a.inMemory();
        if (!src) return nullptr;
        SamplePtr out = stretch::process(*src, mode, time, pitchSemis);
        std::lock_guard<std::mutex> g(m);
        map[key] = {out, ++clock};
        // keep the cache under about 60 million frames by dropping what was used longest ago
        for (;;) {
            int64 total = 0;
            for (auto& kv : map) total += kv.second.data->frames();
            if (total <= 60000000 || map.size() <= 1) break;
            auto oldest = map.begin();
            for (auto it = map.begin(); it != map.end(); ++it) if (it->second.used < oldest->second.used) oldest = it;
            map.erase(oldest);
        }
        return out;
    }
};
StretchCache& stretchCache() { static StretchCache c; return c; }

// The two numbers the stretcher needs for a clip: time ratio and pitch in semitones.
void stretchParams(const Project& p, const Clip& c, double& time, double& pitch) {
    const AudioItem* a = p.audioById(c.audioId);
    const bool synced = c.sync && a && a->bpm > 0.0;
    time = synced ? a->bpm / (double)p.global->tempo.get() : (double)c.timeMul;
    time = clampT(time, 1.0 / 16.0, 16.0);
    // a synced clip in resample mode takes whatever pitch the speed gives it
    pitch = (synced && c.stretchMode == stretch::Resample) ? 0.0 : (double)c.pitchCents / 100.0;
}
}

double Project::clipLengthRatio(const Clip& c) const {
    double time, pitch;
    stretchParams(*this, c, time, pitch);
    return stretch::lengthRatio(c.stretchMode, time, pitch);
}

bool Project::clipNeedsProcessing(const Clip& c) const {
    double time, pitch;
    stretchParams(*this, c, time, pitch);
    if (std::fabs(stretch::lengthRatio(c.stretchMode, time, pitch) - 1.0) > 1.0e-7) return true;
    return c.stretchMode != stretch::Resample && std::fabs(pitch) > 1.0e-4;
}

int64 Project::fullClipLength(const Clip& c) const {
    const AudioItem* a = audioById(c.audioId);
    if (!a || !a->valid()) return c.length;
    return std::max<int64>(1, secondsToTicks(a->seconds() * clipLengthRatio(c)));
}

void Project::setClipStretch(int clipIndex, int mode, bool sync, float timeMul, float pitchCents) {
    if (clipIndex < 0 || clipIndex >= (int)clips.size()) return;
    Clip& c = clips[(size_t)clipIndex];
    const double before = clipLengthRatio(c);
    c.stretchMode = clampT(mode, 0, (int)stretch::NumModes - 1);
    c.sync = sync;
    c.timeMul = clampT(timeMul, 1.0f / 16.0f, 16.0f);
    c.pitchCents = clampT(pitchCents, -2400.0f, 2400.0f);
    const double k = clipLengthRatio(c) / before;
    c.length = std::max<int64>(1, (int64)std::llround((double)c.length * k));
    c.offset = (int64)std::llround((double)c.offset * k);
    c.fadeIn = (int64)std::llround((double)c.fadeIn * k);
    c.fadeOut = (int64)std::llround((double)c.fadeOut * k);
}

double Project::detectAudioTempo(int audioId) {
    for (auto& a : audios) {
        if (a.id != audioId) continue;
        SamplePtr s = a.inMemory();
        a.bpm = s ? stretch::detectTempo(*s) : 0.0;
        return a.bpm;
    }
    return 0.0;
}

void Project::prefetchAudio(double tick) const {
    const double secPerTick = 60.0 / ((double)global->tempo.get() * kPPQ);
    for (auto& c : clips) {
        if (c.type != Clip::AudioClip || tick < (double)c.start || tick >= (double)c.end()) continue;
        const AudioItem* a = audioById(c.audioId);
        if (!a || !a->stream || clipNeedsProcessing(c)) continue;
        double f = (double)c.sourceOffsetFrames + ((double)c.offset + tick - (double)c.start) * secPerTick * a->rate();
        if (c.reverse) f = (double)a->frames() - 1.0 - f;
        a->stream->prefetch((int64)f);
    }
}

// ------------------------------------------------------------------ takes

int Project::splitClip(int clipIndex, int64 tick) {
    if (clipIndex < 0 || clipIndex >= (int)clips.size()) return -1;
    if (tick <= clips[(size_t)clipIndex].start || tick >= clips[(size_t)clipIndex].end()) return -1;
    Clip tail = clips[(size_t)clipIndex];
    Clip& head = clips[(size_t)clipIndex];
    tail.start = tick;
    tail.offset = head.offset + (tick - head.start);
    tail.length = head.end() - tick;
    tail.fadeIn = 0;
    head.length = tick - head.start;
    head.fadeOut = 0;
    clips.push_back(tail);
    return (int)clips.size() - 1;
}

void Project::useTake(int clipIndex) {
    if (clipIndex < 0 || clipIndex >= (int)clips.size()) return;
    const int group = clips[(size_t)clipIndex].takeGroup;
    const int64 s = clips[(size_t)clipIndex].start, e = clips[(size_t)clipIndex].end();
    clips[(size_t)clipIndex].muted = false;
    if (group == 0) return;
    // the other takes are cut at this clip's two ends and silenced in between
    const size_t count = clips.size();
    for (size_t i = 0; i < count; ++i) {
        if ((int)i == clipIndex || clips[i].takeGroup != group || clips[i].end() <= s || clips[i].start >= e) continue;
        int inside = (int)i;
        if (clips[i].start < s) inside = splitClip((int)i, s);
        if (inside >= 0 && clips[(size_t)inside].end() > e) splitClip(inside, e);
        if (inside >= 0) clips[(size_t)inside].muted = true;
    }
}

Pattern& Project::addPattern(const std::string& name) {
    Pattern p;
    p.id = nextPatternId_++;
    p.name = name.empty() ? "Pattern " + std::to_string(patterns.size() + 1) : name;
    patterns.push_back(std::move(p));
    return patterns.back();
}

int Project::channelIndex(int id) const {
    for (size_t i = 0; i < channels.size(); ++i) if (channels[i].id == id) return (int)i;
    return -1;
}
int Project::patternIndex(int id) const {
    for (size_t i = 0; i < patterns.size(); ++i) if (patterns[i].id == id) return (int)i;
    return -1;
}
Pattern* Project::patternById(int id) { const int i = patternIndex(id); return i < 0 ? nullptr : &patterns[(size_t)i]; }
const Pattern* Project::patternById(int id) const { const int i = patternIndex(id); return i < 0 ? nullptr : &patterns[(size_t)i]; }

bool Project::hasStep(const std::vector<Note>& notes, int step) {
    for (auto& n : notes) if (n.tick / kTicksPerStep == step) return true;
    return false;
}
Note* Project::stepNote(std::vector<Note>& notes, int step) {
    for (auto& n : notes) if (n.tick / kTicksPerStep == step) return &n;
    return nullptr;
}
bool Project::isStepSequence(const std::vector<Note>& notes) {
    int last = -1;
    for (auto& n : notes) {
        const int st = n.tick / kTicksPerStep;
        if (n.length != kTicksPerStep || n.slide || st == last) return false;
        last = st;
    }
    return true;
}
void Project::setStep(std::vector<Note>& notes, int step, bool on) {
    const int t = step * kTicksPerStep;
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const Note& n) { return n.tick / kTicksPerStep == step; }), notes.end());
    if (on) {
        Note n;
        n.tick = t;
        notes.push_back(n);
        sortNotes(notes);
    }
}

int Project::patternLengthTicks(const Pattern& p) const {
    int end = p.steps * kTicksPerStep;
    for (auto& kv : p.notes)
        for (auto& n : kv.second) end = std::max(end, n.end());
    const int bar = std::max(ticksPerBar(), kTicksPerStep);
    return ((end + bar - 1) / bar) * bar;
}

int64 Project::songLengthTicks() const {
    int64 end = 0;
    for (auto& c : clips) end = std::max(end, c.start + c.length);
    const int64 bar = std::max(ticksPerBar(), kTicksPerStep);
    end = ((end + bar - 1) / bar) * bar;
    return std::max(end, bar);
}

int Project::trackGroupHead(int track) const {
    if (track < 0 || track >= (int)tracks.size()) return track;
    int head = track;
    while (head > 0 && tracks[(size_t)head].grouped) --head;
    return head;
}

bool Project::trackIsGroupHead(int track) const {
    return track >= 0 && track + 1 < (int)tracks.size() && !tracks[(size_t)track].grouped && tracks[(size_t)track + 1].grouped;
}

bool Project::trackHidden(int track) const {
    if (track <= 0 || track >= (int)tracks.size() || !tracks[(size_t)track].grouped) return false;
    return tracks[(size_t)trackGroupHead(track)].collapsed;
}

bool Project::trackAudible(int track) const {
    if (track < 0 || track >= (int)tracks.size()) return false;
    // a grouped track takes its mute and solo from the top track of its group
    int head = track;
    while (head > 0 && tracks[(size_t)head].grouped) --head;
    bool anySolo = false;
    for (auto& t : tracks) anySolo = anySolo || t.solo;
    const auto& t = tracks[(size_t)track];
    const auto& h = tracks[(size_t)head];
    if (t.mute || h.mute) return false;
    if (anySolo && !t.solo && !h.solo) return false;
    return true;
}

Clip& Project::addPatternClip(int patternIndex, int track, int64 startTick) {
    Clip c;
    c.type = Clip::PatternClip;
    c.track = clampT(track, 0, kNumPlaylistTracks - 1);
    c.start = std::max<int64>(0, startTick);
    const auto& p = patterns[(size_t)clampT(patternIndex, 0, (int)patterns.size() - 1)];
    c.patternId = p.id;
    c.length = patternLengthTicks(p);
    clips.push_back(c);
    return clips.back();
}

int Project::recordSongNote(int channelId, Note note, int takePatternId) {
    const int64 bar = std::max(ticksPerBar(), kTicksPerStep);
    const int64 start = std::max(0, note.tick), end = start + std::max(1, note.length);
    auto upToBar = [&](int64 t) { return ((t + bar - 1) / bar) * bar; };
    // is the track free of other clips over this stretch?
    auto freeOver = [&](int track, int64 from, int64 to, int except) {
        for (int i = 0; i < (int)clips.size(); ++i)
            if (i != except && clips[(size_t)i].track == track && clips[(size_t)i].start < to && clips[(size_t)i].end() > from) return false;
        return true;
    };
    int at = -1;
    // a clip of the current pattern that is playing here
    for (int i = 0; i < (int)clips.size() && at < 0; ++i) {
        const Clip& c = clips[(size_t)i];
        if (c.type == Clip::PatternClip && c.patternId == pattern().id && !c.muted && start >= c.start && start < c.end()) at = i;
    }
    // the take this recording began earlier: its clip reaches forward to here if nothing is in the way
    if (at < 0 && takePatternId != 0) {
        for (int i = 0; i < (int)clips.size(); ++i) {
            const Clip& c = clips[(size_t)i];
            if (c.type != Clip::PatternClip || c.patternId != takePatternId || c.start > start) continue;
            if (at < 0 || c.start > clips[(size_t)at].start) at = i;
        }
        if (at >= 0 && !freeOver(clips[(size_t)at].track, clips[(size_t)at].end(), upToBar(end), at)) at = -1;
    }
    if (at < 0) {
        // nothing to write into: an unused empty pattern if the current one is that, else a new one
        bool unused = pattern().empty() && pattern().events.empty();
        for (auto& c : clips) if (c.type == Clip::PatternClip && c.patternId == pattern().id) unused = false;
        if (!unused) { addPattern(); currentPattern = (int)patterns.size() - 1; }
        const int64 clipStart = (start / bar) * bar;
        int track = 0;
        while (track < kNumPlaylistTracks - 1 && (tracks[(size_t)track].locked || trackHidden(track) || !freeOver(track, clipStart, upToBar(end), -1))) ++track;
        Clip& c = addPatternClip(currentPattern, track, clipStart);
        c.length = upToBar(end) - clipStart;
        at = (int)clips.size() - 1;
    }
    Clip& c = clips[(size_t)at];
    const int pi = patternIndex(c.patternId);
    if (pi < 0) return 0;
    note.tick = (int)(start - c.start + c.offset);
    auto& notes = patterns[(size_t)pi].notesFor(channelId);
    notes.push_back(note);
    sortNotes(notes);
    // the clip grows to hold the note, by whole bars, while its track is free there
    const int64 need = upToBar(end) - c.start;
    if (need > c.length && freeOver(c.track, c.end(), c.start + need, at)) c.length = need;
    return c.patternId;
}

int Project::makeClipUnique(int clipIndex) {
    if (clipIndex < 0 || clipIndex >= (int)clips.size()) return -1;
    const int pi = patternIndex(clips[(size_t)clipIndex].patternId);
    if (pi < 0) return -1;
    Pattern copy = patterns[(size_t)pi];
    copy.id = nextPatternId_++;
    copy.name += " #2";
    patterns.insert(patterns.begin() + pi + 1, copy);
    if (currentPattern > pi) ++currentPattern;
    clips[(size_t)clipIndex].patternId = copy.id;
    return pi + 1;
}

void Project::prepare(double sr) {
    sampleRate = sr;
    for (auto& c : channels) { c.live->plugin->prepare(sr); c.live->reset(); }
    for (auto& t : mixer) {
        for (auto& s : t.slots) if (s) s->prepare(sr);
        t.live->reset();
    }
    // delay lines and send ramps start clean too
    linkStateCache_.clear();
    edgeCache_.clear();
    channelLineCache_.clear();
    slotDryCache_.clear();
}

// Swing delays the second sixteenth of every pair. The whole timeline is warped, so note
// ends move with note starts. At 100% the off-beat lands half a step late.
static int swingWarp(int tick, float swing) {
    if (swing <= 0.0f) return tick;
    const int pair = 2 * kTicksPerStep;
    const int base = (tick / pair) * pair;
    const double u = tick - base;
    const double d = swing * 0.5 * kTicksPerStep;
    const double w = u < kTicksPerStep ? u * (kTicksPerStep + d) / kTicksPerStep
                                       : kTicksPerStep + d + (u - kTicksPerStep) * (kTicksPerStep - d) / kTicksPerStep;
    return base + (int)std::lround(w);
}

std::shared_ptr<RenderState> Project::compile() const {
    auto st = std::make_shared<RenderState>();
    st->global = global;
    for (auto& c : channels) st->channels.push_back(c.live);
    st->layerTargets.resize(channels.size());
    for (size_t i = 0; i < channels.size(); ++i)
        for (int id : channels[i].layerChildren) {
            const int t = channelIndex(id);
            if (t >= 0 && t != (int)i && channels[(size_t)t].layerChildren.empty()) st->layerTargets[i].push_back(t);
        }
    for (auto& p : patterns) {
        auto pr = std::make_shared<PatternRt>();
        pr->lengthTicks = patternLengthTicks(p);
        pr->channelNotes.resize(channels.size());
        for (size_t ci = 0; ci < channels.size(); ++ci) {
            const auto* src = p.find(channels[ci].id);
            if (!src) continue;
            auto& dst = pr->channelNotes[ci];
            dst.reserve(src->size());
            for (auto& n : *src) {
                if (n.muted) continue;
                NoteRt r;
                const float sw = p.swing * channels[ci].live->swingMix.get();
                r.tick = swingWarp(n.tick, sw);
                r.length = std::max(1, swingWarp(n.end(), sw) - r.tick);
                r.key = (short)n.key;
                r.flags = (unsigned char)((n.slide ? kNoteSlide : 0) | (n.porta ? kNotePorta : 0));
                r.color = (unsigned char)n.color;
                r.velocity = n.velocity; r.pan = n.pan; r.modX = n.modX; r.modY = n.modY; r.fine = n.fine; r.release = n.release;
                dst.push_back(r);
            }
            std::stable_sort(dst.begin(), dst.end(), [](const NoteRt& a, const NoteRt& b) { return a.tick < b.tick; });
        }
        st->patterns.push_back(std::move(pr));
    }
    for (auto& c : clips) {
        if (c.type != Clip::PatternClip || c.muted || c.length <= 0) continue;
        if (!trackAudible(c.track)) continue;
        const int pi = patternIndex(c.patternId);
        if (pi < 0) continue;
        st->clips.push_back({c.start, c.length, c.offset, pi});
    }
    std::stable_sort(st->clips.begin(), st->clips.end(), [](const ClipRt& a, const ClipRt& b) { return a.start < b.start; });
    st->currentPattern = clampT(currentPattern, 0, (int)patterns.size() - 1);
    st->songMode = songMode;
    st->songLengthTicks = songLengthTicks();
    st->loopStart = loopStart;
    st->loopEnd = loopEnd;
    st->patLoopStart = patLoopStart;
    st->patLoopEnd = patLoopEnd;
    st->beatTicks = kPPQ * 4 / std::max(1, timeSigDen);
    st->beatsPerBar = timeSigNum;
    if (recordingLinear) {
        // a take recorded without a loop runs on past the end of the song instead of wrapping
        st->loopStart = st->loopEnd = 0;
        st->songLengthTicks = (int64)1 << 40;
    }
    // audio clips and monitored inputs make their mixer tracks carry sound
    std::vector<int> extra;
    auto audible = [&](const Clip& c) {
        if (c.type != Clip::AudioClip || c.muted || c.length <= 0 || !trackAudible(c.track)) return false;
        const AudioItem* a = audioById(c.audioId);
        return a != nullptr && a->valid();
    };
    for (auto& c : clips) if (audible(c)) extra.push_back(clampT(c.mixerTrack, 0, kNumInserts));
    for (int t = 0; t <= kNumInserts; ++t) if (mixer[(size_t)t].input != 0 && mixer[(size_t)t].monitor) extra.push_back(t);
    // a controller has to run even on a track that carries no sound
    for (int t = 0; t <= kNumInserts; ++t)
        for (auto& sl : mixer[(size_t)t].slots) if (sl && sl->numControlOutputs() > 0) { extra.push_back(t); break; }
    compileAutomation(*st);
    compileMixer(*st, extra);
    std::map<AudioStream*, std::vector<int64>> heads;
    const double secPerTick = 60.0 / ((double)global->tempo.get() * kPPQ);
    for (auto& c : clips) {
        if (!audible(c)) continue;
        const AudioItem* a = audioById(c.audioId);
        AudioClipRt r;
        r.start = c.start; r.length = c.length; r.offset = c.offset; r.offsetFrames = c.sourceOffsetFrames;
        r.gain = c.gain; r.fadeIn = c.fadeIn; r.fadeOut = c.fadeOut; r.reverse = c.reverse;
        if (clipNeedsProcessing(c)) {
            // stretched or pitch-shifted: play the processed copy; positions in the file scale with its length
            double time, pitch;
            stretchParams(*this, c, time, pitch);
            r.sample = stretchCache().get(*a, c.stretchMode, time, pitch);
            r.offsetFrames = (int64)std::llround((double)c.sourceOffsetFrames * stretch::lengthRatio(c.stretchMode, time, pitch));
        } else if (a->stream) r.stream = a->stream;
        else r.sample = a->sample;
        if (!r.sample && !r.stream) continue;
        r.frames = r.sample ? r.sample->frames() : r.stream->frames;
        r.rate = r.sample ? r.sample->sampleRate : r.stream->sampleRate;
        const int track = clampT(c.mixerTrack, 0, kNumInserts);
        for (size_t i = 0; i < st->tracks.size(); ++i) if (st->tracks[i].index == track) r.trackPos = (int)i;
        if (r.trackPos < 0) continue;
        if (r.stream) {
            // the pages a clip starts on stay in memory, so pressing play never waits for the disk
            auto frameAt = [&](int64 tick) {
                double f = (double)r.offsetFrames + (double)(c.offset + tick - c.start) * secPerTick * r.rate;
                if (c.reverse) f = (double)r.frames - 1.0 - f;
                return (int64)f;
            };
            auto& h = heads[r.stream.get()];
            h.push_back(frameAt(c.start));
            if (loopEnd > loopStart && loopStart > c.start && loopStart < c.end()) h.push_back(frameAt(loopStart));
        }
        st->audioClips.push_back(std::move(r));
    }
    for (auto& a : audios) if (a.stream) a.stream->setHeads(heads.count(a.stream.get()) ? heads[a.stream.get()] : std::vector<int64>());
    return st;
}

// ------------------------------------------------------------------ persistence

static Json noteToJson(const Note& n) {
    Json a = Json::array();
    a.push(n.tick); a.push(n.length); a.push(n.key); a.push(n.velocity); a.push(n.pan); a.push(n.release);
    a.push(n.modX); a.push(n.modY); a.push(n.fine); a.push(n.color);
    a.push((n.slide ? 1 : 0) | (n.porta ? 2 : 0) | (n.muted ? 4 : 0));
    return a;
}
static Note noteFromJson(const Json& a) {
    Note n;
    n.tick = a.at(0).i(); n.length = std::max(1, a.at(1).i(kTicksPerStep)); n.key = clampT(a.at(2).i(kRootKey), 0, 127);
    n.velocity = a.at(3).f(kDefaultVelocity); n.pan = a.at(4).f(); n.release = a.at(5).f(0.5f);
    n.modX = a.at(6).f(0.5f); n.modY = a.at(7).f(0.5f); n.fine = a.at(8).f(); n.color = a.at(9).i();
    const int fl = a.at(10).i();
    n.slide = fl & 1; n.porta = fl & 2; n.muted = fl & 4;
    return n;
}

Json Project::toJson() const {
    Json j = Json::object();
    j.set("format", "pulp-project");
    j.set("version", kProjectVersion);
    j.set("name", name);
    j.set("tempo", global->tempo.get());
    j.set("masterVolume", global->masterVolume.get());
    j.set("masterPitch", global->masterPitch.get());
    Json ts = Json::array(); ts.push(timeSigNum); ts.push(timeSigDen);
    j.set("timeSig", ts);
    j.set("songMode", songMode);
    j.set("currentPattern", currentPattern);
    Json lp = Json::array(); lp.push((long long)loopStart); lp.push((long long)loopEnd);
    j.set("loop", lp);

    Json chs = Json::array();
    for (auto& c : channels) {
        Json cj = Json::object();
        cj.set("id", c.id);
        cj.set("name", c.name);
        cj.set("color", (long long)c.color);
        cj.set("mixerTrack", c.mixerTrack);
        if (!c.group.empty()) cj.set("group", c.group);
        if (!c.layerChildren.empty()) { Json lc = Json::array(); for (int id : c.layerChildren) lc.push(id); cj.set("layer", lc); }
        Json strip = Json::object();
        for (Param* p : c.live->stripParams()) strip.set(p->info.id, p->get());
        cj.set("strip", strip);
        cj.set("plugin", c.live->plugin->saveState());
        chs.push(cj);
    }
    j.set("channels", chs);

    Json pats = Json::array();
    for (auto& p : patterns) {
        Json pj = Json::object();
        pj.set("id", p.id);
        pj.set("name", p.name);
        pj.set("color", (long long)p.color);
        pj.set("steps", p.steps);
        pj.set("swing", p.swing);
        Json notes = Json::object();
        for (auto& kv : p.notes) {
            if (kv.second.empty()) continue;
            Json arr = Json::array();
            for (auto& n : kv.second) arr.push(noteToJson(n));
            notes.set(std::to_string(kv.first), arr);
        }
        pj.set("notes", notes);
        if (!p.events.empty()) {
            Json evs = Json::object();
            for (auto& kv : p.events) {
                if (kv.second.empty()) continue;
                Json arr = Json::array();
                for (auto& e : kv.second) { Json ej = Json::array(); ej.push(e.tick); ej.push(e.value); ej.push(e.ramp ? 1 : 0); arr.push(ej); }
                evs.set(kv.first, arr);
            }
            if (evs.size() > 0) pj.set("events", evs);
        }
        if (!p.markers.empty()) {
            Json mk = Json::array();
            for (auto& m : p.markers) { Json mj = Json::object(); mj.set("tick", m.tick); mj.set("name", m.name); mk.push(mj); }
            pj.set("markers", mk);
        }
        pats.push(pj);
    }
    j.set("patterns", pats);

    Json pl = Json::object();
    Json cls = Json::array();
    for (auto& c : clips) {
        Json cj = Json::object();
        cj.set("type", c.type); cj.set("track", c.track);
        cj.set("start", (long long)c.start); cj.set("length", (long long)c.length); cj.set("offset", (long long)c.offset);
        cj.set("pattern", c.patternId); cj.set("muted", c.muted);
        if (c.group != 0) cj.set("group", c.group);
        if (c.type == Clip::AutomationClip) cj.set("auto", c.autoId);
        if (c.type == Clip::AudioClip) {
            cj.set("audio", c.audioId); cj.set("mixerTrack", c.mixerTrack); cj.set("gain", c.gain);
            cj.set("fadeIn", (long long)c.fadeIn); cj.set("fadeOut", (long long)c.fadeOut); cj.set("reverse", c.reverse);
            cj.set("sourceOffsetFrames", (long long)c.sourceOffsetFrames);
            if (c.stretchMode != 0) cj.set("stretch", c.stretchMode);
            if (c.sync) cj.set("sync", true);
            if (c.timeMul != 1.0f) cj.set("timeMul", c.timeMul);
            if (c.pitchCents != 0.0f) cj.set("pitch", c.pitchCents);
            if (c.takeGroup != 0) { cj.set("takeGroup", c.takeGroup); cj.set("take", c.take); }
        }
        cls.push(cj);
    }
    pl.set("clips", cls);
    Json trs = Json::array();
    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& t = tracks[i];
        const PlaylistTrack def;
        if (t.name.empty() && t.color == def.color && !t.mute && !t.solo && !t.locked && t.height == def.height && !t.grouped && !t.collapsed) continue;
        Json tj = Json::object();
        tj.set("index", (int)i); tj.set("name", t.name); tj.set("color", (long long)t.color);
        tj.set("mute", t.mute); tj.set("solo", t.solo); tj.set("locked", t.locked); tj.set("height", t.height);
        tj.set("grouped", t.grouped);
        if (t.collapsed) tj.set("collapsed", true);
        trs.push(tj);
    }
    pl.set("tracks", trs);
    Json mks = Json::array();
    for (auto& m : markers) { Json mj = Json::object(); mj.set("tick", (long long)m.tick); mj.set("name", m.name); mks.push(mj); }
    pl.set("markers", mks);
    j.set("playlist", pl);
    Json aud = Json::array();
    for (auto& a : audios) {
        if (a.path.empty()) continue;       // audio that only exists in memory cannot be saved
        Json aj = Json::object();
        aj.set("id", a.id); aj.set("name", a.name); aj.set("file", a.path);
        if (a.bpm > 0.0) aj.set("bpm", a.bpm);
        aud.push(aj);
    }
    j.set("audio", aud);
    if (!autos.empty()) {
        Json arr = Json::array();
        for (auto& a : autos) {
            Json aj = Json::object();
            aj.set("id", a.id); aj.set("name", a.name); aj.set("color", (long long)a.color); aj.set("target", a.target);
            Json pts = Json::array();
            for (auto& p : a.points) { Json pj = Json::array(); pj.push((long long)p.tick); pj.push(p.value); pj.push(p.tension); pj.push(p.shape); pts.push(pj); }
            aj.set("points", pts);
            if (a.lfo.on || a.lfo.shape != 0 || a.lfo.periodSteps != 16.0f || a.lfo.depth != 0.25f || a.lfo.phase != 0.0f) {
                Json lj = Json::object();
                lj.set("on", a.lfo.on); lj.set("shape", a.lfo.shape); lj.set("period", a.lfo.periodSteps); lj.set("depth", a.lfo.depth); lj.set("phase", a.lfo.phase);
                aj.set("lfo", lj);
            }
            if (a.min != 0.0f) aj.set("min", a.min);
            if (a.max != 1.0f) aj.set("max", a.max);
            arr.push(aj);
        }
        j.set("automation", arr);
    }
    if (!links.empty()) {
        Json arr = Json::array();
        for (auto& l : links) {
            Json lj = Json::object();
            lj.set("target", l.target); lj.set("kind", l.kind);
            if (l.kind == ParamLink::Midi) { lj.set("channel", l.midiChannel); lj.set("cc", l.cc); }
            else { lj.set("track", l.track); lj.set("slot", l.slot); lj.set("output", l.output); }
            lj.set("min", l.min); lj.set("max", l.max);
            if (l.smoothMs != 0.0f) lj.set("smooth", l.smoothMs);
            if (!l.formula.empty()) lj.set("formula", l.formula);
            arr.push(lj);
        }
        j.set("links", arr);
    }
    j.set("mixer", mixerToJson());
    return j;
}

bool Project::fromJson(const Json& original, std::string* error) {
    if (original["format"].str("") != "pulp-project") { if (error) *error = "not a PULP project"; return false; }
    if (original["version"].i(0) > kProjectVersion) { if (error) *error = "this project was saved by a newer version of PULP"; return false; }
    Json upgraded;
    if (original["version"].i(0) != kProjectVersion) {
        upgraded = original;
        if (!migrate(upgraded, error)) { if (error && error->empty()) *error = "could not upgrade this project file"; return false; }
    }
    const Json& j = upgraded.isNull() ? original : upgraded;

    name = j["name"].str("untitled");
    global->tempo.set(j["tempo"].f(140.0f));
    global->masterVolume.set(j["masterVolume"].f(0.8f));
    global->masterPitch.set(j["masterPitch"].f(0.0f));
    timeSigNum = clampT(j["timeSig"].at(0).i(4), 1, 16);
    timeSigDen = clampT(j["timeSig"].at(1).i(4), 1, 16);
    songMode = j["songMode"].b(false);
    loopStart = j["loop"].at(0).i64();
    loopEnd = j["loop"].at(1).i64();

    channels.clear();
    patterns.clear();
    clips.clear();
    audios.clear();
    nextAudioId_ = 1;
    for (auto& aj : j["audio"].elements()) {
        AudioItem a;
        a.id = aj["id"].i(nextAudioId_);
        a.name = aj["name"].str("");
        a.path = aj["file"].str("");
        a.bpm = aj["bpm"].num(0.0);
        loadAudioItem(a, nullptr);          // a missing file leaves the clip silent, not the project unloadable
        nextAudioId_ = std::max(nextAudioId_, a.id + 1);
        audios.push_back(std::move(a));
    }
    markers.clear();
    tracks.assign(kNumPlaylistTracks, PlaylistTrack());
    nextChannelId_ = 1;
    nextPatternId_ = 1;

    for (auto& cj : j["channels"].elements()) {
        const std::string type = cj["plugin"]["type"].str("sampler");
        // an instrument that cannot be made here (a hosted plugin that is not installed) is kept as a silent stand-in
        auto plug = PluginRegistry::global().createOrMissing(type, true);
        plug->prepare(sampleRate);
        plug->loadState(cj["plugin"], projectDir);
        Channel c;
        c.id = cj["id"].i(nextChannelId_);
        c.name = cj["name"].str("");
        c.color = (uint32)cj["color"].i64(c.color);
        c.mixerTrack = clampT(cj["mixerTrack"].i(0), 0, kNumMixerTracks - 1);
        c.group = cj["group"].str("");
        for (auto& lj : cj["layer"].elements()) c.layerChildren.push_back(lj.i());
        c.live = std::make_shared<ChannelLive>(plug);
        for (Param* p : c.live->stripParams()) {
            const Json& v = cj["strip"][p->info.id];
            if (!v.isNull()) p->set(v.f());
        }
        nextChannelId_ = std::max(nextChannelId_, c.id + 1);
        channels.push_back(std::move(c));
    }
    for (auto& pj : j["patterns"].elements()) {
        Pattern p;
        p.id = pj["id"].i(nextPatternId_);
        p.name = pj["name"].str("");
        p.color = (uint32)pj["color"].i64(p.color);
        p.steps = clampT(pj["steps"].i(16), 1, 512);
        p.swing = clampT(pj["swing"].f(0.0f), 0.0f, 1.0f);
        for (auto& kv : pj["notes"].items()) {
            auto& dst = p.notes[std::atoi(kv.first.c_str())];
            for (auto& nj : kv.second.elements()) dst.push_back(noteFromJson(nj));
            sortNotes(dst);
        }
        for (auto& mj : pj["markers"].elements()) p.markers.push_back({mj["tick"].i(), mj["name"].str("")});
        for (auto& kv : pj["events"].items()) {
            auto& dst = p.events[kv.first];
            for (auto& ej : kv.second.elements()) dst.push_back({ej.at(0).i(), clampT(ej.at(1).f(), 0.0f, 1.0f), ej.at(2).i() != 0});
            sortEvents(dst);
        }
        nextPatternId_ = std::max(nextPatternId_, p.id + 1);
        patterns.push_back(std::move(p));
    }
    if (patterns.empty()) addPattern();
    currentPattern = clampT(j["currentPattern"].i(0), 0, (int)patterns.size() - 1);

    const Json& pl = j["playlist"];
    for (auto& cj : pl["clips"].elements()) {
        Clip c;
        c.type = cj["type"].i(0); c.track = clampT(cj["track"].i(0), 0, kNumPlaylistTracks - 1);
        c.start = cj["start"].i64(); c.length = cj["length"].i64(); c.offset = cj["offset"].i64();
        c.patternId = cj["pattern"].i(0); c.muted = cj["muted"].b(false);
        c.group = cj["group"].i(0);
        c.autoId = cj["auto"].i(0);
        c.audioId = cj["audio"].i(0); c.mixerTrack = clampT(cj["mixerTrack"].i(0), 0, kNumInserts); c.gain = cj["gain"].f(1.0f);
        c.fadeIn = cj["fadeIn"].i64(); c.fadeOut = cj["fadeOut"].i64(); c.reverse = cj["reverse"].b(false);
        c.sourceOffsetFrames = cj["sourceOffsetFrames"].i64();
        c.stretchMode = clampT(cj["stretch"].i(0), 0, (int)stretch::NumModes - 1);
        c.sync = cj["sync"].b(false);
        c.timeMul = clampT(cj["timeMul"].f(1.0f), 1.0f / 16.0f, 16.0f);
        c.pitchCents = clampT(cj["pitch"].f(0.0f), -2400.0f, 2400.0f);
        c.takeGroup = cj["takeGroup"].i(0);
        c.take = cj["take"].i(0);
        clips.push_back(c);
    }
    for (auto& tj : pl["tracks"].elements()) {
        const int i = tj["index"].i(-1);
        if (i < 0 || i >= kNumPlaylistTracks) continue;
        auto& t = tracks[(size_t)i];
        t.name = tj["name"].str(""); t.color = (uint32)tj["color"].i64(t.color);
        t.mute = tj["mute"].b(false); t.solo = tj["solo"].b(false); t.locked = tj["locked"].b(false); t.height = tj["height"].f(1.0f);
        t.grouped = tj["grouped"].b(false);
        t.collapsed = tj["collapsed"].b(false);
    }
    for (auto& mj : pl["markers"].elements()) markers.push_back({mj["tick"].i64(), mj["name"].str("")});
    mixerFromJson(j["mixer"]);
    autos.clear();
    nextAutoId_ = 1;
    for (auto& aj : j["automation"].elements()) {
        AutoClip a;
        a.id = aj["id"].i(nextAutoId_);
        a.name = aj["name"].str("");
        a.color = (uint32)aj["color"].i64(a.color);
        a.target = aj["target"].str("");
        for (auto& pj : aj["points"].elements())
            a.points.push_back({pj.at(0).i64(), clampT(pj.at(1).f(0.5f), 0.0f, 1.0f), clampT(pj.at(2).f(), -1.0f, 1.0f), clampT(pj.at(3).i(), 0, (int)NumAutoShapes - 1)});
        std::stable_sort(a.points.begin(), a.points.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.tick < y.tick; });
        if (a.points.empty()) a.points.push_back({0, 0.5f, 0.0f, ShapeCurve});
        const Json& lj = aj["lfo"];
        a.lfo.on = lj["on"].b(false); a.lfo.shape = clampT(lj["shape"].i(0), 0, 3); a.lfo.periodSteps = clampT(lj["period"].f(16.0f), 0.05f, 4096.0f);
        a.lfo.depth = clampT(lj["depth"].f(0.25f), 0.0f, 1.0f); a.lfo.phase = lj["phase"].f(0.0f);
        a.min = clampT(aj["min"].f(0.0f), 0.0f, 1.0f);
        a.max = clampT(aj["max"].f(1.0f), 0.0f, 1.0f);
        nextAutoId_ = std::max(nextAutoId_, a.id + 1);
        autos.push_back(std::move(a));
    }
    links.clear();
    linkStateCache_.clear();
    for (auto& lj : j["links"].elements()) {
        ParamLink l;
        l.target = lj["target"].str("");
        l.kind = lj["kind"].i(0) == ParamLink::Controller ? ParamLink::Controller : ParamLink::Midi;
        l.midiChannel = clampT(lj["channel"].i(-1), -1, 15);
        l.cc = clampT(lj["cc"].i(0), 0, 127);
        l.track = lj["track"].i(0); l.slot = lj["slot"].i(0); l.output = lj["output"].i(0);
        l.min = clampT(lj["min"].f(0.0f), 0.0f, 1.0f);
        l.max = clampT(lj["max"].f(1.0f), 0.0f, 1.0f);
        l.smoothMs = clampT(lj["smooth"].f(0.0f), 0.0f, 5000.0f);
        l.formula = lj["formula"].str("");
        if (!l.target.empty()) links.push_back(std::move(l));
    }
    return true;
}


} // namespace pulp
