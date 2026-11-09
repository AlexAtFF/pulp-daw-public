// Project files on disk: saving with portable sample paths, packing a project and its
// audio into one archive, unpacking, and upgrading files written by older versions.
#include "pulp/Audio.h"
#include "pulp/Editor.h"
#include "pulp/Project.h"
#include "pulp/Zip.h"
#include <filesystem>
#include <functional>
#include <set>

namespace pulp {

namespace fs = std::filesystem;
static fs::path u8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
static std::string str(const fs::path& p) { const auto u = p.generic_u8string(); return std::string(u.begin(), u.end()); }

// Keys under which plugins store file references. Their values are paths.
static bool isFileKey(const std::string& key) { return key == "sample" || key == "file" || key == "ir" || key == "wavetable"; }

// Returns a copy of the document with every file reference passed through fn.
static Json remapFiles(const Json& j, const std::function<std::string(const std::string&)>& fn, const std::string& key = {}) {
    if (j.isString()) return isFileKey(key) && !j.str().empty() ? Json(fn(j.str())) : j;
    if (j.isObject()) {
        Json o = Json::object();
        for (auto& kv : j.items()) o.set(kv.first, remapFiles(kv.second, fn, kv.first));
        return o;
    }
    if (j.isArray()) {
        Json a = Json::array();
        for (auto& e : j.elements()) a.push(remapFiles(e, fn, key));
        return a;
    }
    return j;
}

std::vector<std::string> Project::fileRefs() const {
    std::set<std::string> seen;
    std::vector<std::string> out;
    remapFiles(toJson(), [&](const std::string& p) {
        if (p.rfind("factory:", 0) != 0 && p.rfind("mem:", 0) != 0 && seen.insert(p).second) out.push_back(p);
        return p;
    });
    return out;
}

Json Project::toPortableJson() const {
    return remapFiles(toJson(), [&](const std::string& p) { return p.rfind("factory:", 0) == 0 ? p : SampleCache::global().resolve(p, projectDir); });
}

int Project::flushEditors(const std::string& dir) {
    int written = 0;
    for (auto& t : mixer)
        for (auto& sl : t.slots) {
            auto* ed = dynamic_cast<Splice*>(sl.get());
            if (!ed || !ed->unsaved() || !ed->audio()) continue;
            std::error_code ec;
            fs::create_directories(u8p(dir) / "edits", ec);
            fs::path file;
            for (int n = 1; n < 100000; ++n) {
                file = u8p(dir) / "edits" / ("edit_" + std::to_string(n) + ".wav");
                if (!fs::exists(file, ec)) break;
            }
            const std::string path = str(file);
            if (!writeWavFile(path, *ed->audio(), WavFormat::Float32)) continue;
            SampleCache::global().put(path, ed->audio());
            ed->markSaved(path);
            ++written;
        }
    return written;
}

// Upgrades a document in place, one version at a time.
bool Project::migrate(Json& j, std::string* error) {
    int version = j["version"].i(0);
    if (version < 1) { if (error) *error = "this file is too old or is not a PULP project"; return false; }
    if (version == 1) {
        // Version 1 (before the mixer existed) has no "mixer" entry and its channels all play on the master.
        if (!j.has("mixer")) j.set("mixer", Json::array());
        version = 2;
    }
    j.set("version", version);
    return version == kProjectVersion;
}

bool Project::save(const std::string& path, std::string* error) {
    std::error_code ec;
    const fs::path file = u8p(path);
    const fs::path dir = fs::absolute(file, ec).parent_path();
    projectDir = str(dir);
    flushEditors(projectDir);           // audio recorded or edited in an editor goes into the project's folder first
    // Samples inside the project's folder are stored by relative path so the folder can be moved.
    const Json doc = remapFiles(toJson(), [&](const std::string& p) {
        if (p.rfind("factory:", 0) == 0) return p;
        const fs::path sp = u8p(p);
        if (!sp.is_absolute()) return p;
        const fs::path rel = sp.lexically_relative(dir);
        const std::string r = str(rel);
        return (rel.empty() || r.rfind("..", 0) == 0) ? p : r;
    });
    if (!writeTextFile(path, doc.dump(1))) { if (error) *error = "cannot write " + path; return false; }
    return true;
}

bool Project::load(const std::string& path, std::string* error) {
    std::string text;
    if (!readTextFile(path, text)) { if (error) *error = "cannot open " + path; return false; }
    std::string perr;
    Json j = Json::parse(text, &perr);
    if (!perr.empty()) { if (error) *error = "project file is damaged: " + perr; return false; }
    std::error_code ec;
    projectDir = str(fs::absolute(u8p(path), ec).parent_path());
    return fromJson(j, error);
}

bool Project::pack(const std::string& zipPath, std::string* error) const {
    std::vector<ZipEntry> entries;
    std::map<std::string, std::string> packed;      // original reference -> path inside the archive
    std::set<std::string> names;
    for (auto& ref : fileRefs()) {
        const std::string full = SampleCache::global().resolve(ref, projectDir);
        ZipEntry e;
        if (!readFileBytes(full, e.data)) { if (error) *error = "cannot read " + full; return false; }
        std::string base = str(u8p(full).filename());
        std::string name = "samples/" + base;
        for (int n = 2; names.count(name); ++n) name = "samples/" + std::to_string(n) + "_" + base;
        names.insert(name);
        e.name = name;
        packed[ref] = name;
        entries.push_back(std::move(e));
    }
    const Json doc = remapFiles(toJson(), [&](const std::string& p) { auto it = packed.find(p); return it == packed.end() ? p : it->second; });
    ZipEntry proj;
    proj.name = "project.pulp";
    const std::string text = doc.dump(1);
    proj.data.assign(text.begin(), text.end());
    entries.insert(entries.begin(), std::move(proj));
    const auto bytes = zipStore(entries);
    if (!writeFileBytes(zipPath, bytes.data(), bytes.size())) { if (error) *error = "cannot write " + zipPath; return false; }
    return true;
}

bool Project::unpack(const std::string& zipPath, const std::string& destDir, std::string& projectFile, std::string* error) {
    std::vector<unsigned char> bytes;
    if (!readFileBytes(zipPath, bytes)) { if (error) *error = "cannot open " + zipPath; return false; }
    std::vector<ZipEntry> entries;
    if (!zipRead(bytes, entries, error)) return false;
    bool found = false;
    for (auto& e : entries) {
        const std::string out = destDir + "/" + e.name;
        if (!writeFileBytes(out, e.data.data(), e.data.size())) { if (error) *error = "cannot write " + out; return false; }
        if (e.name == "project.pulp") { projectFile = out; found = true; }
    }
    if (!found) { if (error) *error = "the archive has no project.pulp in it"; return false; }
    return true;
}

} // namespace pulp
