#pragma once
// Helpers shared by the test files.
#include "Test.h"
#include "pulp/Audio.h"
#include "pulp/DrumSynth.h"
#include "pulp/Project.h"
#include "pulp/Render.h"
#include "pulp/Sampler.h"

namespace tu {
using namespace pulp;

inline std::shared_ptr<SampleData> sine(double hz, double seconds, double sr, float amp = 0.5f) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = 1;
    const int n = (int)(seconds * sr);
    s->ch[0].resize((size_t)n);
    for (int i = 0; i < n; ++i) s->ch[0][(size_t)i] = amp * (float)std::sin(kTwoPi * hz * i / sr);
    return s;
}

// Frequency from interpolated upward zero crossings between sample a and b.
inline double measureHz(const std::vector<float>& d, size_t a, size_t b, double sr) {
    double first = -1, last = -1;
    int count = 0;
    for (size_t i = a + 1; i < b && i < d.size(); ++i) {
        if (d[i - 1] < 0.0f && d[i] >= 0.0f) {
            const double x = (double)(i - 1) + (double)(-d[i - 1]) / (double)(d[i] - d[i - 1]);
            if (first < 0) first = x;
            last = x;
            ++count;
        }
    }
    if (count < 2) return 0.0;
    return (count - 1) * sr / (last - first);
}

inline double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) m = std::max(m, (double)std::fabs(a[i] - b[i]));
    return m;
}

inline float peakOf(const std::vector<float>& a, size_t from = 0, size_t to = (size_t)-1) {
    float p = 0.0f;
    for (size_t i = from; i < std::min(to, a.size()); ++i) p = std::max(p, std::fabs(a[i]));
    return p;
}

inline double rmsOf(const std::vector<float>& a, size_t from = 0, size_t to = (size_t)-1) {
    double s = 0.0;
    to = std::min(to, a.size());
    if (to <= from) return 0.0;
    for (size_t i = from; i < to; ++i) s += (double)a[i] * a[i];
    return std::sqrt(s / (double)(to - from));
}

inline bool identical(const SampleData& a, const SampleData& b) {
    return a.frames() == b.frames() && a.ch[0] == b.ch[0] && a.ch[1] == b.ch[1];
}

// Adds a sampler channel holding the given sample. Returns copies of what tests need,
// because references into Project::channels go stale when another channel is added.
struct Chan {
    int id;
    std::shared_ptr<ChannelLive> live;
    Sampler* sampler() const { return static_cast<Sampler*>(live->plugin.get()); }
};
inline Chan addSampleChannel(Project& p, SamplePtr s, const std::string& name = "Sample") {
    auto smp = std::make_shared<Sampler>();
    smp->setSample(std::move(s));
    auto& c = p.addChannel(smp, name);
    return {c.id, c.live};
}

inline double samplesPerTick(double bpm, double sr) { return 60.0 * sr / (bpm * kPPQ); }

} // namespace tu
