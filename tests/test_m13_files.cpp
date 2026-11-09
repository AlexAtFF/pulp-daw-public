// Milestone 13: files from other programs. MIDI files with their instruments, SoundFonts.
#include "Util.h"
#include "pulp/MidiFile.h"
#include "pulp/Multisample.h"
#include "pulp/SoundFont.h"
#include <cstring>
#include <filesystem>

using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0;

// ---- a small SoundFont writer, for making test files ----
struct Sf2Writer {
    std::vector<short> pcm;
    std::vector<unsigned char> phdr, pbag, pgen, inst, ibag, igen, shdr;
    static void p16(std::vector<unsigned char>& o, unsigned v) { o.push_back((unsigned char)v); o.push_back((unsigned char)(v >> 8)); }
    static void p32(std::vector<unsigned char>& o, unsigned v) { p16(o, v & 0xffff); p16(o, v >> 16); }
    static void name(std::vector<unsigned char>& o, const char* s) { char b[20] = {}; std::strncpy(b, s, 19); o.insert(o.end(), b, b + 20); }
    int numPgen = 0, numIgen = 0, numPbag = 0, numIbag = 0, numInst = 0, numSamples = 0;
    // returns the sample's index
    int sample(const char* n, const std::vector<float>& audio, int rate, int pitch, int correction, int loopA, int loopB, int link = 0, int type = 1) {
        const unsigned start = (unsigned)pcm.size();
        for (float v : audio) pcm.push_back((short)std::lround(clampT(v, -1.0f, 1.0f) * 32767.0f));
        for (int i = 0; i < 46; ++i) pcm.push_back(0);
        name(shdr, n);
        p32(shdr, start); p32(shdr, start + (unsigned)audio.size()); p32(shdr, start + (unsigned)loopA); p32(shdr, start + (unsigned)loopB);
        p32(shdr, (unsigned)rate); shdr.push_back((unsigned char)pitch); shdr.push_back((unsigned char)(signed char)correction); p16(shdr, (unsigned)link); p16(shdr, (unsigned)type);
        return numSamples++;
    }
    void igenAdd(int oper, int amount) { p16(igen, (unsigned)oper); p16(igen, (unsigned)(unsigned short)(short)amount); ++numIgen; }
    void pgenAdd(int oper, int amount) { p16(pgen, (unsigned)oper); p16(pgen, (unsigned)(unsigned short)(short)amount); ++numPgen; }
    void ibagStart() { p16(ibag, (unsigned)numIgen); p16(ibag, 0); ++numIbag; }
    void pbagStart() { p16(pbag, (unsigned)numPgen); p16(pbag, 0); ++numPbag; }
    int instStart(const char* n) { name(inst, n); p16(inst, (unsigned)numIbag); return numInst++; }
    void presetStart(const char* n, int program, int bank) { name(phdr, n); p16(phdr, (unsigned)program); p16(phdr, (unsigned)bank); p16(phdr, (unsigned)numPbag); p32(phdr, 0); p32(phdr, 0); p32(phdr, 0); }
    std::vector<unsigned char> finish() {
        // terminators
        presetStart("EOP", 0, 0); pbagStart(); pgenAdd(0, 0); name(inst, "EOI"); p16(inst, (unsigned)numIbag); ibagStart(); igenAdd(0, 0);
        name(shdr, "EOS"); for (int i = 0; i < 26; ++i) shdr.push_back(0);
        auto chunk = [](const char* id, const std::vector<unsigned char>& body) {
            std::vector<unsigned char> o(id, id + 4);
            p32(o, (unsigned)body.size());
            o.insert(o.end(), body.begin(), body.end());
            if (body.size() & 1) o.push_back(0);
            return o;
        };
        auto list = [&](const char* type, const std::vector<std::vector<unsigned char>>& parts) {
            std::vector<unsigned char> body(type, type + 4);
            for (auto& p : parts) body.insert(body.end(), p.begin(), p.end());
            return chunk("LIST", body);
        };
        std::vector<unsigned char> smpl;
        for (short v : pcm) p16(smpl, (unsigned)(unsigned short)v);
        const std::vector<unsigned char> ifil{2, 0, 1, 0}, inam{'T', 'e', 's', 't', ' ', 'f', 'o', 'n', 't', 0};
        const std::vector<unsigned char> mod(10, 0);
        std::vector<unsigned char> body{'s', 'f', 'b', 'k'};
        for (auto& part : {list("INFO", {chunk("ifil", ifil), chunk("INAM", inam)}), list("sdta", {chunk("smpl", smpl)}),
                           list("pdta", {chunk("phdr", phdr), chunk("pbag", pbag), chunk("pmod", mod), chunk("pgen", pgen), chunk("inst", inst), chunk("ibag", ibag),
                                         chunk("imod", mod), chunk("igen", igen), chunk("shdr", shdr)})})
            body.insert(body.end(), part.begin(), part.end());
        return chunk("RIFF", body);
    }
};

std::vector<float> tone(double hz, double seconds, double rate, float amp = 0.5f) {
    std::vector<float> out((size_t)(seconds * rate));
    for (size_t i = 0; i < out.size(); ++i) out[i] = amp * (float)std::sin(2.0 * 3.14159265358979 * hz * (double)i / rate);
    return out;
}

std::shared_ptr<SampleData> playTimbre(ms::InstrumentPtr inst, int key, int ticks, int64 samples, float velocity = kDefaultVelocity) {
    Project p;
    auto t = std::make_shared<Timbre>();
    t->setInstrument(inst);
    auto& c = p.addChannel(t, "sf");
    p.global->tempo.set(120.0f);
    Note n;
    n.tick = 0; n.length = ticks; n.key = key; n.velocity = velocity;
    p.pattern().notesFor(c.id).push_back(n);
    p.pattern().steps = 64;
    RenderOptions o;
    o.lengthSamples = samples;
    return renderOffline(p, o);
}
}

TEST(midi_files_keep_their_programs_and_general_midi_is_named) {
    MidiSong s;
    s.tempo = 96.0;
    MidiTrack a;
    a.name = "Bass";
    a.programs[1] = 33;
    for (int i = 0; i < 4; ++i) { Note n; n.tick = i * 480; n.length = 400; n.key = 36 + i; n.color = 1; a.notes.push_back(n); }
    MidiTrack d;
    d.name = "Drums";
    for (int i = 0; i < 8; ++i) { Note n; n.tick = i * 240; n.length = 60; n.key = i % 2 ? 38 : 36; n.color = 9; d.notes.push_back(n); }
    s.tracks = {a, d};
    MidiSong r;
    std::string err;
    CHECK_MSG(readMidi(writeMidi(s), r, &err), "%s", err.c_str());
    CHECK(r.tracks.size() == 2);
    if (r.tracks.size() == 2) {
        CHECK(r.tracks[0].programs[1] == 33);
        CHECK(r.tracks[0].programs[0] == -1);
        CHECK(r.tracks[0].notes.size() == 4 && r.tracks[0].notes[2].key == 38 && r.tracks[0].notes[2].tick == 960 && r.tracks[0].notes[2].color == 1);
        CHECK(r.tracks[1].notes.size() == 8 && r.tracks[1].notes[0].color == 9);
        CHECK(r.tracks[1].programs[9] == -1);
    }
    CHECK_NEAR(r.tempo, 96.0, 0.01);
    CHECK(std::string(gmProgramName(0)) == "Acoustic Grand Piano");
    CHECK(std::string(gmProgramName(73)) == "Flute");
    CHECK(std::string(gmProgramName(128)).empty());
    CHECK(gmDrumPad(36) == 0 && gmDrumPad(38) == 1 && gmDrumPad(42) == 3 && gmDrumPad(46) == 4 && gmDrumPad(49) == 9 && gmDrumPad(51) == 10);
    CHECK(gmDrumPad(20) == -1);
}

TEST(soundfont_presets_become_instruments) {
    Sf2Writer w;
    // two mono samples at different rates, one looped; a stereo pair
    const auto lowTone = tone(220.0, 0.5, 22050.0);          // A3 = key 57, loops over 100 whole cycles (22050 / 220 is not whole: use 441 samples = 4.4 cycles x 5)
    const int sA = w.sample("low", lowTone, 22050, 57, 0, 2205, 2205 + 4410);          // 4410 samples = 44 cycles of 220 Hz
    const int sB = w.sample("high", tone(880.0, 0.3, 44100.0), 44100, 81, 0, 0, 0);
    const int sL = w.sample("padL", tone(330.0, 0.4, 44100.0, 0.5f), 44100, 64, 0, 0, 0, 3, 4);
    const int sR = w.sample("padR", tone(330.0, 0.4, 44100.0, 0.25f), 44100, 64, 0, 0, 0, 2, 2);
    CHECK(sA == 0 && sB == 1 && sL == 2 && sR == 3);
    // instrument 0: low keys play the looped sample, high keys the other, tuned 50 cents up, 6 dB down
    const int i0 = w.instStart("Split");
    w.ibagStart(); w.igenAdd(38, 0);                                                 // global zone: release of one second
    w.ibagStart(); w.igenAdd(43, 0 | (65 << 8)); w.igenAdd(54, 1); w.igenAdd(53, sA);
    w.ibagStart(); w.igenAdd(43, 66 | (127 << 8)); w.igenAdd(52, 50); w.igenAdd(48, 150); w.igenAdd(53, sB);
    // instrument 1: the stereo pair, the root key overridden
    const int i1 = w.instStart("Wide");
    w.ibagStart(); w.igenAdd(58, 52); w.igenAdd(53, sL);
    w.ibagStart(); w.igenAdd(58, 52); w.igenAdd(53, sR);
    w.presetStart("Wide pad", 89, 0);
    w.pbagStart(); w.pgenAdd(41, i1);
    w.presetStart("Split tone", 5, 0);
    w.pbagStart(); w.pgenAdd(43, 24 | (108 << 8)); w.pgenAdd(41, i0);            // the preset narrows the keys to 24..108
    const auto bytes = w.finish();

    std::string err;
    auto f = sf2::openMemory(bytes.data(), bytes.size(), &err);
    CHECK_MSG(f != nullptr, "%s", err.c_str());
    if (!f) return;
    const auto list = sf2::presets(*f);
    CHECK(list.size() == 2);
    CHECK(list[0].name == "Split tone" && list[0].program == 5);        // sorted by program
    CHECK(list[1].name == "Wide pad" && list[1].program == 89);
    auto split = sf2::build(*f, list[0].index, &err);
    CHECK_MSG(split != nullptr, "%s", err.c_str());
    if (!split) return;
    CHECK(split->zones.size() == 2);
    CHECK(split->zones[0].loKey == 24 && split->zones[0].hiKey == 65 && split->zones[0].root == 57 && split->zones[0].loop);
    CHECK(split->zones[1].loKey == 66 && split->zones[1].hiKey == 108 && split->zones[1].root == 81 && !split->zones[1].loop);
    CHECK(split->zones[0].loopStart == 2205 && split->zones[0].loopEnd == 6615);
    CHECK(split->sustain);
    CHECK_NEAR(split->params["release"].num(), 1000.0, 1.0);
    // played: in tune on and off the root keys, the tuned zone 50 cents sharp, the looped zone holding past its half second
    CHECK_NEAR(measureHz(playTimbre(split, 57, 1920, 40000)->ch[0], 4000, 30000, kSr), 220.0, 0.3);
    CHECK_NEAR(measureHz(playTimbre(split, 45, 1920, 40000)->ch[0], 4000, 30000, kSr), 110.0, 0.2);
    auto held = playTimbre(split, 57, 3600, 96000);
    CHECK(rmsOf(held->ch[0], 60000, 80000) > rmsOf(held->ch[0], 5000, 20000) * 0.7);
    CHECK_NEAR(measureHz(held->ch[0], 50000, 80000, kSr), 220.0, 0.3);
    CHECK_NEAR(measureHz(playTimbre(split, 81, 480, 12000)->ch[0], 2000, 10000, kSr), 880.0 * std::pow(2.0, 50.0 / 1200.0), 1.5);
    // the stereo pair comes in as one stereo zone, left twice as loud as right, at its overridden root
    auto wide = sf2::build(*f, list[1].index, &err);
    CHECK(wide != nullptr);
    if (wide) {
        CHECK(wide->zones.size() == 1);
        CHECK(wide->zones[0].sample->channels == 2);
        CHECK(wide->zones[0].root == 52);
        auto r = playTimbre(wide, 52, 480, 12000);
        CHECK_NEAR(measureHz(r->ch[0], 2000, 10000, kSr), 330.0, 0.5);
        CHECK_NEAR(rmsOf(r->ch[0], 2000, 10000) / rmsOf(r->ch[1], 2000, 10000), 2.0, 0.05);
        // saved as a PULP instrument and loaded again, it plays the same
        namespace fs = std::filesystem;
        const std::string dir = t::tmpDir() + "/sf2";
        fs::remove_all(dir);
        fs::create_directories(dir);
        CHECK(ms::saveInstrument(*split, dir + "/Split tone.pulpinst", true, &err));
        auto back = ms::loadInstrument(dir + "/Split tone.pulpinst", &err);
        CHECK_MSG(back != nullptr, "%s", err.c_str());
        if (back) {
            CHECK(back->zones.size() == 2 && back->zones[0].loop && back->zones[0].loopStart == 2205);
            CHECK_NEAR(measureHz(playTimbre(back, 45, 1920, 40000)->ch[0], 4000, 30000, kSr), 110.0, 0.2);
        }
    }
    // not a SoundFont, a cut one, a preset that is not there
    CHECK(sf2::openMemory("RIFFxxxxWAVE", 12, &err) == nullptr);
    CHECK(sf2::openMemory(bytes.data(), 200, &err) == nullptr);
    CHECK(sf2::build(*f, 7, &err) == nullptr);
    for (size_t cut = 0; cut < bytes.size(); cut += 97) (void)sf2::openMemory(bytes.data(), cut, &err);       // never crashes
}
