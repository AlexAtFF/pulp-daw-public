// Milestone 14: mixer tracks change places, and effects change slots, with everything that refers to them.
#include "Util.h"
#include "pulp/Automation.h"
#include "pulp/Controllers.h"
#include "pulp/Effects.h"
#include <filesystem>
using namespace pulp;
using namespace tu;

namespace {
const double kSr = 48000.0;

std::shared_ptr<SampleData> render(Project& p, int block = 512, int threads = 1) {
    RenderOptions o;
    o.lengthSamples = 96000;
    o.blockSize = block;
    o.threads = threads;
    return renderOffline(p, o);
}

// A trigger on insert 1 ducks a bass on insert 2 through a follower and a link; the bass has an effect whose
// gain a clip automates, a pan curve, and a send to insert 4 whose level the pattern's events move.
struct Song {
    Project p;
    Lid* lid = nullptr;
    std::string lidGain;
};
void build(Song& s) {
    Project& p = s.p;
    // the sounds are files, so that the project can be saved and read back
    namespace fs = std::filesystem;
    const std::string dir = t::tmpDir() + "/mixorder";
    fs::create_directories(dir);
    SampleData click;
    click.sampleRate = kSr; click.channels = 1;
    click.ch[0].assign(64, 0.9f);
    writeWavFile(dir + "/click.wav", click, WavFormat::Float32);
    writeWavFile(dir + "/bass.wav", *sine(110.0, 2.0, kSr, 0.4f), WavFormat::Float32);
    p.projectDir = dir;
    p.global->tempo.set(120.0f);
    p.songMode = true;
    const int kick = p.addSamplerChannel(dir + "/click.wav", "Trigger").id;
    p.channels[0].mixerTrack = 1;
    for (int st : {0, 4, 8, 12}) Project::setStep(p.pattern().notesFor(kick), st, true);
    p.addPatternClip(0, 0, 0);
    p.addAudioClip(p.addAudio(dir + "/bass.wav"), 1, 0, 2);

    auto follower = std::make_shared<Follower>();
    follower->loadState(follower->factoryPresets()[4].state);       // "Silent trigger"
    p.setSlot(1, 1, follower);
    ParamLink link;
    link.target = "mix:2:volume";
    link.kind = ParamLink::Controller;
    link.track = 1; link.slot = 1; link.output = 0;
    link.min = 0.0f; link.max = 0.8f;
    p.setLink(link);

    auto lid = std::make_shared<Lid>();
    s.lid = lid.get();
    p.setSlot(2, 0, lid);
    s.lidGain = p.addressOf(lid->pGain);
    Clip& gc = p.addAutomationClip(s.lidGain, 5, 0, 7680);
    p.autoById(gc.autoId)->points = {{0, 0.2f}, {7680, 0.6f}};
    Clip& pc = p.addAutomationClip("mix:2:pan", 6, 0, 7680);
    p.autoById(pc.autoId)->points = {{0, 0.5f}, {7680, 1.0f}};

    p.setRoute(2, 4, true);
    p.mixer[4].live->pan.set(-1.0f);
    p.pattern().events["send:2:4"] = {{0, 0.1f, false}, {1920, 0.9f, false}};
}
}

TEST(mixer_tracks_change_places_and_sound_the_same) {
    Song s;
    build(s);
    Project& p = s.p;
    CHECK(s.lidGain == "fx:2:0:gain");
    const auto ref = render(p);
    const Json before = p.toJson();         // after a render, which leaves the automated controls where the song ends
    CHECK(peakOf(ref->ch[0]) > 0.05f);
    CHECK(rmsOf(ref->ch[0], 300, 1500) < rmsOf(ref->ch[0], 19000, 23500) * 0.5);      // the ducking is at work

    // not the master, not the current track, not out of range; a track with itself is nothing to do
    CHECK(!p.swapMixerTracks(kMasterTrack, 3) && !p.swapMixerTracks(3, kCurrentTrack) && !p.swapMixerTracks(-1, 3) && !p.swapMixerTracks(3, kNumMixerTracks));
    CHECK(p.swapMixerTracks(3, 3));
    CHECK(p.toJson() == before);

    // the bass track goes from 2 to 7
    p.selectedMixerTrack = 2;
    CHECK(p.swapMixerTracks(2, 7));
    CHECK(p.selectedMixerTrack == 7);
    CHECK(p.clips[1].mixerTrack == 7);
    CHECK(p.mixer[7].slots[0].get() == s.lid && !p.mixer[2].slots[0]);
    CHECK(p.addressOf(s.lid->pGain) == "fx:7:0:gain");
    CHECK(p.autos[0].target == "fx:7:0:gain" && p.autos[1].target == "mix:7:pan");
    CHECK(p.pattern().events.count("send:7:4") == 1 && p.pattern().events.count("send:2:4") == 0);
    CHECK(p.pattern().events["send:7:4"].size() == 2);
    CHECK(p.links.size() == 1 && p.links[0].target == "mix:7:volume" && p.links[0].track == 1 && p.links[0].slot == 1);
    CHECK(p.mixer[7].routeTo(4) != nullptr && p.mixer[7].routeTo(kMasterTrack) != nullptr && p.mixer[2].routeTo(4) == nullptr);
    CHECK(p.mixer[2].routeTo(kMasterTrack) != nullptr);          // the empty track that took its place still plays to the master
    CHECK(identical(*ref, *render(p)));

    // the trigger's track, with the follower the link listens to, goes from 1 to 9
    CHECK(p.swapMixerTracks(9, 1));
    CHECK(p.channels[0].mixerTrack == 9);
    CHECK(p.links[0].track == 9 && p.links[0].slot == 1 && p.links[0].target == "mix:7:volume");
    CHECK(identical(*ref, *render(p)));

    // the track the send goes to moves: the send and its events follow
    CHECK(p.swapMixerTracks(4, 3));
    CHECK(p.mixer[7].routeTo(3) != nullptr && p.mixer[7].routeTo(4) == nullptr);
    CHECK(p.pattern().events.count("send:7:3") == 1);
    CHECK(p.mixer[3].live->pan.get() == -1.0f);
    CHECK(identical(*ref, *render(p)));

    // two tracks that both carry something, one feeding the other
    CHECK(p.swapMixerTracks(7, 3));
    CHECK(p.mixer[3].routeTo(7) != nullptr);
    CHECK(p.autos[0].target == "fx:3:0:gain" && p.pattern().events.count("send:3:7") == 1 && p.links[0].target == "mix:3:volume");
    CHECK(identical(*ref, *render(p)));
    CHECK(identical(*ref, *render(p, 77, 3)));

    // saved and loaded in the new order
    Project q;
    CHECK(q.fromJson(p.toJson()));
    CHECK(q.toJson() == p.toJson());
    CHECK(identical(*ref, *render(q)));

    // swapped back the way they came, the project is what it was
    CHECK(p.swapMixerTracks(7, 3) && p.swapMixerTracks(4, 3) && p.swapMixerTracks(9, 1) && p.swapMixerTracks(2, 7));
    p.selectedMixerTrack = 0;
    CHECK(p.toJson() == before);
}

TEST(effects_change_slots_and_automation_follows) {
    Song s;
    build(s);
    Project& p = s.p;
    const auto ref = render(p);

    // the effect with the automated gain goes from slot 0 to slot 3 (an empty one): the same sound
    p.mixer[2].live->slotMix[0].set(0.75f);
    const auto mixed = render(p);
    CHECK(!identical(*ref, *mixed));
    CHECK(p.swapSlots(2, 0, 3));
    CHECK(p.mixer[2].slots[3].get() == s.lid && !p.mixer[2].slots[0]);
    CHECK(p.autos[0].target == "fx:2:3:gain");
    CHECK(p.mixer[2].live->slotMix[3].get() == 0.75f && p.mixer[2].live->slotMix[0].get() == 1.0f);
    CHECK(identical(*mixed, *render(p)));

    // the follower a link listens to goes from slot 1 to slot 0: the link listens there
    CHECK(p.swapSlots(1, 1, 0));
    CHECK(p.links[0].track == 1 && p.links[0].slot == 0);
    CHECK(identical(*mixed, *render(p)));

    CHECK(!p.swapSlots(2, 0, kNumSlots) && !p.swapSlots(kNumMixerTracks, 0, 1) && !p.swapSlots(2, -1, 0));
    CHECK(p.swapSlots(2, 5, 5));

    // through save and load
    Project q;
    CHECK(q.fromJson(p.toJson()));
    CHECK(identical(*mixed, *render(q)));
}
