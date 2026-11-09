// Session: whole MIDI files in and out.
#include "Session.h"
#include "Theme.h"
#include "pulp/MidiFile.h"
#include "pulp/Multisample.h"
#include "pulp/Pads.h"

// The factory instrument that stands in for a General MIDI program ("" = a synthesizer).
static juce::String factoryInstrumentFor(int program) {
    struct Range { int lo, hi; const char* path; };
    static const Range map[] = {
        {0, 3, "Keys/Grand Piano"}, {4, 4, "Keys/Wurlitzer Electric Piano"}, {5, 5, "Keys/Electric Piano"}, {6, 6, "Plucked/Harpsichord"}, {7, 7, "Keys/Pianet Electric Piano"},
        {8, 10, "Mallets/Glockenspiel"}, {11, 11, "Mallets/Vibraphone"}, {12, 13, "Mallets/Marimba"}, {14, 14, "Bells/Tubular Bells"}, {15, 15, "Plucked/Psaltery"},
        {16, 18, "Keys/Tonewheel Organ"}, {19, 23, "Keys/Pipe Organ"},
        {24, 25, "Guitars/Acoustic Guitar"}, {26, 31, "Guitars/Electric Guitar"},
        {32, 32, "Bass/Upright Bass"}, {33, 37, "Bass/Electric Bass"}, {38, 39, "Bass/808 Bass"},
        {40, 40, "Strings/Violin"}, {41, 41, "Strings/Viola"}, {42, 42, "Strings/Cello"}, {43, 43, "Bass/Upright Bass"}, {44, 44, "Strings/String Ensemble"},
        {45, 45, "Plucked/Pizzicato Strings"}, {46, 46, "Plucked/Harp"}, {48, 49, "Strings/String Ensemble"}, {50, 51, "Strings/Synth Strings"},
        {52, 54, "Voices/Choir Aah"}, {56, 56, "Brass/Trumpet"}, {57, 58, "Brass/Trombone"}, {59, 59, "Brass/Trumpet"}, {60, 60, "Brass/French Horn"}, {61, 63, "Brass/Synth Brass"},
        {64, 67, "Woodwinds/Tenor Saxophone"}, {68, 71, "Woodwinds/Clarinet"}, {72, 79, "Woodwinds/Flute"},
        {104, 104, "Plucked/Dan Tranh"}, {105, 105, "Plucked/Banjo"}, {106, 107, "Plucked/Dan Tranh"}, {108, 108, "Mallets/Kalimba"}, {110, 110, "Strings/Violin"},
        {112, 112, "Bells/Bells"}, {114, 114, "Mallets/Marimba"},
    };
    for (auto& r : map) if (program >= r.lo && program <= r.hi) return r.path;
    return {};
}

juce::String Session::importMidiSong(const juce::File& file) {
    pulp::MidiSong song;
    std::string err;
    if (!pulp::readMidiFile(file.getFullPathName().toStdString(), song, &err)) { message("Could not read " + file.getFileName() + ": " + juce::String(err)); return {}; }
    // an untouched project takes the file's tempo; one with music in it keeps its own
    bool hasMusic = !project.clips.empty();
    for (auto& p : project.patterns) hasMusic = hasMusic || !p.empty();
    if (!hasMusic) {
        project.global->tempo.set((float)juce::jlimit(10.0, 522.0, song.tempo));
        project.timeSigNum = song.timeSigNum;
        project.timeSigDen = song.timeSigDen;
    }
    auto& pattern = project.addPattern(file.getFileNameWithoutExtension().toStdString());
    const int patternIndex = project.patternIndex(pattern.id);
    juce::String report;
    int made = 0, lastTick = 0;
    for (auto& track : song.tracks) {
        // a track may hold several MIDI channels (always, in a format 0 file): each becomes a channel
        std::map<int, std::vector<pulp::Note>> byChannel;
        for (auto& n : track.notes) byChannel[n.color & 15].push_back(n);
        for (auto& kv : byChannel) {
            if (made >= 48) break;
            const int midiChannel = kv.first, program = track.programs[midiChannel];
            auto notes = kv.second;
            std::shared_ptr<pulp::Plugin> plug;
            juce::String name = track.name.empty() ? juce::String() : juce::String(juce::CharPointer_UTF8(track.name.c_str())).trim(), sound;
            if (midiChannel == 9) {
                // the drum channel: a pad sampler, with each drum key moved to the pad that holds that sound
                auto kit = std::make_shared<pulp::PadKit>();
                const auto kits = kit->factoryPresets();
                const bool acoustic = factoryDir().getChildFile("Drums/Acoustic Kit/kick.wav").existsAsFile();
                if (!kits.empty()) kit->loadState(kits[(size_t)(acoustic && kits.size() > 2 ? 2 : 0)].state, project.projectDir);
                std::vector<pulp::Note> mapped;
                for (auto n : notes) { const int pad = pulp::gmDrumPad(n.key); if (pad < 0) continue; n.key = pulp::PadKit::kBaseKey + pad; mapped.push_back(n); }
                notes = std::move(mapped);
                plug = kit;
                sound = acoustic ? "acoustic drum kit" : "808 drum kit";
                if (name.isEmpty()) name = "Drums";
            } else {
                const auto rel = factoryInstrumentFor(program);
                const auto instFile = rel.isEmpty() ? juce::File() : factoryDir().getChildFile("Instruments").getChildFile(rel).getChildFile(rel.fromLastOccurrenceOf("/", false, false) + ".pulpinst");
                if (instFile.existsAsFile()) {
                    auto t = std::make_shared<pulp::Timbre>();
                    if (t->loadFile(projectPath(instFile), project.projectDir, &err)) { plug = t; sound = rel.fromLastOccurrenceOf("/", false, false); }
                }
                if (!plug) { plug = pulp::PluginRegistry::global().create("trio"); sound = "Trio (synthesizer)"; }
                if (name.isEmpty()) name = program >= 0 ? juce::String(pulp::gmProgramName(program)) : "MIDI channel " + juce::String(midiChannel + 1);
            }
            if (notes.empty() || !plug) continue;
            for (auto& n : notes) { n.color = 0; n.selected = false; lastTick = juce::jmax(lastTick, n.end()); }
            pulp::sortNotes(notes);
            auto& c = project.addChannel(plug, name.toStdString());
            c.color = theme::palette((int)project.channels.size() - 1).getARGB();
            project.patterns[(size_t)patternIndex].notesFor(c.id) = std::move(notes);
            report << "\n  " << name << ": " << (int)project.patterns[(size_t)patternIndex].notes[c.id].size() << " notes"
                   << (program >= 0 && midiChannel != 9 ? " (" + juce::String(pulp::gmProgramName(program)) + ")" : juce::String()) << " -> " << sound;
            ++made;
        }
    }
    if (made == 0) { message(file.getFileName() + " has no notes PULP can use."); return {}; }
    auto& pat = project.patterns[(size_t)patternIndex];
    pat.steps = juce::jlimit(16, 4096, (int)std::ceil((double)lastTick / pulp::kTicksPerStep / 16.0) * 16);
    project.currentPattern = patternIndex;
    // the pattern goes on the first free playlist track at the start of the song
    int track = 0;
    for (auto& c : project.clips) track = juce::jmax(track, c.track + 1);
    project.addPatternClip(patternIndex, track, 0);
    // tempo changes in the file become an automation clip of the tempo on the track below (only when the
    // project took the file's tempo: one with music in it keeps its own)
    const bool tempoMap = !hasMusic && pulp::addTempoMap(project, song, track + 1, project.patternLengthTicks(project.patterns[(size_t)patternIndex]));
    currentChannel = (int)project.channels.size() - 1;
    selectedChannels = {project.channels.back().id};
    commit("Import MIDI file");
    return juce::String(made) + (made == 1 ? " channel" : " channels") + " from " + file.getFileName() + ", " + juce::String(song.tempo, 1) + " BPM"
         + (hasMusic ? " (the project keeps its own tempo)" : "")
         + (tempoMap ? ", with " + juce::String((int)song.tempos.size() - 1) + " tempo changes as an automation clip" : juce::String())
         + (song.tempos.size() > 1 && hasMusic ? " (the file's tempo changes were left out)" : "") + ":" + report;
}

bool Session::exportMidiSong(const juce::File& file) {
    pulp::MidiSong song;
    song.tempo = project.global->tempo.get();
    song.timeSigNum = project.timeSigNum;
    song.timeSigDen = project.timeSigDen;
    // pad of the drum sampler -> the General MIDI drum key
    static const int drumKey[16] = {36, 38, 39, 42, 46, 41, 45, 48, 37, 49, 51, 56, 60, 70, 35, 76};
    int nextChannel = 0;
    for (auto& ch : project.channels) {
        std::vector<pulp::Note> all;
        if (project.songMode) {
            // the song: every pattern clip, laid end to end as the playlist has them
            for (auto& clip : project.clips) {
                if (clip.type != pulp::Clip::PatternClip || clip.muted) continue;
                const auto* pat = project.patternById(clip.patternId);
                const auto* notes = pat ? pat->find(ch.id) : nullptr;
                if (!notes) continue;
                for (auto n : *notes) {
                    const pulp::int64 at = (pulp::int64)n.tick - clip.offset;
                    if (n.muted || at < 0 || at >= clip.length) continue;
                    n.length = (int)juce::jmin<pulp::int64>(n.length, clip.length - at);
                    n.tick = (int)(clip.start + at);
                    all.push_back(n);
                }
            }
        } else if (const auto* notes = project.pattern().find(ch.id)) {
            for (auto& n : *notes) if (!n.muted) all.push_back(n);
        }
        if (all.empty()) continue;
        const bool drums = dynamic_cast<pulp::PadKit*>(ch.live->plugin.get()) != nullptr;
        if (!drums && nextChannel == 9) ++nextChannel;
        const int midiChannel = drums ? 9 : nextChannel++ % 16;
        for (auto& n : all) {
            n.color = midiChannel;
            if (drums) n.key = drumKey[juce::jlimit(0, 15, n.key - pulp::PadKit::kBaseKey)];
        }
        pulp::sortNotes(all);
        pulp::MidiTrack t;
        t.name = ch.name;
        t.notes = std::move(all);
        song.tracks.push_back(std::move(t));
    }
    if (song.tracks.empty()) { message("There are no notes to export."); return false; }
    if (!pulp::writeMidiFile(file.getFullPathName().toStdString(), song)) { message("Could not write " + file.getFullPathName()); return false; }
    return true;
}

// ------------------------------------------------------------------ SoundFonts

#include "pulp/SoundFont.h"

juce::StringArray Session::soundFontPresets(const juce::File& file) {
    std::string err;
    auto sf = pulp::sf2::open(file.getFullPathName().toStdString(), &err);
    juce::StringArray out;
    if (!sf) { message("Could not read " + file.getFileName() + ": " + juce::String(err)); return out; }
    for (auto& p : pulp::sf2::presets(*sf)) out.add(juce::String(p.bank).paddedLeft('0', 3) + ":" + juce::String(p.program).paddedLeft('0', 3) + "  " + juce::String(p.name));
    return out;
}

// A SoundFont preset becomes a PULP instrument: its samples are written out once, as FLAC, into PULP's own
// folder of instruments, and a Timbre channel plays that. listIndex counts in the order soundFontPresets gives.
int Session::addSoundFontChannel(const juce::File& file, int listIndex) {
    std::string err;
    auto sf = pulp::sf2::open(file.getFullPathName().toStdString(), &err);
    if (!sf) { message("Could not read " + file.getFileName() + ": " + juce::String(err)); return -1; }
    const auto list = pulp::sf2::presets(*sf);
    if (listIndex < 0 || listIndex >= (int)list.size()) return -1;
    auto inst = pulp::sf2::build(*sf, list[(size_t)listIndex].index, &err);
    if (!inst) { message("Could not use that preset: " + juce::String(err)); return -1; }
    const auto safe = juce::File::createLegalFileName(juce::String(inst->name).trim()).replaceCharacter('.', '_');
    const auto dir = dataDir().getChildFile("Instruments").getChildFile(juce::File::createLegalFileName(file.getFileNameWithoutExtension()))
                         .getChildFile(safe.isEmpty() ? juce::String("Preset ") + juce::String(listIndex + 1) : safe);
    dir.createDirectory();
    const auto target = dir.getChildFile(dir.getFileName() + ".pulpinst");
    if (!pulp::ms::saveInstrument(*inst, target.getFullPathName().toStdString(), true, &err)) { message("Could not write the instrument: " + juce::String(err)); return -1; }
    return addInstrumentFileChannel(target);
}
