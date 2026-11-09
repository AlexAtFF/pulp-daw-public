// Milestone 7: loop recording with takes, comping, punch in / out, wait for input,
// latency calibration, metronome sounds.
#include "Util.h"
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0, kBpm = 120.0;       // 25 samples per tick
const int kLat = 137;                           // simulated round trip of the sound card
const int kOffset = Engine::kInputDelay + kLat; // what a take has to be shifted by

std::shared_ptr<SampleData> noise(int frames, uint32 seed = 9u) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = kSr;
    s->channels = 1;
    dsp::Noise a(seed);
    s->ch[0].resize((size_t)frames);
    for (auto& v : s->ch[0]) v = 0.3f * a.next();
    return s;
}

// Runs the engine for `total` samples while a performer plays perf[] in time with what they hear:
// the input the engine sees is the performance delayed by the round-trip latency.
struct Session {
    Project p;
    Engine e;
    std::vector<float> take;
    std::vector<RecSegment> segs;
    void start() {
        p.global->tempo.set((float)kBpm);
        p.songMode = true;
        p.mixer[1].input = 1;
        p.prepare(kSr);
        e.prepare(kSr);
        e.setState(p.compile());
    }
    void run(const SampleData& perf, int from, int total, int block = 256) {
        std::vector<float> in0((size_t)block), in1((size_t)block, 0.0f), o0((size_t)block), o1((size_t)block), chunk((size_t)8192 * 2);
        for (int c = from; c < from + total; c += block) {
            for (int i = 0; i < block; ++i) { const int src = c + i - kLat; in0[(size_t)i] = src >= 0 && src < (int)perf.frames() ? perf.ch[0][(size_t)src] : 0.0f; }
            const float* in[2] = {in0.data(), in1.data()};
            float* out[2] = {o0.data(), o1.data()};
            e.process(in, 2, out, 2, block);
            for (int got; (got = e.recordRead(chunk.data(), 8192)) > 0;)
                for (int i = 0; i < got; ++i) take.push_back(chunk[(size_t)i * 2]);
        }
    }
    void finish() {
        e.stopRecording();
        RecSegment s;
        while (e.recordSegment(s)) segs.push_back(s);
    }
};

std::shared_ptr<SampleData> renderLen(Project& p, int64 len) {
    RenderOptions o;
    o.lengthSamples = len;
    return renderOffline(p, o);
}
}

TEST(loop_recording_keeps_every_pass_as_a_take) {
    const int pass = 48000;                     // the loop is two beats
    const int total = pass * 3 + pass / 2;
    auto perf = noise(total);
    Session s;
    s.p.loopStart = 0;
    s.p.loopEnd = 1920;
    s.start();
    s.e.startRecording();
    s.e.play();
    s.run(*perf, 0, total);
    s.finish();
    CHECK(s.e.recordDropped() == 0);
    CHECK_MSG(s.segs.size() == 4, "%d segments", (int)s.segs.size());
    for (size_t k = 0; k < s.segs.size(); ++k) {
        CHECK_MSG(s.segs[k].frame == (int64)k * pass, "pass %d starts at frame %lld", (int)k, (long long)s.segs[k].frame);
        CHECK(s.segs[k].tick == 0.0);
    }
    const auto pieces = planTakes(s.segs, (int64)s.take.size(), kOffset, samplesPerTick(kBpm, kSr));
    CHECK(pieces.size() == 4);
    if (pieces.size() != 4) return;
    for (int k = 0; k < 3; ++k) CHECK(pieces[(size_t)k].startTick == 0 && pieces[(size_t)k].lengthTicks == 1920);
    CHECK(pieces[3].lengthTicks > 900 && pieces[3].lengthTicks <= 960);

    // each take, played alone, is exactly what was performed during its pass
    auto rec = std::make_shared<SampleData>();
    rec->sampleRate = kSr; rec->channels = 1; rec->ch[0] = s.take;
    for (int k = 0; k < 4; ++k) {
        Project q;
        q.global->tempo.set((float)kBpm);
        q.songMode = true;
        const int id = q.addAudio(rec, "take");
        auto& clip = q.addAudioClip(id, 0, pieces[(size_t)k].startTick, 1);
        clip.length = pieces[(size_t)k].lengthTicks;
        clip.sourceOffsetFrames = pieces[(size_t)k].sourceOffsetFrames;
        const int len = (int)(clip.length * 25);
        auto out = renderLen(q, len);
        double worst = 0.0;
        for (int i = 200; i < len - 200; ++i) worst = std::max(worst, (double)std::fabs(out->ch[0][(size_t)i] - perf->ch[0][(size_t)(k * pass + i)]));
        CHECK_MSG(worst < 1e-7, "take %d is off by %g", k + 1, worst);
    }
}

TEST(takes_are_auditioned_and_comped) {
    Project p;
    p.global->tempo.set((float)kBpm);
    p.songMode = true;
    const double hz[3] = {300.0, 500.0, 700.0};
    for (int k = 0; k < 3; ++k) {
        const int id = p.addAudio(sine(hz[k], 2.0, kSr, 0.4f), "take " + std::to_string(k + 1));
        auto& c = p.addAudioClip(id, k, 0, 1);
        c.takeGroup = 5;
        c.take = k + 1;
        c.muted = k != 2;                   // the newest take plays
    }
    auto whole = renderLen(p, 96000);
    CHECK_NEAR(measureHz(whole->ch[0], 2000, 94000, kSr), 700.0, 0.2);
    // audition take 1: it plays, the others are silenced over its whole length
    p.useTake(0);
    CHECK(!p.clips[0].muted && p.clips[1].muted && p.clips[2].muted && p.clips.size() == 3);
    CHECK_NEAR(measureHz(renderLen(p, 96000)->ch[0], 2000, 94000, kSr), 300.0, 0.2);
    // comp: cut take 2 between beats one and two, and use that piece
    const int mid = p.splitClip(1, 960);
    CHECK(mid >= 0);
    CHECK(p.splitClip(mid, 1920) >= 0);
    CHECK(p.clips[(size_t)mid].start == 960 && p.clips[(size_t)mid].length == 960 && p.clips[(size_t)mid].offset == 960);
    p.useTake(mid);
    auto comp = renderLen(p, 96000);
    CHECK_NEAR(measureHz(comp->ch[0], 2000, 22000, kSr), 300.0, 0.5);          // take 1
    CHECK_NEAR(measureHz(comp->ch[0], 26000, 46000, kSr), 500.0, 0.5);         // take 2
    CHECK_NEAR(measureHz(comp->ch[0], 50000, 94000, kSr), 300.0, 0.5);         // take 1 again
    // nothing doubles up: the level never rises above one take's
    CHECK(peakOf(comp->ch[0]) < 0.41f);
    // the joins are short fades, not clicks or gaps
    CHECK(rmsOf(comp->ch[0], 23000, 25000) > 0.2);
    // clips outside the group are never touched
    const int other = p.addAudio(sine(1000.0, 2.0, kSr, 0.1f), "other");
    p.addAudioClip(other, 9, 0, 1);
    const size_t n = p.clips.size();
    p.useTake(mid);
    CHECK(p.clips.size() == n && !p.clips.back().muted);
    // a split leaves each part playing what it played before
    Project q;
    q.global->tempo.set((float)kBpm);
    q.songMode = true;
    auto src = noise(96000, 3u);
    const int id = q.addAudio(src, "n");
    q.addAudioClip(id, 0, 0);
    auto before = renderLen(q, 96000);
    q.splitClip(0, 1000);
    auto after = renderLen(q, 96000);
    const int cut = 1000 * 25;
    CHECK(maxAbsDiff(std::vector<float>(before->ch[0].begin(), before->ch[0].begin() + cut - 200), std::vector<float>(after->ch[0].begin(), after->ch[0].begin() + cut - 200)) == 0.0);
    CHECK(maxAbsDiff(std::vector<float>(before->ch[0].begin() + cut + 200, before->ch[0].end()), std::vector<float>(after->ch[0].begin() + cut + 200, after->ch[0].end())) == 0.0);
}

TEST(punch_in_and_out_records_only_the_range) {
    const int total = 90000;
    auto perf = noise(total, 31u);
    Session s;
    s.start();
    const double spt = samplesPerTick(kBpm, kSr);
    const double pin = 960.0, pout = 2880.0;
    // the engine keeps the input open a little past the punch-out, because the end of the performance arrives late
    s.e.setPunch(pin, pout + (kOffset + 128) / spt);
    s.e.startRecording();
    s.e.play();
    s.run(*perf, 0, total);
    s.finish();
    CHECK_MSG(s.segs.size() == 1, "%d segments", (int)s.segs.size());
    if (s.segs.empty()) return;
    CHECK(s.segs[0].tick <= pin && s.segs[0].tick > pin - 64.0 / spt - 1e-9);
    // only about the punch range was written, not the whole run
    CHECK_MSG(std::llabs((int64)s.take.size() - (int64)((pout - pin) * spt + kOffset + 128)) <= 192, "%d frames recorded", (int)s.take.size());
    const auto pieces = planTakes(s.segs, (int64)s.take.size(), kOffset, spt, pin, pout);
    CHECK(pieces.size() == 1);
    if (pieces.empty()) return;
    CHECK(pieces[0].startTick == 960 && pieces[0].lengthTicks == 1920);
    auto rec = std::make_shared<SampleData>();
    rec->sampleRate = kSr; rec->channels = 1; rec->ch[0] = s.take;
    Project q;
    q.global->tempo.set((float)kBpm);
    q.songMode = true;
    const int id = q.addAudio(rec, "punch");
    auto& clip = q.addAudioClip(id, 0, pieces[0].startTick, 1);
    clip.length = pieces[0].lengthTicks;
    clip.sourceOffsetFrames = pieces[0].sourceOffsetFrames;
    auto out = renderLen(q, total);
    const int a = (int)(pin * spt), b = (int)(pout * spt);
    CHECK(peakOf(out->ch[0], 0, (size_t)a) == 0.0f);
    CHECK(peakOf(out->ch[0], (size_t)b + 2, (size_t)total) == 0.0f);
    double worst = 0.0;
    for (int i = a + 200; i < b - 200; ++i) worst = std::max(worst, (double)std::fabs(out->ch[0][(size_t)i] - perf->ch[0][(size_t)i]));
    CHECK_MSG(worst < 1e-7, "the punched part is off by %g", worst);

    // punch inside a loop: every pass through the range is its own take
    Session l;
    l.p.loopStart = 0;
    l.p.loopEnd = 1920;                        // 48000 samples
    l.start();
    l.e.setPunch(480.0, 1440.0);
    l.e.startRecording();
    l.e.play();
    auto perf2 = noise(48000 * 3, 41u);
    l.run(*perf2, 0, 48000 * 3 - 1024);
    l.finish();
    CHECK_MSG(l.segs.size() == 3, "%d punch passes", (int)l.segs.size());
    for (auto& sg : l.segs) CHECK(sg.tick <= 480.0 && sg.tick > 470.0);
}

TEST(wait_for_input_starts_the_take_when_sound_arrives) {
    Project p;
    p.global->tempo.set((float)kBpm);
    p.songMode = true;
    p.mixer[1].input = 1;
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setState(p.compile());
    e.startRecording();
    e.play(0.0, true);
    const int block = 64;
    std::vector<float> in0((size_t)block, 0.0f), in1((size_t)block, 0.0f), o0((size_t)block), o1((size_t)block);
    const float* in[2] = {in0.data(), in1.data()};
    float* out[2] = {o0.data(), o1.data()};
    // quiet room: armed, not moving
    for (int i = 0; i < 300; ++i) {
        for (auto& v : in0) v = 0.004f;      // below the threshold
        e.process(in, 2, out, 2, block);
    }
    CHECK(e.waitingForInput());
    CHECK(!e.isPlaying());
    CHECK(e.positionTicks() == 0.0);
    CHECK(!e.recordStarted());
    // the singer starts
    for (auto& v : in0) v = 0.3f;
    e.process(in, 2, out, 2, block);         // this input reaches the engine one quantum later
    e.process(in, 2, out, 2, block);
    CHECK(e.isPlaying());
    CHECK(!e.waitingForInput());
    CHECK(e.recordStarted());
    CHECK(e.recordStartTick() == 0.0);
    for (int i = 0; i < 100; ++i) e.process(in, 2, out, 2, block);
    CHECK(e.positionTicks() > 200.0);
    // the take begins with the sound that started it
    std::vector<float> rec((size_t)8192 * 2);
    const int got = e.recordRead(rec.data(), 8192);
    CHECK(got > 5000);
    CHECK(rec[0] == 0.3f);
    // stop cancels the wait
    e.stop();
    e.stop();
    for (auto& v : in0) v = 0.0f;
    for (int i = 0; i < 4; ++i) e.process(in, 2, out, 2, block);       // the room goes quiet again
    e.play(0.0, true);
    for (int i = 0; i < 10; ++i) e.process(in, 2, out, 2, block);
    CHECK(e.waitingForInput());
    e.stop();
    e.process(in, 2, out, 2, block);
    CHECK(!e.waitingForInput() && !e.isPlaying());
    // a played note starts it too
    auto ch = addSampleChannel(p, sine(440, 0.2, kSr));
    e.setState(p.compile());
    e.play(0.0, true);
    e.process(in, 2, out, 2, block);
    CHECK(e.waitingForInput());
    e.noteOn(0, 60, 0.8f);
    e.process(in, 2, out, 2, block);
    CHECK(e.isPlaying());
    (void)ch;
}

TEST(latency_calibration_measures_the_round_trip) {
    for (int delay : {64, 500, 2048, 7001}) {
        for (float cable : {1.0f, 0.05f}) {
            Project p;
            p.prepare(kSr);
            Engine e;
            e.prepare(kSr);
            e.setState(p.compile());
            const int block = 64;
            std::vector<float> line((size_t)delay, 0.0f);        // the cable from the output back to input 1
            size_t w = 0;
            std::vector<float> in0((size_t)block), in1((size_t)block, 0.0f), o0((size_t)block), o1((size_t)block), rec, chunk((size_t)4096 * 2);
            dsp::Noise hiss(77u);
            e.startCalibration();
            CHECK(e.calibrating());
            float loudest = 0.0f;
            for (int c = 0; c < (int)(1.4 * kSr); c += block) {
                for (int i = 0; i < block; ++i) in0[(size_t)i] = cable * line[(w + (size_t)i) % line.size()] + 0.0005f * hiss.next();
                const float* in[2] = {in0.data(), in1.data()};
                float* out[2] = {o0.data(), o1.data()};
                e.process(in, 2, out, 2, block);
                for (int i = 0; i < block; ++i) { line[(w + (size_t)i) % line.size()] = o0[(size_t)i]; loudest = std::max(loudest, std::fabs(o0[(size_t)i])); }
                w = (w + (size_t)block) % line.size();
                for (int got; (got = e.recordRead(chunk.data(), 4096)) > 0;)
                    for (int i = 0; i < got; ++i) rec.push_back(chunk[(size_t)i * 2]);
            }
            CHECK(!e.calibrating());
            CHECK_MSG(loudest > 0.05f && loudest <= 0.16f, "the test bursts peak at %g", loudest);       // audible but quiet
            float q = 0.0f;
            const int lag = Engine::measureRoundTrip(rec.data(), (int)rec.size(), kSr, &q);
            // the delay line here stands for output latency + input latency: the measurement adds the engine's own input delay
            const int expect = (delay / block) * block + (delay % block ? block : 0);
            (void)expect;
            CHECK_MSG(lag == delay + Engine::kInputDelay, "delay %d, cable %.2f: measured %d (quality %.2f)", delay, cable, lag, q);
            CHECK(q > 0.8f);
        }
    }
    // nothing connected: no result, rather than a wrong one
    std::vector<float> nothing((size_t)(1.2 * kSr), 0.0f);
    CHECK(Engine::measureRoundTrip(nothing.data(), (int)nothing.size(), kSr) == -1);
    dsp::Noise n(3u);
    for (auto& v : nothing) v = 0.2f * n.next();
    float q = 1.0f;
    CHECK(Engine::measureRoundTrip(nothing.data(), (int)nothing.size(), kSr, &q) == -1);
    CHECK(q < 0.5f);
}

TEST(metronome_sounds_click_on_the_beat) {
    std::vector<std::vector<float>> outs;
    for (int sound = 0; sound < Engine::NumClickSounds; ++sound) {
        Project p;
        p.global->tempo.set((float)kBpm);
        p.prepare(kSr);
        Engine e;
        e.prepare(kSr);
        e.setState(p.compile());
        e.setMetronome(true);
        e.setMetronomeSound(sound);
        CHECK(e.metronomeSound() == sound);
        e.play();
        const int n = 96000;
        std::vector<float> l((size_t)n), r((size_t)n);
        float* out[2] = {l.data(), r.data()};
        e.process(nullptr, 0, out, 2, n);
        for (int beat = 0; beat < 4; ++beat) {
            CHECK_MSG(peakOf(l, (size_t)(beat * 24000), (size_t)(beat * 24000 + 200)) > 0.1f, "sound %d: no click on beat %d", sound, beat);
            CHECK(peakOf(l, (size_t)(beat * 24000 + 20000), (size_t)(beat * 24000 + 23900)) < 0.002f);        // and quiet between beats
        }
        CHECK(peakOf(l, 0, 2000) > peakOf(l, 24000, 26000) * 1.15f);         // beat one stands out
        CHECK(peakOf(l) < 0.7f);
        CHECK(l == r);
        outs.push_back(l);
    }
    for (size_t a = 0; a < outs.size(); ++a)
        for (size_t b = a + 1; b < outs.size(); ++b) CHECK(maxAbsDiff(outs[a], outs[b]) > 0.05);
}

TEST(a_mixer_track_can_play_on_another_hardware_output) {
    Project p;
    p.global->tempo.set((float)kBpm);
    auto ch = addSampleChannel(p, sine(440.0, 0.5, kSr, 0.4f));
    p.channels[0].mixerTrack = 2;
    Project::setStep(p.pattern().notesFor(ch.id), 0, true);
    p.mixer[2].output = 2;                       // outputs 3+4 as well
    auto run = [&](int numOut, std::vector<std::vector<float>>& out) {
        p.prepare(kSr);
        Engine e;
        e.prepare(kSr);
        e.setState(p.compile());
        e.play();
        out.assign((size_t)numOut, std::vector<float>(24000));
        for (int c = 0; c < 24000; c += 300) {
            float* ptr[8];
            for (int k = 0; k < numOut; ++k) ptr[k] = out[(size_t)k].data() + c;
            e.process(nullptr, 0, ptr, numOut, 300);
        }
    };
    std::vector<std::vector<float>> out;
    run(6, out);
    CHECK(peakOf(out[0]) > 0.3f);
    CHECK(out[2] == out[0] && out[3] == out[1]);                 // the same signal on the second pair
    CHECK(peakOf(out[4]) == 0.0f && peakOf(out[5]) == 0.0f);     // and nothing on a pair nobody chose
    // off the master, it is heard on its own output only
    p.setRoute(2, kMasterTrack, false);
    run(4, out);
    CHECK(peakOf(out[0]) == 0.0f && peakOf(out[2]) > 0.3f);
    // a device with only two outputs just does not get it
    run(2, out);
    CHECK(peakOf(out[0]) == 0.0f);
    // the setting is saved
    Project q;
    CHECK(q.fromJson(p.toJson()));
    CHECK(q.mixer[2].output == 2 && q.mixer[3].output == 0);
}

TEST(a_take_without_a_loop_runs_past_the_end_of_the_song) {
    Project p;
    p.global->tempo.set((float)kBpm);
    p.songMode = true;
    p.addPatternClip(0, 0, 0);               // the song is one bar long
    p.loopStart = 0;
    p.loopEnd = 960;
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    p.recordingLinear = true;
    e.setState(p.compile());
    e.play();
    std::vector<float> l(48000), r(48000);
    float* out[2] = {l.data(), r.data()};
    for (int i = 0; i < 5; ++i) e.process(nullptr, 0, out, 2, 48000);
    CHECK_MSG(e.positionTicks() > 3840.0 * 2.0, "position %.0f: the song wrapped", e.positionTicks());
    // normally the loop region holds it
    p.recordingLinear = false;
    e.setState(p.compile());
    e.stop();
    e.stop();
    e.play();
    for (int i = 0; i < 5; ++i) e.process(nullptr, 0, out, 2, 48000);
    CHECK(e.positionTicks() < 960.0);
}
