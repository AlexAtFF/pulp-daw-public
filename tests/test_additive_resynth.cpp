// Sheaf: resynthesis. A recording of one note is analysed into a table of harmonic levels over time,
// played back at any pitch and length, and saved with the project as numbers.
#include "SheafTestUtil.h"
#include <filesystem>
using namespace pulp;
using namespace sheaft;

namespace {

// The note the tests analyse: 24 harmonics with known levels, a 40 ms attack, and each harmonic dying
// away at its own rate (the higher, the faster). 13 cents flat of A3, so it is on no key.
const double kNoteHz = 220.0 * std::pow(2.0, -13.0 / 1200.0);
std::vector<double> noteLevels() {
    std::vector<double> a;
    for (int k = 1; k <= 24; ++k) a.push_back(0.3 / k * (1.0 + 0.6 * std::cos(1.7 * k)));
    return a;
}
double noteEnv(int k, double t) { return std::min(1.0, t / 0.04) * std::exp(-t * (0.4 + 0.25 * k)); }
double noteTruth(int k, double t) { return noteLevels()[(size_t)(k - 1)] * noteEnv(k, t); }
std::shared_ptr<SampleData> theNote(double sr = 44100.0, double seconds = 2.5) { return harmonicNote(kNoteHz, seconds, sr, noteLevels(), noteEnv); }

// A Sheaf set up to play the analysed sound as it is: spectrum A = user, no filter, flat envelope.
std::shared_ptr<Sheaf> userSheaf(SamplePtr rec) {
    auto s = plainSheaf(Sheaf::User);
    std::string err;
    CHECK_MSG(s->analyse(std::move(rec), &err), "%s", err.c_str());
    return s;
}

} // namespace

TEST(sheaf_analysis_finds_the_pitch_and_every_harmonic_over_time) {
    std::string err;
    auto a = Sheaf::analyseSample(*theNote(), &err);
    CHECK_MSG(a != nullptr, "%s", err.c_str());
    if (!a) return;
    const double cents = centsOff(a->hz, kNoteHz);
    CHECK_MSG(std::fabs(cents) < 0.5, "pitch found %.3f Hz, %.3f cents from %.3f", a->hz, cents, kNoteHz);
    CHECK(a->frames >= 24 && a->frames <= Sheaf::kMaxFrames);
    CHECK_MSG(a->partials >= 24 && a->partials <= 26, "%d partials kept", a->partials);     // the attack spills a little past the last harmonic
    CHECK_NEAR(a->seconds, 2.5, 0.03);
    CHECK_NEAR(a->offset, 0.0, 0.005);
    CHECK(a->name == "Recording");
    // the levels over time: every frame from 60 ms in (the first frames straddle the attack), every harmonic that is
    // within 50 dB of the loudest thing in the note
    const double top = noteTruth(1, 0.04);
    double worst = 0.0, worstAttack = 0.0;
    int compared = 0;
    for (int f = 0; f < a->frames; ++f) {
        const double t = a->offset + a->frameTime(f);
        if (t > a->seconds - 0.03) continue;                    // the last window hangs over the end of the recording
        for (int k = 1; k <= 24; ++k) {
            const double truth = noteTruth(k, t);
            if (truth < top * 0.00316) continue;
            const double off = std::fabs(db((double)a->level(f, k) / truth));
            if (t >= 0.06) { worst = std::max(worst, off); ++compared; }
            else if (t >= 0.02) worstAttack = std::max(worstAttack, off);
        }
    }
    CHECK_MSG(worst < 1.0, "a harmonic is %.2f dB from its true level", worst);
    CHECK(compared > 300);
    printf("    pitch %.3f cents out; %d levels (%d frames x 24 harmonics, from 60 ms on, down to -50 dB) within %.2f dB of the truth; %.2f dB in the attack (20..60 ms)\n",
           cents, compared, a->frames, worst, worstAttack);
    CHECK_MSG(worstAttack < 3.0, "during the attack a harmonic is %.2f dB out", worstAttack);

    // a note that does not change: the plain harmonic levels, within a tenth of a dB (the bound asked for is 1 dB)
    std::vector<double> lv = {0.2, 0.1, 0.05, 0.0, 0.08, 0.002, 0.03, 0.0005, 0.01, 0.02};
    auto steady = Sheaf::analyseSample(*harmonicNote(146.83, 1.5, 48000.0, lv, {}), &err);
    CHECK(steady != nullptr);
    if (steady) {
        CHECK_MSG(std::fabs(centsOff(steady->hz, 146.83)) < 0.2, "steady note: pitch %.3f Hz", steady->hz);
        CHECK(steady->holds);
        auto pluck = Sheaf::analyseSample(*harmonicNote(146.83, 1.5, 48000.0, lv, [](int, double t) { return std::exp(-4.0 * t); }), &err);
        CHECK(pluck && !pluck->holds);                 // a sound that dies away says so, so that the program does not loop it
        CHECK_NEAR(steady->ref, 0.2, 0.002);
        const int mid = steady->frames / 2;
        double off = 0.0;
        for (int k = 1; k <= 10; ++k) {
            if (lv[(size_t)(k - 1)] == 0.0) { CHECK_MSG(db((double)steady->level(mid, k) / 0.2) < -55.0, "harmonic %d should be missing, reads %.1f dB", k, db((double)steady->level(mid, k) / 0.2)); continue; }
            off = std::max(off, std::fabs(db((double)steady->level(mid, k) / lv[(size_t)(k - 1)])));
        }
        CHECK_MSG(off < 0.1, "steady note: a harmonic is %.3f dB out", off);
        printf("    a steady note: harmonics from 0 to -52 dB within %.3f dB\n", off);
    }
}

TEST(sheaf_analysis_copes_with_other_recordings_and_refuses_what_it_cannot_use) {
    std::string err;
    {   // stereo at 96 kHz, a low note with all 256 partials: everything is kept and it still fits a project comfortably
        std::vector<double> lv;
        for (int k = 1; k <= 400; ++k) lv.push_back(0.5 / k);
        auto rec = harmonicNote(55.0, 5.0, 96000.0, lv, {}, 2);
        auto a = Sheaf::analyseSample(*rec, &err);
        CHECK_MSG(a != nullptr, "%s", err.c_str());
        if (a) {
            CHECK_MSG(std::fabs(centsOff(a->hz, 55.0)) < 0.5, "pitch %.4f", a->hz);
            CHECK(a->partials == 256 && a->frames == Sheaf::kMaxFrames);
            double off = 0.0;
            for (int k = 1; k <= 256; ++k) off = std::max(off, std::fabs(db((double)a->level(a->frames / 2, k) / (0.5 / k))));
            CHECK_MSG(off < 1.0, "256 partials: one is %.2f dB out", off);
            Json j = Json::object();
            j.set("analysis", a->toJson());
            const size_t bytes = j.dump().size();
            CHECK_MSG(bytes < 200000, "the biggest table is %zu bytes of JSON", bytes);
            printf("    the biggest table (64 frames x 256 partials) is %zu bytes of JSON; 256 partials within %.2f dB\n", bytes, off);
        }
    }
    {   // a note whose fundamental is almost missing still gets its own pitch, not the octave
        std::vector<double> lv = {0.004, 0.3, 0.2, 0.25, 0.1, 0.12, 0.05};
        auto a = Sheaf::analyseSample(*harmonicNote(110.0, 1.2, 44100.0, lv, {}), &err);
        CHECK(a != nullptr);
        if (a) CHECK_MSG(std::fabs(centsOff(a->hz, 110.0)) < 1.0, "weak fundamental: pitch %.3f Hz", a->hz);
    }
    {   // a high note and a short one
        std::vector<double> lv = {0.3, 0.1, 0.05};
        auto a = Sheaf::analyseSample(*harmonicNote(1760.0, 0.2, 44100.0, lv, {}), &err);
        CHECK_MSG(a != nullptr, "%s", err.c_str());
        if (a) {
            CHECK_MSG(std::fabs(centsOff(a->hz, 1760.0)) < 1.0, "high note: pitch %.3f Hz", a->hz);
            CHECK_NEAR(db((double)a->level(a->frames / 2, 2) / 0.1), 0.0, 0.3);
        }
    }
    {   // the name comes from the file
        auto rec = theNote();
        rec->path = "C:/sounds/My Cello A3.wav";
        Sheaf s;
        CHECK(s.analyse(rec, &err));
        CHECK(s.analysisName() == "My Cello A3");
    }
    // what cannot be analysed says why, and leaves the analysis that was there
    Sheaf s;
    CHECK(!s.hasAnalysis() && s.analysisName().empty() && !s.analysisHolds());
    CHECK(s.analyse(theNote(), &err));
    const auto before = s.analysis();
    auto silent = std::make_shared<SampleData>();
    silent->ch[0].assign(44100, 0.0f);
    err.clear();
    CHECK(!s.analyse(silent, &err) && !err.empty());
    auto tiny = std::make_shared<SampleData>();
    tiny->ch[0].assign(300, 0.3f);
    err.clear();
    CHECK(!s.analyse(tiny, &err) && !err.empty());
    auto noise = std::make_shared<SampleData>();
    dsp::Noise rnd(7);
    noise->ch[0].resize(44100);
    for (auto& v : noise->ch[0]) v = 0.3f * rnd.next();
    err.clear();
    CHECK_MSG(!s.analyse(noise, &err) && !err.empty(), "noise was analysed as a note");
    err.clear();
    CHECK(!s.analyse(nullptr, &err) && !err.empty());
    CHECK(s.analysis() == before);
    s.clearAnalysis();
    CHECK(!s.hasAnalysis() && s.saveExtra().isNull());
}

TEST(sheaf_plays_the_analysed_sound_in_tune_on_any_key) {
    auto s = userSheaf(theNote());
    const auto an = s->analysis();
    CHECK(an != nullptr);
    if (!an) return;
    // frozen half way through: the spectrum is the table's, interpolated between two frames
    s->pUserSpeed->set(0.0f);
    s->pUserStart->set(0.5f);
    const double at = (an->frames - 1) * std::sqrt(0.5);
    const int f0i = (int)at;
    auto table = [&](int k) { return (double)an->level(f0i, k) + ((double)an->level(f0i + 1, k) - (double)an->level(f0i, k)) * (at - f0i); };
    double worstCents = 0.0, worstDb = 0.0;
    for (double sr : {44100.0, 48000.0, 96000.0})
        for (int key : {38, 57, 60, 77, 96}) {
            const double hz = keyToHz(key);
            const auto out = hold(*s, key, sr, 48000);
            const auto want = harmonicsOf(hz, sr, 24);
            const auto got = lineFrequencies(out, 6000, sr, want);
            for (size_t i = 0; i < want.size(); ++i) {
                if (want[i] > 0.9 * 0.5 * sr) break;
                if (table((int)i + 1) < table(1) * 0.003) continue;        // the harmonics that are more than 50 dB down by then are not measured
                worstCents = std::max(worstCents, std::fabs(centsOff(got[i], want[i])));
                const double rel = db(ampFine(out, 6000, 16384, want[i], sr) / ampFine(out, 6000, 16384, hz, sr));
                worstDb = std::max(worstDb, std::fabs(rel - db(table((int)i + 1) / table(1))));
            }
            CHECK_MSG(strayDb(out, 6000, 32768, sr, harmonicsOf(hz, sr, 256)) < -100.0, "key %d at %.0f Hz: something that is not a harmonic", key, sr);
        }
    CHECK_MSG(worstCents < 1.0, "a partial of the resynthesised note is %.3f cents off", worstCents);
    CHECK_MSG(worstDb < 0.05, "a partial is %.3f dB from the table", worstDb);
    // and against the recording itself: half way through, every harmonic within 1 dB
    double vsTruth = 0.0;
    const auto out = hold(*s, 60, 48000.0, 48000);
    for (int k = 1; k <= 12; ++k)
        vsTruth = std::max(vsTruth, std::fabs(db(ampFine(out, 6000, 16384, keyToHz(60) * k, 48000.0) / ampFine(out, 6000, 16384, keyToHz(60), 48000.0)) - db(noteTruth(k, 1.25) / noteTruth(1, 1.25))));
    CHECK_MSG(vsTruth < 1.0, "against the recording a harmonic is %.2f dB out", vsTruth);
    printf("    five keys at three rates: partials within %.4f cents, levels within %.3f dB of the table and %.2f dB of the recording\n", worstCents, worstDb, vsTruth);
    // the "user" shape with nothing analysed is a saw
    auto empty = plainSheaf(Sheaf::User), saw = plainSheaf(Sheaf::Saw);
    CHECK(hold(*empty, 60, 48000.0, 9600) == hold(*saw, 60, 48000.0, 9600));
}

TEST(sheaf_replays_the_evolution_at_any_speed_and_loops_to_hold) {
    const double sr = 48000.0;
    auto s = userSheaf(theNote());
    s->pUserLoop->set(0.0f);
    auto levelAt = [&](const std::vector<float>& d, int k, double seconds, double hz) { return ampAt(d, (size_t)(seconds * sr) - 2048, 4096, hz * k, sr); };
    // speed 1, on the recording's own pitch and two octaves... no: an octave up, where the same evolution must be heard
    for (int key : {57, 69}) {
        const double hz = keyToHz(key);
        const auto out = hold(*s, key, sr, 144000);
        double worst = 0.0;
        for (double t = 0.1; t < 2.3; t += 0.1)
            for (int k = 1; k <= 12; ++k) {
                if (noteTruth(k, t) < noteTruth(1, 0.04) * 0.00562) continue;      // down to -45 dB
                const double got = db(levelAt(out, k, t, hz) / levelAt(out, 1, 0.3, hz)), want = db(noteTruth(k, t) / noteTruth(1, 0.3));
                worst = std::max(worst, std::fabs(got - want));
            }
        CHECK_MSG(worst < 0.5, "key %d: the evolution is %.2f dB from the recording's", key, worst);
        if (key == 69) printf("    the evolution replayed an octave up: every harmonic within %.2f dB of the recording from 0.1 to 2.2 s (bound 0.5 dB)\n", worst);
        // past the end it holds the last frame: level and spectrum stop changing
        CHECK_NEAR(db(levelAt(out, 1, 2.48, hz) / levelAt(out, 1, 2.3, hz)), db(noteTruth(1, 2.48) / noteTruth(1, 2.3)), 1.5);
    }
    // half speed: what the recording did at t happens at 2t. Double speed: at t / 2.
    for (float speed : {0.5f, 2.0f}) {
        s->pUserSpeed->set(speed);
        const auto out = hold(*s, 57, sr, 240000);
        double worst = 0.0;
        for (double t = 0.15; t < 2.2; t += 0.15)
            for (int k = 1; k <= 8; ++k) {
                if (noteTruth(k, t) < noteTruth(1, 0.04) * 0.00562) continue;
                const double got = db(levelAt(out, k, t / speed, kNoteHz * std::pow(2.0, 13.0 / 1200.0)) / levelAt(out, 1, 0.3 / speed, kNoteHz * std::pow(2.0, 13.0 / 1200.0)));
                worst = std::max(worst, std::fabs(got - db(noteTruth(k, t) / noteTruth(1, 0.3))));
            }
        CHECK_MSG(worst < 0.5, "speed %.1f: the evolution is %.2f dB out", speed, worst);
    }
    // speed 0 freezes it where the start point is
    s->pUserSpeed->set(0.0f);
    s->pUserStart->set(0.2f);
    {
        const auto out = hold(*s, 57, sr, 96000);
        CHECK_NEAR(db(levelAt(out, 1, 0.3, 220.0) / levelAt(out, 1, 1.8, 220.0)), 0.0, 0.01);
        CHECK_NEAR(db(levelAt(out, 3, 0.3, 220.0) / levelAt(out, 1, 0.3, 220.0)), db(noteTruth(3, 0.5) / noteTruth(1, 0.5)), 1.0);
    }
    // the loop: the sound runs to the loop end, then back and forth between the loop points for as long as the note is held
    s->pUserSpeed->set(1.0f);
    s->pUserStart->set(0.0f);
    s->pUserLoop->set(1.0f);
    s->pUserLoopStart->set(0.4f);       // 1.0 s
    s->pUserLoopEnd->set(0.6f);         // 1.5 s
    {
        const auto out = hold(*s, 57, sr, 480000);          // ten seconds
        const double ref = levelAt(out, 1, 0.3, 220.0) / noteTruth(1, 0.3);
        const double hi = noteTruth(1, 1.0) * ref, lo = noteTruth(1, 1.5) * ref;
        double least = 1e9, most = 0.0;
        for (double t = 1.6; t < 9.9; t += 0.05) { const double v = levelAt(out, 1, t, 220.0); least = std::min(least, v); most = std::max(most, v); }
        CHECK_MSG(db(least / lo) > -0.5 && db(most / hi) < 0.5, "held for ten seconds the fundamental runs from %.2f dB below the loop end's level to %.2f dB above the loop start's", -db(least / lo), db(most / hi));
        CHECK(db(most / least) > 1.5);              // and it does move between them
        // the turn-round points: at 1.5 s (loop end), 2.0 s (back at the start), 2.5 s (the end again)
        CHECK_NEAR(db(levelAt(out, 1, 1.5, 220.0) / lo), 0.0, 0.3);
        CHECK_NEAR(db(levelAt(out, 1, 2.0, 220.0) / hi), 0.0, 0.3);
        CHECK_NEAR(db(levelAt(out, 1, 2.5, 220.0) / lo), 0.0, 0.3);
        // no step where it turns round
        CHECK_MSG(maxStep(out, 60000, 480000) <= maxStep(out, 30000, 60000) * 1.05f, "a step of %.5f in the loop", maxStep(out, 60000, 480000));
    }
    // loop points the wrong way round are the same loop; a loop of no length holds one frame
    s->pUserLoopStart->set(0.6f);
    s->pUserLoopEnd->set(0.4f);
    const auto swapped = hold(*s, 57, sr, 144000);
    s->pUserLoopStart->set(0.4f);
    s->pUserLoopEnd->set(0.6f);
    CHECK(swapped == hold(*s, 57, sr, 144000));
    s->pUserLoopEnd->set(0.4f);
    const auto held = hold(*s, 57, sr, 144000);
    CHECK_NEAR(db(levelAt(held, 1, 1.4, 220.0) / levelAt(held, 1, 2.8, 220.0)), 0.0, 0.01);
}

TEST(sheaf_analysis_is_saved_as_numbers_and_survives_save_and_load) {
    namespace fs = std::filesystem;
    auto s = userSheaf(theNote());
    s->pUserSpeed->set(1.3f);
    s->pMorph->set(0.2f);
    s->spec[1].shape->set((float)Sheaf::Bell);
    const auto an = s->analysis();
    std::vector<Note> notes = {mk(0, 1920, 52), mk(480, 960, 64, 0.5f), mk(1440, 1200, 71, 1.0f)};
    const RenderSpec rs{48000.0, 120.0, 512, 1, 144000};
    const auto before = play(s, notes, rs);
    CHECK(peakOf(before->ch[0]) > 0.05f);

    // through JSON text
    const std::string text = s->saveState().dump();
    CHECK_MSG(text.size() < 200000, "the state is %zu bytes", text.size());
    CHECK(text.find(".wav") == std::string::npos && text.find("\"sample\"") == std::string::npos && text.find("\"file\"") == std::string::npos);
    std::string perr;
    const Json parsed = Json::parse(text, &perr);
    CHECK_MSG(perr.empty(), "%s", perr.c_str());
    auto t = std::make_shared<Sheaf>();
    t->loadState(parsed);
    CHECK(t->hasAnalysis() && t->analysisName() == "Recording");
    const auto back = t->analysis();
    CHECK(back && back->q == an->q && back->amp == an->amp && back->hz == an->hz && back->seconds == an->seconds && back->ref == an->ref);
    CHECK(back && back->frames == an->frames && back->partials == an->partials && back->offset == an->offset && back->holds == an->holds && back->loudest == an->loudest);
    CHECK(t->saveState() == s->saveState());
    const auto after = play(t, notes, rs);
    CHECK_MSG(identical(*before, *after), "the render changed after save and load (largest difference %g)", maxAbsDiff(before->ch[0], after->ch[0]));
    printf("    the state with a %d x %d table is %zu bytes of JSON\n", an->frames, an->partials, text.size());

    // in a project file on disk (the type is not in the registry yet, so the test puts it there)
    for (auto& ty : PluginRegistry::global().types()) CHECK_MSG(ty.id == "additive" || ty.name != "Sheaf", "the name Sheaf is already taken by %s", ty.id.c_str());
    if (!PluginRegistry::global().find("additive"))
        PluginRegistry::global().add({"additive", "Sheaf", "Synths", true, [] { return std::make_shared<Sheaf>(); }});
    const std::string dir = t::tmpDir() + "/sheaf";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Project a, b;
    a.projectDir = dir;
    a.global->tempo.set(120.0f);
    {
        auto& c = a.addChannel(s, "Resynth");
        a.pattern().notesFor(c.id) = notes;
    }
    std::string err;
    CHECK_MSG(a.save(dir + "/song.pulp", &err), "%s", err.c_str());
    CHECK_MSG(b.load(dir + "/song.pulp", &err), "%s", err.c_str());
    CHECK(b.channels.size() == 1);
    if (b.channels.size() == 1) {
        auto* loaded = dynamic_cast<Sheaf*>(b.channels[0].live->plugin.get());
        CHECK(loaded != nullptr);
        if (loaded) {
            CHECK(loaded->hasAnalysis() && loaded->analysisName() == "Recording");
            CHECK(loaded->analysis() && loaded->analysis()->q == an->q);
            CHECK(loaded->pUserSpeed->get() == 1.3f);
        }
        RenderOptions o;
        o.lengthSamples = 144000;
        const auto ra = renderOffline(a, o), rb = renderOffline(b, o);
        CHECK_MSG(identical(*ra, *rb), "the project renders differently after being saved and opened");
        CHECK(identical(*ra, *before));
    }
    // the project file holds no reference to a recording: nothing for the project to go looking for
    CHECK(a.fileRefs().empty());
    std::string onDisk;
    CHECK(readTextFile(dir + "/song.pulp", onDisk));
    CHECK(onDisk.find("\"analysis\"") != std::string::npos);
    fs::remove_all(dir);
}

TEST(sheaf_presets_and_states_without_a_table_keep_the_analysed_sound) {
    auto s = userSheaf(theNote());
    const auto an = s->analysis();
    const auto presets = s->factoryPresets();
    for (auto& pr : presets) {
        s->loadState(pr.state);
        CHECK_MSG(s->analysis() == an, "preset %s dropped the analysis", pr.name.c_str());
    }
    // a null extra, an empty one, and one with rubbish in it: all leave it alone
    s->loadExtra(Json(), "");
    CHECK(s->analysis() == an);
    s->loadExtra(Json::object(), "");
    CHECK(s->analysis() == an);
    Json bad = Json::object();
    Json inner = Json::object();
    inner.set("frames", 3);
    inner.set("partials", 9000);
    bad.set("analysis", inner);
    s->loadExtra(bad, "");
    CHECK(s->analysis() == an);
    Json wrongSize = Json::parse(s->saveExtra().dump());
    Json cut = Json::object();
    for (auto& kv : wrongSize["analysis"].items()) if (kv.first != "q") cut.set(kv.first, kv.second);
    Json shortTable = Json::array();
    for (int i = 0; i < 10; ++i) shortTable.push(Json(5));
    cut.set("q", shortTable);
    Json holder = Json::object();
    holder.set("analysis", cut);
    s->loadExtra(holder, "");
    CHECK(s->analysis() == an);
    // a state saved by an instrument with nothing analysed has no extra at all, so loading it keeps the sound
    Sheaf blank;
    const Json blankState = blank.saveState();
    CHECK(!blankState.has("extra"));
    s->loadState(blankState);
    CHECK(s->analysis() == an);
    // a state that does carry a table replaces it
    Sheaf other;
    std::vector<double> lv = {0.2, 0.1, 0.05};
    CHECK(other.analyse(harmonicNote(330.0, 1.0, 44100.0, lv, {})));
    s->loadState(other.saveState());
    CHECK(s->analysis() != an && s->analysis()->partials == 3);
    CHECK_NEAR(s->analysis()->hz, 330.0, 0.1);
}

TEST(sheaf_takes_a_new_analysis_while_notes_are_sounding) {
    // The control thread swaps the table between quanta; the audio thread picks it up at the next one.
    auto s = plainSheaf(Sheaf::User, 4);       // four partials, so the waveform itself has no steep edges to hide a click behind
    s->spec[1].shape->set((float)Sheaf::User);
    s->pMorph->set(0.5f);
    Drive d(*s, 48000.0);
    d.on(0, 57, 1);
    d.on(100, 64, 2);
    std::vector<double> lv = {0.2, 0.1, 0.05, 0.02};
    auto recA = theNote(), recB = harmonicNote(330.0, 0.6, 44100.0, lv, {});
    const int64 allocBefore = Rt::allocationsInRt.load();
    for (int q = 0; q < 600; ++q) {
        if (q == 100) CHECK(s->analyse(recA));
        if (q == 250) CHECK(s->analyse(recB));
        if (q == 400) s->clearAnalysis();
        if (q == 500) CHECK(s->analyse(recA));
        d.run(kQuantum);
    }
    CHECK(Rt::allocationsInRt.load() == allocBefore);
    CHECK(allFinite(d.L) && allFinite(d.R));
    CHECK(peakOf(d.L, 0, 6400) > 0.05f && peakOf(d.L, 20000, 25000) > 0.01f);
    // every swap is taken up within one quantum and ramped: no step bigger than the sound itself makes
    CHECK_MSG(maxStep(d.L, 0, d.L.size()) < 0.1f, "a step of %.4f at a swap", maxStep(d.L, 0, d.L.size()));
    CHECK(s->activeVoices() == 2);
}

TEST(sheaf_morphs_the_analysed_sound_against_spectrum_b_and_filters_it) {
    const double sr = 48000.0, hz = keyToHz(57);
    auto make = [&](float morph) {
        auto s = userSheaf(theNote());
        s->pUserSpeed->set(0.0f);
        s->pUserStart->set(0.3f);
        s->spec[1].shape->set((float)Sheaf::Square);
        s->pMorph->set(morph);
        return s;
    };
    auto a = make(0.0f), b = make(1.0f), h = make(0.5f);
    const auto oa = hold(*a, 57, sr, 48000), ob = hold(*b, 57, sr, 48000), oh = hold(*h, 57, sr, 48000);
    auto lv = [&](const std::vector<float>& d, int k) { return ampFine(d, 6000, 16384, hz * k, sr); };
    double worst = 0.0;
    for (int k = 1; k <= 9; ++k) {
        // the user partials start in other phases than a square's, but each is one sine: its level is still on the line from A to B
        const double want = 0.5 * (lv(oa, k) + ((k & 1) ? lv(ob, k) : 0.0));
        worst = std::max(worst, std::fabs(db(lv(oh, k) / want)));
    }
    CHECK_MSG(worst < 0.01, "half way from the analysed sound to a square a partial is %.4f dB off the line", worst);
    CHECK(db(lv(ob, 2) / lv(ob, 1)) < -110.0 && lv(oa, 2) > 0.001);
    // the filter and the tilt work on it like on any spectrum
    auto f = make(0.0f);
    f->pFilterType->set((float)Sheaf::LowPass);
    f->pCutoff->set(880.0f);
    f->pSlope->set(24.0f);
    f->spec[0].tilt->set(3.0f);
    const auto of = hold(*f, 57, sr, 48000);
    // tilt +3 dB per octave, then 24 dB per octave above 880 Hz (harmonic 4), compared at harmonics 2 and 8
    CHECK_NEAR(db(lv(of, 8) / lv(of, 2)) - db(lv(oa, 8) / lv(oa, 2)), 3.0 * 2.0 - 24.0, 0.05);
    // the level of an analysed sound is set by its loudest moment, like any other spectrum: energy one at that frame
    auto loud = userSheaf(theNote());
    loud->pUserLoop->set(0.0f);
    const auto ol = hold(*loud, 57, sr, 48000);
    CHECK_MSG(peakOf(ol) > 0.15f && peakOf(ol) < 0.7f, "an analysed sound peaks at %.2f", peakOf(ol));
}
