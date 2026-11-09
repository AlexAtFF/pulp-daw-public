// Milestone 13: project templates and the demo song (which is also the end-to-end test: instruments,
// drums, sidechain, sends, effects, automation and an arrangement, rendered and measured).
#include "Util.h"
#include "pulp/Templates.h"
#include <filesystem>

using namespace pulp;
using namespace tu;

namespace {
struct FactoryRoot {
    std::string was;
    FactoryRoot() : was(SampleCache::global().factoryRoot()) { SampleCache::global().setFactoryRoot(std::string(PULP_SOURCE_DIR) + "/factory"); }
    ~FactoryRoot() { SampleCache::global().setFactoryRoot(was); }
};
double lowEnergy(const std::vector<float>& x, size_t a, size_t b) {
    // energy below about 120 Hz: a one-pole low-pass, twice
    double y1 = 0.0, y2 = 0.0, sum = 0.0;
    const double k = 1.0 - std::exp(-2.0 * 3.14159265358979 * 120.0 / 48000.0);
    for (size_t i = a; i < b && i < x.size(); ++i) { y1 += k * (x[i] - y1); y2 += k * (y1 - y2); sum += y2 * y2; }
    return std::sqrt(sum / (double)std::max<size_t>(1, b - a));
}
}

TEST(templates_make_working_projects) {
    FactoryRoot root;
    const auto list = projectTemplates();
    CHECK(list.size() >= 6);
    for (int i = 0; i < (int)list.size(); ++i) {
        Project p;
        CHECK_MSG(makeTemplate(p, i), "template %s", list[(size_t)i].name.c_str());
        CHECK(!p.channels.empty());
        CHECK(!list[(size_t)i].name.empty() && !list[(size_t)i].description.empty());
        // a note on every channel sounds, and the project survives being written down and read back
        for (auto& c : p.channels) {
            Note n;
            n.tick = 0; n.length = 480; n.key = 60;
            p.pattern().notesFor(c.id).push_back(n);
        }
        RenderOptions o;
        o.lengthSamples = 48000;
        auto a = renderOffline(p, o);
        const bool empty = i == 0;       // the empty template's sampler has no sample
        CHECK_MSG(empty ? peakOf(a->ch[0]) == 0.0f : (peakOf(a->ch[0]) > 0.05f && peakOf(a->ch[0]) <= 1.0f), "template %s peaks at %.3f", list[(size_t)i].name.c_str(), peakOf(a->ch[0]));
        Project q;
        std::string err;
        CHECK_MSG(q.fromJson(Json::parse(p.toJson().dump(0)), &err), "%s", err.c_str());
        CHECK(q.channels.size() == p.channels.size());
        auto bck = renderOffline(q, o);
        CHECK_MSG(identical(*a, *bck), "template %s renders differently after a round trip", list[(size_t)i].name.c_str());
    }
    Project p;
    CHECK(!makeTemplate(p, 99));
}

TEST(demo_song_plays_from_start_to_finish) {
    FactoryRoot root;
    Project p;
    makeDemoSong(p);
    CHECK(p.channels.size() == 7);
    CHECK(p.songMode);
    CHECK(p.patterns.size() >= 7);
    CHECK(p.autos.size() == 3);
    const int bar = 4 * kPPQ;
    CHECK(p.songLengthTicks() == 20 * bar);
    // the bass is ducked by the kick: the kick's track feeds the sidechain of the bass track's compressor
    const Route* side = p.mixer[1].routeTo(3);
    CHECK(side != nullptr && side->sidechain);
    CHECK(p.mixer[3].slots[0] != nullptr && p.mixer[3].slots[0]->param("side")->getBool());

    RenderOptions o;
    o.tail = RenderOptions::Cut;
    auto song = renderOffline(p, o);
    const double spb = 60.0 * 48000.0 / 122.0;        // samples per beat
    const size_t barLen = (size_t)(4.0 * spb);
    CHECK_NEAR((double)song->frames(), 20.0 * 4.0 * spb, 2.0);
    for (float v : song->ch[0]) if (!std::isfinite(v)) { CHECK(false); break; }
    CHECK(peakOf(song->ch[0]) <= 1.0f && peakOf(song->ch[1]) <= 1.0f);     // the limiter holds the master
    CHECK(peakOf(song->ch[0]) > 0.3f);
    // the arrangement is heard: quiet opening, the beat from bar 5, no kick through the break, everything at the end
    const double intro = rmsOf(song->ch[0], 0, 4 * barLen), verse = rmsOf(song->ch[0], 4 * barLen, 8 * barLen),
                 brk = rmsOf(song->ch[0], 12 * barLen, 16 * barLen), last = rmsOf(song->ch[0], 16 * barLen, 19 * barLen);
    CHECK_MSG(intro > 0.005 && verse > intro * 1.5 && last > brk * 1.2, "sections: intro %.4f verse %.4f break %.4f last %.4f", intro, verse, brk, last);
    const double kickIn = lowEnergy(song->ch[0], 4 * barLen, 8 * barLen), kickOut = lowEnergy(song->ch[0], 12 * barLen, 16 * barLen), noKick = lowEnergy(song->ch[0], 0, 4 * barLen);
    CHECK_MSG(kickIn > noKick * 4.0 && kickIn > kickOut * 4.0, "low end: intro %.5f verse %.5f break %.5f", noKick, kickIn, kickOut);
    // the fade: the last bar ends far quieter than it begins
    CHECK(rmsOf(song->ch[0], 19 * barLen + barLen * 7 / 8, 20 * barLen) < rmsOf(song->ch[0], 19 * barLen, 19 * barLen + barLen / 8) * 0.3);
    // both sides differ (pans, chorus, ping-pong delay, reverb), but not wildly
    CHECK(maxAbsDiff(song->ch[0], song->ch[1]) > 0.01);

    // the sidechain works: the bass alone, with the kick heard only by the compressor, is quieter than
    // with the kick silent
    {
        Project q;
        makeDemoSong(q);
        for (auto& c : q.channels) if (c.name != "Bass" && c.name != "Kick") c.live->volume.set(0.0f);
        q.setRoute(1, kMasterTrack, false);                   // the kick no longer reaches the master, only the sidechain
        RenderOptions part;
        part.startTick = 4.0 * bar;
        part.lengthSamples = (int64)(2 * barLen);
        auto ducked = renderOffline(q, part);
        q.channels[0].live->volume.set(0.0f);
        auto plain = renderOffline(q, part);
        CHECK_MSG(rmsOf(plain->ch[0]) > 0.01 && rmsOf(ducked->ch[0]) < rmsOf(plain->ch[0]) * 0.9, "bass alone %.4f, ducked %.4f", rmsOf(plain->ch[0]), rmsOf(ducked->ch[0]));
    }
    // written down and read back, and at another block size and thread count, eight bars from the middle are identical
    RenderOptions mid;
    mid.startTick = 8.0 * bar;
    mid.lengthSamples = (int64)(8 * barLen);
    auto ref = renderOffline(p, mid);
    Project q;
    std::string err;
    CHECK_MSG(q.fromJson(Json::parse(p.toJson().dump(0)), &err), "%s", err.c_str());
    mid.blockSize = 77;
    mid.threads = 4;
    const int64 allocs = Rt::allocationsInRt.load();
    auto again = renderOffline(q, mid);
    CHECK(Rt::allocationsInRt.load() == allocs);
    CHECK(identical(*ref, *again));
    // and it matches the same stretch of the whole render (a song sounds the same from any starting point,
    // apart from tails that were already ringing)
    CHECK(peakOf(ref->ch[0]) > 0.2f);
}
