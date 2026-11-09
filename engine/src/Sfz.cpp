// SFZ instruments: a text format that maps sample files to keys. This reads the part of it that
// matters for sampled instruments (regions with key and velocity ranges, root key, tuning, level,
// loop points, round robin) into the multisample engine.
#include "pulp/Multisample.h"
#include <cctype>
#include <filesystem>
#include <map>
#include <tuple>

namespace pulp::ms {

namespace fs = std::filesystem;
namespace {

using Opcodes = std::map<std::string, std::string>;

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// "c4", "A#3", "60": a key number, or -1.
int keyValue(const std::string& v) {
    if (v.empty()) return -1;
    if (std::isdigit((unsigned char)v[0]) || v[0] == '-') return clampT(std::atoi(v.c_str()), 0, 127);
    const int k = noteFromName(v, 4);
    return k;
}

// Splits SFZ text into headers and their opcodes. A value runs until the next "name=" or header.
struct Section { std::string header; Opcodes ops; };
std::vector<Section> parse(const std::string& raw) {
    // comments, then #define substitution
    std::string text;
    std::map<std::string, std::string> defines;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();
        std::string line = raw.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t c = line.find("//");
        if (c != std::string::npos) line.erase(c);
        if (line.rfind("#define", 0) == 0) {
            char name[128] = {}, value[256] = {};
            if (sscanf(line.c_str(), "#define %127s %255s", name, value) == 2) defines[name] = value;
            continue;
        }
        if (line.rfind("#include", 0) == 0) continue;       // not followed
        text += line + "\n";
    }
    for (auto& d : defines)
        for (size_t at = text.find(d.first); at != std::string::npos; at = text.find(d.first, at)) { text.replace(at, d.first.size(), d.second); at += d.second.size(); }

    std::vector<Section> out;
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        if (text[i] == '<') {
            const size_t e = text.find('>', i);
            if (e == std::string::npos) break;
            out.push_back({lower(text.substr(i + 1, e - i - 1)), {}});
            i = e + 1;
            continue;
        }
        if (std::isspace((unsigned char)text[i])) { ++i; continue; }
        // an opcode name up to '='
        const size_t eq = text.find('=', i);
        if (eq == std::string::npos) break;
        const size_t lt = text.find('<', i);
        if (lt != std::string::npos && lt < eq) { i = lt; continue; }
        std::string name = lower(text.substr(i, eq - i));
        while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
        // the value ends where the next "word=" begins, at a header, or at the end of the line
        size_t end = eq + 1;
        while (end < n && text[end] != '\n' && text[end] != '<') {
            if (std::isspace((unsigned char)text[end])) {
                size_t w = end;
                while (w < n && text[w] != '\n' && std::isspace((unsigned char)text[w])) ++w;
                size_t t = w;
                while (t < n && (std::isalnum((unsigned char)text[t]) || text[t] == '_')) ++t;
                if (t > w && t < n && text[t] == '=') break;        // a new opcode starts here
                if (w >= n || text[w] == '\n' || text[w] == '<') break;
                end = w;
                continue;
            }
            ++end;
        }
        std::string value = text.substr(eq + 1, end - eq - 1);
        while (!value.empty() && std::isspace((unsigned char)value.back())) value.pop_back();
        if (!out.empty()) out.back().ops[name] = value;
        i = end;
    }
    return out;
}

} // namespace

std::shared_ptr<Instrument> loadSfz(const std::string& path, std::string* error) {
    std::string text;
    if (!readTextFile(path, text)) { if (error) *error = "cannot open " + path; return nullptr; }
    const fs::path file = fs::path(std::u8string(path.begin(), path.end()));
    const fs::path dir = file.parent_path();
    auto inst = std::make_shared<Instrument>();
    { const auto stem = file.stem().u8string(); inst->name = std::string(stem.begin(), stem.end()); }

    Opcodes control, global, master, group;
    std::string defaultPath;
    int looped = 0, missing = 0;
    struct Rr { int group; int count; };
    int nextGroup = 0;
    std::map<std::string, int> rrGroups;        // key range + velocity range -> group id
    float attack = -1, decay = -1, sustain = -1, release = -1;

    for (auto& sec : parse(text)) {
        if (sec.header == "control") { control = sec.ops; if (control.count("default_path")) defaultPath = control["default_path"]; continue; }
        if (sec.header == "global") { global = sec.ops; master.clear(); group.clear(); continue; }
        if (sec.header == "master") { master = sec.ops; group.clear(); continue; }
        if (sec.header == "group") { group = sec.ops; continue; }
        if (sec.header != "region") continue;
        Opcodes op = global;
        for (auto& kv : master) op[kv.first] = kv.second;
        for (auto& kv : group) op[kv.first] = kv.second;
        for (auto& kv : sec.ops) op[kv.first] = kv.second;
        auto get = [&](const char* k, const std::string& def = {}) { auto it = op.find(k); return it == op.end() ? def : it->second; };
        std::string sample = get("sample");
        if (sample.empty() || sample[0] == '*') continue;        // generated waveforms are not samples
        std::replace(sample.begin(), sample.end(), '\\', '/');
        std::string dp = defaultPath;
        std::replace(dp.begin(), dp.end(), '\\', '/');
        const auto full = (dir / fs::path(std::u8string(dp.begin(), dp.end())) / fs::path(std::u8string(sample.begin(), sample.end()))).lexically_normal().generic_u8string();
        Zone z;
        z.file = std::string(full.begin(), full.end());
        z.sample = SampleCache::global().load(z.file);
        if (!z.sample) { ++missing; continue; }
        const int key = keyValue(get("key"));
        z.loKey = key >= 0 ? key : std::max(0, keyValue(get("lokey", "0")));
        z.hiKey = key >= 0 ? key : (keyValue(get("hikey", "127")) < 0 ? 127 : keyValue(get("hikey", "127")));
        const int centre = keyValue(get("pitch_keycenter"));
        z.root = centre >= 0 ? centre : (key >= 0 ? key : 60);
        z.root = clampT(z.root - std::atoi(get("transpose", "0").c_str()), 0, 127);
        z.cents = -(float)std::atof(get("tune", get("pitch", "0")).c_str());      // "tune" raises the playback; our cents say how sharp the recording is
        z.loVel = clampT(std::atoi(get("lovel", "0").c_str()), 0, 127);
        z.hiVel = clampT(std::atoi(get("hivel", "127").c_str()), 0, 127);
        z.gain = dbToGain((float)std::atof(get("volume", "0").c_str()));
        const std::string mode = lower(get("loop_mode"));
        const int64 ls = std::atoll(get("loop_start", "0").c_str()), le = std::atoll(get("loop_end", "0").c_str());
        if ((mode == "loop_continuous" || mode == "loop_sustain" || (mode.empty() && le > ls)) && le > ls + 8 && le < z.sample->frames()) {
            z.loop = true;
            z.loopStart = ls;
            z.loopEnd = le + 1;         // SFZ's loop end is the last sample played
            ++looped;
        }
        const int seqLen = std::max(1, std::atoi(get("seq_length", "1").c_str()));
        if (seqLen > 1) {
            const std::string id = std::to_string(z.loKey) + ":" + std::to_string(z.hiKey) + ":" + std::to_string(z.loVel) + ":" + std::to_string(z.hiVel);
            if (!rrGroups.count(id)) rrGroups[id] = ++nextGroup;
            z.group = rrGroups[id];
            z.groupSize = seqLen;
            z.turn = clampT(std::atoi(get("seq_position", "1").c_str()) - 1, 0, seqLen - 1);
        }
        if (op.count("ampeg_attack")) attack = (float)std::atof(op["ampeg_attack"].c_str()) * 1000.0f;
        if (op.count("ampeg_decay")) decay = (float)std::atof(op["ampeg_decay"].c_str()) * 1000.0f;
        if (op.count("ampeg_sustain")) sustain = (float)std::atof(op["ampeg_sustain"].c_str()) / 100.0f;
        if (op.count("ampeg_release")) release = (float)std::atof(op["ampeg_release"].c_str()) * 1000.0f;
        inst->zones.push_back(std::move(z));
    }
    if (inst->zones.empty()) {
        if (error) *error = missing > 0 ? "the SFZ file's samples could not be found" : "the SFZ file has no sample regions";
        return nullptr;
    }
    std::stable_sort(inst->zones.begin(), inst->zones.end(), [](const Zone& a, const Zone& b) {
        return std::tie(a.loKey, a.loVel, a.turn) < std::tie(b.loKey, b.loVel, b.turn);
    });
    inst->sustain = looped * 2 > (int)inst->zones.size();
    Json p = Json::object();
    p.set("attack", attack >= 0 ? attack : 0.0f);
    p.set("decay", decay >= 0 ? std::max(1.0f, decay) : 600.0f);
    p.set("sustain", sustain >= 0 ? clampT(sustain, 0.0f, 1.0f) : 1.0f);
    p.set("release", release >= 0 ? std::max(1.0f, release) : (inst->sustain ? 220.0f : 450.0f));
    p.set("loop", inst->sustain ? 1.0f : 0.0f);
    inst->params = p;
    return inst;
}

} // namespace pulp::ms
