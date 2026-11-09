// Session: project files, undo history, autosave and crash recovery, WAV export.
#include "Session.h"
#include "PluginHost.h"
#include "pulp/Render.h"
#include "pulp/Flac.h"
#include "pulp/Templates.h"
#if __has_include("pulp/Mp3.h")
 #include "pulp/Mp3.h"
#endif
#if __has_include("pulp/Ogg.h")
 #include "pulp/Ogg.h"
#endif
#include <set>

juce::File Session::dataDirOverride;

juce::File Session::dataDir() {
    if (dataDirOverride != juce::File()) return dataDirOverride;
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("PULP");
}

juce::String Session::projectTitle() const {
    return (projectFile == juce::File() ? juce::String("untitled") : projectFile.getFileNameWithoutExtension()) + (dirty ? " *" : "");
}

// ------------------------------------------------------------------ undo history

void Session::resetHistory(const juce::String& name) {
    history.clear();
    history.push_back({name, project.toJson().dump(0)});
    historyPos = 0;
    lastWasUndo_ = false;
    dirty = false;
    dirtySinceAutosave_ = false;
}

void Session::restore(const std::string& json) {
    std::string err;
    const auto doc = pulp::Json::parse(json, &err);
    // hosted plugins the project already holds are kept and given the snapshot's state, not loaded afresh
    PluginHost::get().beginReuse(project);
    const bool ok = err.empty() && project.fromJson(doc, &err);
    PluginHost::get().endReuse();
    if (!ok) { message("Could not restore that step: " + juce::String(err)); return; }
    selectedChannels.clear();
    currentChannel = juce::jlimit(0, juce::jmax(0, (int)project.channels.size() - 1), currentChannel);
    if (auto* ch = current()) selectedChannels.insert(ch->id);
    dirty = true;
    dirtySinceAutosave_ = true;
    publish();
    notify();
}

void Session::jumpToHistory(int index) {
    if (index < 0 || index >= (int)history.size() || index == historyPos) return;
    lastWasUndo_ = index < historyPos;
    historyPos = index;
    restore(history[(size_t)index].json);
}

void Session::undoStep() { if (historyPos > 0) jumpToHistory(historyPos - 1); }
void Session::redoStep() { if (historyPos + 1 < (int)history.size()) jumpToHistory(historyPos + 1); }

void Session::undoToggle() {
    // as in the reference program: Ctrl+Z flips between the last step and the one before it
    if (lastWasUndo_ && historyPos + 1 < (int)history.size()) { redoStep(); lastWasUndo_ = false; }
    else if (historyPos > 0) { undoStep(); lastWasUndo_ = true; }
}

// ------------------------------------------------------------------ files

void Session::newProject() {
    engine.stop();
    newDefaultProject();
    project.prepare(engine.sampleRate());
    publish();
    notify();
}

// A new project from one of the built-in templates (-1: the demo song).
void Session::newFromTemplate(int index) {
    engine.stop();
    pulp::SampleCache::global().setFactoryRoot(factoryDir().getFullPathName().toStdString());
    if (index < 0) pulp::makeDemoSong(project);
    else if (!pulp::makeTemplate(project, index)) { newDefaultProject(); }
    currentChannel = 0;
    selectedChannels.clear();
    if (!project.channels.empty()) selectedChannels.insert(project.channels[0].id);
    projectFile = juce::File();
    resetHistory(index < 0 ? "Demo song" : "New from template");
    project.prepare(engine.sampleRate());
    publish();
    notify();
}

bool Session::openProject(const juce::File& file) {
    juce::File toOpen = file;
    std::string err;
    if (file.hasFileExtension("pulpz")) {
        // a packed project: unpack into a folder beside it, then open that
        const auto dest = file.getSiblingFile(file.getFileNameWithoutExtension());
        std::string projectFilePath;
        if (!pulp::Project::unpack(file.getFullPathName().toStdString(), dest.getFullPathName().toStdString(), projectFilePath, &err)) {
            message("Could not unpack " + file.getFileName() + ": " + juce::String(err));
            return false;
        }
        toOpen = juce::File(juce::String(projectFilePath));
    }
    pulp::Project loaded;
    loaded.sampleRate = engine.sampleRate();
    if (!loaded.load(toOpen.getFullPathName().toStdString(), &err)) {
        message("Could not open " + toOpen.getFileName() + ": " + juce::String(err));
        return false;
    }
    engine.stop();
    project = std::move(loaded);
    project.prepare(engine.sampleRate());
    projectFile = toOpen;
    currentChannel = 0;
    selectedChannels.clear();
    if (!project.channels.empty()) selectedChannels.insert(project.channels[0].id);
    resetHistory("Open " + toOpen.getFileName());
    noteRecent(toOpen);
    publish();
    notify();
    return true;
}

bool Session::saveProject(const juce::File& file) {
    std::string err;
    project.name = file.getFileNameWithoutExtension().toStdString();
    // write beside the target first, then swap, so a failed save cannot destroy the old file
    const auto temp = file.getSiblingFile(file.getFileName() + ".saving");
    if (!project.save(temp.getFullPathName().toStdString(), &err)) { message("Could not save: " + juce::String(err)); return false; }
    if (!temp.replaceFileIn(file)) { message("Could not replace " + file.getFullPathName()); return false; }
    projectFile = file;
    dirty = false;
    dirtySinceAutosave_ = false;
    noteRecent(file);
    notify();
    return true;
}

bool Session::saveNewVersion() {
    if (projectFile == juce::File()) return false;
    // song.pulp -> song_2.pulp, song_7.pulp -> song_8.pulp
    auto base = projectFile.getFileNameWithoutExtension();
    int n = 1;
    const int us = base.lastIndexOfChar('_');
    if (us > 0 && base.substring(us + 1).containsOnly("0123456789") && base.substring(us + 1).isNotEmpty()) { n = base.substring(us + 1).getIntValue(); base = base.substring(0, us); }
    juce::File next;
    do { next = projectFile.getSiblingFile(base + "_" + juce::String(++n) + ".pulp"); } while (next.existsAsFile());
    return saveProject(next);
}

bool Session::packProject(const juce::File& zip) {
    std::string err;
    if (!project.pack(zip.getFullPathName().toStdString(), &err)) { message("Could not pack the project: " + juce::String(err)); return false; }
    return true;
}

juce::StringArray Session::recentFiles() const {
    juce::StringArray out;
    if (auto x = loadSettings("RECENT"))
        for (auto* e : x->getChildWithTagNameIterator("FILE")) if (juce::File(e->getStringAttribute("path")).existsAsFile()) out.add(e->getStringAttribute("path"));
    return out;
}

void Session::noteRecent(const juce::File& f) {
    auto list = recentFiles();
    list.removeString(f.getFullPathName());
    list.insert(0, f.getFullPathName());
    while (list.size() > 12) list.remove(list.size() - 1);
    auto x = std::make_unique<juce::XmlElement>("RECENT");
    for (auto& p : list) x->createNewChildElement("FILE")->setAttribute("path", p);
    saveSettings(std::move(x));
}

// ------------------------------------------------------------------ autosave and crash recovery

static bool canPersist() { return Session::persistSettings || Session::dataDirOverride != juce::File(); }

juce::Array<juce::File> Session::listBackups() const {
    juce::Array<juce::File> files;
    if (!canPersist()) return files;
    dataDir().getChildFile("autosave").findChildFiles(files, juce::File::findFiles, false, "*.pulp");
    struct Newest { static int compareElements(const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime() ? -1 : 1; } };
    Newest cmp;
    files.sort(cmp);
    return files;
}

bool Session::autosaveNow() {
    lastAutosave_ = juce::Time::getMillisecondCounter();
    dirtySinceAutosave_ = false;
    if (!canPersist()) return false;
    const auto dir = dataDir().getChildFile("autosave");
    dir.createDirectory();
    const auto name = (projectFile == juce::File() ? juce::String("untitled") : projectFile.getFileNameWithoutExtension());
    const auto file = dir.getChildFile(name + "__" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".pulp");
    // audio that an editor holds only in memory is written out first, so the backup can find it
    project.flushEditors(editsDir().getFullPathName().toStdString());
    // backups carry absolute sample paths so they open from the autosave folder,
    // and remember which project file they belong to
    auto doc = project.toPortableJson();
    doc.set("recoveredFrom", projectFile.getFullPathName().toStdString());
    if (!file.replaceWithText(juce::String(doc.dump(1)))) return false;
    // keep the ten newest backups of this project
    juce::Array<juce::File> mine;
    for (auto& f : listBackups()) if (f.getFileName().startsWith(name + "__")) mine.add(f);
    for (int i = 10; i < mine.size(); ++i) mine[i].deleteFile();
    return true;
}

bool Session::restoreBackup(const juce::File& backup) {
    std::string err;
    const auto doc = pulp::Json::parse(backup.loadFileAsString().toStdString(), &err);
    if (!openProject(backup)) return false;
    // it is a recovered copy, not the saved project: point back at the file it came from and mark it unsaved
    const juce::String from(doc["recoveredFrom"].str(""));
    projectFile = from.isNotEmpty() ? juce::File(from) : juce::File();
    if (projectFile != juce::File()) project.projectDir = projectFile.getParentDirectory().getFullPathName().toStdString();
    dirty = true;
    notify();
    return true;
}

void Session::beginRunLock() {
    if (!canPersist()) return;
    const auto lock = dataDir().getChildFile("running.lock");
    crashedLastTime = lock.existsAsFile();
    dataDir().createDirectory();
    lock.replaceWithText(juce::Time::getCurrentTime().toString(true, true));
}

void Session::endRunLock() {
    if (canPersist()) dataDir().getChildFile("running.lock").deleteFile();
}

// ------------------------------------------------------------------ export

juce::StringArray Session::exportFormats() {
    juce::StringArray f{"wav", "flac"};
   #if __has_include("pulp/Mp3.h")
    f.add("mp3");
   #endif
   #if __has_include("pulp/Ogg.h")
    f.add("ogg");
   #endif
    return f;
}

class Session::ExportThread : public juce::Thread {
public:
    ExportThread(Session& s, std::string json, std::string projectDir, juce::File file, ExportOptions opt,
                 std::function<void(double)> progress, std::function<void(juce::String)> done)
        : juce::Thread("PULP export"), s_(s), json_(std::move(json)), dir_(std::move(projectDir)), file_(std::move(file)), opt_(opt),
          progress_(std::move(progress)), done_(std::move(done)) {}

    // Writes the audio in the format the file's ending names.
    bool write(const juce::File& file, const pulp::SampleData& audio, std::string& err) const {
        const auto path = file.getFullPathName().toStdString();
        const auto ext = file.getFileExtension().toLowerCase();
        // a file is never written in one format under another format's name
        if (!exportFormats().contains(ext.substring(1))) { err = "this build cannot write " + ext.toStdString() + " files"; return false; }
        if (ext == ".flac") return pulp::writeFlacFile(path, audio, opt_.bits == 16 ? 16 : 24, &err);
       #if __has_include("pulp/Mp3.h")
        if (ext == ".mp3") return pulp::writeMp3File(path, audio, opt_.mp3Kbps, &err);
       #endif
       #if __has_include("pulp/Ogg.h")
        if (ext == ".ogg") return pulp::writeOggFile(path, audio, opt_.oggQuality, &err);
       #endif
        const auto fmt = opt_.bits == 16 ? pulp::WavFormat::Pcm16 : (opt_.bits == 32 ? pulp::WavFormat::Float32 : pulp::WavFormat::Pcm24);
        return pulp::writeWavFile(path, audio, fmt, opt_.dither && opt_.bits != 32, &err);
    }

    // Mixer tracks that have channels or audio clips of their own.
    static std::set<int> sources(const pulp::Project& p) {
        std::set<int> out;
        for (auto& c : p.channels) out.insert(juce::jlimit(0, pulp::kNumMixerTracks - 1, c.mixerTrack));
        for (auto& c : p.clips) if (c.type == pulp::Clip::AudioClip) out.insert(juce::jlimit(0, pulp::kNumMixerTracks - 1, c.mixerTrack));
        return out;
    }

    // One render of the project, on a copy of its own so that playback and editing carry on. stem < 0:
    // everything. Otherwise only what plays through that mixer track: the other tracks that have channels or
    // audio of their own are silenced, unless the stem's track feeds them.
    juce::String render(int stem, const juce::File& file, double done, double share) {
        pulp::Project copy;
        copy.projectDir = dir_;
        copy.sampleRate = opt_.sampleRate;
        std::string err;
        if (!copy.fromJson(pulp::Json::parse(json_, &err), &err)) return "Could not prepare the export: " + juce::String(err);
        copy.songMode = opt_.song;
        // plugins are told that this is not real time (hosted plugins may raise their quality)
        for (auto& c : copy.channels) c.live->plugin->setOffline(true);
        for (auto& t : copy.mixer) for (auto& sl : t.slots) if (sl) sl->setOffline(true);
        if (stem >= 0) {
            std::set<int> keep{stem};
            std::vector<int> todo{stem};
            while (!todo.empty()) {
                const int t = todo.back();
                todo.pop_back();
                for (auto& r : copy.mixer[(size_t)t].routes) if (keep.insert(r.to).second) todo.push_back(r.to);
            }
            for (int t : sources(copy)) if (t != 0 && !keep.count(t)) copy.mixer[(size_t)t].live->mute.set(1.0f);
            // what goes straight to the master belongs to the master's own stem only
            if (stem != 0) {
                for (auto& c : copy.channels) if (c.mixerTrack == 0) if (auto* m = c.live->stripParam("mute")) m->set(1.0f);
                for (auto& c : copy.clips) if (c.type == pulp::Clip::AudioClip && c.mixerTrack == 0) c.muted = true;
            }
        }
        pulp::RenderOptions ro;
        ro.sampleRate = opt_.sampleRate;
        ro.blockSize = 4096;
        ro.tail = opt_.tail;
        ro.threads = juce::jlimit(1, 16, juce::SystemStats::getNumCpus() - 1);
        double last = -1.0;
        auto audio = pulp::renderOffline(copy, ro, [&](double p) {
            const double total = done + p * share;
            if (total - last > 0.01) { last = total; auto cb = progress_; juce::MessageManager::callAsync([cb, total] { if (cb) cb(total); }); }
            return !threadShouldExit() && !s_.exportCancel_.load();
        });
        if (!audio) return "Export cancelled";
        if (stem >= 0 && audio->peak() <= 0.0f) return {};       // a mixer track with nothing playing gets no file
        if (!write(file, *audio, err)) return "Could not write the file: " + juce::String(err);
        return {};
    }

    void run() override {
        juce::String error;
        if (!opt_.stems) error = render(-1, file_, 0.0, 1.0);
        else {
            pulp::Project look;
            look.projectDir = dir_;
            std::string err;
            if (!look.fromJson(pulp::Json::parse(json_, &err), &err)) error = "Could not prepare the export: " + juce::String(err);
            else {
                const auto tracks = sources(look);
                int n = 0;
                for (int t : tracks) {
                    if (error.isNotEmpty()) break;
                    juce::String name = look.mixer[(size_t)t].name.empty() ? juce::String(pulp::Project::mixerTrackName(t)) : juce::String(look.mixer[(size_t)t].name);
                    if (t == 0) name = "Master (not routed)";
                    const auto f = file_.getSiblingFile(file_.getFileNameWithoutExtension() + " - " + juce::File::createLegalFileName(name) + file_.getFileExtension());
                    error = render(t, f, (double)n / (double)tracks.size(), 1.0 / (double)tracks.size());
                    ++n;
                }
            }
        }
        auto done = done_;
        Session* s = &s_;
        juce::MessageManager::callAsync([done, error, s] {
            if (s->exportThread_ && !s->exportThread_->isThreadRunning()) s->exportThread_.reset();
            if (done) done(error);
        });
    }

private:
    Session& s_;
    std::string json_, dir_;
    juce::File file_;
    ExportOptions opt_;
    std::function<void(double)> progress_;
    std::function<void(juce::String)> done_;
};

void Session::exportWav(const juce::File& file, const ExportOptions& opt, std::function<void(double)> progress, std::function<void(juce::String)> done) {
    if (exportThread_) {
        if (exportThread_->isThreadRunning()) { if (done) done("An export is already running"); return; }
        exportThread_.reset();
    }
    exportCancel_ = false;
    exportThread_ = std::make_unique<ExportThread>(*this, project.toJson().dump(0), project.projectDir, file, opt, std::move(progress), std::move(done));
    exportThread_->startThread(juce::Thread::Priority::normal);
}

void Session::cancelExport() { exportCancel_ = true; }
