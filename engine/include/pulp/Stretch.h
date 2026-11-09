#pragma once
// Offline time-stretching, pitch shifting and tempo detection for audio clips.
// Everything here runs on a control thread and returns new audio; the engine then plays the
// result like any other sample, so playback and export stay identical.
#include "pulp/Audio.h"

namespace pulp::stretch {

enum Mode {
    Resample = 0,   // speed and pitch move together, like a record deck
    Rhythmic = 1,   // time-domain overlap-add that keeps drum hits sharp
    Tonal = 2,      // phase vocoder that keeps sustained notes smooth
    NumModes
};
const char* modeName(int mode);

// ratio = output length / input length.
std::shared_ptr<SampleData> resample(const SampleData& in, double ratio);       // band-limited; pitch moves by 1 / ratio
std::shared_ptr<SampleData> rhythmic(const SampleData& in, double ratio);       // pitch stays
std::shared_ptr<SampleData> tonal(const SampleData& in, double ratio);          // pitch stays

// The clip processor. timeRatio is output length / input length; pitchSemis shifts the pitch
// without changing the length. In Resample mode the two are one control: length = timeRatio / 2^(pitch/12).
std::shared_ptr<SampleData> process(const SampleData& in, int mode, double timeRatio, double pitchSemis);
double lengthRatio(int mode, double timeRatio, double pitchSemis);              // output length / input length of process()

// Estimated tempo in BPM (60 to 200), or 0 when the audio has no clear pulse.
// A file whose length is a whole number of bars at the estimate is taken to be a loop and gets the exact value.
double detectTempo(const SampleData& s, double* confidence = nullptr);

} // namespace pulp::stretch
