// Milestone 14: a MIDI file's tempo changes are read, written, and put on the playlist as tempo automation.
#include "Util.h"
#include "pulp/Automation.h"
#include "pulp/Effects.h"
#include "pulp/MidiFile.h"
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0;
const int kBar = 3840;

MidiSong songWithTempos(std::vector<std::pair<int, double>> tempos) {
    MidiSong s;
    s.tempos = std::move(tempos);
    s.tempo = s.tempos.empty() ? 120.0 : s.tempos.front().second;
    MidiTrack t;
    t.name = "Clicks";
    for (int bar = 0; bar < 4; ++bar) {
        Note n;
        n.tick = bar * kBar; n.length = 240; n.key = 60; n.velocity = 0.8f;
        t.notes.push_back(n);
    }
    s.tracks.push_back(t);
    return s;
}
}

TEST(midi_tempo_changes_are_read_and_written) {
    const auto song = songWithTempos({{0, 120.0}, {kBar, 60.0}, {2 * kBar, 180.0}, {3 * kBar + 480, 90.0}});
    MidiSong back;
    std::string err;
    CHECK_MSG(readMidi(writeMidi(song), back, &err), "%s", err.c_str());
    CHECK(back.tempos.size() == 4);
    for (size_t i = 0; i < back.tempos.size() && i < song.tempos.size(); ++i) {
        CHECK(back.tempos[i].first == song.tempos[i].first);
        CHECK_NEAR(back.tempos[i].second, song.tempos[i].second, 0.001);
    }
    CHECK_NEAR(back.tempo, 120.0, 0.001);
    CHECK(back.tracks.size() == 1 && back.tracks[0].notes.size() == 4);

    // a file with one tempo: one entry, and no tempo map is made of it
    MidiSong one;
    CHECK(readMidi(writeMidi(songWithTempos({})), one, &err));
    CHECK(one.tempos.size() == 1 && one.tempos[0].first == 0);
    CHECK_NEAR(one.tempos[0].second, 120.0, 0.001);
    Project p;
    CHECK(!addTempoMap(p, one, 1, 4 * kBar));
    CHECK(p.autos.empty() && p.clips.empty());

    // a first tempo change that comes late: the writer puts song.tempo at the start, so the file reads back with both
    MidiSong lateSong = songWithTempos({{kBar, 60.0}}), late;
    lateSong.tempo = 120.0;
    CHECK(readMidi(writeMidi(lateSong), late, &err));
    CHECK(late.tempos.size() == 2 && late.tempos[0].first == 0 && late.tempos[1].first == kBar);
    CHECK_NEAR(late.tempos[0].second, 120.0, 0.001);
    CHECK_NEAR(late.tempos[1].second, 60.0, 0.001);

    // two tempo events on one tick: the later one stands
    MidiSong twice = songWithTempos({{0, 100.0}, {kBar, 70.0}});
    twice.tempos.push_back({kBar, 140.0});
    MidiSong got;
    CHECK(readMidi(writeMidi(twice), got, &err));
    CHECK(got.tempos.size() == 2);
    CHECK_NEAR(got.tempos[1].second, 140.0, 0.001);
}

TEST(midi_tempo_map_becomes_tempo_automation) {
    const auto song = songWithTempos({{0, 120.0}, {kBar, 60.0}, {2 * kBar, 180.0}});
    Project p;
    p.global->tempo.set(120.0f);
    p.songMode = true;
    auto click = std::make_shared<SampleData>();
    click->sampleRate = kSr; click->channels = 1;
    click->ch[0].assign(64, 0.9f);
    auto ch = addSampleChannel(p, click, "Click");
    p.pattern().notesFor(ch.id) = song.tracks[0].notes;
    p.pattern().steps = 64;
    p.addPatternClip(0, 0, 0);
    CHECK(addTempoMap(p, song, 1, 4 * kBar));
    CHECK(p.autos.size() == 1 && p.autos[0].target == "tempo" && p.clips.size() == 2);
    CHECK(p.clips[1].type == Clip::AutomationClip && p.clips[1].track == 1 && p.clips[1].start == 0 && p.clips[1].length == 4 * kBar);
    CHECK(p.songLengthTicks() == 4 * kBar);

    // bar 1 at 120 (2 s), bar 2 at 60 (4 s), bars 3 and 4 at 180 (1.333 s each)
    p.global->tempo.set(97.0f);         // wherever the tempo box was left
    RenderOptions o;
    o.tail = RenderOptions::Cut;
    const auto out = renderOffline(p, o);
    const int64 want[4] = {0, 96000, 96000 + 192000, 96000 + 192000 + 64000};
    for (int b = 0; b < 4; ++b) {
        // the click starts within one 64-sample step of where the tempo map puts it
        int64 found = -1;
        for (int64 i = std::max<int64>(0, want[b] - 200); i < want[b] + 200 && i < out->frames(); ++i)
            if (std::fabs(out->ch[0][(size_t)i]) > 0.5f) { found = i; break; }
        CHECK_MSG(found >= 0 && std::llabs(found - want[b]) <= 64, "bar %d: the click is at %lld, the tempo map says %lld", b + 1, (long long)found, (long long)want[b]);
    }
    CHECK_MSG(std::llabs(out->frames() - (96000 + 192000 + 128000)) <= 128, "%lld samples", (long long)out->frames());

    // with an effect that makes the mix late (a look-ahead limiter on the master) the file is still as long as the
    // song: the end used to come early by the effect's delay whenever the tempo was automated
    p.setSlot(kMasterTrack, 0, std::make_shared<Lid>());
    const auto late = renderOffline(p, o);
    CHECK(p.compile()->latency > 0);
    CHECK_MSG(late->frames() == out->frames(), "%lld samples with the limiter, %lld without", (long long)late->frames(), (long long)out->frames());
    for (int b = 0; b < 4; ++b) CHECK(peakOf(late->ch[0], (size_t)want[b], (size_t)want[b] + 128) > 0.3f);

    // saved and loaded, the map is still there
    Project q;
    CHECK(q.fromJson(p.toJson()));
    CHECK(q.autos.size() == 1 && q.autos[0].points.size() == p.autos[0].points.size() && q.autos[0].target == "tempo");
}
