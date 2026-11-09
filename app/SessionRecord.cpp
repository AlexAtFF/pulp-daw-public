// Session: recording audio. The engine keeps the hardware input in a ring; a background
// thread drains it into WAV files as the take happens, so a crash does not lose it. When the
// transport stops, each file becomes audio clips placed where they were performed: one clip,
// or one per loop pass (takes) when a loop region was recorded over.
#include "Session.h"
#include "pulp/Editor.h"

class Session::RecordThread : public juce::Thread {
public:
    struct Take {
        int mixerTrack = 0;
        int mode = 1;                   // 1 = input 1 mono, 2 = input 2 mono, 3 = stereo
        juce::File file;
        pulp::WavStreamWriter writer;
    };
    RecordThread(Session& s) : juce::Thread("PULP recorder"), s_(s) {}
    std::vector<std::unique_ptr<Take>> takes;

    void run() override {
        std::vector<float> buf((size_t)4096 * 2), mono((size_t)4096);
        juce::uint32 lastFlush = juce::Time::getMillisecondCounter();
        for (;;) {
            const bool stopping = threadShouldExit();
            int got;
            while ((got = s_.engine.recordRead(buf.data(), 4096)) > 0) {
                for (auto& t : takes) {
                    if (t->mode == 3) t->writer.write(buf.data(), got);
                    else {
                        const int ch = t->mode == 2 ? 1 : 0;
                        for (int i = 0; i < got; ++i) mono[(size_t)i] = buf[(size_t)i * 2 + (size_t)ch];
                        t->writer.write(mono.data(), got);
                    }
                }
            }
            if (stopping) break;
            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastFlush > 500) { for (auto& t : takes) t->writer.flush(); lastFlush = now; }
            wait(10);
        }
        for (auto& t : takes) t->writer.close();
    }
private:
    Session& s_;
};

// Polls the engine while the calibration bursts play, then measures.
class Session::CalibrationPoll : public juce::Timer {
public:
    CalibrationPoll(Session& s, std::function<void(int, float)> done) : s_(s), done_(std::move(done)) { startTimer(50); }
    void timerCallback() override {
        std::vector<float> chunk((size_t)8192 * 2);
        for (int got; (got = s_.engine.recordRead(chunk.data(), 8192)) > 0;)
            for (int i = 0; i < got; ++i) { rec_[0].push_back(chunk[(size_t)i * 2]); rec_[1].push_back(chunk[(size_t)i * 2 + 1]); }
        if (s_.engine.calibrating() && ++ticks_ < 80) return;
        stopTimer();
        // whichever input the cable (or microphone) is on
        int best = -1;
        float bestQ = 0.0f;
        for (int c = 0; c < 2; ++c) {
            float q = 0.0f;
            const int lag = pulp::Engine::measureRoundTrip(rec_[c].data(), (int)rec_[c].size(), s_.engine.sampleRate(), &q);
            if (lag >= 0 && q > bestQ) { best = lag; bestQ = q; }
            else if (best < 0 && q > bestQ) bestQ = q;
        }
        auto done = std::move(done_);
        auto& s = s_;
        if (best >= 0) s.storeCalibration(best);
        s.calibration_.reset();         // deletes this
        if (done) done(best, bestQ);
    }
private:
    Session& s_;
    std::function<void(int, float)> done_;
    std::vector<float> rec_[2];
    int ticks_ = 0;
};

bool Session::anyInputArmed() const {
    for (int t = 0; t <= pulp::kNumInserts; ++t) if (project.mixer[(size_t)t].input != 0) return true;
    return false;
}

juce::File Session::recordingsDir() const {
    if (projectFile != juce::File()) return projectFile.getParentDirectory().getChildFile("recordings");
    return dataDir().getChildFile("recordings");
}

// ------------------------------------------------------------------ latency

juce::String Session::calibrationKey() const {
    if (null_) return "null|" + juce::String(testLoopFrames);
    if (auto* d = devices.getCurrentAudioDevice()) {
        const auto setup = devices.getAudioDeviceSetup();
        return d->getTypeName() + "|" + setup.outputDeviceName + "|" + setup.inputDeviceName + "|" + juce::String(d->getCurrentSampleRate(), 0) + "|" + juce::String(d->getCurrentBufferSizeSamples());
    }
    return {};
}

int Session::calibratedLatency() const {
    const auto it = calibrations_.find(calibrationKey());
    return it == calibrations_.end() ? -1 : it->second;
}

int Session::recordingLatencyFrames() const {
    // a measured round trip already holds everything, the engine's own input delay included
    if (const int measured = calibratedLatency(); measured >= 0) return measured + recordOffsetFrames;
    int frames = pulp::Engine::kInputDelay;
    if (!null_)
        if (auto* d = devices.getCurrentAudioDevice()) frames += d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();
    return frames + recordOffsetFrames;
}

void Session::loadCalibrations() {
    calibrations_.clear();
    if (auto x = loadSettings("CALIBRATION"))
        for (auto* e : x->getChildWithTagNameIterator("DEVICE")) calibrations_[e->getStringAttribute("key")] = e->getIntAttribute("frames", -1);
}

void Session::storeCalibration(int frames) {
    const auto key = calibrationKey();
    if (key.isEmpty()) return;
    if (frames < 0) calibrations_.erase(key);
    else calibrations_[key] = frames;
    auto x = std::make_unique<juce::XmlElement>("CALIBRATION");
    for (auto& kv : calibrations_) {
        auto* e = x->createNewChildElement("DEVICE");
        e->setAttribute("key", kv.first);
        e->setAttribute("frames", kv.second);
    }
    saveSettings(std::move(x));
    notify();
}

bool Session::calibrate(std::function<void(int, float)> done) {
    if (calibration_ || recordThread_ || engine.isPlaying()) return false;
    engine.startCalibration();
    calibration_ = std::make_unique<CalibrationPoll>(*this, std::move(done));
    return true;
}

// ------------------------------------------------------------------ record options

void Session::loadRecordOptions() {
    if (auto x = loadSettings("RECORD")) {
        recOpt.punch = x->getBoolAttribute("punch", false);
        recOpt.loopTakes = x->getBoolAttribute("loopTakes", true);
        recOpt.waitForInput = x->getBoolAttribute("waitForInput", false);
        recOpt.replaceNotes = x->getBoolAttribute("replaceNotes", false);
        engine.setMetronomeSound(x->getIntAttribute("click", 0));
    }
}

void Session::saveRecordOptions() {
    auto x = std::make_unique<juce::XmlElement>("RECORD");
    x->setAttribute("punch", recOpt.punch);
    x->setAttribute("loopTakes", recOpt.loopTakes);
    x->setAttribute("waitForInput", recOpt.waitForInput);
    x->setAttribute("replaceNotes", recOpt.replaceNotes);
    x->setAttribute("click", engine.metronomeSound());
    saveSettings(std::move(x));
    notify();
}

// ------------------------------------------------------------------ takes

bool Session::beginAudioRecording() {
    if (recordThread_ || !anyInputArmed()) return false;
    auto thread = std::make_unique<RecordThread>(*this);
    const auto dir = recordingsDir();
    dir.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
    for (int t = 0; t <= pulp::kNumInserts; ++t) {
        const auto& tr = project.mixer[(size_t)t];
        if (tr.input == 0) continue;
        auto take = std::make_unique<RecordThread::Take>();
        take->mixerTrack = t;
        take->mode = tr.input;
        const auto base = juce::File::createLegalFileName(tr.name.empty() ? juce::String(pulp::Project::mixerTrackName(t)) : juce::String(tr.name));
        take->file = dir.getNonexistentChildFile(base + "_" + stamp, ".wav", false);
        // the dry signal is recorded, 32-bit float, at the device's rate
        if (!take->writer.open(take->file.getFullPathName().toStdString(), engine.sampleRate(), take->mode == 3 ? 2 : 1)) {
            message("Could not create " + take->file.getFullPathName());
            return false;
        }
        thread->takes.push_back(std::move(take));
    }
    // How the transport behaves during the take. With a loop region and "loop recording" on, the region
    // loops and every pass is a take. Otherwise the song runs straight on, past its end if need be.
    const bool region = project.loopEnd > project.loopStart;
    recPunch_ = recOpt.punch && region;
    recPunchIn_ = project.loopStart;
    recPunchOut_ = project.loopEnd;
    project.recordingLinear = !(region && recOpt.loopTakes);
    if (recPunch_) {
        // the end of what is sung inside the range arrives late by the round trip, so the input stays open that much longer
        const double spt = 60.0 * engine.sampleRate() / ((double)project.global->tempo.get() * pulp::kPPQ);
        engine.setPunch((double)recPunchIn_, (double)recPunchOut_ + (recordingLatencyFrames() + 2 * pulp::kQuantum) / spt);
    } else engine.setPunch(0.0, 0.0);
    publish();
    recordThread_ = std::move(thread);
    engine.startRecording();
    recordThread_->startThread(juce::Thread::Priority::normal);
    return true;
}

void Session::finishAudioRecording() {
    if (!recordThread_) return;
    engine.stopRecording();
    recordThread_->stopThread(4000);
    auto* thread = static_cast<RecordThread*>(recordThread_.get());
    std::vector<pulp::RecSegment> segments;
    for (pulp::RecSegment s; engine.recordSegment(s);) segments.push_back(s);
    const double samplesPerTick = 60.0 * engine.sampleRate() / ((double)project.global->tempo.get() * pulp::kPPQ);
    const int latency = recordingLatencyFrames();
    int maxGroup = 0;
    for (auto& c : project.clips) maxGroup = juce::jmax(maxGroup, c.takeGroup);
    int placed = 0, takesMade = 0;
    for (auto& take : thread->takes) {
        const auto frames = take->writer.frames();
        const auto pieces = pulp::planTakes(segments, frames, latency, samplesPerTick, recPunch_ ? (double)recPunchIn_ : 0.0, recPunch_ ? (double)recPunchOut_ : 0.0);
        if (pieces.empty()) { take->file.deleteFile(); continue; }       // nothing worth keeping
        std::string err;
        const int id = project.addAudio(projectPath(take->file), &err);
        if (id == 0) { message("The take was saved but could not be loaded: " + take->file.getFullPathName()); continue; }
        // the first run of empty playlist tracks that holds every pass
        const int need = (int)pieces.size();
        int track = 0;
        for (; track + need < pulp::kNumPlaylistTracks; ++track) {
            bool used = false;
            for (auto& c : project.clips) if (c.track >= track && c.track < track + need) { used = true; break; }
            if (!used) break;
        }
        const int group = need > 1 ? ++maxGroup : 0;
        // the take that plays is the last one that ran the whole way; the others wait, muted, to be auditioned
        pulp::int64 longest = 0;
        for (auto& p : pieces) longest = juce::jmax(longest, p.lengthTicks);
        int active = need - 1;
        while (active > 0 && pieces[(size_t)active].lengthTicks < longest - pulp::kTicksPerStep) --active;
        for (int k = 0; k < need; ++k) {
            const auto& piece = pieces[(size_t)k];
            auto& clip = project.addAudioClip(id, track + k, piece.startTick, take->mixerTrack);
            // skip the head of the file that is only latency, so the take sits where it was performed
            clip.sourceOffsetFrames = piece.sourceOffsetFrames;
            clip.length = piece.lengthTicks;
            clip.takeGroup = group;
            clip.take = group != 0 ? k + 1 : 0;
            clip.muted = group != 0 && k != active;
            auto& name = project.tracks[(size_t)(track + k)].name;
            if (name.empty()) name = need > 1 ? "Take " + std::to_string(k + 1) : "Recording";
            ++placed;
        }
        if (need > 1) takesMade = need;
    }
    const auto dropped = engine.recordDropped();
    recordThread_.reset();
    project.recordingLinear = false;
    engine.setPunch(0.0, 0.0);
    if (placed > 0) commit(takesMade > 1 ? "Record " + juce::String(takesMade) + " takes" : "Record audio");
    else publish();
    if (dropped > 0) message("The disk could not keep up: " + juce::String(dropped) + " samples of the take were lost.");
}

// ------------------------------------------------------------------ audio files into the playlist

int Session::addAudioFile(const juce::File& f, int track, pulp::int64 tick) {
    std::string err;
    const int id = project.addAudio(projectPath(f), &err);
    if (id == 0) { message("Could not load " + f.getFileName() + ": " + juce::String(err)); return -1; }
    for (auto& c : project.clips) c.selected = false;
    project.addAudioClip(id, track, tick).selected = true;
    const int clipIndex = (int)project.clips.size() - 1;
    // Listen for a tempo. A loop that is not at the project's tempo can be fitted to it.
    const auto* item = project.audioById(id);
    double bpm = item ? item->bpm : 0.0;
    if (item && bpm <= 0.0 && item->seconds() >= 1.5 && item->seconds() <= 600.0) bpm = project.detectAudioTempo(id);
    commit("Add audio clip");
    const double tempo = project.global->tempo.get();
    if (bpm > 0.0 && std::fabs(bpm - tempo) > 0.5 && onAskStretch) {
        const int clipAudio = id;
        onAskStretch(f.getFileName(), bpm, [this, clipIndex, clipAudio](bool yes) {
            if (!yes || clipIndex >= (int)project.clips.size() || project.clips[(size_t)clipIndex].audioId != clipAudio) return;
            project.setClipStretch(clipIndex, 1, true, 1.0f, 0.0f);
            commit("Fit audio to the tempo");
        });
    }
    return clipIndex;
}

// ------------------------------------------------------------------ editors

void Session::pumpEditors() {
    for (auto& t : project.mixer)
        for (auto& sl : t.slots)
            if (auto* ed = dynamic_cast<pulp::Splice*>(sl.get())) ed->pump();
}

juce::File Session::editsDir() const {
    return (projectFile != juce::File() ? projectFile.getParentDirectory() : dataDir());
}
