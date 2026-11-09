// pulp_render: renders a .pulp project to a WAV file with no window and no sound card.
#include "pulp/Audio.h"
#include "pulp/DrumSynth.h"
#include "pulp/Multisample.h"
#include "pulp/NoteSynth.h"
#include "pulp/Project.h"
#include "pulp/Render.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>

using namespace pulp;

static void usage() {
    printf("PULP command-line renderer\n\n"
           "  pulp_render <project.pulp> <out.wav> [options]\n\n"
           "  --rate <hz>        sample rate (default 48000)\n"
           "  --bits <16|24|32>  16 or 24-bit PCM, or 32-bit float (default 24)\n"
           "  --dither           add dither to 16 and 24-bit output\n"
           "  --song | --pattern force song mode or pattern mode (default: as saved)\n"
           "  --tail <seconds>   longest tail to keep after the end (default 10)\n"
           "  --factory <dir>    factory library folder (default: 'factory' beside this program)\n\n"
           "  pulp_render --make-factory <dir>\n"
           "      writes the synthesized part of the factory library and its manifest into <dir>\n\n"
           "  pulp_render --stress [channels 100] [mixer tracks 60] [effects 150] [seconds 20] [threads] [block 256]\n"
           "      builds a project that size in memory and plays it at the pace of a sound card, timing every block\n\n"
           "  pulp_render --build-library <sources.json> <cache dir> <factory dir>\n"
           "      builds the factory instruments: from the recordings fetched by tools\\fetch-samples.py where the\n"
           "      manifest has them, and by synthesis for the rest. Adds them to <factory dir>\\manifest.json.\n");
}

// ---- the factory instruments ----

namespace fs = std::filesystem;

static void manifestEntry(Json& list, const std::string& factoryDir, const std::string& path, const std::string& url, const std::string& author,
                          const std::string& source, const std::string& licence) {
    std::vector<unsigned char> bytes;
    readFileBytes(factoryDir + "/" + path, bytes);
    Json e = Json::object();
    e.set("path", path);
    e.set("url", url);
    e.set("author", author);
    e.set("source", source);
    e.set("licence", licence);
    e.set("sha256", sha256Hex(bytes.data(), bytes.size()));
    list.push(e);
}

// Saves an instrument under Instruments/<category>/<name>/ and lists its files in the manifest.
static bool storeInstrument(ms::Instrument& inst, const std::string& factoryDir, Json& list, const std::vector<std::string>& zoneUrls,
                            const std::string& author, const std::string& source, const std::string& licence) {
    const std::string rel = "Instruments/" + inst.category + "/" + inst.name;
    std::error_code ec;
    fs::remove_all(fs::path(factoryDir) / rel, ec);
    std::string err;
    // the audio the builder made for each zone remembers which source file it came from
    std::vector<std::string> from;
    for (auto& z : inst.zones) from.push_back(z.file);
    if (!ms::saveInstrument(inst, factoryDir + "/" + rel + "/" + inst.name + ".pulpinst", true, &err)) { fprintf(stderr, "error: %s: %s\n", inst.name.c_str(), err.c_str()); return false; }
    auto saved = ms::loadInstrument(factoryDir + "/" + rel + "/" + inst.name + ".pulpinst", &err);
    if (!saved || saved->zones.size() != inst.zones.size()) { fprintf(stderr, "error: %s did not load back: %s\n", inst.name.c_str(), err.c_str()); return false; }
    manifestEntry(list, factoryDir, rel + "/" + inst.name + ".pulpinst", "", "PULP", "Instrument description written by PULP's multisample builder", "CC0-1.0");
    for (size_t i = 0; i < saved->zones.size(); ++i) {
        std::string url;
        for (auto& u : zoneUrls) if (!from[i].empty() && u.size() >= from[i].size() && u.compare(u.size() - from[i].size(), from[i].size(), from[i]) == 0) url = u;
        manifestEntry(list, factoryDir, rel + "/" + saved->zones[i].file, url, author, source, licence);
    }
    return true;
}

static int buildLibrary(const std::string& sourcesPath, const std::string& cacheDir, const std::string& factoryDir) {
    std::string text, perr;
    Json src;
    if (readTextFile(sourcesPath, text)) src = Json::parse(text, &perr);
    if (!perr.empty()) { fprintf(stderr, "error: %s: %s\n", sourcesPath.c_str(), perr.c_str()); return 1; }
    // keep what is already in the manifest (the synthesized drums), drop the instruments of an earlier run
    Json list = Json::array();
    std::string mtext;
    if (readTextFile(factoryDir + "/manifest.json", mtext)) {
        const Json old = Json::parse(mtext);
        for (auto& e : old["files"].elements()) {
            const std::string p = e["path"].str("");
            if (p.rfind("Instruments/", 0) != 0 && e["source"].str("").find("recorded") == std::string::npos) list.push(e);
        }
        // sounds an earlier run made from recordings are made again: clear their folders first
        std::error_code ec;
        for (auto& e : old["files"].elements()) {
            const std::string p = e["path"].str("");
            if (e["source"].str("").find("recorded") != std::string::npos && p.rfind("Instruments/", 0) != 0) fs::remove(fs::path(factoryDir) / p, ec);
        }
    }
    std::map<std::string, Json> sources;
    for (auto& s : src["sources"].elements()) sources[s["id"].str("")] = s;
    std::vector<std::string> made;
    int recorded = 0, synthesized = 0, hits = 0, problems = 0;

    for (auto& ij : src["instruments"].elements()) {
        const std::string name = ij["name"].str(""), category = ij["category"].str("Other");
        std::vector<ms::Note> notes;
        std::vector<std::string> urls;
        std::string author, sourceName, licence;
        std::vector<std::pair<std::string, SamplePtr>> drumHits;
        for (auto& fj : ij["files"].elements()) {
            const std::string file = fj["file"].str("");
            std::string err;
            auto audio = readAudioFile(cacheDir + "/" + file, &err);
            if (!audio) { fprintf(stderr, "  missing: %s (%s)\n", file.c_str(), err.c_str()); ++problems; continue; }
            const Json& sj = sources[fj["source"].str("")];
            author = sj["author"].str("");
            sourceName = sj["name"].str("");
            licence = sj["licence"].str("");
            urls.push_back(fj["url"].str(""));
            if (!fj["hit"].str("").empty()) { drumHits.push_back({fj["hit"].str(""), audio}); continue; }
            ms::Note n;
            n.sample = audio;
            // the zone remembers the end of its source URL, so the manifest can say where each file came from
            n.file = fj["url"].str("").substr(fj["url"].str("").find_last_of('/') + 1);
            n.key = fj["midi"].i(-1);
            n.layer = fj["layer"].i(0);
            n.take = fj["take"].i(0);
            notes.push_back(std::move(n));
        }
        const std::string credit = sourceName + (author.empty() ? "" : " by " + author) + ", " + licence;
        if (!drumHits.empty()) {
            // drum hits are kept as single sounds: trimmed, and brought to one level
            for (auto& h : drumHits) {
                const auto r = ms::findSound(*h.second, 60.0f);
                auto cut = std::make_shared<SampleData>();
                cut->sampleRate = h.second->sampleRate;
                cut->channels = h.second->channels;
                float peak = 0.0f;
                for (int c = 0; c < cut->channels; ++c) {
                    cut->ch[c].assign(h.second->ch[c].begin() + r.start, h.second->ch[c].begin() + r.end);
                    for (float v : cut->ch[c]) peak = std::max(peak, std::fabs(v));
                }
                if (peak > 0.0f) for (int c = 0; c < cut->channels; ++c) for (auto& v : cut->ch[c]) v *= 0.89f / peak;
                // drum kits sit with the other kits; other single sounds (effects) under their own category
                const std::string rel = (category == "Drums" ? std::string("Drums/") : category + "/") + name + "/" + h.first + ".wav";
                if (!writeWavFile(factoryDir + "/" + rel, *cut, WavFormat::Pcm24)) { ++problems; continue; }
                manifestEntry(list, factoryDir, rel, "", author, sourceName + " (recorded; trimmed and levelled by PULP)", licence);
                ++hits;
            }
        }
        if (notes.empty()) continue;
        ms::BuildOptions opt;
        opt.sustain = ij["sustain"].b(false) ? 1 : 0;
        ms::BuildReport report;
        auto inst = ms::build(notes, name, opt, &report);
        inst->category = category;
        inst->credit = credit;
        printf("%s (%s, %d notes)\n%s", name.c_str(), category.c_str(), (int)inst->zones.size(), report.text().c_str());
        for (auto& l : report.lines) if (!l.note.empty()) ++problems;
        if (inst->zones.empty() || !storeInstrument(*inst, factoryDir, list, urls, author, sourceName + " (recorded; trimmed, looped and mapped by PULP)", licence)) { ++problems; continue; }
        made.push_back(name);
        ++recorded;
    }
    // everything that has no recording is synthesized
    for (auto& name : notesynth::names()) {
        if (std::find(made.begin(), made.end(), name) != made.end()) continue;
        const auto set = notesynth::make(name);
        std::vector<ms::Note> notes;
        for (auto& kn : set.notes) {
            ms::Note n;
            n.sample = kn.second;
            n.key = kn.first;
            n.file = name + " " + std::to_string(kn.first);
            notes.push_back(std::move(n));
        }
        ms::BuildOptions opt;
        opt.sustain = set.sustain ? 1 : 0;
        auto inst = ms::build(notes, name, opt);
        inst->category = set.category;
        inst->credit = "Synthesized by PULP (" + set.how + "), CC0";
        if (inst->zones.empty() || !storeInstrument(*inst, factoryDir, list, {}, "PULP", "Synthesized by PULP (engine/src/NoteSynth.cpp): " + set.how, "CC0-1.0")) { ++problems; continue; }
        printf("%s (%s, %d notes, synthesized)\n", name.c_str(), set.category.c_str(), (int)inst->zones.size());
        ++synthesized;
    }
    Json m = Json::object();
    m.set("format", "pulp-factory-manifest");
    m.set("version", 1);
    m.set("files", list);
    if (!writeTextFile(factoryDir + "/manifest.json", m.dump(1))) return 1;
    printf("\nfactory instruments: %d from recordings, %d synthesized, %d drum hits; %d problems\n", recorded, synthesized, hits, problems);
    return 0;
}

// Writes every synthesized factory sound plus manifest.json (path, source, licence, checksum).
static int makeFactory(const std::string& dir) {
    auto files = drums::makeFactoryDrums(48000.0);
    Json list = Json::array();
    for (auto& f : files) {
        const auto bytes = encodeWav(*f.audio, WavFormat::Pcm24);
        if (!writeFileBytes(dir + "/" + f.path, bytes.data(), bytes.size())) { fprintf(stderr, "error: cannot write %s\n", f.path.c_str()); return 1; }
        Json e = Json::object();
        e.set("path", f.path);
        e.set("url", "");
        e.set("author", "PULP");
        e.set("source", f.source);
        e.set("licence", f.licence);
        e.set("sha256", sha256Hex(bytes.data(), bytes.size()));
        list.push(e);
    }
    // The rest of the library (the instruments and hits that --build-library made) stays in the manifest
    // for as long as its files are there: this step runs on every build and must not forget them.
    int kept = 0;
    std::string mtext;
    if (readTextFile(dir + "/manifest.json", mtext)) {
        const Json old = Json::parse(mtext);
        for (auto& e : old["files"].elements()) {
            const std::string p = e["path"].str("");
            bool mine = false;
            for (auto& f : files) mine = mine || f.path == p;
            std::error_code ec;
            if (!mine && !p.empty() && std::filesystem::exists(std::filesystem::path(dir) / p, ec)) { list.push(e); ++kept; }
        }
    }
    Json m = Json::object();
    m.set("format", "pulp-factory-manifest");
    m.set("version", 1);
    m.set("files", list);
    if (!writeTextFile(dir + "/manifest.json", m.dump(1))) return 1;
    printf("wrote %d factory files to %s (%d other entries kept)\n", (int)files.size(), dir.c_str(), kept);
    return 0;
}

int runStress(int numChannels, int numTracks, int numEffects, double seconds, int threads, int block);     // Stress.cpp

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--make-factory") return makeFactory(argv[2]);
    if (argc == 5 && std::string(argv[1]) == "--build-library") return buildLibrary(argv[2], argv[3], argv[4]);
    if (argc >= 2 && std::string(argv[1]) == "--stress") {
        auto num = [&](int i, double def) { return argc > i ? std::atof(argv[i]) : def; };
        const unsigned cores = std::thread::hardware_concurrency();
        return runStress((int)num(2, 100), (int)num(3, 60), (int)num(4, 150), num(5, 20), (int)num(6, std::max(1u, std::min(16u, cores ? cores - 1 : 4u))), (int)num(7, 256));
    }
    if (argc < 3) { usage(); return 2; }
    const std::string in = argv[1], out = argv[2];
    RenderOptions opt;
    WavFormat fmt = WavFormat::Pcm24;
    bool dither = false;
    int forceMode = -1;
    std::string factory = (std::filesystem::path(argv[0]).parent_path() / "factory").string();
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--rate") opt.sampleRate = std::atof(next());
        else if (a == "--bits") { const int b = std::atoi(next()); fmt = b == 16 ? WavFormat::Pcm16 : (b == 32 ? WavFormat::Float32 : WavFormat::Pcm24); }
        else if (a == "--dither") dither = true;
        else if (a == "--song") forceMode = 1;
        else if (a == "--pattern") forceMode = 0;
        else if (a == "--tail") opt.maxTailSeconds = std::atof(next());
        else if (a == "--factory") factory = next();
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
    }
    if (opt.sampleRate < 8000 || opt.sampleRate > 384000) { fprintf(stderr, "bad sample rate\n"); return 2; }
    SampleCache::global().setFactoryRoot(factory);

    Project project;
    project.sampleRate = opt.sampleRate;
    std::string err;
    if (!project.load(in, &err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    if (forceMode >= 0) project.songMode = forceMode == 1;

    int lastPct = -1;
    auto audio = renderOffline(project, opt, [&](double p) {
        const int pct = (int)(p * 100.0);
        if (pct != lastPct && pct % 10 == 0) { lastPct = pct; fprintf(stderr, "\r%3d%%", pct); fflush(stderr); }
        return true;
    });
    fprintf(stderr, "\r");
    if (!audio) { fprintf(stderr, "error: render failed\n"); return 1; }
    if (!writeWavFile(out, *audio, fmt, dither, &err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    printf("%s: %.2f s, %.0f Hz, peak %.1f dB\n", out.c_str(), (double)audio->frames() / opt.sampleRate, opt.sampleRate,
           (double)gainToDb(audio->peak()));
    return 0;
}
