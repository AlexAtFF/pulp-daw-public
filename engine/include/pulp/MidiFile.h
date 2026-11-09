#pragma once
// Standard MIDI file reading and writing (formats 0 and 1), converted to and from PULP notes.
#include "pulp/Project.h"

namespace pulp {

struct MidiTrack {
    std::string name;
    std::vector<Note> notes;        // ticks at kPPQ; Note::color holds the MIDI channel (0..15)
    int programs[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   // General MIDI program per channel, -1 when the file does not say
};

struct MidiSong {
    double tempo = 120.0;           // first tempo in the file
    std::vector<std::pair<int, double>> tempos;     // every tempo in the file: (tick at kPPQ, BPM), in order, one per tick
    int timeSigNum = 4, timeSigDen = 4;
    std::vector<MidiTrack> tracks;  // only tracks that contain notes
};

bool readMidi(const std::vector<unsigned char>& bytes, MidiSong& out, std::string* error = nullptr);
bool readMidiFile(const std::string& path, MidiSong& out, std::string* error = nullptr);
std::vector<unsigned char> writeMidi(const MidiSong& song);     // writes song.tempos if there are any, else song.tempo
bool writeMidiFile(const std::string& path, const MidiSong& song);

// General MIDI: what a program number is called ("" outside 0..127), and which pad of the drum sampler
// (0..15, laid out as PadKit does) the usual sound on a key of the drum channel belongs to (-1: none).
// Puts the song's tempo changes on the playlist as an automation clip of the tempo: each tempo held until the
// next. Returns false, and adds nothing, when the song has one tempo throughout.
bool addTempoMap(Project& p, const MidiSong& song, int track, int64 lengthTicks);

const char* gmProgramName(int program);
int gmDrumPad(int key);

} // namespace pulp
