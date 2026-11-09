// Milestone 14: a group of playlist tracks can be collapsed. Its tracks are hidden, not silenced.
#include "Util.h"
using namespace pulp;
using namespace tu;

namespace {
std::vector<int64> onsets(const std::vector<float>& d, float thr = 0.01f) {
    std::vector<int64> o;
    for (size_t i = 0; i < d.size(); ++i)
        if (std::fabs(d[i]) >= thr && (i == 0 || std::fabs(d[i - 1]) < thr * 0.1f)) o.push_back((int64)i);
    return o;
}
}

TEST(collapsed_track_group_is_hidden_but_still_plays_and_is_saved) {
    Project p;
    p.global->tempo.set(120.0f);
    auto s = std::make_shared<SampleData>();
    s->sampleRate = 48000.0;
    s->channels = 1;
    s->ch[0].assign(32, 0.0f);
    s->ch[0][0] = 0.5f;
    auto a = addSampleChannel(p, s, "Click");
    Project::setStep(p.patterns[0].notesFor(a.id), 0, true);
    Project::setStep(p.patterns[0].notesFor(a.id), 8, true);
    p.songMode = true;
    const int bar = 16 * kTicksPerStep;
    p.addPatternClip(0, 1, 0);           // on the top track of the group
    p.addPatternClip(0, 2, bar);         // on a grouped track
    p.addPatternClip(0, 3, 2 * bar);     // on a second grouped track
    p.addPatternClip(0, 4, 3 * bar);     // outside the group
    p.tracks[2].grouped = p.tracks[3].grouped = true;

    CHECK(p.trackIsGroupHead(1));
    CHECK(!p.trackIsGroupHead(0) && !p.trackIsGroupHead(2) && !p.trackIsGroupHead(3) && !p.trackIsGroupHead(4));
    CHECK(p.trackGroupHead(3) == 1 && p.trackGroupHead(2) == 1 && p.trackGroupHead(1) == 1 && p.trackGroupHead(4) == 4);
    for (int t = 0; t < 6; ++t) CHECK(!p.trackHidden(t));

    RenderOptions o;
    const auto open = renderOffline(p, o);
    CHECK(onsets(open->ch[0]).size() == 8);

    p.tracks[1].collapsed = true;
    CHECK(!p.trackHidden(0) && !p.trackHidden(1) && p.trackHidden(2) && p.trackHidden(3) && !p.trackHidden(4));
    // what is hidden plays exactly as before
    const auto shut = renderOffline(p, o);
    CHECK(shut->frames() == open->frames());
    CHECK(shut->ch[0] == open->ch[0]);
    CHECK(shut->ch[1] == open->ch[1]);

    // the flag means nothing on a track that is not the top of a group, and out-of-range tracks are never hidden
    p.tracks[4].collapsed = true;
    CHECK(!p.trackHidden(5));
    CHECK(!p.trackHidden(-1) && !p.trackHidden((int)p.tracks.size()));
    p.tracks[4].collapsed = false;

    // saved and loaded
    Project q;
    CHECK(q.fromJson(p.toJson()));
    CHECK(q.toJson() == p.toJson());
    CHECK(q.tracks[1].collapsed && q.trackHidden(2) && q.trackHidden(3) && !q.trackHidden(4));

    // a project saved before this existed (no "collapsed" key) loads with every group shown
    p.tracks[1].collapsed = false;
    const std::string old = p.toJson().dump();
    CHECK(old.find("collapsed") == std::string::npos);
    Project r;
    CHECK(r.fromJson(Json::parse(old)));
    CHECK(!r.tracks[1].collapsed && !r.trackHidden(2));
}
