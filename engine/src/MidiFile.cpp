#include "pulp/MidiFile.h"
#include "pulp/Audio.h"

namespace pulp {

namespace {
struct Reader {
    const unsigned char* p;
    const unsigned char* end;
    bool ok = true;
    unsigned u8() { if (p >= end) { ok = false; return 0; } return *p++; }
    unsigned u16() { const unsigned a = u8(); return (a << 8) | u8(); }
    unsigned u32() { const unsigned a = u16(); return (a << 16) | u16(); }
    unsigned var() {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) { const unsigned b = u8(); v = (v << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
        return v;
    }
};
void putVar(std::vector<unsigned char>& o, unsigned v) {
    unsigned char buf[4];
    int n = 0;
    buf[n++] = (unsigned char)(v & 0x7F);
    while ((v >>= 7) != 0) buf[n++] = (unsigned char)((v & 0x7F) | 0x80);
    while (n > 0) o.push_back(buf[--n]);
}
void put16(std::vector<unsigned char>& o, unsigned v) { o.push_back((unsigned char)(v >> 8)); o.push_back((unsigned char)v); }
void put32(std::vector<unsigned char>& o, unsigned v) { put16(o, v >> 16); put16(o, v & 0xFFFF); }
}

bool readMidi(const std::vector<unsigned char>& bytes, MidiSong& out, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    Reader r{bytes.data(), bytes.data() + bytes.size()};
    if (bytes.size() < 14 || std::memcmp(bytes.data(), "MThd", 4) != 0) return fail("not a MIDI file");
    r.p += 4;
    const unsigned headLen = r.u32();
    const unsigned format = r.u16();
    const unsigned numTracks = r.u16();
    const unsigned division = r.u16();
    (void)format;
    if (division & 0x8000) return fail("SMPTE-timed MIDI files are not supported");
    if (division == 0) return fail("bad MIDI time division");
    r.p = bytes.data() + 8 + headLen;
    const double scale = (double)kPPQ / (double)division;
    out = MidiSong();
    bool tempoSet = false;

    for (unsigned t = 0; t < numTracks && r.ok && r.p + 8 <= r.end; ++t) {
        if (std::memcmp(r.p, "MTrk", 4) != 0) return fail("damaged MIDI track");
        r.p += 4;
        const unsigned len = r.u32();
        Reader tr{r.p, std::min(r.p + len, r.end)};
        r.p += len;
        MidiTrack track;
        struct Open { int tick; float vel; bool on = false; };
        Open open[16][128] = {};
        unsigned long long tick = 0;
        unsigned status = 0;
        while (tr.ok && tr.p < tr.end) {
            tick += tr.var();
            unsigned b = tr.u8();
            if (b & 0x80) { status = b; if (b < 0xF0) b = tr.u8(); }
            else if (status == 0) break;                    // running status with nothing running
            const unsigned type = status & 0xF0, chan = status & 0x0F;
            const int now = (int)std::llround((double)tick * scale);
            if (status == 0xFF) {
                const unsigned meta = tr.u8();
                const unsigned mlen = tr.var();
                if (tr.p + mlen > tr.end) break;
                if (meta == 0x51 && mlen == 3) {
                    const unsigned us = (tr.p[0] << 16) | (tr.p[1] << 8) | tr.p[2];
                    if (us > 0) {
                        if (!tempoSet) { out.tempo = 60000000.0 / us; tempoSet = true; }
                        out.tempos.push_back({now, 60000000.0 / us});
                    }
                } else if (meta == 0x58 && mlen >= 2) {
                    out.timeSigNum = clampT((int)tr.p[0], 1, 16);
                    out.timeSigDen = clampT(1 << tr.p[1], 1, 16);
                } else if (meta == 0x03) track.name.assign((const char*)tr.p, mlen);
                tr.p += mlen;
                status = 0;
                continue;
            }
            if (status == 0xF0 || status == 0xF7) { const unsigned slen = tr.var(); tr.p += std::min<size_t>(slen, (size_t)(tr.end - tr.p)); status = 0; continue; }
            if (type == 0x90 || type == 0x80) {
                const unsigned key = b & 0x7F, vel = tr.u8() & 0x7F;
                Open& o = open[chan][key];
                if (o.on) {                                  // close the note that was sounding
                    Note n;
                    n.tick = o.tick; n.length = std::max(1, now - o.tick); n.key = (int)key; n.velocity = o.vel; n.color = (int)chan;
                    track.notes.push_back(n);
                    o.on = false;
                }
                if (type == 0x90 && vel > 0) { o.on = true; o.tick = now; o.vel = (float)vel / 127.0f; }
            } else if (type == 0xC0 || type == 0xD0) {
                // one data byte, already read
                if (type == 0xC0 && track.programs[chan] < 0) track.programs[chan] = (int)(b & 0x7F);
            } else {
                tr.u8();                                    // second data byte
            }
        }
        if (!track.notes.empty()) { sortNotes(track.notes); out.tracks.push_back(std::move(track)); }
    }
    if (out.tracks.empty()) return fail("the MIDI file has no notes");
    // tempo changes in order (they may come from several tracks); the last one written for a tick stands
    std::stable_sort(out.tempos.begin(), out.tempos.end(), [](const std::pair<int, double>& a, const std::pair<int, double>& b) { return a.first < b.first; });
    std::vector<std::pair<int, double>> tempos;
    for (auto& t : out.tempos) {
        if (!tempos.empty() && tempos.back().first == t.first) tempos.back() = t;
        else tempos.push_back(t);
    }
    out.tempos = std::move(tempos);
    if (!out.tempos.empty()) out.tempo = out.tempos.front().second;
    return true;
}

bool addTempoMap(Project& p, const MidiSong& song, int track, int64 lengthTicks) {
    bool changes = false;
    for (auto& t : song.tempos) changes = changes || std::fabs(t.second - song.tempos.front().second) > 1.0e-6;
    if (!changes) return false;
    const ParamInfo& info = p.global->tempo.info;
    auto norm = [&](double bpm) { return clampT((float)((bpm - info.min) / (info.max - info.min)), 0.0f, 1.0f); };
    int64 length = std::max<int64>(lengthTicks, song.tempos.back().first + 1);
    std::vector<AutoPoint> points;
    points.push_back({0, norm(song.tempos.front().second), 0.0f, ShapeHold});         // whatever came before the first tempo event
    for (auto& t : song.tempos) {
        if (t.first <= 0) { points[0].value = norm(t.second); continue; }
        points.push_back({t.first, norm(t.second), 0.0f, ShapeHold});
    }
    if (points.back().tick < length) points.push_back({length, points.back().value, 0.0f, ShapeHold});
    Clip& c = p.addAutomationClip("tempo", track, 0, length);
    AutoClip* a = p.autoById(c.autoId);
    if (!a) return false;
    a->name = "Tempo changes";
    a->min = 0.0f; a->max = 1.0f;
    a->points = std::move(points);
    c.length = length;
    return true;
}

bool readMidiFile(const std::string& path, MidiSong& out, std::string* error) {
    std::vector<unsigned char> bytes;
    if (!readFileBytes(path, bytes)) { if (error) *error = "cannot open " + path; return false; }
    return readMidi(bytes, out, error);
}

std::vector<unsigned char> writeMidi(const MidiSong& song) {
    std::vector<unsigned char> o = {'M', 'T', 'h', 'd'};
    put32(o, 6);
    put16(o, 1);
    put16(o, (unsigned)song.tracks.size() + 1);
    put16(o, (unsigned)kPPQ);
    auto writeTrack = [&](const std::vector<unsigned char>& body) {
        o.insert(o.end(), {'M', 'T', 'r', 'k'});
        put32(o, (unsigned)body.size() + 4);
        o.insert(o.end(), body.begin(), body.end());
        o.insert(o.end(), {0x00, 0xFF, 0x2F, 0x00});
    };
    // conductor track: tempo and time signature
    {
        std::vector<unsigned char> b;
        auto tempoBytes = [&](double bpm) {
            const unsigned us = (unsigned)std::llround(60000000.0 / std::max(1.0, bpm));
            b.insert(b.end(), {0xFF, 0x51, 0x03, (unsigned char)(us >> 16), (unsigned char)(us >> 8), (unsigned char)us});
        };
        const bool atStart = !song.tempos.empty() && song.tempos.front().first <= 0;
        b.push_back(0x00);
        tempoBytes(atStart ? song.tempos.front().second : song.tempo);
        unsigned char denPow = 0;
        while ((1 << denPow) < song.timeSigDen) ++denPow;
        b.insert(b.end(), {0x00, 0xFF, 0x58, 0x04, (unsigned char)song.timeSigNum, denPow, 24, 8});
        // the tempo changes after the start
        int at = 0;
        for (auto& t : song.tempos) {
            if (t.first <= 0) continue;
            putVar(b, (unsigned)(t.first - at));
            tempoBytes(t.second);
            at = t.first;
        }
        writeTrack(b);
    }
    for (auto& tr : song.tracks) {
        struct Ev { int tick; bool on; int key, chan, vel; };
        std::vector<Ev> evs;
        for (auto& n : tr.notes) {
            if (n.muted) continue;
            const int vel = clampT((int)std::lround(n.velocity * 127.0f), 1, 127);
            evs.push_back({n.tick, true, n.key, n.color & 15, vel});
            evs.push_back({n.end(), false, n.key, n.color & 15, 0});
        }
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.tick != b.tick ? a.tick < b.tick : (!a.on && b.on); });
        std::vector<unsigned char> b;
        if (!tr.name.empty()) {
            b.insert(b.end(), {0x00, 0xFF, 0x03});
            putVar(b, (unsigned)tr.name.size());
            b.insert(b.end(), tr.name.begin(), tr.name.end());
        }
        for (int ch = 0; ch < 16; ++ch)
            if (tr.programs[ch] >= 0) { b.push_back(0x00); b.push_back((unsigned char)(0xC0 | ch)); b.push_back((unsigned char)clampT(tr.programs[ch], 0, 127)); }
        int last = 0;
        for (auto& e : evs) {
            putVar(b, (unsigned)std::max(0, e.tick - last));
            last = e.tick;
            b.push_back((unsigned char)((e.on ? 0x90 : 0x80) | e.chan));
            b.push_back((unsigned char)e.key);
            b.push_back((unsigned char)e.vel);
        }
        writeTrack(b);
    }
    return o;
}

bool writeMidiFile(const std::string& path, const MidiSong& song) {
    const auto b = writeMidi(song);
    return writeFileBytes(path, b.data(), b.size());
}

const char* gmProgramName(int program) {
    static const char* names[128] = {
        "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
        "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
        "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
        "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
        "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
        "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
        "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
        "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
        "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
        "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
        "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)",
        "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
        "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)",
        "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
        "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
        "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot"};
    return program >= 0 && program < 128 ? names[program] : "";
}

// Pads as PadKit lays them out: 0 kick, 1 snare, 2 clap, 3 closed hat, 4 open hat, 5 to 7 toms, 8 rimshot,
// 9 crash, 10 ride, 11 cowbell, 12 percussion, 13 shaker, 14 second kick, 15 other.
int gmDrumPad(int key) {
    switch (key) {
        case 35: return 14;
        case 36: return 0;
        case 37: return 8;
        case 38: case 40: return 1;
        case 39: return 2;
        case 42: case 44: return 3;
        case 46: return 4;
        case 41: case 43: return 5;
        case 45: case 47: return 6;
        case 48: case 50: return 7;
        case 49: case 52: case 55: case 57: return 9;
        case 51: case 53: case 59: return 10;
        case 56: return 11;
        case 54: case 69: case 70: case 82: return 13;
        default: return key >= 60 && key <= 81 ? 12 : -1;
    }
}

} // namespace pulp
