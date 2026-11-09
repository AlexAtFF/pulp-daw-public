// Milestone 3: the Trio synth, slide notes, piano-roll tools, MIDI files, metronome.
#include "Util.h"
#include "pulp/MidiFile.h"
#include "pulp/NoteTools.h"
#include "pulp/Trio.h"
using namespace pulp;
using namespace tu;

namespace {
struct TrioChan { int id; std::shared_ptr<ChannelLive> live; Trio* synth; };
TrioChan addTrio(Project& p) {
    auto t = std::make_shared<Trio>();
    Trio* raw = t.get();
    auto& c = p.addChannel(t, "Trio");
    return {c.id, c.live, raw};
}
// one oscillator only, filter open, flat envelope: a clean tone for measuring
void plain(Trio& t, int shape) {
    t.param("o1shape")->set((float)shape);
    t.param("o1level")->set(1.0f);
    t.param("o2level")->set(0.0f);
    t.param("o3level")->set(0.0f);
    t.param("cutoff")->set(20000.0f);
    t.param("keytrack")->set(0.0f);
    t.param("attack")->set(0.0f);
    t.param("sustain")->set(1.0f);
}
Note mk(int tick, int len, int key, float vel = kDefaultVelocity) { Note n; n.tick = tick; n.length = len; n.key = key; n.velocity = vel; return n; }
}

TEST(trio_oscillators_are_in_tune) {
    const double sr = 48000.0;
    for (int shape = 0; shape < 5; ++shape)
        for (int key : {36, 57, 60, 72, 84}) {
            Project p;
            auto ch = addTrio(p);
            plain(*ch.synth, shape);
            p.pattern().notesFor(ch.id).push_back(mk(0, kPPQ * 8, key));
            RenderOptions o;
            o.lengthSamples = 48000;
            auto out = renderOffline(p, o);
            CHECK(out != nullptr);
            if (!out) return;
            const double hz = measureHz(out->ch[0], 2000, 46000, sr);
            const double cents = 1200.0 * std::log2(hz / keyToHz(key));
            CHECK_MSG(std::fabs(cents) < 1.0, "shape %d key %d: %.3f Hz, %.2f cents off", shape, key, hz, cents);
            CHECK(peakOf(out->ch[0]) > 0.1f && peakOf(out->ch[0]) < 1.0f);
        }
    // coarse and fine tuning move the pitch by the stated amount
    Project p;
    auto ch = addTrio(p);
    plain(*ch.synth, 0);
    ch.synth->param("o1coarse")->set(7.0f);
    ch.synth->param("o1fine")->set(50.0f);
    p.pattern().notesFor(ch.id).push_back(mk(0, kPPQ * 8, 60));
    RenderOptions o;
    o.lengthSamples = 48000;
    auto out = renderOffline(p, o);
    CHECK_NEAR(1200.0 * std::log2(measureHz(out->ch[0], 2000, 46000, sr) / keyToHz(60)), 750.0, 1.0);
}

TEST(trio_saw_is_band_limited) {
    // A naive saw at a high note folds strong aliases back below the fundamental.
    const double sr = 48000.0;
    Project p;
    auto ch = addTrio(p);
    plain(*ch.synth, BlepOsc::Saw);
    p.pattern().notesFor(ch.id).push_back(mk(0, kPPQ * 8, 96));      // about 2093 Hz
    RenderOptions o;
    o.lengthSamples = 1 << 15;
    auto out = renderOffline(p, o);
    const int n = 1 << 14;
    std::vector<float> re(out->ch[0].begin() + 8192, out->ch[0].begin() + 8192 + n), im((size_t)n, 0.0f);
    for (int i = 0; i < n; ++i) re[(size_t)i] *= (float)(0.5 - 0.5 * std::cos(kTwoPi * i / n));
    dsp::fft(re.data(), im.data(), n);
    auto mag = [&](double hz) { const int b = (int)std::lround(hz * n / sr); double m = 0; for (int k = b - 2; k <= b + 2; ++k) m = std::max(m, (double)std::hypot(re[(size_t)k], im[(size_t)k])); return m; };
    const double f0 = keyToHz(96), fund = mag(f0);
    double worst = 0.0;
    for (double hz = 100.0; hz < f0 - 200.0; hz += 37.0) worst = std::max(worst, mag(hz));
    CHECK_MSG(20.0 * std::log10(worst / fund) < -55.0, "alias below the fundamental at %.1f dB", 20.0 * std::log10(worst / fund));
}

TEST(trio_presets_all_sound_and_reload) {
    Trio t;
    const auto presets = t.factoryPresets();
    CHECK(presets.size() >= 20);
    std::vector<std::string> names;
    for (auto& pr : presets) {
        CHECK(std::find(names.begin(), names.end(), pr.name) == names.end());
        names.push_back(pr.name);
        Project p;
        auto ch = addTrio(p);
        ch.synth->loadState(pr.state);
        for (int k : {48, 60, 67}) p.pattern().notesFor(ch.id).push_back(mk(0, kPPQ * 2, k));
        RenderOptions o;
        o.lengthSamples = 48000;
        auto out = renderOffline(p, o);
        bool finite = true;
        for (float v : out->ch[0]) finite = finite && std::isfinite(v);
        CHECK_MSG(finite, "%s produced a non-finite sample", pr.name.c_str());
        CHECK_MSG(peakOf(out->ch[0]) > 0.02f, "%s is silent (peak %.4f)", pr.name.c_str(), peakOf(out->ch[0]));
        CHECK_MSG(peakOf(out->ch[0]) < 1.5f, "%s is far too loud (peak %.2f)", pr.name.c_str(), peakOf(out->ch[0]));
        // state round trip
        Project q;
        CHECK(q.fromJson(p.toJson()));
        CHECK(q.channels[0].live->plugin->saveState() == p.channels[0].live->plugin->saveState());
    }
}

TEST(slide_notes_bend_only_their_colour_group) {
    const double sr = 48000.0, bpm = 120.0;
    Project p;
    p.global->tempo.set((float)bpm);
    auto ch = addTrio(p);
    plain(*ch.synth, 0);
    Note held = mk(0, 16 * kTicksPerStep, 60);
    Note other = mk(0, 16 * kTicksPerStep, 72);
    other.color = 3;
    other.pan = 1.0f;       // right only
    held.pan = -1.0f;       // left only
    Note slide = mk(4 * kTicksPerStep, 4 * kTicksPerStep, 67);
    slide.slide = true;
    p.pattern().notesFor(ch.id) = {held, other, slide};
    sortNotes(p.pattern().notesFor(ch.id));
    RenderOptions o;
    o.lengthSamples = (int64)(16 * kTicksPerStep * samplesPerTick(bpm, sr));
    auto out = renderOffline(p, o);
    auto at = [&](double step) { return (size_t)std::llround(step * kTicksPerStep * samplesPerTick(bpm, sr)); };
    CHECK_NEAR(measureHz(out->ch[0], at(1), at(3.5), sr), keyToHz(60), 0.5);              // before the slide
    const double mid = measureHz(out->ch[0], at(5.5), at(6.5), sr);
    CHECK_MSG(mid > keyToHz(62) && mid < keyToHz(65.5), "mid-slide pitch %.2f Hz", mid);  // on the way up
    CHECK_NEAR(measureHz(out->ch[0], at(9), at(15), sr), keyToHz(67), 0.8);               // arrived and stays
    CHECK_NEAR(measureHz(out->ch[1], at(9), at(15), sr), keyToHz(72), 0.8);               // other colour untouched
}

TEST(note_tools) {
    using namespace tools;
    {   // quantize with strength
        std::vector<Note> n = {mk(250, 200, 60), mk(470, 100, 62), mk(960 + 119, 240, 64)};
        QuantizeOpts q;
        quantize(n, q);
        CHECK(n[0].tick == 240 && n[1].tick == 480 && n[2].tick == 960);
        CHECK(n[0].length == 200);
        std::vector<Note> h = {mk(300, 200, 60)};
        q.strength = 0.5f;
        quantize(h, q);
        CHECK(h[0].tick == 270);
        std::vector<Note> e = {mk(240, 200, 60)};
        q.strength = 1.0f; q.starts = false; q.ends = true;
        quantize(e, q);
        CHECK(e[0].tick == 240 && e[0].length == 240);
    }
    {   // only the selection is touched
        std::vector<Note> n = {mk(250, 100, 60), mk(730, 100, 62)};
        n[1].selected = true;
        quantize(n, {});
        CHECK(n[0].tick == 250 && n[1].tick == 720);
    }
    {   // legato, chop, glue
        std::vector<Note> n = {mk(0, 100, 60), mk(480, 100, 64), mk(1440, 100, 67)};
        legato(n);
        CHECK(n[0].length == 480 && n[1].length == 960 && n[2].length == 100);
        std::vector<Note> c = {mk(0, 960, 60)};
        chop(c, 240);
        CHECK(c.size() == 4 && c[3].tick == 720 && c[3].length == 240);
        glue(c);
        CHECK(c.size() == 1 && c[0].length == 960);
        std::vector<Note> g = {mk(0, 240, 60), mk(240, 240, 61)};
        glue(g);
        CHECK(g.size() == 2);       // different keys stay apart
    }
    {   // arpeggiate a chord
        std::vector<Note> n = {mk(0, 960, 60), mk(0, 960, 64), mk(0, 960, 67)};
        arpeggiate(n, 240, ArpUp, 1, 1.0f);
        CHECK(n.size() == 4);
        CHECK(n[0].key == 60 && n[1].key == 64 && n[2].key == 67 && n[3].key == 60);
        CHECK(n[1].tick == 240 && n[1].length == 240);
        std::vector<Note> d = {mk(0, 960, 60), mk(0, 960, 64), mk(0, 960, 67)};
        arpeggiate(d, 120, ArpDown, 2, 0.5f);
        CHECK(d.size() == 8 && d[0].key == 79 && d[1].key == 76 && d[0].length == 60);
    }
    {   // strum and flam
        std::vector<Note> n = {mk(480, 960, 60), mk(480, 960, 64), mk(480, 960, 67)};
        strum(n, 30, false, 0.0f);
        CHECK(n[0].key == 60 && n[0].tick == 480 && n[1].tick == 510 && n[2].tick == 540);
        CHECK(n[2].end() == 1440);
        std::vector<Note> f = {mk(480, 240, 60)};
        flam(f, 60, 0.5f);
        CHECK(f.size() == 2 && f[0].tick == 420 && f[0].length == 60);
        CHECK_NEAR(f[0].velocity, kDefaultVelocity * 0.5f, 1e-6);
    }
    {   // articulate, transpose, flip, reverse, shift
        std::vector<Note> n = {mk(0, 100, 60), mk(480, 100, 64)};
        articulate(n, 0.0f, 20);
        CHECK(n[0].length == 460);
        articulate(n, 0.5f, 0);
        CHECK(n[0].length == 230);
        transpose(n, 12);
        CHECK(n[0].key == 72 && n[1].key == 76);
        flipVertical(n);
        CHECK(n[0].key == 76 && n[1].key == 72);
        std::vector<Note> r = {mk(0, 240, 60), mk(480, 480, 64)};
        reverseTime(r);
        CHECK(r[0].key == 64 && r[0].tick == 0 && r[1].key == 60 && r[1].tick == 720);
        shiftTime(r, 240);
        CHECK(r[0].tick == 240);
        std::vector<Note> top = {mk(0, 100, 127)};
        transpose(top, 1);
        CHECK(top[0].key == 127);   // refused, not clipped
    }
    {   // randomize is reproducible and stays in the scale
        std::vector<Note> a, b;
        for (int i = 0; i < 32; ++i) a.push_back(mk(i * 240, 240, 60));
        b = a;
        RandomizeOpts ro;
        ro.velocity = 0.5f; ro.pitchRange = 12; ro.scaleRoot = 0; ro.scaleIndex = 0; ro.seed = 7;
        randomize(a, ro);
        randomize(b, ro);
        bool same = true, inKey = true, moved = false;
        for (size_t i = 0; i < a.size(); ++i) {
            same = same && a[i].key == b[i].key && a[i].velocity == b[i].velocity;
            inKey = inKey && inScale(a[i].key, 0, scales()[0]);
            moved = moved || a[i].key != 60;
        }
        CHECK(same && inKey && moved);
    }
    {   // levels and LFO
        std::vector<Note> n = {mk(0, 240, 60, 0.4f), mk(240, 240, 60, 0.8f)};
        scaleLevels(n, Velocity, 0.5f, 0.0f, 0.6f);
        CHECK_NEAR(n[0].velocity, 0.5, 1e-6);
        CHECK_NEAR(n[1].velocity, 0.7, 1e-6);
        std::vector<Note> l;
        for (int i = 0; i < 8; ++i) l.push_back(mk(i * 240, 240, 60));
        lfo(l, Pan, LfoSawUp, 8 * 240, 0.0f, 1.0f, 0.0f);
        CHECK_NEAR(l[0].pan, -1.0, 1e-6);
        CHECK_NEAR(l[4].pan, 0.0, 1e-6);
    }
    {   // trimmer: keep 3 beats, drop 1
        std::vector<Note> n;
        for (int i = 0; i < 8; ++i) n.push_back(mk(i * 960, 480, 60 + i));
        trimPattern(n, 3 * 960, 960);
        CHECK(n.size() == 6);
        CHECK(n[3].key == 64 && n[3].tick == 3 * 960);
    }
    {   // duplicate after
        std::vector<Note> n = {mk(0, 240, 60), mk(240, 240, 62)};
        duplicateAfter(n);
        CHECK(n.size() == 4 && n[2].tick == 480 && n[3].tick == 720 && n[2].selected && !n[0].selected);
    }
    {   // scales and chords
        CHECK(scales().size() >= 12 && chords().size() >= 15);
        CHECK(inScale(62, 60, scales()[0]) && !inScale(61, 60, scales()[0]));
        CHECK(snapToScale(61, 60, scales()[0]) == 60);
        CHECK(keyName(60) == "C5" && keyName(61) == "C#5" && keyName(0) == "C0");
    }
}

TEST(riff_generator_is_musical_and_reproducible) {
    using namespace tools;
    for (int kind = 0; kind < 3; ++kind)
        for (uint32 seed : {1u, 2u, 99u}) {
            RiffOpts o;
            o.kind = kind; o.seed = seed; o.root = 57; o.scaleIndex = 1; o.bars = 4;
            const auto a = riff(o), b = riff(o);
            CHECK(a.size() >= 8);
            CHECK(a.size() == b.size());
            bool same = true, inKey = true, inRange = true;
            for (size_t i = 0; i < a.size(); ++i) {
                same = same && a[i].tick == b[i].tick && a[i].key == b[i].key;
                inKey = inKey && inScale(a[i].key, o.root, scales()[1]);
                inRange = inRange && a[i].tick >= 0 && a[i].end() <= 4 * o.ticksPerBar && a[i].length > 0;
            }
            CHECK_MSG(same && inKey && inRange, "kind %d seed %u: same %d inKey %d inRange %d", kind, seed, same, inKey, inRange);
        }
    RiffOpts o;
    o.seed = 1;
    const auto a = riff(o);
    o.seed = 2;
    const auto b = riff(o);
    bool differ = a.size() != b.size();
    for (size_t i = 0; !differ && i < a.size(); ++i) differ = a[i].key != b[i].key;
    CHECK(differ);
}

TEST(midi_file_round_trip) {
    MidiSong s;
    s.tempo = 97.0;
    s.timeSigNum = 3;
    MidiTrack a, b;
    a.name = "Lead";
    a.notes = {mk(0, 480, 60, 0.5f), mk(480, 240, 64, 1.0f), mk(480, 960, 67, 0.25f)};
    b.name = "Bass";
    b.notes = {mk(960, 1920, 36)};
    b.notes[0].color = 2;
    s.tracks = {a, b};
    const auto bytes = writeMidi(s);
    MidiSong r;
    std::string err;
    CHECK_MSG(readMidi(bytes, r, &err), "%s", err.c_str());
    CHECK_NEAR(r.tempo, 97.0, 0.01);
    CHECK(r.timeSigNum == 3 && r.timeSigDen == 4);
    CHECK(r.tracks.size() == 2);
    if (r.tracks.size() == 2) {
        CHECK(r.tracks[0].name == "Lead" && r.tracks[1].name == "Bass");
        CHECK(r.tracks[0].notes.size() == 3);
        for (size_t i = 0; i < 3 && i < r.tracks[0].notes.size(); ++i) {
            CHECK(r.tracks[0].notes[i].tick == a.notes[i].tick && r.tracks[0].notes[i].length == a.notes[i].length && r.tracks[0].notes[i].key == a.notes[i].key);
            CHECK_NEAR(r.tracks[0].notes[i].velocity, a.notes[i].velocity, 1.0 / 127.0);
        }
        CHECK(r.tracks[1].notes[0].color == 2 && r.tracks[1].notes[0].length == 1920);
    }
    // A file at another resolution (96 PPQ, format 0, running status) converts to 960.
    const unsigned char smf[] = {'M','T','h','d',0,0,0,6,0,0,0,1,0,96, 'M','T','r','k',0,0,0,20,
                                 0x00,0x90,60,100, 0x60,60,0, 0x00,62,90, 0x30,0x80,62,0, 0x00,0xFF,0x2F,0x00, 0,0};
    MidiSong r2;
    CHECK(readMidi(std::vector<unsigned char>(smf, smf + sizeof smf), r2, &err));
    CHECK(r2.tracks.size() == 1 && r2.tracks[0].notes.size() == 2);
    if (r2.tracks.size() == 1 && r2.tracks[0].notes.size() == 2) {
        CHECK(r2.tracks[0].notes[0].tick == 0 && r2.tracks[0].notes[0].length == 960);
        CHECK(r2.tracks[0].notes[1].tick == 960 && r2.tracks[0].notes[1].length == 480);
    }
    const unsigned char junk[] = {'R', 'I', 'F', 'F', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(!readMidi(std::vector<unsigned char>(junk, junk + sizeof junk), r2, &err));
}

TEST(metronome_clicks_on_the_beat_and_stays_out_of_exports) {
    const double sr = 48000.0, bpm = 120.0;
    Project p;
    p.global->tempo.set((float)bpm);
    p.prepare(sr);
    Engine e;
    e.prepare(sr);
    e.setState(p.compile());
    e.setMetronome(true);
    e.play();
    std::vector<float> l(96000), r(96000);
    float* out[2] = {l.data(), r.data()};
    e.process(nullptr, 0, out, 2, 96000);
    for (int beat = 0; beat < 4; ++beat) {
        const size_t on = (size_t)(beat * 24000);
        CHECK_MSG(peakOf(l, on, on + 400) > 0.1f, "no click at beat %d", beat);
        if (beat > 0) CHECK(peakOf(l, on - 6000, on - 10) < 1.0e-3f);
    }
    // the first beat of the bar is the higher click
    CHECK(measureHz(l, 0, 800, sr) > measureHz(l, 24000, 24800, sr) * 1.3);
    // an offline render has no metronome
    RenderOptions o;
    o.lengthSamples = 48000;
    auto silent = renderOffline(p, o);
    CHECK(peakOf(silent->ch[0]) == 0.0f);
}

TEST(pattern_loop_region_repeats_just_that_range) {
    const double sr = 48000.0, bpm = 120.0;
    Project p;
    p.global->tempo.set((float)bpm);
    auto kick = drums::makeKick({}, sr);
    auto ch = addSampleChannel(p, kick);
    for (int s : {0, 4, 6, 8, 12}) Project::setStep(p.pattern().notesFor(ch.id), s, true);
    p.patLoopStart = 4 * kTicksPerStep;
    p.patLoopEnd = 8 * kTicksPerStep;
    RenderOptions o;
    o.loop = true;
    o.startTick = p.patLoopStart;
    o.lengthSamples = 48000;
    auto out = renderOffline(p, o);
    const double spt = samplesPerTick(bpm, sr);
    std::vector<float> expected(48000, 0.0f);
    const int64 loopLen = (int64)std::llround(4 * kTicksPerStep * spt);
    for (int rep = 0; rep * loopLen < 48000; ++rep)
        for (int s : {0, 2}) {
            const int64 on = rep * loopLen + (int64)std::llround(s * kTicksPerStep * spt);
            for (int64 i = 0; i < kick->frames() && on + i < 48000; ++i) expected[(size_t)(on + i)] += kick->ch[0][(size_t)i];
        }
    CHECK_MSG(maxAbsDiff(out->ch[0], expected) < 1.0e-6, "difference %g", maxAbsDiff(out->ch[0], expected));
}
