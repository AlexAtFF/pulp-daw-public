// Milestone 6: project files, packing, migration, export options.
#include "Util.h"
#include "pulp/Effects.h"
#include "pulp/Trio.h"
#include "pulp/Zip.h"
#include <filesystem>
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0;
namespace fs = std::filesystem;

// A project that uses most of what exists: two samples on disk, a synth, patterns, clips,
// mixer routing, effects, a sidechain.
void buildSong(Project& p, const std::string& dir) {
    fs::create_directories(dir + "/audio");
    writeWavFile(dir + "/audio/kick.wav", *drums::makeKick({}, kSr), WavFormat::Float32);
    drums::HatSpec h;
    writeWavFile(dir + "/audio/hat.wav", *drums::makeHat(h, kSr), WavFormat::Float32);
    p.projectDir = dir;
    p.name = "Round trip song";
    p.global->tempo.set(126.0f);
    auto& kick = p.addSamplerChannel(dir + "/audio/kick.wav", "Kick");
    const int kickId = kick.id;
    p.channels.back().mixerTrack = 1;
    auto& hat = p.addSamplerChannel(dir + "/audio/hat.wav", "Hat");
    const int hatId = hat.id;
    p.channels.back().mixerTrack = 2;
    p.channels.back().live->echoCount.set(2);
    auto trio = std::make_shared<Trio>();
    trio->loadState(trio->factoryPresets()[6].state);
    auto& syn = p.addChannel(trio, "Pluck");
    const int synId = syn.id;
    p.channels.back().mixerTrack = 3;
    for (int s : {0, 4, 8, 12}) Project::setStep(p.patterns[0].notesFor(kickId), s, true);
    for (int s : {2, 6, 10, 14}) Project::setStep(p.patterns[0].notesFor(hatId), s, true);
    p.patterns[0].swing = 0.25f;
    p.addPattern("Melody");
    int t = 0;
    for (int k : {57, 60, 64, 67, 64, 60}) { Note n; n.key = k; n.tick = t; n.length = 360; n.velocity = 0.6f + 0.05f * (float)(k % 5); p.patterns[1].notesFor(synId).push_back(n); t += 480; }
    p.patterns[1].markers.push_back({960, "Lift"});
    const int bar = p.ticksPerBar();
    p.addPatternClip(0, 0, 0);
    p.addPatternClip(0, 0, bar);
    p.addPatternClip(1, 1, bar).length = bar;
    p.tracks[1].name = "Lead";
    p.markers.push_back({bar, "Verse"});
    p.songMode = true;
    p.setRoute(2, 10, true);
    p.mixer[2].routes.back().level->set(0.6f);
    p.setSlot(10, 0, std::make_shared<Chamber>());
    p.mixer[10].name = "Verb bus";
    auto comp = std::make_shared<Squeeze>();
    comp->pSide->set(1); comp->pThresh->set(-28); comp->pRatio->set(8);
    p.setSlot(3, 0, comp);
    p.setRoute(1, 3, true, true);
    p.setSlot(0, 0, std::make_shared<ContourEq>());
    p.mixer[0].slots[0]->param("b2gain")->set(-3.0f);
    p.setSlot(0, 1, std::make_shared<Lid>());
    p.mixer[1].live->volume.set(0.7f);
    p.mixer[3].live->eqGain[2].set(4.0f);
}
}

TEST(zip_store_round_trip) {
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
    std::vector<ZipEntry> in = {{"project.pulp", {'{', '}'}}, {"samples/a b.wav", std::vector<unsigned char>(5000, 7)}, {"empty.txt", {}}};
    in[1].data[1234] = 99;
    const auto bytes = zipStore(in);
    std::vector<ZipEntry> out;
    std::string err;
    CHECK_MSG(zipRead(bytes, out, &err), "%s", err.c_str());
    CHECK(out.size() == 3 && out[1].name == "samples/a b.wav" && out[1].data == in[1].data && out[2].data.empty());
    auto bad = bytes;
    bad[300] ^= 0x55;       // corrupt a byte inside the second file
    CHECK(!zipRead(bad, out, &err));
    CHECK(!zipRead(std::vector<unsigned char>(40, 0), out, &err));
    // an entry that tries to escape its folder is refused
    const auto evil = zipStore({{"../outside.txt", {'x'}}});
    CHECK(!zipRead(evil, out, &err));
}

TEST(full_project_saves_reloads_and_renders_the_same) {
    const std::string dir = t::tmpDir() + "/song_a";
    fs::remove_all(dir);
    Project a;
    buildSong(a, dir);
    std::string err;
    CHECK_MSG(a.save(dir + "/song.pulp", &err), "%s", err.c_str());
    // sample paths inside the project folder are stored relative
    std::string text;
    CHECK(readTextFile(dir + "/song.pulp", text));
    CHECK(text.find("\"audio/kick.wav\"") != std::string::npos);
    CHECK(text.find(dir) == std::string::npos);

    Project b;
    CHECK_MSG(b.load(dir + "/song.pulp", &err), "%s", err.c_str());
    CHECK(b.channels.size() == 3 && b.patterns.size() == 2 && b.clips.size() == 3);
    CHECK(b.mixer[10].name == "Verb bus" && b.mixer[1].routeTo(3) && b.mixer[1].routeTo(3)->sidechain);
    CHECK(b.patterns[1].markers.size() == 1 && b.markers.size() == 1);
    RenderOptions o;
    auto ra = renderOffline(a, o), rb = renderOffline(b, o);
    CHECK(ra && rb && peakOf(ra->ch[0]) > 0.05f);
    CHECK(identical(*ra, *rb));
    // saving the reloaded project gives the same file
    CHECK(b.save(dir + "/song2.pulp"));
    std::string text2;
    CHECK(readTextFile(dir + "/song2.pulp", text2));
    CHECK(text == text2);

    // the whole folder can be moved
    const std::string moved = t::tmpDir() + "/song_moved";
    fs::remove_all(moved);
    fs::rename(dir, moved);
    SampleCache::global().clear();
    Project c;
    CHECK_MSG(c.load(moved + "/song.pulp", &err), "%s", err.c_str());
    auto rc = renderOffline(c, o);
    CHECK(rc && identical(*ra, *rc));
}

TEST(packed_project_unpacks_and_renders_the_same) {
    const std::string dir = t::tmpDir() + "/song_p";
    fs::remove_all(dir);
    Project a;
    buildSong(a, dir);
    a.save(dir + "/song.pulp");
    std::string err;
    CHECK_MSG(a.pack(t::tmpDir() + "/song_packed.pulpz", &err), "%s", err.c_str());
    CHECK(a.fileRefs().size() == 2);
    RenderOptions o;
    auto ra = renderOffline(a, o);

    const std::string out = t::tmpDir() + "/song_unpacked";
    fs::remove_all(out);
    fs::remove_all(dir);            // the originals are gone: only the archive is left
    SampleCache::global().clear();
    std::string projectFile;
    CHECK_MSG(Project::unpack(t::tmpDir() + "/song_packed.pulpz", out, projectFile, &err), "%s", err.c_str());
    CHECK(fs::exists(out + "/samples/kick.wav") && fs::exists(out + "/samples/hat.wav"));
    Project b;
    CHECK_MSG(b.load(projectFile, &err), "%s", err.c_str());
    auto rb = renderOffline(b, o);
    CHECK(ra && rb && identical(*ra, *rb));
}

TEST(older_project_versions_are_upgraded) {
    // A version 1 file, as written before the mixer existed.
    const char* v1 = R"({"format":"pulp-project","version":1,"name":"Old","tempo":120,"masterVolume":0.8,"masterPitch":0,
        "timeSig":[4,4],"songMode":false,"currentPattern":0,"loop":[0,0],
        "channels":[{"id":1,"name":"Lead","color":4284442739,"mixerTrack":0,"strip":{"volume":0.5},"plugin":{"type":"trio","params":{"cutoff":900}}}],
        "patterns":[{"id":1,"name":"Pattern 1","color":4283399848,"steps":16,"swing":0,"notes":{"1":[[0,240,60,0.78,0,0.5,0.5,0.5,0,0,0]]}}],
        "playlist":{"clips":[],"tracks":[],"markers":[]}})";
    std::string err;
    Json doc = Json::parse(v1, &err);
    CHECK(err.empty());
    Project p;
    CHECK_MSG(p.fromJson(doc, &err), "%s", err.c_str());
    CHECK(p.channels.size() == 1 && p.channels[0].live->volume.get() == 0.5f);
    CHECK(p.channels[0].live->plugin->param("cutoff")->get() == 900.0f);
    CHECK(p.mixer.size() == (size_t)kNumMixerTracks && p.mixer[5].routeTo(0) != nullptr);
    CHECK(p.toJson()["version"].i() == kProjectVersion);
    RenderOptions o;
    o.lengthSamples = 12000;
    CHECK(peakOf(renderOffline(p, o)->ch[0]) > 0.01f);
    // newer than we know, not a project, and version 0 are all refused with a message
    Project q;
    Json future = doc;
    future.set("version", kProjectVersion + 1);
    CHECK(!q.fromJson(future, &err) && err.find("newer") != std::string::npos);
    Json notOurs = doc;
    notOurs.set("format", "something-else");
    CHECK(!q.fromJson(notOurs, &err));
    Json ancient = doc;
    ancient.set("version", 0);
    CHECK(!q.fromJson(ancient, &err));
}

TEST(export_tail_modes) {
    const double bpm = 120.0;
    auto build = [&](Project& p) {
        p.global->tempo.set((float)bpm);
        auto ch = addSampleChannel(p, sine(330, 0.2, kSr, 0.4f));
        Project::setStep(p.pattern().notesFor(ch.id), 15, true);       // rings past the end of the bar
        p.setSlot(0, 0, std::make_shared<Repeater>());
        p.mixer[0].slots[0]->param("mix")->set(0.5f);
    };
    const int64 bar = (int64)std::llround(16 * kTicksPerStep * samplesPerTick(bpm, kSr));
    Project a, b, c;
    build(a); build(b); build(c);
    RenderOptions leave, cut, wrap;
    cut.tail = RenderOptions::Cut;
    wrap.tail = RenderOptions::Wrap;
    auto rl = renderOffline(a, leave), rc = renderOffline(b, cut), rw = renderOffline(c, wrap);
    CHECK(rl->frames() > bar + 4800);
    CHECK(rc->frames() == bar && rw->frames() == bar);
    // cut is the start of the full render; wrap is that plus the tail folded onto the start
    bool same = true;
    for (int64 i = 0; i < bar; ++i) same = same && rc->ch[0][(size_t)i] == rl->ch[0][(size_t)i];
    CHECK(same);
    double worst = 0.0;
    for (int64 i = 0; i < bar; ++i) {
        float want = rl->ch[0][(size_t)i];
        for (int64 k = i + bar; k < rl->frames(); k += bar) want += rl->ch[0][(size_t)k];
        worst = std::max(worst, (double)std::fabs(rw->ch[0][(size_t)i] - want));
    }
    CHECK_MSG(worst < 1e-6, "wrap differs by %g", worst);
    CHECK(peakOf(rw->ch[0], 0, 4800) > 0.01f);       // the echo tail now sounds at the start
}

TEST(small_block_playback_matches_the_export_render) {
    // What plays in real-time-sized blocks on one thread is bit-identical to a big-block multi-threaded export.
    const std::string dir = t::tmpDir() + "/song_e";
    fs::remove_all(dir);
    Project a, b;
    buildSong(a, dir);
    buildSong(b, dir);
    RenderOptions live, exp;
    live.blockSize = 64; live.threads = 1;
    exp.blockSize = 4096; exp.threads = 6;
    auto rl = renderOffline(a, live), re = renderOffline(b, exp);
    CHECK(rl && re && peakOf(rl->ch[0]) > 0.05f);
    CHECK(identical(*rl, *re));
    // 16-bit and 24-bit files hold what was rendered, within one step of their resolution
    CHECK(writeWavFile(dir + "/out16.wav", *re, WavFormat::Pcm16, true));
    CHECK(writeWavFile(dir + "/out24.wav", *re, WavFormat::Pcm24));
    CHECK(writeWavFile(dir + "/out32.wav", *re, WavFormat::Float32));
    auto f16 = readWavFile(dir + "/out16.wav"), f24 = readWavFile(dir + "/out24.wav"), f32 = readWavFile(dir + "/out32.wav");
    CHECK(f16 && f24 && f32);
    CHECK(f32->ch[0] == re->ch[0]);
    // only compare where the render stays inside full scale; anything above is clipped by the fixed-point formats
    double w16 = 0.0, w24 = 0.0;
    for (size_t i = 0; i < re->ch[0].size(); ++i)
        if (std::fabs(re->ch[0][i]) < 0.999f) {
            w16 = std::max(w16, (double)std::fabs(f16->ch[0][i] - re->ch[0][i]));
            w24 = std::max(w24, (double)std::fabs(f24->ch[0][i] - re->ch[0][i]));
        }
    CHECK_MSG(w24 <= 1.0 / 8388608.0 && w16 <= 2.0 / 32768.0, "24-bit error %g, 16-bit (dithered) error %g", w24, w16);
}
