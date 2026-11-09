#include "pulp/Templates.h"
#include "pulp/Multisample.h"
#include "pulp/Pads.h"
#include "pulp/Sampler.h"

namespace pulp {

namespace {

const int kBar = 4 * kPPQ;       // every template is in 4/4
const int kStep = kTicksPerStep;

// A built-in instrument or effect, on one of its presets (by name; the plugin's own defaults if it has no such preset).
std::shared_ptr<Plugin> make(const char* type, const char* preset = nullptr) {
    auto p = PluginRegistry::global().create(type);
    if (p && preset)
        for (auto& pr : p->factoryPresets())
            if (pr.name == preset) { p->loadState(pr.state); break; }
    return p;
}

// An instrument from the factory library ("Keys/Grand Piano"), or the fallback synth when it is not there.
std::shared_ptr<Plugin> factoryInstrument(const char* path, const char* fallbackType, const char* fallbackPreset) {
    const std::string p = path;
    const std::string name = p.substr(p.find_last_of('/') + 1);
    auto t = std::make_shared<Timbre>();
    if (t->loadFile("factory:Instruments/" + p + "/" + name + ".pulpinst", {})) return t;
    return make(fallbackType, fallbackPreset);
}

std::shared_ptr<Plugin> drumKit(int kit) {
    auto k = std::make_shared<PadKit>();
    const auto kits = k->factoryPresets();
    if (kit >= 0 && kit < (int)kits.size()) k->loadState(kits[(size_t)kit].state);
    return k;
}

std::shared_ptr<Plugin> oneShot(const char* factoryPath) {
    auto s = std::make_shared<Sampler>();
    s->setSample(SampleCache::global().load(std::string("factory:") + factoryPath));
    return s;
}

struct Builder {
    Project& p;
    int add(std::shared_ptr<Plugin> plug, const char* name, int mixerTrack, uint32 color, float volume = 0.78f) {
        if (!plug) plug = make("trio");
        auto& c = p.addChannel(plug, name);
        c.mixerTrack = mixerTrack;
        c.color = color;
        c.live->volume.set(volume);
        if (mixerTrack > 0) { p.mixer[(size_t)mixerTrack].name = name; p.mixer[(size_t)mixerTrack].color = color; }
        return c.id;
    }
    std::shared_ptr<Plugin> fx(int track, int slot, const char* type, std::initializer_list<std::pair<const char*, float>> values = {}, const char* preset = nullptr) {
        auto e = make(type, preset);
        if (!e) return nullptr;
        for (auto& kv : values) if (auto* prm = e->param(kv.first)) prm->set(kv.second);
        p.setSlot(track, slot, e);
        return e;
    }
    void send(int from, int to, float level) {
        if (p.setRoute(from, to, true)) if (auto* r = p.mixer[(size_t)from].routeTo(to)) r->level->set(level);
    }
    int pattern(const char* name, int bars, uint32 color = 0xff4f7ea8) {
        auto& pat = p.patterns.size() == 1 && p.patterns[0].empty() && p.patterns[0].name.rfind("Pattern", 0) == 0 && !used_ ? p.patterns[0] : p.addPattern(name);
        used_ = true;
        pat.name = name;
        pat.steps = bars * 16;
        pat.color = color;
        return p.patternIndex(pat.id);
    }
    void note(int pat, int channelId, int tick, int length, int key, float velocity = kDefaultVelocity) {
        Note n;
        n.tick = tick; n.length = length; n.key = key; n.velocity = velocity;
        p.patterns[(size_t)pat].notesFor(channelId).push_back(n);
    }
    void finish() { for (auto& pat : p.patterns) for (auto& kv : pat.notes) sortNotes(kv.second); }
    bool used_ = false;
};

const uint32 kRed = 0xffb8524a, kOrange = 0xffc98a3c, kYellow = 0xffb5a642, kGreen = 0xff5d9a57, kTeal = 0xff3f9a94, kBlue = 0xff4f7ea8, kPurple = 0xff8a63ad, kPink = 0xffb0608c;

void basic(Project& p) {
    Builder b{p};
    const char* files[4][2] = {{"Drums/808 Kit/808 Kick.wav", "Kick"}, {"Drums/808 Kit/808 Clap.wav", "Clap"}, {"Drums/808 Kit/808 Hat Closed.wav", "Hat"}, {"Drums/808 Kit/808 Snare.wav", "Snare"}};
    const uint32 colors[4] = {kRed, kOrange, kYellow, kGreen};
    for (int i = 0; i < 4; ++i) b.add(oneShot(files[i][0]), files[i][1], 0, colors[i]);
    b.fx(kMasterTrack, 0, "limiter");
}

// The parts every genre starter has: a kick of its own (so other parts can duck under it), a drum kit, a
// reverb bus on insert 10, a limiter on the master.
void starter(Project& p, float tempo, const char* kickFile, int kit) {
    Builder b{p};
    p.global->tempo.set(tempo);
    b.add(oneShot(kickFile), "Kick", 1, kRed);
    b.add(drumKit(kit), "Drums", 2, kOrange);
    b.fx(10, 0, "reverb", {{"mix", 1.0f}, {"decay", 2.2f}, {"lowcut", 250.0f}});
    p.mixer[10].name = "Reverb";
    p.mixer[10].color = kTeal;
    b.fx(kMasterTrack, 0, "limiter");
}

void ducking(Project& p, int kickTrack, int track, int slot, float thresh = -26.0f) {
    Builder b{p};
    b.fx(track, slot, "comp", {{"thresh", thresh}, {"ratio", 6.0f}, {"attack", 2.0f}, {"release", 140.0f}, {"side", 1.0f}});
    p.setRoute(kickTrack, track, true, true);
}

} // namespace

std::vector<TemplateInfo> projectTemplates() {
    return {
        {"Empty", "One empty sampler channel and nothing else"},
        {"Basic with limiter", "Kick, clap, hat and snare, with a limiter on the master"},
        {"Trap starter", "140 BPM: 808 kit, a long 808 bass, bells and a dark pad, each on its own mixer track, reverb bus"},
        {"House starter", "124 BPM: 909 kick and kit, a bass that ducks under the kick, stabs and a pad, reverb bus"},
        {"Lo-fi starter", "82 BPM: a damped acoustic kit, electric piano, upright bass and a soft pad through a darker master"},
        {"Synthwave starter", "100 BPM: 909 kit, a driving bass, a wide pad, a lead with delay, reverb bus"},
    };
}

bool makeTemplate(Project& p, int index) {
    p = Project();
    Builder b{p};
    switch (index) {
        case 0:
            b.add(std::make_shared<Sampler>(), "Sampler", 0, kBlue);
            break;
        case 1:
            basic(p);
            break;
        case 2:
            starter(p, 140.0f, "Drums/808 Kit/808 Kick.wav", 0);
            b.add(factoryInstrument("Bass/808 Bass", "zest", "Round Sub Bass"), "808 bass", 3, kPurple);
            b.add(make("fm6", "Chapel Bell"), "Bells", 4, kYellow, 0.6f);
            b.add(make("zest", "Dark Drone Pad"), "Pad", 5, kBlue, 0.6f);
            b.send(4, 10, 0.35f);
            b.send(5, 10, 0.25f);
            break;
        case 3:
            starter(p, 124.0f, "Drums/909 Kit/909 Kick.wav", 1);
            b.add(make("zest", "Round Sub Bass"), "Bass", 3, kPurple);
            ducking(p, 1, 3, 0);
            b.add(make("zest", "Trance Stack Pluck"), "Stabs", 4, kYellow, 0.6f);
            b.add(make("wavetable", "Tidal Pad"), "Pad", 5, kBlue, 0.55f);
            ducking(p, 1, 5, 0, -30.0f);
            b.send(4, 10, 0.3f);
            b.send(5, 10, 0.3f);
            break;
        case 4:
            starter(p, 82.0f, "Drums/Acoustic Kit/kick.wav", 6);
            b.add(factoryInstrument("Keys/Wurlitzer Electric Piano", "fm6", "Velvet Keys"), "Electric piano", 3, kYellow);
            b.add(factoryInstrument("Bass/Upright Bass", "zest", "Round Sub Bass"), "Upright bass", 4, kPurple);
            b.add(make("zest", "Warm Analog Pad"), "Pad", 5, kBlue, 0.5f);
            b.fx(3, 0, "chorus", {{"mix", 0.3f}, {"rate", 0.4f}});
            b.send(3, 10, 0.25f);
            b.send(5, 10, 0.3f);
            b.fx(kMasterTrack, 0, "filter", {{"cutoff", 9000.0f}, {"reso", 0.05f}});      // the darker master
            b.fx(kMasterTrack, 1, "limiter");
            break;
        case 5:
            starter(p, 100.0f, "Drums/909 Kit/909 Kick.wav", 1);
            b.add(make("zest", "Pick Bass"), "Bass", 3, kPurple);
            ducking(p, 1, 3, 0);
            b.add(make("zest", "PWM String Machine"), "Pad", 4, kBlue, 0.55f);
            b.add(make("zest", "Stadium Saw Stack"), "Lead", 5, kPink, 0.55f);
            b.fx(5, 0, "delay", {{"sync", 4.0f}, {"feedback", 0.45f}, {"mix", 0.28f}, {"ping", 1.0f}});
            b.send(4, 10, 0.3f);
            b.send(5, 10, 0.3f);
            break;
        default: return false;
    }
    return true;
}

// ------------------------------------------------------------------ the demo song

void makeDemoSong(Project& p) {
    p = Project();
    p.name = "Demo song";
    Builder b{p};
    p.global->tempo.set(122.0f);

    // ---- instruments, each on its own mixer track ----
    const int kick = b.add(oneShot("Drums/909 Kit/909 Kick.wav"), "Kick", 1, kRed, 0.82f);
    const int drums = b.add(drumKit(1), "Drums", 2, kOrange, 0.7f);
    const int bass = b.add(make("zest", "Round Sub Bass"), "Bass", 3, kPurple, 0.8f);
    const int keys = b.add(factoryInstrument("Keys/Electric Piano", "fm6", "Velvet Keys"), "Keys", 4, kYellow, 0.6f);
    const int pad = b.add(make("wavetable", "Tidal Pad"), "Pad", 5, kBlue, 0.5f);
    const int lead = b.add(make("fm6", "Glass Tines"), "Lead", 6, kPink, 0.6f);
    const int arp = b.add(factoryInstrument("Mallets/Vibraphone", "wavetable", "Bell Pluck"), "Vibes", 7, kTeal, 0.55f);

    // ---- the mix ----
    ducking(p, 1, 3, 0);                                        // the bass ducks under the kick
    b.fx(4, 0, "chorus", {{"mix", 0.3f}, {"rate", 0.5f}});
    b.fx(5, 0, "filter", {{"cutoff", 600.0f}, {"reso", 0.25f}});     // opened by an automation clip
    b.fx(6, 0, "delay", {{"sync", 4.0f}, {"feedback", 0.4f}, {"mix", 0.25f}, {"ping", 1.0f}});
    b.fx(2, 0, "eq7", {});
    b.fx(10, 0, "reverb", {{"mix", 1.0f}, {"decay", 2.6f}, {"lowcut", 300.0f}, {"size", 0.7f}});
    p.mixer[10].name = "Reverb";
    p.mixer[10].color = kTeal;
    b.send(2, 10, 0.08f);
    b.send(4, 10, 0.28f);
    b.send(5, 10, 0.35f);
    b.send(6, 10, 0.3f);
    b.send(7, 10, 0.35f);
    b.fx(kMasterTrack, 0, "limiter", {{"gain", 2.0f}});

    // ---- patterns (four bars each): A minor, F, C, G ----
    const int roots[4] = {45, 41, 48, 43};                      // A, F, C, G
    const int chords[4][3] = {{57, 60, 64}, {57, 60, 65}, {55, 60, 64}, {55, 59, 62}};
    const int pDrums = b.pattern("Drums", 4, kOrange);
    for (int bar = 0; bar < 4; ++bar) {
        for (int beat = 0; beat < 4; ++beat) {
            b.note(pDrums, kick, bar * kBar + beat * kPPQ, kStep, kRootKey);
            b.note(pDrums, drums, bar * kBar + beat * kPPQ + kPPQ / 2, kStep, PadKit::kBaseKey + 3, 0.55f + 0.1f * (beat % 2));      // off-beat closed hat
            if (beat % 2 == 1) b.note(pDrums, drums, bar * kBar + beat * kPPQ, kStep, PadKit::kBaseKey + 2, 0.75f);               // clap on two and four
        }
        b.note(pDrums, drums, bar * kBar + 15 * kStep, kStep, PadKit::kBaseKey + 4, 0.5f);                                           // open hat into the next bar
        for (int s = 0; s < 16; s += 2) b.note(pDrums, drums, bar * kBar + s * kStep + kStep, kStep / 2, PadKit::kBaseKey + 3, 0.28f); // quiet sixteenths between
    }
    const int pFill = b.pattern("Drum fill", 1, kOrange);
    for (int s = 0; s < 16; ++s) {
        if (s < 8 && s % 4 == 0) b.note(pFill, kick, s * kStep, kStep, kRootKey);
        if (s >= 8) b.note(pFill, drums, s * kStep, kStep, PadKit::kBaseKey + (s < 12 ? 6 : 5), 0.5f + 0.03f * s);                   // toms rolling down
        if (s % 2 == 0) b.note(pFill, drums, s * kStep, kStep, PadKit::kBaseKey + 3, 0.5f);
    }
    b.note(pFill, drums, 12 * kStep, kStep, PadKit::kBaseKey + 1, 0.8f);
    b.note(pFill, drums, 14 * kStep, kStep, PadKit::kBaseKey + 1, 0.9f);

    const int pBass = b.pattern("Bass", 4, kPurple);
    for (int bar = 0; bar < 4; ++bar)
        for (int s = 0; s < 16; s += 2) {
            const bool up = s == 6 || s == 14;
            b.note(pBass, bass, bar * kBar + s * kStep, kStep + kStep / 2, roots[bar] - 12 + (up ? 12 : 0), s % 4 == 2 ? 0.85f : 0.7f);
        }
    const int pKeys = b.pattern("Keys", 4, kYellow);
    for (int bar = 0; bar < 4; ++bar) {
        // a pushed rhythm: the chord on the "and" of one, held, and again before three
        for (int hit : {2, 7, 10}) for (int k : chords[bar]) b.note(pKeys, keys, bar * kBar + hit * kStep, hit == 2 ? 4 * kStep : 3 * kStep, k, hit == 2 ? 0.7f : 0.55f);
    }
    const int pPad = b.pattern("Pad", 4, kBlue);
    for (int bar = 0; bar < 4; ++bar) {
        for (int k : chords[bar]) b.note(pPad, pad, bar * kBar, kBar - 20, k + 12, 0.6f);
        b.note(pPad, pad, bar * kBar, kBar - 20, roots[bar] + 12, 0.6f);
    }
    const int pLead = b.pattern("Lead", 4, kPink);
    {
        // tick (in steps), length (steps), key
        const int tune[][3] = {{0, 3, 76}, {3, 3, 79}, {6, 2, 81}, {8, 6, 76}, {14, 2, 74},
                               {16, 3, 72}, {19, 3, 76}, {22, 2, 77}, {24, 8, 72},
                               {32, 3, 79}, {35, 3, 76}, {38, 2, 72}, {40, 6, 79}, {46, 2, 81},
                               {48, 4, 83}, {52, 4, 79}, {56, 8, 74}};
        for (auto& t : tune) b.note(pLead, lead, t[0] * kStep, t[1] * kStep - 10, t[2], 0.75f);
    }
    const int pArp = b.pattern("Vibes", 4, kTeal);
    for (int bar = 0; bar < 4; ++bar)
        for (int s = 0; s < 16; ++s) {
            const int order[8] = {0, 1, 2, 1, 2, 0, 1, 2};
            const int k = chords[bar][order[s % 8]] + 12 + (s % 8 >= 5 ? 12 : 0);
            b.note(pArp, arp, bar * kBar + s * kStep, kStep - 10, k, 0.4f + 0.05f * (float)((s * 5) % 4));
        }
    b.finish();

    // ---- the arrangement: 20 bars ----
    // track 0 drums, 1 bass, 2 keys, 3 pad, 4 lead, 5 vibes, 6 and 7 automation
    const char* names[8] = {"Drums", "Bass", "Keys", "Pad", "Lead", "Vibes", "Filter", "Fade"};
    for (int i = 0; i < 8; ++i) p.tracks[(size_t)i].name = names[i];
    auto place = [&](int pat, int track, int bar, int bars = 4) {
        auto& c = p.addPatternClip(pat, track, (int64)bar * kBar);
        c.length = (int64)bars * kBar;
    };
    place(pPad, 3, 0); place(pKeys, 2, 0);                                           // 1-4: pad and keys
    place(pPad, 3, 4); place(pKeys, 2, 4); place(pDrums, 0, 4); place(pBass, 1, 4);   // 5-8: the beat and the bass come in
    place(pPad, 3, 8); place(pKeys, 2, 8); place(pDrums, 0, 8, 3); place(pFill, 0, 11, 1); place(pBass, 1, 8); place(pLead, 4, 8);   // 9-12: the tune
    place(pPad, 3, 12); place(pArp, 5, 12); place(pLead, 4, 12);                      // 13-16: the beat drops out
    place(pPad, 3, 16); place(pKeys, 2, 16); place(pDrums, 0, 16); place(pBass, 1, 16); place(pLead, 4, 16); place(pArp, 5, 16);     // 17-20: everything
    p.songMode = true;

    // ---- automation: the pad's filter opens over the first eight bars and again through the break; a fade at the end ----
    {
        auto& c = p.addAutomationClip("fx:5:0:cutoff", 6, 0, 8 * kBar);
        if (auto* a = p.autoById(c.autoId)) { a->name = "Pad filter opens"; a->points = {{0, 0.35f, 0.4f, ShapeCurve}, {8 * kBar, 0.85f, 0.0f, ShapeCurve}}; }
    }
    {
        auto& c = p.addAutomationClip("fx:5:0:cutoff", 6, 12 * kBar, 4 * kBar);
        if (auto* a = p.autoById(c.autoId)) { a->name = "Break sweep"; a->points = {{0, 0.85f, 0.0f, ShapeCurve}, {2 * kBar, 0.45f, 0.0f, ShapeCurve}, {4 * kBar, 0.92f, 0.5f, ShapeCurve}}; }
    }
    {
        auto& c = p.addAutomationClip("mix:0:volume", 7, 19 * kBar, kBar);
        if (auto* a = p.autoById(c.autoId)) {
            const float now = p.mixer[0].live->volume.getNorm();
            a->name = "Fade out";
            a->points = {{0, now, 0.0f, ShapeCurve}, {kBar, 0.0f, -0.3f, ShapeCurve}};
        }
    }
}

} // namespace pulp
