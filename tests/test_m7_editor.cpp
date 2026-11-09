// Milestone 7: the audio editor's operations and Splice, the recorder / editor effect.
#include "Util.h"
#include "pulp/Editor.h"
#include <filesystem>
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0;
std::shared_ptr<SampleData> ramp(int n) {       // sample i has the value i / n on the left and its negative on the right
    auto s = std::make_shared<SampleData>();
    s->sampleRate = kSr; s->channels = 2;
    s->ch[0].resize((size_t)n); s->ch[1].resize((size_t)n);
    for (int i = 0; i < n; ++i) { s->ch[0][(size_t)i] = (float)i / (float)n; s->ch[1][(size_t)i] = -(float)i / (float)n; }
    return s;
}
}

TEST(editor_operations_change_only_the_selection) {
    auto s = ramp(1000);
    auto t = edit::trim(*s, 100, 300);
    CHECK(t->frames() == 200 && t->ch[0][0] == s->ch[0][100] && t->ch[1][199] == s->ch[1][299] && t->channels == 2);
    auto c = edit::cut(*s, 100, 300);
    CHECK(c->frames() == 800 && c->ch[0][99] == s->ch[0][99] && c->ch[0][100] == s->ch[0][300]);
    auto z = edit::silence(*s, 100, 300);
    CHECK(z->frames() == 1000 && z->ch[0][99] == s->ch[0][99] && z->ch[0][100] == 0.0f && z->ch[1][299] == 0.0f && z->ch[0][300] == s->ch[0][300]);
    auto r = edit::reverse(*s, 100, 300);
    CHECK(r->ch[0][100] == s->ch[0][299] && r->ch[0][299] == s->ch[0][100] && r->ch[1][150] == s->ch[1][249] && r->ch[0][300] == s->ch[0][300]);
    auto g = edit::gain(*s, 500, 600, 0.5f);
    CHECK(g->ch[0][550] == s->ch[0][550] * 0.5f && g->ch[0][600] == s->ch[0][600] && g->ch[0][499] == s->ch[0][499]);
    auto fi = edit::fade(*s, 400, 600, true);
    CHECK(fi->ch[0][400] == 0.0f && std::fabs(fi->ch[0][500] - s->ch[0][500] * 0.5f) < 1e-6f && fi->ch[0][600] == s->ch[0][600]);
    auto fo = edit::fade(*s, 400, 600, false);
    CHECK(fo->ch[0][400] == s->ch[0][400] && std::fabs(fo->ch[0][599]) < 0.004f && fo->ch[0][399] == s->ch[0][399]);
    auto n = edit::normalize(*s, 0, 500);        // the loudest sample in the range goes to 0.98
    CHECK_NEAR(n->ch[0][499], 0.98, 1e-6);
    CHECK(n->ch[0][500] == s->ch[0][500]);
    // nothing selected means everything
    CHECK(edit::reverse(*s, 0, 0)->ch[0][0] == s->ch[0][999]);
    CHECK_NEAR(edit::normalize(*s, 5, 5)->peak(), 0.98, 1e-6);
    // the source is never changed, and a range past the end is clamped
    CHECK(s->ch[0][150] == 0.15f);
    CHECK(edit::cut(*s, 900, 5000)->frames() == 900);
    CHECK(edit::normalize(*edit::silence(*s, 0, 0), 0, 0)->peak() == 0.0f);
}

TEST(noise_removal_takes_out_the_noise_and_leaves_the_sound) {
    // half a second of hiss alone, then a tone in the same hiss
    const int n = 96000, lead = 24000;
    auto s = std::make_shared<SampleData>();
    s->sampleRate = kSr; s->channels = 1;
    s->ch[0].resize((size_t)n);
    dsp::Noise hiss(15u);
    for (int i = 0; i < n; ++i) {
        const float tone = i >= lead ? 0.3f * (float)std::sin(kTwoPi * 440.0 * i / kSr) : 0.0f;
        s->ch[0][(size_t)i] = tone + 0.02f * hiss.next();
    }
    const auto profile = edit::learnNoise(*s, 2000, 22000);
    CHECK(profile.valid());
    auto clean = edit::removeNoise(*s, 0, 0, profile, 1.0f);
    CHECK(clean->frames() == n);
    const double before = rmsOf(s->ch[0], 4000, 20000), after = rmsOf(clean->ch[0], 4000, 20000);
    CHECK_MSG(20.0 * std::log10(after / before) < -20.0, "hiss went down by only %.1f dB", -20.0 * std::log10(after / before));
    // the tone is still there at its level, and cleaner than before
    const double toneBefore = rmsOf(s->ch[0], 40000, 90000), toneAfter = rmsOf(clean->ch[0], 40000, 90000);
    CHECK_MSG(std::fabs(20.0 * std::log10(toneAfter / toneBefore)) < 0.5, "tone level moved by %.2f dB", 20.0 * std::log10(toneAfter / toneBefore));
    CHECK_NEAR(measureHz(clean->ch[0], 40000, 90000, kSr), 440.0, 0.05);
    auto residual = [&](const std::vector<float>& d) {       // what is left after taking the pure tone out
        double sum = 0.0;
        for (int i = 40000; i < 90000; ++i) { const double e = d[(size_t)i] - 0.3 * std::sin(kTwoPi * 440.0 * i / kSr); sum += e * e; }
        return std::sqrt(sum / 50000.0);
    };
    CHECK_MSG(residual(clean->ch[0]) < residual(s->ch[0]) * 0.5, "residual %g, was %g", residual(clean->ch[0]), residual(s->ch[0]));
    // amount zero changes nothing audible, and a partial range leaves the rest bit-exact
    CHECK(maxAbsDiff(edit::removeNoise(*s, 0, 0, profile, 0.0f)->ch[0], s->ch[0]) < 0.01);
    auto part = edit::removeNoise(*s, 30000, 60000, profile, 1.0f);
    bool same = true;
    for (int i = 0; i < 30000; ++i) same = same && part->ch[0][(size_t)i] == s->ch[0][(size_t)i];
    for (int i = 60000; i < n; ++i) same = same && part->ch[0][(size_t)i] == s->ch[0][(size_t)i];
    CHECK(same);
    // without a profile nothing happens
    CHECK(edit::removeNoise(*s, 0, 0, edit::NoiseProfile(), 1.0f)->ch[0] == s->ch[0]);
}

TEST(splice_passes_sound_through_and_records_it) {
    namespace fs = std::filesystem;
    const std::string dir = t::tmpDir() + "/splice";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // a tone on mixer track 1 with Splice in its first slot
    auto build = [&](Project& p) {
        p.global->tempo.set(120.0f);
        auto ch = addSampleChannel(p, sine(330.0, 1.0, kSr, 0.4f));
        p.channels[0].mixerTrack = 1;
        Project::setStep(p.pattern().notesFor(ch.id), 0, true);
        Project::setStep(p.pattern().notesFor(ch.id), 8, true);
    };
    Project plain, with;
    build(plain);
    build(with);
    auto ed = std::make_shared<Splice>();
    with.setSlot(1, 0, ed);
    RenderOptions o;
    o.lengthSamples = 96000;
    auto a = renderOffline(plain, o);
    ed->pRec->set(1.0f);
    const int64 allocs = Rt::allocationsInRt.load();
    auto b = renderOffline(with, o);
    CHECK(Rt::allocationsInRt.load() == allocs);
    CHECK(identical(*a, *b));                   // in the slot it changes nothing
    CHECK(ed->capturing());
    // offline nothing drains the capture ring, so it fills and stops; live, pump() keeps up:
    with.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setState(with.compile());
    ed->pRec->set(1.0f);
    e.play();
    std::vector<float> l(96000), r(96000);
    for (int c = 0; c < 96000; c += 480) {
        float* out[2] = {l.data() + c, r.data() + c};
        e.process(nullptr, 0, out, 2, 480);
        ed->pump();
    }
    ed->pRec->set(0.0f);
    float* out[2] = {l.data(), r.data()};
    std::vector<float> l2(480), r2(480);
    float* out2[2] = {l2.data(), r2.data()};
    e.process(nullptr, 0, out2, 2, 480);
    ed->pump();
    (void)out;
    CHECK(!ed->capturing());
    auto rec = ed->audio();
    CHECK(rec != nullptr);
    if (!rec) return;
    CHECK_MSG(rec->frames() >= 96000 - 64 && rec->frames() <= 96000 + 480, "%lld frames recorded", (long long)rec->frames());
    CHECK(rec->channels == 2 && rec->sampleRate == kSr);
    // what it recorded is what went through the track (the track's output goes to the master unchanged here)
    double worst = 0.0;
    for (int i = 0; i < 90000; ++i) worst = std::max(worst, (double)std::fabs(rec->ch[0][(size_t)i] - l[(size_t)i]));
    CHECK_MSG(worst < 1e-7, "recording differs from the sound by %g", worst);
    CHECK(ed->dropped() == 0 && ed->unsaved());

    // edits are undoable
    ed->setAudio(edit::trim(*rec, 1000, 5000), true);
    CHECK(ed->audio()->frames() == 4000 && ed->canUndo());
    CHECK(ed->undo());
    CHECK(ed->audio()->frames() == rec->frames());

    // an unsaved recording survives a project snapshot (that is what undo uses) ...
    const Json snap = with.toJson();
    Project back;
    CHECK(back.fromJson(snap));
    auto* ed2 = dynamic_cast<Splice*>(back.mixer[1].slots[0].get());
    CHECK(ed2 != nullptr);
    if (ed2) {
        CHECK(ed2->audio() != nullptr && ed2->audio()->frames() == rec->frames() && ed2->unsaved());
        CHECK(!ed2->pRec->getBool());
    }
    CHECK(with.fileRefs().size() == 0);         // nothing on disk yet
    // ... and saving the project writes it into the project's folder and reloads it from there
    CHECK(with.save(dir + "/song.pulp"));
    CHECK(!ed->unsaved());
    CHECK(fs::exists(dir + "/edits/edit_1.wav"));
    Project loaded;
    std::string err;
    CHECK_MSG(loaded.load(dir + "/song.pulp", &err), "%s", err.c_str());
    auto* ed3 = dynamic_cast<Splice*>(loaded.mixer[1].slots[0].get());
    CHECK(ed3 != nullptr);
    if (ed3) {
        CHECK(ed3->audio() != nullptr && ed3->audio()->ch[0] == rec->ch[0] && !ed3->unsaved());
        CHECK(ed3->file == "edits/edit_1.wav");
    }
}

TEST(splice_record_modes) {
    Project p;
    p.global->tempo.set(120.0f);
    auto ed = std::make_shared<Splice>();
    p.setSlot(0, 0, ed);                        // on the master
    auto ch = addSampleChannel(p, sine(440.0, 0.1, kSr, 0.5f));
    Project::setStep(p.pattern().notesFor(ch.id), 4, true);     // first sound at step 4 = half a second in
    p.prepare(kSr);
    Engine e;
    e.prepare(kSr);
    e.setState(p.compile());
    std::vector<float> l(480), r(480);
    float* out[2] = {l.data(), r.data()};
    auto run = [&](int blocks) { for (int i = 0; i < blocks; ++i) { e.process(nullptr, 0, out, 2, 480); ed->pump(); } };
    // "on play": armed while stopped records nothing
    ed->pMode->set((float)Splice::OnPlay);
    ed->pRec->set(1.0f);
    run(20);
    CHECK(ed->audio() == nullptr);
    e.play();
    run(20);
    CHECK(ed->audio() != nullptr && ed->audio()->frames() > 8000);
    // "on input": waits for the first sound, so the recording starts with it
    ed->pRec->set(0.0f);
    e.stop(); e.stop();
    run(4);
    ed->clear();
    CHECK(ed->audio() == nullptr);
    ed->pMode->set((float)Splice::OnInput);
    ed->pRec->set(1.0f);
    e.play();
    run(120);
    CHECK(ed->audio() != nullptr);
    if (ed->audio()) {
        CHECK(peakOf(ed->audio()->ch[0], 0, 64) > 0.0f);        // no silent lead-in: the first block already holds sound
        CHECK_MSG(ed->audio()->frames() < 480 * 120 - 20000, "%lld frames: it recorded the silence before the sound", (long long)ed->audio()->frames());
    }
}
