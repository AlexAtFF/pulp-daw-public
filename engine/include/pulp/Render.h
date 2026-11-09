#pragma once
// Offline rendering: drives an Engine in a loop, with no sound card.
#include "pulp/Audio.h"
#include "pulp/Project.h"
#include <functional>

namespace pulp {

struct RenderOptions {
    double sampleRate = 48000.0;
    int blockSize = 512;            // any size gives the same audio
    bool loop = false;              // keep looping (needs lengthSamples)
    int64 lengthSamples = 0;        // > 0: render exactly this many samples
    double maxTailSeconds = 10.0;   // after the last bar, keep going until silence or this long
    enum Tail { LeaveRemainder, Cut, Wrap };
    int tail = LeaveRemainder;      // Cut: stop on the last bar. Wrap: fold the tail back onto the start (for loops).
    double startTick = 0.0;
    int threads = 1;                // worker threads for the mixer graph; any number gives the same audio
};

// Renders the project as it stands (song or pattern mode is taken from the project).
// The project's plugins are re-prepared, so it must not be attached to a running engine.
// progress gets 0..1 and can return false to cancel.
std::shared_ptr<SampleData> renderOffline(Project& project, const RenderOptions& opt,
                                          const std::function<bool(double)>& progress = {});

} // namespace pulp
