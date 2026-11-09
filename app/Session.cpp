#include "Session.h"
#include "Theme.h"
#include "pulp/DrumSynth.h"
#include "pulp/Multisample.h"
#include "pulp/Sampler.h"
#include "pulp/Pads.h"
#include "pulp/Creative.h"
#include "pulp/Sheaf.h"

// Drives the engine at real-time pace with no sound card.
class Session::NullDevice : public juce::Thread {
public:
    explicit NullDevice(Session& s) : juce::Thread("PULP null audio"), s_(s) {}
    void run() override {
        const int block = 480;
        std::vector<float> l((size_t)block), r((size_t)block), in0((size_t)block), in1((size_t)block, 0.0f);
        std::vector<float> cable((size_t)1 << 16, 0.0f);       // what went out, for the test loop
        size_t cableW = 0;
        float* out[2] = {l.data(), r.data()};
        const float* in[2] = {in0.data(), in1.data()};
        auto next = juce::Time::getMillisecondCounterHiRes();
        double phase = 0.0;
        while (!threadShouldExit()) {
            // optional test tone on input 1, so recording can be checked with no sound card and no sound
            const bool tone = s_.testInput;
            for (int i = 0; i < block; ++i) { in0[(size_t)i] = tone ? 0.25f * (float)std::sin(phase) : 0.0f; phase += pulp::kTwoPi * 440.0 / 48000.0; }
            // optional test loop: the output comes back on input 1 a fixed number of frames later, as through a cable
            const int loop = juce::jlimit(0, (int)cable.size() - block, s_.testLoopFrames);
            if (loop >= block)
                for (int i = 0; i < block; ++i) in0[(size_t)i] += 0.5f * cable[(cableW + (size_t)i + cable.size() - (size_t)loop) & (cable.size() - 1)];
            s_.engine.process(in, 2, out, 2, block);
            for (int i = 0; i < block; ++i) cable[(cableW + (size_t)i) & (cable.size() - 1)] = l[(size_t)i];
            cableW = (cableW + (size_t)block) & (cable.size() - 1);
            next += 1000.0 * block / 48000.0;
            const double waitMs = next - juce::Time::getMillisecondCounterHiRes();
            if (waitMs > 0) wait((int)waitMs + 1);
            else if (waitMs < -200) next = juce::Time::getMillisecondCounterHiRes();
        }
    }
private:
    Session& s_;
};

static pulp::uint32 theme_colorFor(int i) { return theme::palette(i).getARGB(); }

const std::vector<std::pair<const char*, int>>& Session::snapChoices() {
    static const std::vector<std::pair<const char*, int>> c = {
        {"Line", 0}, {"(none)", 1}, {"1/6 step", 40}, {"1/4 step", 60}, {"1/3 step", 80}, {"1/2 step", 120}, {"Step", 240},
        {"3/2 step", 360}, {"1/6 beat", 160}, {"1/4 beat", 240}, {"1/3 beat", 320}, {"1/2 beat", 480}, {"Beat", 960}, {"3/2 beat", 1440}, {"Bar", -1}};
    return c;
}

static pulp::ParamInfo snapInfo() {
    pulp::ParamInfo i;
    i.id = "snap"; i.name = "Snap";
    for (auto& c : Session::snapChoices()) i.choices.push_back(c.first);
    i.min = 0; i.max = (float)(i.choices.size() - 1); i.def = 0; i.steps = (int)i.choices.size();
    return i;
}

int Session::lineSnap(double pixelsPerTick, int ticksPerBar) {
    for (int t : {60, 120, 240, 480, 960})
        if (t * pixelsPerTick >= 9.0) return t;
    return ticksPerBar;
}

int Session::snapTicks(double pixelsPerTick) const {
    const int t = snapChoices()[(size_t)snap.getInt()].second;
    if (t == 0) return lineSnap(pixelsPerTick, project.ticksPerBar());
    if (t < 0) return project.ticksPerBar();
    return t;
}

void Session::liveNoteOn(int key, float velocity, bool sound, double atTick) {
    auto* ch = current();
    if (!ch || key < 0 || key > 127) return;
    if (sound) engine.noteOn(currentChannel, key, velocity);
    const double now = atTick >= 0.0 ? atTick : engine.positionTicks();
    if (recording && recOpt.replaceNotes && !stepRecording && engine.isPlaying() && !project.songMode) {
        // replace mode: the note about to be written must survive the wipe
        const int grid = inputSnap > 0 ? inputSnap : snapTicks();
        const int patLen = project.patternLengthTicks(project.pattern());
        int tick = grid > 1 ? (int)std::lround(now / grid) * grid : (int)now;
        if (tick >= patLen) tick -= patLen;
        replaceKept_.push_back({tick, key});
    }
    if (stepRecording) {
        // step mode: place the note at the cursor and move on by one snap unit
        const int len = juce::jmax(1, inputSnap > 0 ? inputSnap : snapTicks());
        pulp::Note n;
        n.tick = stepCursor; n.length = len; n.key = key; n.velocity = velocity;
        auto& notes = project.pattern().notesFor(ch->id);
        notes.push_back(n);
        pulp::sortNotes(notes);
        stepCursor += len;
        commit("Step-record note");
        return;
    }
    if (recording && engine.isPlaying())
        held_.push_back({key, now, velocity, ch->id});
}

void Session::liveNoteOff(int key, bool sound, double atTick) {
    if (sound) engine.noteOff(currentChannel, key);
    for (size_t i = 0; i < held_.size(); ++i) {
        if (held_[i].key != key) continue;
        const auto h = held_[i];
        held_.erase(held_.begin() + (long long)i);
        const int idx = project.channelIndex(h.channelId);
        if (idx < 0) return;
        if (project.songMode) {
            // song mode: the note goes into the pattern that plays at this place in the song (or a new one)
            const int grid = inputSnap > 0 ? inputSnap : snapTicks();
            const double endTick = atTick >= 0.0 ? atTick : engine.positionTicks();
            const double played = endTick > h.startTick ? endTick - h.startTick : 0.0;      // 0: the song looped while the key was down
            pulp::Note n;
            n.key = key;
            n.velocity = h.velocity;
            n.tick = grid > 1 ? (int)std::lround(h.startTick / grid) * grid : (int)h.startTick;
            n.length = juce::jmax(grid > 1 ? grid : 30, grid > 1 ? (int)std::lround(played / grid) * grid : (int)played);
            takePattern_ = project.recordSongNote(h.channelId, n, takePattern_);
            commit("Record note");
            return;
        }
        const int patLen = project.patternLengthTicks(project.pattern());
        double end = atTick >= 0.0 ? atTick : engine.positionTicks();
        if (end <= h.startTick) end += patLen;              // the pattern looped while the key was down
        const int grid = inputSnap > 0 ? inputSnap : snapTicks();
        pulp::Note n;
        n.key = key;
        n.velocity = h.velocity;
        // input quantize: starts snap to the grid, lengths to at least one grid unit
        n.tick = grid > 1 ? (int)std::lround(h.startTick / grid) * grid : (int)h.startTick;
        if (n.tick >= patLen) n.tick -= patLen;
        n.length = juce::jmax(grid > 1 ? grid : 30, grid > 1 ? (int)std::lround((end - h.startTick) / grid) * grid : (int)(end - h.startTick));
        auto& notes = project.pattern().notesFor(h.channelId);
        notes.push_back(n);
        pulp::sortNotes(notes);
        commit("Record note");
        return;
    }
}

Session::Session() : snap(snapInfo()) {
    pulp::SampleCache::global().setFactoryRoot(factoryDir().getFullPathName().toStdString());
}
Session::~Session() { shutdown(); }

juce::File Session::settingsFile() { return dataDir().getChildFile("settings.xml"); }

juce::File Session::factoryDir() {
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("factory");
}

void Session::start(bool noAudio) {
    engine.prepare(48000.0);
    project.prepare(48000.0);
    loadCalibrations();
    loadRecordOptions();
    startMidi(!noAudio);                // silent test runs take MIDI only from their script
    // spread instruments and independent mixer tracks over the cores (one is left for the interface)
    engine.setThreads(juce::jlimit(1, 16, juce::SystemStats::getNumCpus() - 1));
    publish();
    started_ = true;
    if (noAudio) {
        running_ = true;
        null_ = std::make_unique<NullDevice>(*this);
        null_->startThread(juce::Thread::Priority::high);
    } else {
        auto saved = loadSettings("DEVICESETUP");
        devices.addAudioCallback(this);
        // two inputs and two outputs are asked for, so a two-microphone interface's inputs are both available for recording
        devices.initialise(2, 2, saved.get(), true);
        if (!saved) chooseFirstRunDevice();
    }
    startTimerHz(20);
}

void Session::shutdown() {
    if (!started_) return;
    started_ = false;
    stopTimer();
    calibration_.reset();
    midiOut_.reset();
    stopMidi();
    if (recordThread_) finishAudioRecording();
    if (exportThread_) { exportCancel_ = true; exportThread_->stopThread(8000); exportThread_.reset(); }
    if (null_) { null_->stopThread(2000); null_.reset(); }
    else {
        devices.removeAudioCallback(this);
        devices.closeAudioDevice();
    }
    running_ = false;
}

bool Session::persistSettings = true;

std::unique_ptr<juce::XmlElement> Session::loadSettings(const juce::String& tag) {
    if (!settingsFile().existsAsFile()) return nullptr;
    if (auto root = juce::parseXML(settingsFile()))
        if (auto* e = root->getChildByName(tag)) return std::make_unique<juce::XmlElement>(*e);
    return nullptr;
}

void Session::saveSettings(std::unique_ptr<juce::XmlElement> section) {
    if ((!persistSettings && dataDirOverride == juce::File()) || !section) return;
    std::unique_ptr<juce::XmlElement> root;
    if (settingsFile().existsAsFile()) root = juce::parseXML(settingsFile());
    if (!root || !root->hasTagName("PULP")) root = std::make_unique<juce::XmlElement>("PULP");
    if (auto* old = root->getChildByName(section->getTagName())) root->removeChildElement(old, true);
    root->addChildElement(section.release());
    settingsFile().getParentDirectory().createDirectory();
    root->writeTo(settingsFile());
}

void Session::saveDeviceSettings() {
    if (null_) return;
    saveSettings(devices.createStateXml());
}

void Session::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    const double sr = device->getCurrentSampleRate();
    deviceRate_ = sr;
    engine.prepare(sr);
    project.prepare(sr);
    running_ = true;
}

void Session::audioDeviceStopped() { running_ = false; }

void Session::audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                               int numSamples, const juce::AudioIODeviceCallbackContext&) {
    juce::ScopedNoDenormals noDenormals;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    engine.process(in, numIn, out, numOut, numSamples);
    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
    const double budget = 1000.0 * numSamples / deviceRate_;
    engine.reportCallbackLoad((float)(ms / budget));
    if (ms > budget) xruns_.fetch_add(1);
}

void Session::seek(double tick) {
    project.prefetchAudio(tick);
    engine.seek(tick);
}

void Session::timerCallback() {
    engine.collectGarbage();
    pumpEditors();
    // knob movements recorded while playing: hardware controls are sampled here, and when the transport
    // has stopped by itself the movements are written into the project
    if (recording && engine.isPlaying()) {
        for (auto& l : project.links) {
            if (l.kind != pulp::ParamLink::Midi) continue;
            pulp::Param* p = project.findParam(l.target);
            if (!p) continue;
            const float v = p->getNorm();
            auto seen = midiLinkSeen_.find(p);
            if (seen == midiLinkSeen_.end()) midiLinkSeen_[p] = v;
            else if (std::fabs(seen->second - v) > 1.0e-4f) { seen->second = v; recordParamValue(p); }
        }
    } else if (!autoRec_.empty() && !engine.isPlaying()) finishAutomationRecording();
    // audio clips that follow the tempo are fitted again once a tempo change has settled
    const float tempo = project.global->tempo.get();
    if (tempo != compiledTempo_) {
        bool synced = false;
        for (auto& c : project.clips) synced = synced || (c.type == pulp::Clip::AudioClip && c.sync);
        if (!synced) compiledTempo_ = tempo;
        else if (tempo != seenTempo_) { seenTempo_ = tempo; tempoStable_ = 0; }
        else if (++tempoStable_ >= 3) publish();
    }
    // note recording in replace mode: wipe the notes the playhead passes, except the ones just played
    if (recording && recOpt.replaceNotes && engine.isPlaying() && !project.songMode && !recordThread_) {
        if (auto* ch = current()) {
            const double pos = engine.positionTicks();
            if (replaceLast_ >= 0.0 && pos != replaceLast_) {
                auto& notes = project.pattern().notesFor(ch->id);
                const size_t before = notes.size();
                auto passed = [&](int tick) { return pos > replaceLast_ ? (tick >= replaceLast_ && tick < pos) : (tick >= replaceLast_ || tick < pos); };
                notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const pulp::Note& n) {
                    if (!passed(n.tick)) return false;
                    for (auto& k : replaceKept_) if (k.first == n.tick && k.second == n.key) return false;
                    return true;
                }), notes.end());
                if (notes.size() != before) commit("Replace notes");
            }
            replaceLast_ = pos;
        }
    } else { replaceLast_ = -1.0; replaceKept_.clear(); }
    if (!recording || !engine.isPlaying()) takePattern_ = 0;        // the next recording in song mode starts a take of its own
    // autosave while there are unsaved changes
    const auto now = juce::Time::getMillisecondCounter();
    if (dirtySinceAutosave_ && autosaveSeconds > 0 && now - lastAutosave_ > (juce::uint32)autosaveSeconds * 1000u) autosaveNow();
}

void Session::publish() {
    compiledTempo_ = seenTempo_ = project.global->tempo.get();
    tempoStable_ = 0;
    engine.setState(project.compile());
    refreshMidiOuts();
}

void Session::commit(const juce::String& what) {
    publish();
    // every finished edit becomes a step in the undo history
    if (historyPos + 1 < (int)history.size()) history.erase(history.begin() + historyPos + 1, history.end());
    history.push_back({what, project.toJson().dump(0)});
    size_t bytes = 0;
    for (auto& h : history) bytes += h.json.size();
    while (history.size() > 2 && (history.size() > 300 || bytes > (size_t)96 * 1024 * 1024)) { bytes -= history.front().json.size(); history.erase(history.begin()); }
    historyPos = (int)history.size() - 1;
    lastWasUndo_ = false;
    if (!dirty) lastAutosave_ = juce::Time::getMillisecondCounter();
    dirty = true;
    dirtySinceAutosave_ = true;
    notify();
}

void Session::notify() {
    currentChannel = juce::jlimit(0, juce::jmax(0, (int)project.channels.size() - 1), currentChannel);
    listeners_.call([](Listener& l) { l.projectChanged(); });
}

// ------------------------------------------------------------------ channels

// The audio files PULP reads: WAV, FLAC and AIFF always; OGG and MP3 when their codecs are built in.
juce::String Session::audioFileEndings() {
    juce::String e = "wav;flac;aif;aiff";
   #if __has_include("pulp/Ogg.h")
    e << ";ogg";
   #endif
   #if __has_include("pulp/Mp3.h")
    e << ";mp3";
   #endif
    return e;
}
juce::String Session::audioFilePattern() { return "*." + audioFileEndings().replace(";", ";*."); }
bool Session::isAudioFile(const juce::File& f) { return f.hasFileExtension(audioFileEndings()); }
bool Session::isInstrumentFile(const juce::File& f) { return f.hasFileExtension("pulpinst;sfz"); }

int Session::addInstrumentFileChannel(const juce::File& file) {
    auto t = std::make_shared<pulp::Timbre>();
    std::string err;
    if (!t->loadFile(projectPath(file), project.projectDir, &err)) { message("Could not load " + file.getFileName() + ": " + juce::String(err)); return -1; }
    const auto inst = t->instrument();
    auto& c = project.addChannel(t, inst && !inst->name.empty() ? inst->name : file.getFileNameWithoutExtension().toStdString());
    c.color = theme_colorFor((int)project.channels.size() - 1);
    currentChannel = (int)project.channels.size() - 1;
    selectedChannels = {c.id};
    commit("Add instrument");
    return currentChannel;
}

int Session::buildInstrumentChannel(const juce::StringArray& audioFiles, int index) {
    std::vector<std::string> paths;
    for (auto& f : audioFiles) if (isAudioFile(juce::File(f))) paths.push_back(projectPath(juce::File(f)));
    if (paths.empty()) return -1;
    auto* existing = channel(index);
    auto* onto = existing ? dynamic_cast<pulp::Timbre*>(existing->live->plugin.get()) : nullptr;
    std::shared_ptr<pulp::Timbre> made;
    if (!onto) made = std::make_shared<pulp::Timbre>();
    pulp::Timbre* t = onto ? onto : made.get();
    pulp::ms::BuildReport report;
    std::string err;
    if (!t->buildFromFiles(paths, project.projectDir, &report, &err)) { message("Could not build an instrument: " + juce::String(err)); return -1; }
    lastBuildReport = juce::String(report.text());
    if (onto) { existing->name = t->instrument()->name; commit("Build instrument from samples"); return index; }
    auto& c = project.addChannel(made, t->instrument()->name);
    c.color = theme_colorFor((int)project.channels.size() - 1);
    currentChannel = (int)project.channels.size() - 1;
    selectedChannels = {c.id};
    commit("Build instrument from samples");
    return currentChannel;
}

std::string Session::projectPath(const juce::File& file) {
    const auto fac = factoryDir();
    if (file.isAChildOf(fac)) return ("factory:" + file.getRelativePathFrom(fac).replaceCharacter('\\', '/')).toStdString();
    return file.getFullPathName().toStdString();
}

juce::File Session::resolvePath(const std::string& path) const {
    return juce::File(juce::String(pulp::SampleCache::global().resolve(path, project.projectDir)));
}

void Session::selectChannel(int index, bool addToSelection) {
    if (!channel(index)) return;
    currentChannel = index;
    if (!addToSelection) selectedChannels.clear();
    selectedChannels.insert(project.channels[(size_t)index].id);
    notify();
}

int Session::useInstrument(const std::string& typeId, int preset) {
    auto* ch = current();
    if (preset >= 0 && ch && typeId == ch->live->plugin->typeId()) {
        const auto presets = ch->live->plugin->factoryPresets();
        if (preset < (int)presets.size()) { ch->live->plugin->loadState(presets[(size_t)preset].state, project.projectDir); commit("Choose preset"); }
        return currentChannel;
    }
    return addInstrumentChannel(typeId, preset);
}

int Session::addInstrumentChannel(const std::string& typeId, int preset) {
    auto plug = pulp::PluginRegistry::global().create(typeId);
    if (!plug) return -1;
    if (preset >= 0) { const auto presets = plug->factoryPresets(); if (preset < (int)presets.size()) plug->loadState(presets[(size_t)preset].state, project.projectDir); }
    const auto* type = pulp::PluginRegistry::global().find(typeId);
    // a pad sampler arrives with a kit in it, so it makes a sound straight away
    if (typeId == "pads" && preset < 0) { const auto kits = plug->factoryPresets(); if (!kits.empty()) plug->loadState(kits[0].state, project.projectDir); }
    auto& c = project.addChannel(plug, type ? type->name : std::string(plug->displayName()));
    c.color = theme_colorFor((int)project.channels.size() - 1);
    currentChannel = (int)project.channels.size() - 1;
    selectedChannels = {c.id};
    commit("Add channel");
    return currentChannel;
}

int Session::addSampleChannel(const juce::File& file) {
    std::string err;
    auto sample = pulp::SampleCache::global().load(projectPath(file), project.projectDir, &err);
    if (!sample) { message("Could not load " + file.getFileName() + ": " + juce::String(err)); return -1; }
    auto smp = std::make_shared<pulp::Sampler>();
    smp->setSample(sample);
    auto& c = project.addChannel(smp, file.getFileNameWithoutExtension().toStdString());
    c.color = theme_colorFor((int)project.channels.size() - 1);
    currentChannel = (int)project.channels.size() - 1;
    selectedChannels = {c.id};
    commit("Add sample channel");
    return currentChannel;
}

bool Session::setChannelSample(int index, const juce::File& file) {
    auto* c = channel(index);
    if (!c) return false;
    // a multisample instrument is rebuilt from the file; a sampler just takes it
    if (dynamic_cast<pulp::Timbre*>(c->live->plugin.get())) return buildInstrumentChannel({file.getFullPathName()}, index) >= 0;
    if (auto* chop = dynamic_cast<pulp::Chop*>(c->live->plugin.get())) {
        std::string err;
        auto loop = pulp::SampleCache::global().load(projectPath(file), project.projectDir, &err);
        if (!loop) { message("Could not load " + file.getFileName() + ": " + juce::String(err)); return false; }
        chop->setSample(loop);
        c->name = file.getFileNameWithoutExtension().toStdString();
        commit("Load loop");
        return true;
    }
    if (dynamic_cast<pulp::PadKit*>(c->live->plugin.get())) return setPadSample(index, 0, file, false);
    if (auto* motes = dynamic_cast<pulp::Motes*>(c->live->plugin.get())) {
        std::string err;
        auto sound = pulp::SampleCache::global().load(projectPath(file), project.projectDir, &err);
        if (!sound) { message("Could not load " + file.getFileName() + ": " + juce::String(err)); return false; }
        motes->setSample(sound);
        c->name = file.getFileNameWithoutExtension().toStdString();
        commit("Load sound");
        return true;
    }
    if (auto* sheaf = dynamic_cast<pulp::Sheaf*>(c->live->plugin.get())) {
        // a recording of one note: its harmonics are measured and become the "User" spectrum
        std::string err;
        auto sound = pulp::SampleCache::global().load(projectPath(file), project.projectDir, &err);
        if (!sound) { message("Could not load " + file.getFileName() + ": " + juce::String(err)); return true; }
        if (!sheaf->analyse(sound, &err)) { message("Sheaf could not make a sound of " + file.getFileName() + ": " + juce::String(err)); return true; }
        // spectrum A plays what was analysed, from its start, at its own speed; a sound that sustains is looped
        sheaf->spec[0].shape->set((float)pulp::Sheaf::User);
        sheaf->pUserLoop->set(sheaf->analysisHolds() ? 1.0f : 0.0f);
        sheaf->pUserStart->set(0.0f);
        sheaf->pUserSpeed->set(1.0f);
        c->name = file.getFileNameWithoutExtension().toStdString();
        commit("Analyse sound");
        return true;
    }
    auto* smp = dynamic_cast<pulp::Sampler*>(c->live->plugin.get());
    if (!smp) return false;
    std::string err;
    auto sample = pulp::SampleCache::global().load(projectPath(file), project.projectDir, &err);
    if (!sample) { message("Could not load " + file.getFileName() + ": " + juce::String(err)); return false; }
    smp->setSample(sample);
    c->name = file.getFileNameWithoutExtension().toStdString();
    commit("Replace sample");
    return true;
}

void Session::replaceChannelInstrument(int index, const std::string& typeId) {
    auto* c = channel(index);
    auto plug = pulp::PluginRegistry::global().create(typeId);
    if (!c || !plug) return;
    plug->prepare(project.sampleRate);
    auto live = std::make_shared<pulp::ChannelLive>(plug);
    auto oldParams = c->live->stripParams();
    auto newParams = live->stripParams();
    for (size_t i = 0; i < newParams.size(); ++i) newParams[i]->set(oldParams[i]->get());
    c->live = live;
    c->name = plug->displayName();
    commit("Replace instrument");
}

void Session::deleteChannel(int index) {
    if (!channel(index)) return;
    selectedChannels.erase(project.channels[(size_t)index].id);
    project.removeChannel(index);
    currentChannel = juce::jlimit(0, juce::jmax(0, (int)project.channels.size() - 1), currentChannel);
    commit("Delete channel");
}

void Session::cloneChannel(int index) {
    auto* src = channel(index);
    if (!src) return;
    const auto state = src->live->plugin->saveState();
    auto plug = pulp::PluginRegistry::global().create(src->live->plugin->typeId());
    if (!plug) return;
    plug->prepare(project.sampleRate);
    plug->loadState(state, project.projectDir);
    // Generated samples have no path to reload from: share the data directly.
    if (auto* a = dynamic_cast<pulp::Sampler*>(src->live->plugin.get()))
        if (auto* b = dynamic_cast<pulp::Sampler*>(plug.get())) b->setSample(a->sample());
    const int srcId = src->id;
    const auto name = src->name;
    const auto color = src->color;
    const auto group = src->group;
    const int track = src->mixerTrack;
    auto oldParams = src->live->stripParams();
    auto& c = project.addChannel(plug, name + " #2");
    auto newParams = c.live->stripParams();
    for (size_t i = 0; i < newParams.size(); ++i) newParams[i]->set(oldParams[i]->get());
    c.color = color;
    c.group = group;
    c.mixerTrack = track;
    const int newId = c.id;
    for (auto& p : project.patterns)
        if (auto* n = p.find(srcId)) { auto copy = *n; p.notes[newId] = std::move(copy); }
    // put the clone right under its source
    auto moved = std::move(project.channels.back());
    project.channels.pop_back();
    project.channels.insert(project.channels.begin() + index + 1, std::move(moved));
    currentChannel = index + 1;
    selectedChannels = {newId};
    commit("Clone channel");
}

void Session::moveChannel(int index, int delta) {
    const int to = index + delta;
    if (!channel(index) || !channel(to)) return;
    std::swap(project.channels[(size_t)index], project.channels[(size_t)to]);
    if (currentChannel == index) currentChannel = to;
    else if (currentChannel == to) currentChannel = index;
    commit("Move channel");
}

// ------------------------------------------------------------------ patterns

void Session::selectPattern(int index) {
    index = juce::jlimit(0, (int)project.patterns.size() - 1, index);
    if (index == project.currentPattern) return;
    project.currentPattern = index;
    publish();
    notify();
}

void Session::setTimeSignature(int beats, int unit) {
    project.timeSigNum = juce::jlimit(1, 16, beats);
    project.timeSigDen = unit == 8 ? 8 : (unit == 2 ? 2 : (unit == 16 ? 16 : 4));
    commit("Time signature");
}

void Session::newPattern() {
    // F4: the next empty pattern, or a new one after the last
    for (int i = project.currentPattern + 1; i < (int)project.patterns.size(); ++i)
        if (project.patterns[(size_t)i].empty()) { selectPattern(i); return; }
    project.addPattern();
    project.currentPattern = (int)project.patterns.size() - 1;
    commit("New pattern");
}

void Session::clonePattern() {
    pulp::Pattern copy = project.pattern();
    const auto name = copy.name;
    auto& p = project.addPattern(name + " #2");
    const int id = p.id;
    p = copy;
    p.id = id;
    p.name = name + " #2";
    auto moved = std::move(project.patterns.back());
    project.patterns.pop_back();
    project.patterns.insert(project.patterns.begin() + project.currentPattern + 1, std::move(moved));
    project.currentPattern += 1;
    commit("Clone pattern");
}

void Session::deletePattern() {
    if (project.patterns.size() <= 1) {
        project.patterns[0].notes.clear();
        commit("Clear pattern");
        return;
    }
    const int id = project.pattern().id;
    project.clips.erase(std::remove_if(project.clips.begin(), project.clips.end(),
                                       [&](const pulp::Clip& c) { return c.type == pulp::Clip::PatternClip && c.patternId == id; }),
                        project.clips.end());
    project.patterns.erase(project.patterns.begin() + project.currentPattern);
    project.currentPattern = juce::jlimit(0, (int)project.patterns.size() - 1, project.currentPattern);
    commit("Delete pattern");
}

// ------------------------------------------------------------------ transport

void Session::togglePlay() {
    if (engine.isPlaying() || engine.waitingForInput()) { stop(); return; }
    // record armed and a mixer track has an input: this play is a take
    if (recording && anyInputArmed()) {
        if (!project.songMode) setSongMode(true);       // takes are recorded into the playlist
        beginAudioRecording();
    }
    // notes recorded in song mode with no loop region: the song runs on past its end, as it does for a take
    if (recording && project.songMode && !recordThread_ && project.loopEnd <= project.loopStart) { project.recordingLinear = true; publish(); }
    if (project.songMode) project.prefetchAudio(engine.positionTicks());
    // "wait for input": armed for recording, the transport holds still until sound arrives or a note is played
    const bool wait = recording && recOpt.waitForInput;
    engine.play(wait ? 0.0 : (double)countInBars * project.ticksPerBar(), wait);
}
void Session::stop() {
    engine.stop();
    finishAutomationRecording();
    if (recordThread_) {
        // give the audio thread a moment to see the stop, then turn the take into a clip
        juce::Thread::sleep(30);
        finishAudioRecording();
    }
    if (project.recordingLinear) { project.recordingLinear = false; publish(); }
}

void Session::setSongMode(bool song) {
    if (project.songMode == song) return;
    project.songMode = song;
    commit("Song mode");
}

void Session::setTempo(double bpm) {
    project.global->tempo.set((float)juce::jlimit(10.0, 522.0, bpm));
    notify();
}

void Session::tapTempo() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (!taps_.isEmpty() && now - taps_.getLast() > 2000.0) taps_.clear();
    taps_.add(now);
    while (taps_.size() > 8) taps_.remove(0);
    if (taps_.size() >= 2) {
        const double avg = (taps_.getLast() - taps_.getFirst()) / (taps_.size() - 1);
        if (avg > 1.0) setTempo(std::round(60000.0 / avg));
    }
}

bool Session::previewFile(const juce::File& file) {
    if (!isAudioFile(file)) return false;
    auto sample = pulp::SampleCache::global().load(projectPath(file), project.projectDir);
    if (!sample) return false;
    engine.preview.play(sample, 0.7f);
    return true;
}

// ------------------------------------------------------------------ device

juce::String Session::deviceSummary() const {
    if (null_) return "No audio device (silent test mode)";
    if (auto* d = devices.getCurrentAudioDevice())
        return d->getTypeName() + ": " + d->getName() + ", " + juce::String(d->getCurrentSampleRate(), 0) + " Hz, "
             + juce::String(d->getCurrentBufferSizeSamples()) + " samples";
    return "No audio device open";
}

// First run: use the ASIO driver that belongs to the Windows default output device when
// there is one (lowest latency), otherwise Windows Audio on the default device.
void Session::chooseFirstRunDevice() {
    juce::String defaultOut;
    juce::AudioIODeviceType* asio = nullptr;
    for (auto* t : devices.getAvailableDeviceTypes()) {
        t->scanForDevices();
        if (t->getTypeName() == "Windows Audio") {
            const auto names = t->getDeviceNames(false);
            defaultOut = names[juce::jmax(0, t->getDefaultDeviceIndex(false))];
        }
        if (t->getTypeName() == "ASIO") asio = t;
    }
    if (asio != nullptr && defaultOut.isNotEmpty()) {
        for (auto& name : asio->getDeviceNames(false)) {
            auto words = juce::StringArray::fromTokens(name, " ", "");
            words.removeString("ASIO", true);
            bool all = !words.isEmpty();
            for (auto& w : words) all = all && defaultOut.containsIgnoreCase(w);
            if (all && openDevice("ASIO", name).isEmpty()) return;
        }
        // the ASIO driver did not open: go back to Windows Audio on the default device
        openDevice("Windows Audio", {});
    }
}

juce::String Session::openDevice(const juce::String& type, const juce::String& name, double sampleRate, int bufferSize) {
    if (null_) return "running without an audio device";
    juce::AudioIODeviceType* found = nullptr;
    for (auto* t : devices.getAvailableDeviceTypes())
        if (t->getTypeName().equalsIgnoreCase(type)) found = t;
    if (!found) return "no such driver type: " + type;
    found->scanForDevices();
    devices.setCurrentAudioDeviceType(found->getTypeName(), true);
    auto setup = devices.getAudioDeviceSetup();
    const auto outs = found->getDeviceNames(false), ins = found->getDeviceNames(true);
    setup.outputDeviceName = name.isNotEmpty() ? name : outs[juce::jmax(0, found->getDefaultDeviceIndex(false))];
    if (found->hasSeparateInputsAndOutputs()) setup.inputDeviceName = ins[juce::jmax(0, found->getDefaultDeviceIndex(true))];
    else setup.inputDeviceName = setup.outputDeviceName;
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;
    setup.sampleRate = sampleRate;
    setup.bufferSize = bufferSize;
    return devices.setAudioDeviceSetup(setup, true);
}

juce::String Session::describeDevices() {
    juce::String s;
    for (auto* t : devices.getAvailableDeviceTypes()) {
        t->scanForDevices();
        s << t->getTypeName() << "\n";
        for (auto& n : t->getDeviceNames(false)) s << "  out: " << n << "\n";
        for (auto& n : t->getDeviceNames(true)) s << "  in:  " << n << "\n";
    }
    s << "current: " << deviceSummary() << "\n";
    return s;
}

// ------------------------------------------------------------------ default project

void Session::newDefaultProject() {
    pulp::SampleCache::global().setFactoryRoot(factoryDir().getFullPathName().toStdString());
    project = pulp::Project();
    // The reference program's empty project: kick, clap, hat and snare on an empty pattern.
    const char* files[4][2] = {{"808 Kick", "Kick"}, {"808 Clap", "Clap"}, {"808 Hat Closed", "Hat"}, {"808 Snare", "Snare"}};
    const auto kit = factoryDir().getChildFile("Drums").getChildFile("808 Kit");
    for (int i = 0; i < 4; ++i) {
        const auto f = kit.getChildFile(juce::String(files[i][0]) + ".wav");
        auto smp = std::make_shared<pulp::Sampler>();
        if (f.existsAsFile()) smp->setSample(pulp::SampleCache::global().load(projectPath(f)));
        auto& c = project.addChannel(smp, files[i][1]);
        c.color = theme_colorFor(i);
    }
    // like the reference program's starting template: a limiter on the master so a loud kit cannot clip
    project.setSlot(pulp::kMasterTrack, 0, pulp::PluginRegistry::global().create("limiter"));
    currentChannel = 0;
    selectedChannels = {project.channels[0].id};
    projectFile = juce::File();
    resetHistory("New project");
}
