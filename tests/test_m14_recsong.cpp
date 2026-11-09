// Milestone 14: notes played while recording in song mode land in the pattern that plays at that place.
#include "Util.h"
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0, kBpm = 120.0;       // 25 samples per tick
const int kBar = 16 * kTicksPerStep;
int64 at(double ticks) { return (int64)std::llround(ticks * samplesPerTick(kBpm, kSr)); }
std::vector<int64> onsets(const std::vector<float>& d, float thr = 0.01f) {
    std::vector<int64> o;
    for (size_t i = 0; i < d.size(); ++i)
        if (std::fabs(d[i]) >= thr && (i == 0 || std::fabs(d[i - 1]) < thr * 0.1f)) o.push_back((int64)i);
    return o;
}
Note played(int songTick, int length = 120) {
    Note n;
    n.tick = songTick;
    n.length = length;
    return n;
}
}

TEST(notes_recorded_in_song_mode_go_where_the_song_is) {
    Project p;
    p.global->tempo.set((float)kBpm);
    auto s = std::make_shared<SampleData>();
    s->sampleRate = kSr;
    s->channels = 1;
    s->ch[0].assign(32, 0.0f);
    s->ch[0][0] = 0.5f;
    auto ch = addSampleChannel(p, s, "Click");
    p.songMode = true;
    Project::setStep(p.patterns[0].notesFor(ch.id), 0, true);
    p.addPatternClip(0, 0, 0);
    const int first = p.patterns[0].id;

    // where a clip of the current pattern is playing, the note joins that pattern, at its place in the clip
    CHECK(p.recordSongNote(ch.id, played(480)) == first);
    CHECK(p.patterns.size() == 1 && p.clips.size() == 1);
    CHECK(p.patterns[0].notesFor(ch.id).size() == 2 && p.patterns[0].notesFor(ch.id)[1].tick == 480);

    // where nothing of it plays, a new pattern takes the note, in a clip that starts on the bar line
    const int take = p.recordSongNote(ch.id, played(2 * kBar + 240));
    CHECK(take != first && take != 0);
    CHECK(p.patterns.size() == 2 && p.currentPattern == 1 && p.patterns[1].id == take);
    CHECK(p.clips.size() == 2);
    CHECK(p.clips[1].patternId == take && p.clips[1].start == 2 * kBar && p.clips[1].length == kBar && p.clips[1].track == 0);
    CHECK(p.patterns[1].notesFor(ch.id).size() == 1 && p.patterns[1].notesFor(ch.id)[0].tick == 240);

    // later in the same take: the take's clip reaches forward, no third pattern
    CHECK(p.recordSongNote(ch.id, played(4 * kBar + 100), take) == take);
    CHECK(p.patterns.size() == 2 && p.clips.size() == 2);
    CHECK(p.clips[1].length == 3 * kBar);
    CHECK(p.patterns[1].notesFor(ch.id).back().tick == 2 * kBar + 100);

    // a held note that runs past the end of its clip makes the clip a bar longer
    CHECK(p.recordSongNote(ch.id, played(5 * kBar - 240, 480), take) == take);
    CHECK(p.clips[1].length == 4 * kBar);

    // another clip stands in the way on that track: the take cannot reach over it, a new one starts beside it
    p.addPatternClip(0, 0, 7 * kBar);
    p.currentPattern = 1;
    const int second = p.recordSongNote(ch.id, played(8 * kBar + 10), take);
    CHECK(second != take && second != first);
    CHECK(p.patterns.size() == 3 && p.currentPattern == 2);
    CHECK(p.clips.back().patternId == second && p.clips.back().start == 8 * kBar && p.clips.back().track == 0);
    // ...and where the track is taken, the new clip goes on the next free one
    p.currentPattern = 1;
    const int third = p.recordSongNote(ch.id, played(7 * kBar + 500));
    // (the current pattern's clip does not cover bar 8, the first pattern's does: but that is not the current pattern)
    CHECK(third != take && third != second && third != first);
    CHECK(p.clips.back().start == 7 * kBar && p.clips.back().track == 1);

    // a clip that starts part-way into its pattern: the note is placed by the clip's offset
    p.addPattern("Offset");
    p.currentPattern = (int)p.patterns.size() - 1;
    Clip& oc = p.addPatternClip(p.currentPattern, 3, 10 * kBar);
    oc.offset = 960;
    const int offsetPattern = p.pattern().id;
    CHECK(p.recordSongNote(ch.id, played(10 * kBar + 100)) == offsetPattern);
    CHECK(p.pattern().notesFor(ch.id).size() == 1 && p.pattern().notesFor(ch.id)[0].tick == 1060);

    // an empty pattern that is nowhere in the song is used rather than left behind
    p.addPattern("Empty");
    p.currentPattern = (int)p.patterns.size() - 1;
    const size_t patterns = p.patterns.size();
    CHECK(p.recordSongNote(ch.id, played(12 * kBar)) == p.pattern().id);
    CHECK(p.patterns.size() == patterns);

    // every note sounds exactly where it was played
    RenderOptions o;
    const auto out = renderOffline(p, o);
    const auto on = onsets(out->ch[0]);
    const std::vector<int64> want = {at(0), at(480), at(2 * kBar + 240), at(4 * kBar + 100), at(5 * kBar - 240),
                                     at(7 * kBar), at(7 * kBar + 480), at(7 * kBar + 500), at(8 * kBar + 10), at(10 * kBar + 100), at(12 * kBar)};
    CHECK_MSG(on == want, "found %d onsets of %d", (int)on.size(), (int)want.size());

    // and all of it is in the saved project
    Project q;
    CHECK(q.fromJson(p.toJson()));
    CHECK(q.toJson() == p.toJson());
}
