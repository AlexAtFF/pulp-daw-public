#include "pulp/SoundFont.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace pulp::sf2 {

namespace {
struct Gen { unsigned short oper; short amount; unsigned lo() const { return (unsigned)amount & 0xff; } unsigned hi() const { return ((unsigned)amount >> 8) & 0xff; } };
struct Bag { unsigned short gen; };
struct Preset { std::string name; unsigned short program, bank, bag; };
struct Inst { std::string name; unsigned short bag; };
struct Sample { std::string name; unsigned start, end, loopStart, loopEnd, rate; int pitch, correction; unsigned short link, type; };

unsigned u16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
unsigned u32(const unsigned char* p) { return u16(p) | (u16(p + 2) << 16); }
std::string fixed(const unsigned char* p, size_t n) {
    std::string s((const char*)p, n);
    const size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// generator numbers used here
enum { kStartOffs = 0, kEndOffs = 1, kStartLoopOffs = 2, kEndLoopOffs = 3, kStartCoarse = 4, kEndCoarse = 12, kPan = 17, kReleaseVol = 38, kInstrument = 41,
       kKeyRange = 43, kVelRange = 44, kStartLoopCoarse = 45, kAttenuation = 48, kEndLoopCoarse = 50, kCoarseTune = 51, kFineTune = 52, kSampleId = 53,
       kSampleModes = 54, kRootKey = 58 };

struct Zone {
    std::map<int, Gen> gens;
    bool has(int g) const { return gens.count(g) != 0; }
    int value(int g, int def = 0) const { auto it = gens.find(g); return it == gens.end() ? def : (int)it->second.amount; }
};
} // namespace

struct File {
    std::vector<Preset> presets;
    std::vector<Bag> pbag, ibag;
    std::vector<Gen> pgen, igen;
    std::vector<Inst> insts;
    std::vector<Sample> samples;
    std::vector<short> pcm;
    std::string name;
};

std::shared_ptr<const File> openMemory(const void* data, size_t size, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return std::shared_ptr<const File>(); };
    const unsigned char* d = (const unsigned char*)data;
    if (size < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "sfbk", 4) != 0) return fail("not a SoundFont file");
    auto f = std::make_shared<File>();
    // walk the chunks, descending into LIST chunks
    std::map<std::string, std::pair<const unsigned char*, size_t>> chunks;
    std::vector<std::pair<size_t, size_t>> todo{{12, size}};
    while (!todo.empty()) {
        auto [pos, end] = todo.back();
        todo.pop_back();
        while (pos + 8 <= end) {
            const std::string id((const char*)d + pos, 4);
            const size_t len = u32(d + pos + 4);
            if (pos + 8 + len > end) break;
            if (id == "LIST") { if (len >= 4) todo.push_back({pos + 12, pos + 8 + len}); }
            else chunks[id] = {d + pos + 8, len};
            pos += 8 + len + (len & 1);
        }
    }
    auto get = [&](const char* id, size_t record) -> std::pair<const unsigned char*, size_t> {
        auto it = chunks.find(id);
        if (it == chunks.end()) return {nullptr, 0};
        return {it->second.first, it->second.second / record};
    };
    if (auto c = chunks.find("INAM"); c != chunks.end()) f->name = fixed(c->second.first, c->second.second);
    if (auto [p, n] = get("smpl", 2); p) {
        f->pcm.resize(n);
        for (size_t i = 0; i < n; ++i) f->pcm[i] = (short)u16(p + i * 2);
    }
    if (auto [p, n] = get("phdr", 38); p) for (size_t i = 0; i < n; ++i) { const auto* r = p + i * 38; f->presets.push_back({fixed(r, 20), (unsigned short)u16(r + 20), (unsigned short)u16(r + 22), (unsigned short)u16(r + 24)}); }
    if (auto [p, n] = get("pbag", 4); p) for (size_t i = 0; i < n; ++i) f->pbag.push_back({(unsigned short)u16(p + i * 4)});
    if (auto [p, n] = get("pgen", 4); p) for (size_t i = 0; i < n; ++i) f->pgen.push_back({(unsigned short)u16(p + i * 4), (short)u16(p + i * 4 + 2)});
    if (auto [p, n] = get("inst", 22); p) for (size_t i = 0; i < n; ++i) f->insts.push_back({fixed(p + i * 22, 20), (unsigned short)u16(p + i * 22 + 20)});
    if (auto [p, n] = get("ibag", 4); p) for (size_t i = 0; i < n; ++i) f->ibag.push_back({(unsigned short)u16(p + i * 4)});
    if (auto [p, n] = get("igen", 4); p) for (size_t i = 0; i < n; ++i) f->igen.push_back({(unsigned short)u16(p + i * 4), (short)u16(p + i * 4 + 2)});
    if (auto [p, n] = get("shdr", 46); p)
        for (size_t i = 0; i < n; ++i) {
            const auto* r = p + i * 46;
            f->samples.push_back({fixed(r, 20), u32(r + 20), u32(r + 24), u32(r + 28), u32(r + 32), u32(r + 36), (int)r[40], (int)(signed char)r[41], (unsigned short)u16(r + 42), (unsigned short)u16(r + 44)});
        }
    // each list ends with a terminator record; a file without the lists it needs is no use
    if (f->presets.size() < 2 || f->pbag.empty() || f->insts.size() < 2 || f->ibag.empty() || f->samples.empty() || f->pcm.empty()) return fail("the SoundFont holds no presets with samples");
    return f;
}

std::shared_ptr<const File> open(const std::string& path, std::string* error) {
    std::vector<unsigned char> bytes;
    if (!readFileBytes(path, bytes)) { if (error) *error = "cannot open " + path; return nullptr; }
    return openMemory(bytes.data(), bytes.size(), error);
}

std::vector<PresetInfo> presets(const File& f) {
    std::vector<PresetInfo> out;
    for (size_t i = 0; i + 1 < f.presets.size(); ++i) out.push_back({f.presets[i].name, f.presets[i].bank, f.presets[i].program, (int)i});
    std::stable_sort(out.begin(), out.end(), [](const PresetInfo& a, const PresetInfo& b) { return a.bank != b.bank ? a.bank < b.bank : a.program < b.program; });
    return out;
}

std::shared_ptr<ms::Instrument> build(const File& f, int presetIndex, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return std::shared_ptr<ms::Instrument>(); };
    if (presetIndex < 0 || presetIndex + 1 >= (int)f.presets.size()) return fail("no such preset");
    auto zonesOf = [](const std::vector<Bag>& bags, const std::vector<Gen>& gens, unsigned from, unsigned to) {
        std::vector<Zone> out;
        for (unsigned b = from; b < to && b + 1 < bags.size(); ++b) {
            Zone z;
            for (unsigned g = bags[b].gen; g < bags[b + 1].gen && g < gens.size(); ++g) z.gens[gens[g].oper] = gens[g];
            out.push_back(std::move(z));
        }
        return out;
    };
    auto inst = std::make_shared<ms::Instrument>();
    inst->name = f.presets[(size_t)presetIndex].name;
    inst->category = "SoundFont";
    inst->credit = f.name.empty() ? std::string("from a SoundFont") : "from the SoundFont " + f.name;
    std::map<unsigned, SamplePtr> made;          // sample index -> its audio (stereo pairs joined under the left one)
    double releaseSum = 0.0;
    int releaseCount = 0;
    bool anyLoop = false;

    auto pzones = zonesOf(f.pbag, f.pgen, f.presets[(size_t)presetIndex].bag, f.presets[(size_t)presetIndex + 1].bag);
    Zone pglobal;
    if (!pzones.empty() && !pzones[0].has(kInstrument)) { pglobal = pzones[0]; pzones.erase(pzones.begin()); }
    for (auto& pz : pzones) {
        if (!pz.has(kInstrument)) continue;
        const unsigned ii = (unsigned)(unsigned short)pz.value(kInstrument);
        if (ii + 1 >= f.insts.size()) continue;
        auto izones = zonesOf(f.ibag, f.igen, f.insts[ii].bag, f.insts[ii + 1].bag);
        Zone iglobal;
        if (!izones.empty() && !izones[0].has(kSampleId)) { iglobal = izones[0]; izones.erase(izones.begin()); }
        // which samples this instrument uses, to pair left with right
        std::map<unsigned, bool> used;
        for (auto& iz : izones) if (iz.has(kSampleId)) used[(unsigned)(unsigned short)iz.value(kSampleId)] = true;
        for (auto& iz : izones) {
            if (!iz.has(kSampleId)) continue;
            const unsigned si = (unsigned)(unsigned short)iz.value(kSampleId);
            if (si + 1 >= f.samples.size() + 1 || si >= f.samples.size()) continue;
            const Sample& sh = f.samples[si];
            if (sh.type & 0x8000) continue;                                             // a sample in ROM: not in the file
            const bool right = (sh.type & 2) != 0, left = (sh.type & 4) != 0;
            if (right && sh.link < f.samples.size() && used.count(sh.link) && (f.samples[sh.link].type & 4)) continue;   // comes in with its left half
            // instrument generators, with the instrument's global zone as the default; preset generators add to them
            auto ig = [&](int g, int def = 0) { return iz.has(g) ? iz.value(g) : iglobal.value(g, def); };
            auto pg = [&](int g) { return pz.has(g) ? pz.value(g) : pglobal.value(g, 0); };
            auto range = [&](int g, int& lo, int& hi) {
                lo = 0; hi = 127;
                const Zone* src = iz.has(g) ? &iz : (iglobal.has(g) ? &iglobal : nullptr);
                if (src) { lo = (int)src->gens.at(g).lo(); hi = (int)src->gens.at(g).hi(); }
                const Zone* psrc = pz.has(g) ? &pz : (pglobal.has(g) ? &pglobal : nullptr);
                if (psrc) { lo = std::max(lo, (int)psrc->gens.at(g).lo()); hi = std::min(hi, (int)psrc->gens.at(g).hi()); }
            };
            ms::Zone z;
            range(kKeyRange, z.loKey, z.hiKey);
            range(kVelRange, z.loVel, z.hiVel);
            if (z.loKey > z.hiKey || z.loVel > z.hiVel) continue;
            long long start = (long long)sh.start + ig(kStartOffs) + 32768LL * ig(kStartCoarse);
            long long end = (long long)sh.end + ig(kEndOffs) + 32768LL * ig(kEndCoarse);
            long long loopA = (long long)sh.loopStart + ig(kStartLoopOffs) + 32768LL * ig(kStartLoopCoarse);
            long long loopB = (long long)sh.loopEnd + ig(kEndLoopOffs) + 32768LL * ig(kEndLoopCoarse);
            start = clampT<long long>(start, 0, (long long)f.pcm.size());
            end = clampT<long long>(end, start, (long long)f.pcm.size());
            if (end - start < 8) continue;
            auto it = made.find(si);
            if (it == made.end()) {
                auto s = std::make_shared<SampleData>();
                s->sampleRate = sh.rate > 0 ? (double)sh.rate : 44100.0;
                s->channels = 1;
                s->ch[0].resize((size_t)(end - start));
                for (long long i = start; i < end; ++i) s->ch[0][(size_t)(i - start)] = (float)f.pcm[(size_t)i] / 32768.0f;
                // the other half of a stereo pair, when it is the same length
                if (left && sh.link < f.samples.size() && used.count(sh.link) && (f.samples[sh.link].type & 2)) {
                    const Sample& rh = f.samples[sh.link];
                    if ((long long)rh.end - (long long)rh.start == (long long)sh.end - (long long)sh.start && rh.end <= f.pcm.size()) {
                        const long long rstart = (long long)rh.start + (start - (long long)sh.start);
                        if (rstart >= 0 && rstart + (end - start) <= (long long)f.pcm.size()) {
                            s->channels = 2;
                            s->ch[1].resize(s->ch[0].size());
                            for (long long i = 0; i < end - start; ++i) s->ch[1][(size_t)i] = (float)f.pcm[(size_t)(rstart + i)] / 32768.0f;
                        }
                    }
                }
                it = made.emplace(si, s).first;
            }
            z.sample = it->second;
            const int root = ig(kRootKey, -1);
            z.root = clampT(root >= 0 && root <= 127 ? root : (sh.pitch <= 127 ? sh.pitch : 60), 0, 127);
            // cents says how sharp the recording is: playback tunes it back. A SoundFont states the correction to apply.
            const double tune = (double)(ig(kCoarseTune) + pg(kCoarseTune)) * 100.0 + (double)(ig(kFineTune) + pg(kFineTune)) + (double)sh.correction;
            z.cents = (float)-tune;
            const int mode = ig(kSampleModes) & 3;
            if ((mode == 1 || mode == 3) && loopB - loopA >= 8 && loopA >= start && loopB <= end) {
                z.loop = true;
                z.loopStart = loopA - start;
                z.loopEnd = loopB - start;
                anyLoop = true;
            }
            const double attenuation = (double)(ig(kAttenuation) + pg(kAttenuation)) * 0.04;     // centibels, and as most players weigh it
            z.gain = (float)std::pow(10.0, -clampT(attenuation, 0.0, 96.0) / 20.0);
            inst->zones.push_back(std::move(z));
            const int rel = iz.has(kReleaseVol) || iglobal.has(kReleaseVol) ? ig(kReleaseVol) : -12000;
            releaseSum += std::pow(2.0, (double)clampT(rel, -12000, 8000) / 1200.0);
            ++releaseCount;
        }
    }
    if (inst->zones.empty()) return fail("that preset has no samples in the file");
    std::stable_sort(inst->zones.begin(), inst->zones.end(), [](const ms::Zone& a, const ms::Zone& b) { return a.loKey != b.loKey ? a.loKey < b.loKey : a.loVel < b.loVel; });
    inst->sustain = anyLoop;
    Json params = Json::object();
    params.set("release", clampT(1000.0 * releaseSum / std::max(1, releaseCount), 20.0, 4000.0));
    params.set("loop", anyLoop ? 1.0 : 0.0);
    inst->params = params;
    return inst;
}

} // namespace pulp::sf2
