#pragma once
// Single notes made by synthesis, for factory instruments that have no licence-clean recording
// (and for the sounds that were synthesizers to begin with, like an 808 bass). Each instrument
// is a handful of notes across its range; they go through the same multisample builder as
// recorded notes do.
#include "pulp/Audio.h"

namespace pulp::notesynth {

struct NoteSet {
    std::string name, category;
    bool sustain = false;                                   // held sounds are looped by the builder
    std::vector<std::pair<int, std::shared_ptr<SampleData>>> notes;     // key (60 = middle C), audio
    std::string how;                                        // one line for the credits: how it was made
};

std::vector<std::string> names();                           // every instrument that can be made
// Empty notes if the name is unknown. every > 1 makes only every Nth note of the set (quicker, for tests).
NoteSet make(const std::string& name, double sampleRate = 48000.0, int every = 1);

} // namespace pulp::notesynth
