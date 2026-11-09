// Session: the pad drum sampler and the loop slicer.
#include "Session.h"
#include "pulp/Pads.h"

static pulp::PadKit* padKitOf(pulp::Channel* ch) { return ch ? dynamic_cast<pulp::PadKit*>(ch->live->plugin.get()) : nullptr; }
static pulp::Chop* chopOf(pulp::Channel* ch) { return ch ? dynamic_cast<pulp::Chop*>(ch->live->plugin.get()) : nullptr; }

bool Session::setPadSample(int index, int pad, const juce::File& file, bool addLayer) {
    auto* kit = padKitOf(channel(index));
    if (!kit) return false;
    std::string err;
    auto sample = pulp::SampleCache::global().load(projectPath(file), project.projectDir, &err);
    if (!sample) { message("Could not load " + file.getFileName() + ": " + juce::String(err)); return false; }
    if (addLayer && kit->layerCount(pad) > 0) {
        if (kit->addLayer(pad, sample) < 0) { message("That pad already has its four layers."); return false; }
    } else {
        kit->clearPad(pad);
        kit->setLayer(pad, 0, sample);
        kit->setPadName(pad, file.getFileNameWithoutExtension().toStdString());
    }
    commit(addLayer ? "Add pad layer" : "Load pad sound");
    return true;
}

int Session::loadPadKit(int index, const juce::Array<juce::File>& files) {
    auto* kit = padKitOf(channel(index));
    if (!kit) return 0;
    std::vector<std::string> paths;
    for (auto& f : files) if (isAudioFile(f)) paths.push_back(projectPath(f));
    std::sort(paths.begin(), paths.end());
    if (paths.empty()) { message("There are no audio files there."); return 0; }
    const int filled = kit->loadKit(paths, project.projectDir);
    commit("Load drum kit");
    return filled;
}

int Session::loadPadKit(int index, const juce::File& folder) {
    juce::Array<juce::File> files;
    for (const auto& e : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findFiles)) files.add(e.getFile());
    const int filled = loadPadKit(index, files);
    if (filled > 0) if (auto* ch = channel(index)) { ch->name = folder.getFileName().toStdString(); notify(); }
    return filled;
}

int Session::addChopChannel(const juce::File& file) {
    std::string err;
    auto sample = pulp::SampleCache::global().load(projectPath(file), project.projectDir, &err);
    if (!sample) { message("Could not load " + file.getFileName() + ": " + juce::String(err)); return -1; }
    auto chop = std::make_shared<pulp::Chop>();
    chop->setSample(sample);
    auto& c = project.addChannel(chop, file.getFileNameWithoutExtension().toStdString());
    c.color = 0xff6b8f4e;
    currentChannel = (int)project.channels.size() - 1;
    selectedChannels = {c.id};
    commit("Slice a loop");
    return currentChannel;
}

// One note per slice, each where its slice sits in the loop, so the pattern plays the loop at the project's tempo.
int Session::writeSliceNotes(int index) {
    auto* ch = channel(index);
    auto* chop = chopOf(ch);
    auto sample = chop ? chop->sample() : nullptr;
    if (!sample) { message("Load a loop first."); return 0; }
    const auto starts = chop->slices();
    const double ticks = chop->beats() * pulp::kPPQ;
    auto& notes = project.pattern().notesFor(ch->id);
    notes.clear();
    for (size_t i = 0; i < starts.size(); ++i) {
        pulp::Note n;
        n.tick = (int)std::lround((double)starts[i] / (double)sample->frames() * ticks);
        const int end = i + 1 < starts.size() ? (int)std::lround((double)starts[i + 1] / (double)sample->frames() * ticks) : (int)std::lround(ticks);
        n.length = juce::jmax(1, end - n.tick);
        n.key = pulp::Chop::kBaseKey + (int)i;
        notes.push_back(n);
    }
    pulp::sortNotes(notes);
    // the pattern is made long enough to hold the loop
    const int steps = (int)std::ceil(ticks / pulp::kTicksPerStep);
    if (project.pattern().steps < steps) project.pattern().steps = juce::jmin(steps, 1024);
    commit("Write slices to the piano roll");
    return (int)starts.size();
}
