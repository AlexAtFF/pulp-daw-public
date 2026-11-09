// Milestone 4: song mode, playlist clips, track mute / solo, looping.
#include "Util.h"
using namespace pulp;
using namespace tu;

namespace {
std::shared_ptr<SampleData> click(int len = 32, float amp = 0.5f) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = 48000.0;
    s->channels = 1;
    s->ch[0].assign((size_t)len, 0.0f);
    s->ch[0][0] = amp;
    return s;
}
std::vector<int64> onsets(const std::vector<float>& d, float thr = 0.01f) {
    std::vector<int64> o;
    for (size_t i = 0; i < d.size(); ++i)
        if (std::fabs(d[i]) >= thr && (i == 0 || std::fabs(d[i - 1]) < thr * 0.1f)) o.push_back((int64)i);
    return o;
}
const double kSr = 48000.0, kBpm = 120.0;
int64 at(double ticks) { return (int64)std::llround(ticks * samplesPerTick(kBpm, kSr)); }
const int kBar = 16 * kTicksPerStep;

// Pattern 1: a click on steps 0 and 8.  Pattern 2: a click on step 4.
void twoPatterns(Project& p, int& chA) {
    p.global->tempo.set((float)kBpm);
    auto a = addSampleChannel(p, click(), "Click");
    chA = a.id;
    Project::setStep(p.patterns[0].notesFor(a.id), 0, true);
    Project::setStep(p.patterns[0].notesFor(a.id), 8, true);
    p.addPattern("Two");
    Project::setStep(p.patterns[1].notesFor(a.id), 4, true);
    p.songMode = true;
}
}

TEST(song_mode_plays_clips_on_exact_samples) {
    Project p;
    int ch;
    twoPatterns(p, ch);
    p.addPatternClip(0, 0, 0);
    p.addPatternClip(1, 1, kBar);
    p.addPatternClip(0, 3, 2 * kBar);
    p.addPatternClip(1, 0, 2 * kBar);            // overlaps the clip above on another track
    CHECK(p.songLengthTicks() == 3 * kBar);
    RenderOptions o;
    auto out = renderOffline(p, o);
    CHECK(out != nullptr);
    if (!out) return;
    const auto on = onsets(out->ch[0]);
    const std::vector<int64> want = {at(0), at(8 * 240), at(kBar + 4 * 240), at(2 * kBar), at(2 * kBar + 4 * 240), at(2 * kBar + 8 * 240)};
    CHECK_MSG(on == want, "found %d onsets, first %lld", (int)on.size(), on.empty() ? -1LL : (long long)on[0]);
    CHECK(out->frames() >= at(3 * kBar));
    CHECK(out->frames() < at(3 * kBar) + 48000);     // stops after the song, plus a short tail
}

TEST(clip_offset_and_length_trim_the_pattern) {
    Project p;
    int ch;
    twoPatterns(p, ch);
    // Play only steps 6..11 of pattern 1 (so just the step-8 click), starting at bar 2.
    auto& c = p.addPatternClip(0, 0, kBar);
    c.offset = 6 * 240;
    c.length = 6 * 240;
    RenderOptions o;
    auto out = renderOffline(p, o);
    const auto on = onsets(out->ch[0]);
    CHECK_MSG(on.size() == 1 && on[0] == at(kBar + 2 * 240), "onsets %d", (int)on.size());

    // A held note is cut off where its clip ends.
    Project q;
    q.global->tempo.set((float)kBpm);
    auto tone = addSampleChannel(q, sine(440, 4.0, kSr, 0.3f));
    tone.sampler()->pEnvOn->set(1);
    tone.sampler()->pRelease->set(1.0f);
    Note n; n.length = 16 * 240;
    q.patterns[0].notesFor(tone.id).push_back(n);
    q.songMode = true;
    auto& qc = q.addPatternClip(0, 0, 0);
    qc.length = 4 * 240;
    RenderOptions qo;
    qo.lengthSamples = at(8 * 240);
    auto qout = renderOffline(q, qo);
    CHECK(rmsOf(qout->ch[0], 1000, (size_t)at(3.8 * 240)) > 0.15);
    CHECK(peakOf(qout->ch[0], (size_t)at(4.3 * 240), (size_t)qo.lengthSamples) < 1.0e-3f);
}

TEST(track_mute_solo_groups_and_muted_clips) {
    auto count = [](Project& p) { RenderOptions o; return (int)onsets(renderOffline(p, o)->ch[0]).size(); };
    Project p;
    int ch;
    twoPatterns(p, ch);
    p.addPatternClip(0, 0, 0);           // 2 clicks
    p.addPatternClip(1, 1, 0);           // 1 click
    p.addPatternClip(1, 2, kBar);        // 1 click
    CHECK(count(p) == 4);
    p.tracks[0].mute = true;
    CHECK(count(p) == 2);
    p.tracks[0].mute = false;
    p.tracks[1].solo = true;
    CHECK(count(p) == 1);
    p.tracks[2].grouped = true;          // follows track 1, which is soloed
    CHECK(count(p) == 2);
    p.tracks[1].solo = false;
    p.tracks[1].mute = true;             // the group head's mute silences the group
    CHECK(count(p) == 2);
    p.tracks[1].mute = false;
    p.clips[0].muted = true;
    CHECK(count(p) == 2);
    // all of it survives save and load
    Project q;
    CHECK(q.fromJson(p.toJson()));
    CHECK(q.toJson() == p.toJson());
    CHECK(q.tracks[2].grouped && q.clips[0].muted);
}

TEST(song_loop_region_and_whole_song_loop) {
    Project p;
    int ch;
    twoPatterns(p, ch);
    p.addPatternClip(0, 0, 0);
    p.addPatternClip(1, 0, kBar);
    {   // whole song loops by default: bars 1-2 repeat
        RenderOptions o;
        o.loop = true;
        o.lengthSamples = at(4 * kBar);
        const auto on = onsets(renderOffline(p, o)->ch[0]);
        const std::vector<int64> want = {at(0), at(8 * 240), at(kBar + 4 * 240), at(2 * kBar), at(2 * kBar + 8 * 240), at(3 * kBar + 4 * 240)};
        CHECK(on == want);
    }
    {   // a loop region covering only bar 2
        p.loopStart = kBar;
        p.loopEnd = 2 * kBar;
        RenderOptions o;
        o.loop = true;
        o.startTick = kBar;
        o.lengthSamples = at(3 * kBar);
        const auto on = onsets(renderOffline(p, o)->ch[0]);
        const std::vector<int64> want = {at(4 * 240), at(kBar + 4 * 240), at(2 * kBar + 4 * 240)};
        CHECK(on == want);
    }
}

TEST(song_render_is_identical_at_any_block_size_and_make_unique) {
    std::shared_ptr<SampleData> ref;
    for (int block : {512, 5, 64, 1000}) {
        Project p;
        int ch;
        twoPatterns(p, ch);
        auto k = addSampleChannel(p, drums::makeKick({}, kSr), "Kick");
        for (int s : {0, 4, 8, 12}) Project::setStep(p.patterns[1].notesFor(k.id), s, true);
        p.patterns[1].swing = 0.3f;
        for (int b = 0; b < 4; ++b) { p.addPatternClip(b % 2, b % 3, b * kBar); p.addPatternClip(1, 4, b * kBar + 480).offset = 240; }
        RenderOptions o;
        o.blockSize = block;
        auto out = renderOffline(p, o);
        if (!ref) { ref = out; CHECK(peakOf(out->ch[0]) > 0.3f); continue; }
        CHECK_MSG(identical(*ref, *out), "block %d differs", block);
    }
    // make unique: the clip gets its own pattern and edits to it leave the original alone
    Project p;
    int ch;
    twoPatterns(p, ch);
    p.addPatternClip(0, 0, 0);
    p.addPatternClip(0, 0, kBar);
    const int ni = p.makeClipUnique(1);
    CHECK(ni == 1 && p.patterns.size() == 3);
    CHECK(p.clips[1].patternId == p.patterns[1].id && p.clips[0].patternId == p.patterns[0].id);
    p.patterns[1].notesFor(ch).clear();
    RenderOptions o;
    CHECK(onsets(renderOffline(p, o)->ch[0]).size() == 2);
}

TEST(switching_between_pattern_and_song_mode) {
    Project p;
    int ch;
    twoPatterns(p, ch);
    p.addPatternClip(1, 0, 0);
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    p.songMode = false;
    e.setState(p.compile());
    e.play();
    std::vector<float> l(48000), r(48000);
    float* out[2] = {l.data(), r.data()};
    e.process(nullptr, 0, out, 2, 9600);                 // pattern 1: click at step 0
    CHECK(peakOf(l, 0, 9600) > 0.3f);
    p.songMode = true;                                   // song mode restarts from the top and plays pattern 2 (step 4)
    e.setState(p.compile());
    e.process(nullptr, 0, out, 2, 48000);
    const auto on = onsets(l);
    CHECK_MSG(on.size() >= 1 && on[0] == at(4 * 240), "first onset %lld", on.empty() ? -1LL : (long long)on[0]);
    CHECK(e.isPlaying());
}
