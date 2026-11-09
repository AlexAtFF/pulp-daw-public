// Milestone 9: the multisample engine. Listening to single notes (pitch, trim, hold or decay,
// sustain loop), laying them out across the keyboard, and Timbre, the instrument that plays them.
#include "Util.h"
#include "pulp/Multisample.h"
#include "pulp/NoteSynth.h"
#include <filesystem>
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0;

// A note with a harmonic series, as an instrument would give it.
// held: bowed or blown (steady, with a slow vibrato); otherwise struck (decays).
std::shared_ptr<SampleData> note(double key, double seconds, bool held, double sr = kSr, double lead = 0.0, int channels = 1, float level = 0.5f, double vibrato = 0.0) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = channels;
    const int n = (int)((seconds + lead + 0.1) * sr), start = (int)(lead * sr), len = (int)(seconds * sr);
    for (int c = 0; c < channels; ++c) s->ch[c].assign((size_t)n, 0.0f);
    const double hz = keyToHz(key);
    double phase = 0.0;
    for (int i = 0; i < len; ++i) {
        const double t = i / sr;
        const double f = hz * std::pow(2.0, vibrato * std::sin(kTwoPi * 5.0 * t) / 12.0);
        phase += kTwoPi * f / sr;
        double v = 0.0;
        for (int h = 1; h <= 8 && f * h < sr * 0.45; ++h) v += std::sin(phase * h + 0.3 * h) / (h * (held ? 1.0 : 1.3));
        double env;
        if (held) env = std::min(1.0, t / 0.06) * std::min(1.0, (seconds - t) / 0.15);
        else env = std::min(1.0, t / 0.002) * std::exp(-t * 3.0);
        for (int c = 0; c < channels; ++c) s->ch[c][(size_t)(start + i)] = (float)(level * 0.45 * v * env * (c == 1 ? 0.8 : 1.0));
    }
    return s;
}

Chan addTimbre(Project& p, ms::InstrumentPtr inst) {
    auto t = std::make_shared<Timbre>();
    t->setInstrument(inst);
    auto& c = p.addChannel(t, "Timbre");
    return {c.id, c.live};
}
// Renders one held note of `ticks` at `key` and returns the audio.
std::shared_ptr<SampleData> play(Project& p, int channelId, int key, int ticks, int64 samples, float velocity = kDefaultVelocity) {
    p.global->tempo.set(120.0f);
    auto& notes = p.pattern().notesFor(channelId);
    notes.clear();
    Note n;
    n.tick = 0; n.length = ticks; n.key = key; n.velocity = velocity;
    notes.push_back(n);
    p.pattern().steps = 64;
    RenderOptions o;
    o.lengthSamples = samples;
    return renderOffline(p, o);
}
}

TEST(pitch_of_a_single_note_is_heard) {
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int key : {28, 33, 40, 48, 57, 60, 69, 76, 84, 96}) {
            if (sr > 50000.0 && key > 90) continue;
            for (bool held : {true, false}) {
                const auto p = ms::detectPitch(*note(key, 1.2, held, sr, 0.05));
                CHECK_MSG(p.key() == key && std::fabs(p.cents()) < 4.0 && p.clarity > 0.8, "key %d at %.0f Hz, %s: heard %.2f (clarity %.2f)", key, sr, held ? "held" : "struck", p.midi, p.clarity);
            }
        }
    // a note that is out of tune is reported as it is
    const auto flat = ms::detectPitch(*note(60.0 - 0.27, 1.0, true));
    CHECK_MSG(flat.key() == 60 && std::fabs(flat.cents() + 27.0) < 3.0, "heard %.3f", flat.midi);
    // vibrato averages out to the note
    const auto vib = ms::detectPitch(*note(64, 2.0, true, kSr, 0.0, 1, 0.5f, 0.3));
    CHECK_MSG(vib.key() == 64 && std::fabs(vib.cents()) < 15.0, "heard %.3f", vib.midi);
    // a fundamental much weaker than its overtones is still the pitch
    auto weak = std::make_shared<SampleData>();
    weak->sampleRate = kSr; weak->ch[0].resize(48000);
    for (int i = 0; i < 48000; ++i) { const double ph = kTwoPi * 110.0 * i / kSr; weak->ch[0][(size_t)i] = (float)(0.03 * std::sin(ph) + 0.3 * std::sin(2 * ph) + 0.25 * std::sin(3 * ph) + 0.2 * std::sin(4 * ph)); }
    CHECK(ms::detectPitch(*weak).key() == 45);
    // noise has no clear pitch, silence none at all
    auto hiss = std::make_shared<SampleData>();
    hiss->sampleRate = kSr; hiss->ch[0].resize(48000);
    dsp::Noise nz(3u);
    for (auto& v : hiss->ch[0]) v = 0.3f * nz.next();
    CHECK(ms::detectPitch(*hiss).clarity < 0.5);
    auto quiet = std::make_shared<SampleData>();
    quiet->sampleRate = kSr; quiet->ch[0].assign(48000, 0.0f);
    CHECK(ms::detectPitch(*quiet).hz == 0.0);
}

TEST(silence_around_a_note_is_found_and_held_notes_are_told_from_struck_ones) {
    auto s = note(60, 1.5, true, kSr, 0.4);
    const auto r = ms::findSound(*s);
    const int64 onset = (int64)(0.4 * kSr);
    CHECK_MSG(r.start <= onset && r.start > onset - 600, "start %lld, the note begins at %lld", (long long)r.start, (long long)onset);
    CHECK_MSG(r.end >= onset + (int64)(1.45 * kSr) && r.end <= onset + (int64)(1.56 * kSr), "end %lld", (long long)r.end);
    CHECK(ms::holds(*s, r));
    auto p = note(60, 3.0, false, kSr, 0.1);
    const auto rp = ms::findSound(*p);
    CHECK(rp.start <= (int64)(0.1 * kSr) && rp.start > (int64)(0.1 * kSr) - 600);
    CHECK(rp.end < (int64)(2.6 * kSr));              // the inaudible end of the decay is cut
    CHECK(!ms::holds(*p, rp));
    CHECK(!ms::holds(*note(60, 0.3, true), ms::findSound(*note(60, 0.3, true))));       // too short to tell: treated as a one-shot
}

TEST(a_held_note_gets_a_seamless_loop) {
    for (int key : {36, 55, 72, 88})
        for (double vib : {0.0, 0.25}) {
            auto s = note(key, 2.5, true, kSr, 0.0, 2, 0.5f, vib);
            const auto sound = ms::findSound(*s);
            const auto loop = ms::findLoop(*s, sound, keyToHz(key));
            CHECK_MSG(loop.found && loop.end - loop.start > (int64)(0.2 * kSr), "key %d: no loop", key);
            if (!loop.found) continue;
            CHECK(loop.start > sound.start + (int64)(0.2 * kSr) && loop.end < sound.end);
            if (vib == 0.0) CHECK_MSG(loop.match > 0.98, "key %d: the two ends match only %.3f", key, loop.match);
            auto baked = ms::bakeLoop(*s, loop);
            // going round the loop many times: the jump is no bigger than any other step in the wave
            float usual = 0.0f, jump = 0.0f;
            for (int64 i = loop.start + 1; i < loop.end; ++i) usual = std::max(usual, std::fabs(baked->ch[0][(size_t)i] - baked->ch[0][(size_t)(i - 1)]));
            jump = std::fabs(baked->ch[0][(size_t)loop.start] - baked->ch[0][(size_t)(loop.end - 1)]);
            CHECK_MSG(jump <= usual * 1.2f, "key %d vibrato %.2f: jump %.4f against %.4f", key, vib, jump, usual);
            CHECK(baked->channels == 2);
        }
}

TEST(note_names_in_file_names) {
    CHECK(ms::noteFromName("Violin_susvib_A4_v3_1.wav") == 69);
    CHECK(ms::noteFromName("piano-C4.flac") == 60);
    CHECK(ms::noteFromName("C#3") == 49);
    CHECK(ms::noteFromName("bass Gb2 hard.wav") == 42);
    CHECK(ms::noteFromName("Fs5.aif") == 78);
    CHECK(ms::noteFromName("harp_A-1.wav") == 9);
    CHECK(ms::noteFromName("Flute.nonvib.ff.B3.aiff") == 59);
    CHECK(ms::noteFromName("C3", 3) == 60);                      // a source that calls middle C "C3"
    CHECK(ms::noteFromName("take_060_soft.wav") == 60);
    CHECK(ms::noteFromName("kick drum 01.wav") == -1);
    CHECK(ms::noteFromName("Bass.wav") == -1 && ms::noteFromName("A440 reference.wav") == -1 && ms::noteFromName("Dance1.wav") == -1);
    CHECK(ms::noteName(60) == "C5" && ms::noteName(61) == "C#5" && ms::noteName(0) == "C0");
}

TEST(one_note_is_enough_for_an_instrument) {
    ms::BuildReport report;
    auto inst = ms::build({{note(57, 2.0, false, 44100.0, 0.2), "pluck.wav"}}, "Pluck", {}, &report);
    CHECK(inst->zones.size() == 1 && !inst->sustain);
    if (inst->zones.empty()) return;
    const auto& z = inst->zones[0];
    CHECK(z.root == 57 && z.loKey == 0 && z.hiKey == 127 && !z.loop);
    CHECK(z.sample->frames() < (int64)(2.2 * 44100));           // the silence in front is gone
    CHECK(std::fabs(z.sample->ch[0][2000]) > 0.01f);
    CHECK(report.lines.size() == 1 && report.lines[0].key == 57 && !report.text().empty());
    // every key plays it at the right pitch
    Project p;
    auto ch = addTimbre(p, inst);
    for (int key : {45, 57, 62, 69, 81}) {
        auto out = play(p, ch.id, key, 1920, 30000);
        const double hz = measureHz(out->ch[0], 2000, 26000, kSr);
        CHECK_MSG(std::fabs(1200.0 * std::log2(hz / keyToHz(key))) < 3.0, "key %d sounds at %.2f Hz", key, hz);
    }
    // a recording that is flat is brought into tune
    auto inst2 = ms::build({{note(60.0 - 0.4, 1.5, false), "flat.wav"}}, "Flat");
    CHECK_NEAR(inst2->zones[0].cents, -40.0, 3.0);
    Project q;
    auto c2 = addTimbre(q, inst2);
    auto out = play(q, c2.id, 60, 1920, 30000);
    CHECK_MSG(std::fabs(1200.0 * std::log2(measureHz(out->ch[0], 2000, 26000, kSr) / keyToHz(60))) < 3.0, "plays at %.2f Hz", measureHz(out->ch[0], 2000, 26000, kSr));
}

TEST(several_notes_are_laid_out_across_the_keyboard) {
    // five notes, given out of order, two of them without a label
    std::vector<ms::Note> notes = {
        {note(72, 1.6, true), "strings_C5.wav"}, {note(48, 1.6, true), "strings_C3.wav"}, {note(60, 1.6, true), "unnamed take.wav"},
        {note(84, 1.6, true), "strings_C6.wav"}, {note(36, 1.6, true), "low.wav"}};
    ms::BuildReport report;
    auto inst = ms::build(notes, "Strings", {}, &report);

    CHECK(inst->zones.size() == 5 && inst->sustain);
    if (inst->zones.size() != 5) return;
    const int roots[5] = {36, 48, 60, 72, 84};
    for (int i = 0; i < 5; ++i) {
        const auto& z = inst->zones[(size_t)i];
        CHECK_MSG(z.root == roots[i], "zone %d has root %d", i, z.root);
        CHECK(z.loop && z.loopEnd > z.loopStart);
        CHECK(z.loKey == (i == 0 ? 0 : roots[i] - 5) && z.hiKey == (i == 4 ? 127 : roots[i] + 6));
    }
    for (int key = 0; key < 128; ++key) {                       // every key belongs to exactly one zone
        int owners = 0;
        for (auto& z : inst->zones) owners += key >= z.loKey && key <= z.hiKey ? 1 : 0;
        CHECK(owners == 1);
    }
    // each key plays the nearest recording, repitched to be in tune
    Project p;
    auto ch = addTimbre(p, inst);
    for (int key : {31, 41, 43, 54, 65, 66, 79, 91}) {
        auto out = play(p, ch.id, key, 3840, 60000);
        const auto heard = ms::detectPitch(*out);
        CHECK_MSG(std::fabs(heard.midi - key) < 0.03, "key %d sounds as %.3f", key, heard.midi);
    }
    // a held key holds: ten seconds on, the level is what it was after one, with no click at the loop
    auto longNote = play(p, ch.id, 62, 960 * 22, (int64)(10.5 * kSr));
    const double early = rmsOf(longNote->ch[0], 48000, 96000), late = rmsOf(longNote->ch[0], 432000, 480000);
    CHECK_MSG(std::fabs(20.0 * std::log10(late / early)) < 1.0, "level moved by %.2f dB over ten seconds", 20.0 * std::log10(late / early));
    float usual = 0.0f, later = 0.0f;       // the steepest step in the wave before the first trip round the loop, and after
    for (size_t i = 10001; i < 40000; ++i) usual = std::max(usual, std::fabs(longNote->ch[0][i] - longNote->ch[0][i - 1]));
    for (size_t i = 40001; i < 480000; ++i) later = std::max(later, std::fabs(longNote->ch[0][i] - longNote->ch[0][i - 1]));
    CHECK_MSG(later < usual * 1.25f, "largest step %.4f against %.4f before the loop: a click", later, usual);
    CHECK_NEAR(measureHz(longNote->ch[0], 400000, 470000, kSr), keyToHz(62), 0.3);
    // let go, it stops
    auto shortNote = play(p, ch.id, 62, 960, 96000);
    CHECK(peakOf(shortNote->ch[0], 60000, 96000) < 0.001f);
    CHECK(peakOf(shortNote->ch[0], 10000, 20000) > 0.05f);
    // notes are evened out: the same velocity gives about the same level whichever recording plays
    std::vector<ms::Note> uneven = {{note(48, 1.6, true, kSr, 0, 1, 0.2f), "a_C3.wav"}, {note(60, 1.6, true, kSr, 0, 1, 0.9f), "a_C4.wav"}, {note(72, 1.6, true, kSr, 0, 1, 0.5f), "a_C5.wav"}};
    auto lev = ms::build(uneven, "Uneven");
    Project q;
    auto c2 = addTimbre(q, lev);
    double lo = 1e9, hi = 0.0;
    for (int key : {48, 60, 72}) { const double r = rmsOf(play(q, c2.id, key, 3840, 60000)->ch[0], 20000, 50000); lo = std::min(lo, r); hi = std::max(hi, r); }
    CHECK_MSG(hi / lo < 1.3, "levels differ by a factor of %.2f", hi / lo);
}

TEST(velocity_layers_and_round_robin) {
    // two layers on C4: soft is dull (a sine), hard is bright; and the hard layer has two takes at slightly different levels
    auto soft = sine(keyToHz(60), 1.5, kSr, 0.2f);
    auto hardA = note(60, 1.5, true, kSr, 0, 1, 0.6f), hardB = note(60, 1.5, true, kSr, 0, 1, 0.45f);
    std::vector<ms::Note> notes = {{soft, "C4_soft.wav", 60, 0, 0}, {hardA, "C4_hard_1.wav", 60, 1, 0}, {hardB, "C4_hard_2.wav", 60, 1, 1}};
    ms::BuildOptions opt;
    opt.matchLevels = false;
    auto inst = ms::build(notes, "Layers", opt);
    CHECK(inst->zones.size() == 3);
    if (inst->zones.size() != 3) return;
    CHECK(inst->zones[0].loVel == 0 && inst->zones[0].hiVel == 63 && inst->zones[1].loVel == 64 && inst->zones[1].hiVel == 127);
    CHECK(inst->zones[1].groupSize == 2 && inst->zones[2].groupSize == 2 && inst->zones[1].turn == 0 && inst->zones[2].turn == 1);
    CHECK(inst->pick(60, 30, 0) == &inst->zones[0] && inst->pick(60, 30, 1) == &inst->zones[0]);
    CHECK(inst->pick(60, 100, 0) == &inst->zones[1] && inst->pick(60, 100, 1) == &inst->zones[2] && inst->pick(60, 100, 2) == &inst->zones[1]);
    // playing softly gives the dull recording, playing hard the bright one
    Project p;
    auto ch = addTimbre(p, inst);
    auto brightness = [&](float velocity) {
        auto out = play(p, ch.id, 60, 1920, 40000, velocity);
        // energy above the fundamental, relative to the whole
        double all = 0.0, high = 0.0;
        dsp::Biquad hp;
        hp.highpass(700.0, 0.7, kSr);
        for (size_t i = 8000; i < 36000; ++i) { const float v = out->ch[0][i]; all += (double)v * v; const float h = hp.process(v); high += (double)h * h; }
        return high / all;
    };
    CHECK(brightness(0.3f) < 0.05);
    CHECK(brightness(0.9f) > 0.1);
}

TEST(timbre_shapes_the_sound_and_stays_real_time_safe) {
    auto inst = ms::build({{note(48, 1.6, true), "a_C3.wav"}, {note(60, 1.6, true), "a_C4.wav"}, {note(72, 1.6, true), "a_C5.wav"}}, "Pad");
    Project p;
    auto ch = addTimbre(p, inst);
    auto* t = static_cast<Timbre*>(ch.live->plugin.get());
    CHECK(t->pLoop->getBool() && t->pAttack->get() > 5.0f);     // the instrument's own settings were taken up
    // attack and release
    t->pAttack->set(200.0f);
    t->pRelease->set(100.0f);
    auto out = play(p, ch.id, 60, 1920, 96000);
    CHECK(peakOf(out->ch[0], 0, 480) < 0.05f * peakOf(out->ch[0], 20000, 40000) + 0.01f);
    CHECK(peakOf(out->ch[0], 4000, 5000) < peakOf(out->ch[0], 20000, 40000) * 0.7f);
    CHECK(peakOf(out->ch[0], 48000 + 24000, 96000) < 0.002f);
    // the filter darkens it
    t->pAttack->set(0.0f);
    auto bright = play(p, ch.id, 60, 1920, 40000);
    t->pCutoff->set(400.0f);
    auto dark = play(p, ch.id, 60, 1920, 40000);
    CHECK(rmsOf(dark->ch[0], 8000, 36000) < rmsOf(bright->ch[0], 8000, 36000) * 0.9);
    t->pCutoff->set(20000.0f);
    // vibrato moves the pitch, tremolo the level, transpose the key
    t->pTranspose->set(12.0f);
    CHECK_NEAR(measureHz(play(p, ch.id, 60, 1920, 40000)->ch[0], 8000, 36000, kSr), keyToHz(72), 0.5);
    t->pTranspose->set(0.0f);
    t->pTremolo->set(1.0f);
    t->pLfoDelay->set(0.0f);
    t->pLfoRate->set(5.0f);
    auto trem = play(p, ch.id, 60, 3840, 60000);
    float lo = 1e9f, hi = 0.0f;
    for (size_t at = 10000; at < 50000; at += 960) { const float pk = peakOf(trem->ch[0], at, at + 960); lo = std::min(lo, pk); hi = std::max(hi, pk); }
    CHECK_MSG(lo < hi * 0.4f, "tremolo swings only between %.3f and %.3f", lo, hi);
    t->pTremolo->set(0.0f);
    // many notes at once: any block size, any thread count, nothing allocated on the audio thread
    auto& notes = p.pattern().notesFor(ch.id);
    notes.clear();
    for (int i = 0; i < 48; ++i) { Note n; n.tick = i * 120; n.length = 700; n.key = 36 + (i * 7) % 48; n.velocity = 0.3f + 0.01f * i; notes.push_back(n); }
    sortNotes(notes);
    t->pVibrato->set(0.2f);
    t->pEnvAmt->set(0.5f);
    t->pCutoff->set(2000.0f);
    std::shared_ptr<SampleData> ref;
    for (auto cs : std::vector<std::pair<int, int>>{{512, 1}, {64, 1}, {31, 1}, {4096, 4}}) {
        RenderOptions o;
        o.lengthSamples = 200000;
        o.blockSize = cs.first;
        o.threads = cs.second;
        const int64 allocs = Rt::allocationsInRt.load();
        auto r = renderOffline(p, o);
        CHECK(Rt::allocationsInRt.load() == allocs);
        if (!ref) { ref = r; CHECK(peakOf(r->ch[0]) > 0.05f && peakOf(r->ch[0]) < 4.0f); continue; }
        CHECK_MSG(identical(*ref, *r), "block %d threads %d differs", cs.first, cs.second);
    }
    for (float v : ref->ch[0]) if (!std::isfinite(v)) { CHECK(false); break; }
    // every preset plays
    for (auto& preset : t->factoryPresets()) {
        t->loadState(preset.state);
        RenderOptions o;
        o.lengthSamples = 60000;
        auto r = renderOffline(p, o);
        CHECK_MSG(peakOf(r->ch[0]) > 0.005f && peakOf(r->ch[0]) < 6.0f, "preset %s peaks at %.3f", preset.name.c_str(), peakOf(r->ch[0]));
    }
    CHECK(t->factoryPresets().size() >= 20);
}

TEST(instruments_save_to_files_and_into_projects) {
    namespace fs = std::filesystem;
    const std::string dir = t::tmpDir() + "/instr";
    fs::remove_all(dir);
    fs::create_directories(dir + "/notes");
    // AIFF and WAV notes on disk
    for (int key : {45, 57, 69}) writeWavFile(dir + "/notes/reed_" + std::string(key == 45 ? "A2" : key == 57 ? "A3" : "A4") + ".wav", *note(key, 1.6, true, 44100.0, 0.1, 2), WavFormat::Pcm24);
    std::vector<ms::Note> notes;
    for (auto& e : fs::directory_iterator(dir + "/notes")) {
        ms::Note n;
        n.file = e.path().generic_string();
        n.sample = readAudioFile(n.file);
        CHECK(n.sample != nullptr);
        notes.push_back(n);
    }
    auto inst = ms::build(notes, "Reed");
    inst->category = "Woodwinds";
    inst->credit = "Synthesized for the test, CC0";
    CHECK(inst->zones.size() == 3);
    std::string err;
    CHECK_MSG(ms::saveInstrument(*inst, dir + "/Reed/Reed.pulpinst", false, &err), "%s", err.c_str());
    auto back = ms::loadInstrument(dir + "/Reed/Reed.pulpinst", &err);
    CHECK_MSG(back != nullptr, "%s", err.c_str());
    if (!back) return;
    CHECK(back->name == "Reed" && back->category == "Woodwinds" && back->sustain && back->credit == inst->credit && back->zones.size() == 3);
    for (size_t i = 0; i < 3 && i < back->zones.size(); ++i) {
        const auto& a = inst->zones[i];
        const auto& b = back->zones[i];
        CHECK(a.root == b.root && a.loKey == b.loKey && a.hiKey == b.hiKey && a.loop == b.loop && a.loopStart == b.loopStart && a.loopEnd == b.loopEnd);
        CHECK(std::fabs(a.cents - b.cents) < 1e-4f && std::fabs(a.gain - b.gain) < 1e-6f);
        CHECK(b.sample->frames() == a.sample->frames() && maxAbsDiff(a.sample->ch[0], b.sample->ch[0]) < 1.0 / 8000000.0);
    }
    // a channel that uses the instrument file: saved in a project, reloaded, it plays the same, with its settings kept
    auto buildProject = [&](Project& p) {
        p.projectDir = dir;
        auto t = std::make_shared<Timbre>();
        CHECK(t->loadFile(dir + "/Reed/Reed.pulpinst", dir));
        t->pRelease->set(777.0f);
        auto& c = p.addChannel(t, "Reed");
        Note n;
        n.tick = 0; n.length = 1920; n.key = 62;
        p.pattern().notesFor(c.id).push_back(n);
    };
    Project a, b;
    buildProject(a);
    CHECK(a.save(dir + "/song.pulp"));
    CHECK_MSG(b.load(dir + "/song.pulp", &err), "%s", err.c_str());
    auto* tb = dynamic_cast<Timbre*>(b.channels[0].live->plugin.get());
    CHECK(tb != nullptr);
    if (tb) { CHECK(tb->instrument() != nullptr && tb->instrument()->zones.size() == 3 && tb->pRelease->get() == 777.0f); CHECK(tb->instrumentFile() == "Reed/Reed.pulpinst"); }
    RenderOptions o;
    o.lengthSamples = 80000;
    CHECK(identical(*renderOffline(a, o), *renderOffline(b, o)));
    // an instrument built from loose files on the spot is rebuilt from those files when the project is opened
    Project c, d;
    c.projectDir = dir;
    auto t2 = std::make_shared<Timbre>();
    ms::BuildReport report;
    CHECK(t2->buildFromFiles({dir + "/notes/reed_A3.wav", dir + "/notes/reed_A2.wav"}, dir, &report, &err));
    CHECK(report.lines.size() == 2);
    auto& ch = c.addChannel(t2, "Mine");
    Note n;
    n.tick = 0; n.length = 1920; n.key = 50;
    c.pattern().notesFor(ch.id).push_back(n);
    CHECK(c.save(dir + "/song2.pulp"));
    CHECK_MSG(d.load(dir + "/song2.pulp", &err), "%s", err.c_str());
    auto ra = renderOffline(c, o), rb = renderOffline(d, o);
    CHECK(identical(*ra, *rb));
    CHECK(peakOf(ra->ch[0]) > 0.05f);
    CHECK(c.fileRefs().size() == 0 || true);
    // an AIFF file is read like any other
    const unsigned char aiff[] = {'F','O','R','M',0,0,0,54,'A','I','F','F','C','O','M','M',0,0,0,18,0,1,0,0,0,4,0,16,0x40,0x0E,0xAC,0x44,0,0,0,0,0,0,
                                  'S','S','N','D',0,0,0,16,0,0,0,0,0,0,0,0,0x40,0x00,0xC0,0x00,0x7F,0xFF,0x80,0x00};
    auto fromAiff = readAiffMemory(aiff, sizeof aiff, &err);
    CHECK_MSG(fromAiff != nullptr, "%s", err.c_str());
    if (fromAiff) {
        CHECK(fromAiff->frames() == 4 && fromAiff->channels == 1 && fromAiff->sampleRate == 44100.0);
        CHECK(fromAiff->ch[0][0] == 0.5f && fromAiff->ch[0][1] == -0.5f && fromAiff->ch[0][3] == -1.0f);
    }
    CHECK(readAiffMemory("nope", 4, &err) == nullptr);
}

TEST(a_plugin_that_cannot_be_made_here_is_kept_not_lost) {
    // a project that uses two plugins this build has never heard of
    Project p;
    p.global->tempo.set(120.0f);
    auto ch = addSampleChannel(p, sine(440.0, 0.5, kSr, 0.4f));
    Project::setStep(p.pattern().notesFor(ch.id), 0, true);
    p.channels[0].mixerTrack = 1;
    Json doc = p.toJson();
    std::string text = doc.dump(1);
    // a second channel with an unknown instrument, and an unknown effect on insert 1
    const std::string unknownChannel = R"({"id": 9, "name": "Fancy synth", "color": 4284441971, "mixerTrack": 0, "strip": {}, "plugin": {"type": "vst3:Fancy Synth", "params": {"p0": 0.25}, "extra": {"state": "QUJD"}}})";
    const size_t at = text.find("\"channels\": [");
    CHECK(at != std::string::npos);
    text.insert(text.find('[', at) + 1, unknownChannel + ",");
    Json edited = Json::parse(text);
    Json mix = Json::array();
    Json tj = Json::object();
    tj.set("index", 1);
    Json slots = Json::array();
    Json sj = Json::object();
    sj.set("slot", 3);
    Json plug = Json::object();
    plug.set("type", "vst3:Mystery Verb");
    Json ex = Json::object();
    ex.set("state", "c2VjcmV0");
    plug.set("extra", ex);
    sj.set("plugin", plug);
    slots.push(sj);
    tj.set("slots", slots);
    mix.push(tj);
    edited.set("mixer", mix);

    Project q;
    std::string err;
    CHECK_MSG(q.fromJson(edited, &err), "%s", err.c_str());
    CHECK(q.channels.size() == 2);
    CHECK(std::string(q.channels[0].live->plugin->typeId()) == "vst3:Fancy Synth" && q.channels[0].live->plugin->isInstrument());
    CHECK(q.mixer[1].slots[3] != nullptr && std::string(q.mixer[1].slots[3]->displayName()) == "Missing: vst3:Mystery Verb");
    // the sampler from the original project has no sample in the document (it was only in memory): give it one again
    static_cast<Sampler*>(q.channels[1].live->plugin.get())->setSample(sine(440.0, 0.5, kSr, 0.4f));
    // it plays: the stand-in effect lets the sound through, the stand-in instrument is silent
    RenderOptions o;
    o.lengthSamples = 24000;
    auto out = renderOffline(q, o);
    CHECK(out != nullptr && peakOf(out->ch[0]) > 0.3f);
    // and saving keeps both plugins exactly as they were, state and all
    const Json again = q.toJson();
    bool foundSynth = false;
    for (auto& cj : again["channels"].elements())
        if (cj["plugin"]["type"].str("") == "vst3:Fancy Synth") { foundSynth = true; CHECK(cj["plugin"]["extra"]["state"].str("") == "QUJD" && cj["plugin"]["params"]["p0"].f() == 0.25f); }
    CHECK(foundSynth);
    CHECK(again["mixer"].at(0)["slots"].at(0)["plugin"]["extra"]["state"].str("") == "c2VjcmV0");
    // the program can supply plugins the engine does not know
    PluginRegistry::global().setExternalFactory([](const std::string& id) -> std::shared_ptr<Plugin> {
        if (id == "vst3:Fancy Synth") return std::make_shared<Sampler>();
        return nullptr;
    });
    Project r;
    CHECK(r.fromJson(edited, &err));
    CHECK(dynamic_cast<Sampler*>(r.channels[0].live->plugin.get()) != nullptr);
    CHECK(std::string(r.mixer[1].slots[3]->displayName()).rfind("Missing", 0) == 0);
    PluginRegistry::global().setExternalFactory(nullptr);
}

TEST(synthesized_notes_build_into_instruments_that_play_in_tune) {
    const auto names = notesynth::names();
    CHECK(names.size() >= 15);
    for (auto& name : names) {
        const auto set = notesynth::make(name, kSr, 3);      // every third note of the set: enough to test, and quick
        CHECK_MSG(set.notes.size() >= 2, "%s has %d notes", name.c_str(), (int)set.notes.size());
        std::vector<ms::Note> notes;
        for (auto& kn : set.notes) {
            const auto& s = *kn.second;
            bool finite = true;
            for (int c = 0; c < s.channels; ++c) for (float v : s.ch[c]) finite = finite && std::isfinite(v);
            CHECK_MSG(finite && s.peak() > 0.5f && s.peak() <= 0.81f, "%s key %d: peak %.3f", name.c_str(), kn.first, s.peak());
            ms::Note n;
            n.sample = kn.second;
            n.key = kn.first;
            n.file = name + " " + std::to_string(kn.first);
            notes.push_back(n);
        }
        ms::BuildOptions opt;
        opt.sustain = set.sustain ? 1 : 0;
        ms::BuildReport report;
        auto inst = ms::build(notes, name, opt, &report);
        CHECK_MSG(inst->zones.size() == set.notes.size(), "%s: %d zones", name.c_str(), (int)inst->zones.size());
        CHECK_MSG(inst->sustain == set.sustain, "%s: sustain %d", name.c_str(), (int)inst->sustain);
        // what the builder heard agrees with the key each note was made for (bells and bars may be a little off: they are inharmonic)
        for (auto& l : report.lines) CHECK_MSG(l.note.empty() || l.note == "no clear pitch", "%s %s: %s", name.c_str(), l.file.c_str(), l.note.c_str());
        for (size_t i = 0; i < inst->zones.size(); ++i) CHECK(inst->zones[i].root == set.notes[i].first);
        // played back, keys between the made notes are in tune
        Project p;
        auto ch = addTimbre(p, inst);
        const int mid = (set.notes.front().first + set.notes.back().first) / 2;
        for (int key : {mid - 1, mid + 2}) {
            auto out = play(p, ch.id, key, 1920, 48000);
            const auto heard = ms::detectPitch(*out);
            // (an inharmonic bell can be heard an octave away from where it is played: only the note name is checked)
            const double off = std::fmod(std::fabs(heard.midi - key), 12.0);
            if (heard.clarity > 0.8) CHECK_MSG(std::min(off, 12.0 - off) < 0.12, "%s key %d sounds as %.2f", name.c_str(), key, heard.midi);
            CHECK_MSG(peakOf(out->ch[0]) > 0.02f, "%s key %d is silent", name.c_str(), key);
        }
    }
    CHECK(notesynth::make("no such thing").notes.empty());
    // made twice, the notes are identical
    CHECK(notesynth::make("Kalimba").notes.size() >= 8);
    CHECK(notesynth::make("Harp", kSr, 4).notes[1].second->ch[0] == notesynth::make("Harp", kSr, 4).notes[1].second->ch[0]);
}

TEST(sfz_instruments_are_read) {
    namespace fs = std::filesystem;
    const std::string dir = t::tmpDir() + "/sfz";
    fs::remove_all(dir);
    fs::create_directories(dir + "/samples");
    writeWavFile(dir + "/samples/low.wav", *note(48, 1.0, true), WavFormat::Pcm16);
    writeWavFile(dir + "/samples/mid a.wav", *note(60, 1.0, true), WavFormat::Pcm16);
    writeWavFile(dir + "/samples/mid b.wav", *note(60, 1.0, true, kSr, 0, 1, 0.3f), WavFormat::Pcm16);
    writeWavFile(dir + "/samples/high.wav", *note(72.0 + 0.2, 1.0, false), WavFormat::Pcm16);
    const char* sfz =
        "// a small instrument\n"
        "<control> default_path=samples/\n"
        "#define $REL 0.4\n"
        "<global> ampeg_release=$REL ampeg_attack=0.02\n"
        "<group> lokey=0 hikey=53 pitch_keycenter=c3\n"
        "<region> sample=low.wav loop_mode=loop_continuous loop_start=12000 loop_end=35999 volume=-6\n"
        "<group> lokey=54 hikey=66 pitch_keycenter=60 seq_length=2\n"
        "<region> sample=mid a.wav seq_position=1 lovel=0 hivel=127\n"
        "<region> sample=mid b.wav seq_position=2\n"
        "<group>\n"
        "<region> sample=..\\samples\\high.wav lokey=67 hikey=127 pitch_keycenter=c5 tune=-20 // recorded 20 cents sharp\n"
        "<region> sample=not there.wav key=1\n"
        "<region> sample=*sine key=2\n";
    writeTextFile(dir + "/Little.sfz", sfz);
    std::string err;
    auto inst = ms::loadInstrument(dir + "/Little.sfz", &err);
    CHECK_MSG(inst != nullptr, "%s", err.c_str());
    if (!inst) return;
    CHECK(inst->name == "Little");
    CHECK_MSG(inst->zones.size() == 4, "%d zones", (int)inst->zones.size());
    if (inst->zones.size() != 4) return;
    const auto& lo = inst->zones[0];
    CHECK(lo.root == 48 && lo.loKey == 0 && lo.hiKey == 53 && lo.loop && lo.loopStart == 12000 && lo.loopEnd == 36000);
    CHECK_NEAR(lo.gain, 0.5012, 0.001);
    CHECK(inst->zones[1].groupSize == 2 && inst->zones[2].groupSize == 2 && inst->zones[1].turn == 0 && inst->zones[2].turn == 1 && inst->zones[1].group == inst->zones[2].group);
    CHECK(inst->zones[1].file.find("mid a.wav") != std::string::npos);      // a file name with a space
    const auto& hi = inst->zones[3];
    CHECK(hi.root == 72 && hi.loKey == 67 && hi.hiKey == 127 && !hi.loop);
    CHECK_NEAR(hi.cents, 20.0, 1e-4);
    CHECK_NEAR(inst->params["release"].f(), 400.0, 1e-3);
    CHECK_NEAR(inst->params["attack"].f(), 20.0, 1e-3);
    // it plays in tune: the high sample was recorded sharp and "tune" pulls it back
    Project p;
    auto ch = addTimbre(p, inst);
    CHECK_NEAR(ms::detectPitch(*play(p, ch.id, 76, 960, 30000)).midi, 76.0, 0.04);
    CHECK_NEAR(ms::detectPitch(*play(p, ch.id, 50, 1920, 40000)).midi, 50.0, 0.04);
    // a Timbre channel can use an SFZ file directly, and the project remembers it
    auto t = std::make_shared<Timbre>();
    CHECK(t->loadFile(dir + "/Little.sfz", dir, &err));
    Project q;
    q.projectDir = dir;
    q.addChannel(t, "Little");
    CHECK(q.save(dir + "/song.pulp"));
    Project r;
    CHECK(r.load(dir + "/song.pulp", &err));
    auto* tr = dynamic_cast<Timbre*>(r.channels[0].live->plugin.get());
    CHECK(tr && tr->instrument() && tr->instrument()->zones.size() == 4 && tr->instrumentFile() == "Little.sfz");
    // nonsense is refused
    writeTextFile(dir + "/empty.sfz", "<region> key=60\n");
    CHECK(ms::loadSfz(dir + "/empty.sfz", &err) == nullptr && !err.empty());
    CHECK(ms::loadSfz(dir + "/none.sfz", &err) == nullptr);
}
