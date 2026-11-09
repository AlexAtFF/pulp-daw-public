#pragma once
// SoundFont 2 files (.sf2): listing their presets and turning one into an instrument for Timbre.
#include "pulp/Multisample.h"

namespace pulp::sf2 {

struct PresetInfo {
    std::string name;
    int bank = 0, program = 0;
    int index = 0;              // its place in the file, for build()
};

struct File;                    // a parsed SoundFont
std::shared_ptr<const File> open(const std::string& utf8Path, std::string* error = nullptr);
std::shared_ptr<const File> openMemory(const void* data, size_t size, std::string* error = nullptr);
std::vector<PresetInfo> presets(const File& f);     // sorted by bank, then program
// The zones of one preset as a Timbre instrument (the audio lives in memory: save it with ms::saveInstrument).
// Key and velocity ranges, root keys, tuning, loops, attenuation and stereo pairs are kept; the SoundFont's
// own envelopes, filters and modulators are not, apart from the release time, which goes into the
// instrument's suggested settings.
std::shared_ptr<ms::Instrument> build(const File& f, int presetIndex, std::string* error = nullptr);

} // namespace pulp::sf2
