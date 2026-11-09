#pragma once
// Mixer runtime: one TrackLive per mixer track (its strip parameters, buffers and meters),
// the delay lines used for plugin delay compensation, and the worker pool that spreads
// independent parts of the graph over the CPU cores.
#include "pulp/Dsp.h"
#include "pulp/Plugin.h"
#include <thread>

namespace pulp {

constexpr int kNumSlots = 10;
constexpr int kMaxPdcSamples = 16384;       // longest compensated delay on one path (about 0.34 s at 48 kHz)

// A stereo delay used to line paths up. Created on a control thread, then only touched by audio.
struct PdcDelay {
    dsp::DelayLine l, r;
    PdcDelay() { l.prepare(kMaxPdcSamples); r.prepare(kMaxPdcSamples); }
    // Delays n samples in place by d samples (0 <= d <= kMaxPdcSamples).
    void process(float* L, float* R, int n, int d) {
        for (int i = 0; i < n; ++i) {
            l.push(L[i]); r.push(R[i]);
            L[i] = l.tap(d); R[i] = r.tap(d);
        }
    }
};

class TrackLive {
public:
    TrackLive();
    Param volume, pan, stereoSep, mute, phase, swap;
    Param eqFreq[3], eqGain[3], eqQ[3];
    Param slotMix[kNumSlots], slotBypass[kNumSlots];
    std::vector<Param*> params();
    Param* param(const std::string& id);
    static float faderToGain(float v);          // 0.8 is unity, 1.0 is +5.6 dB
    static float gainToFader(float g);

    // ---- audio / render threads ----
    float in[2][kQuantum];          // summed input for this quantum
    float pre[2][kQuantum];         // audio clips and the monitored hardware input, added to the track's input
    float side[2][kQuantum];        // summed sidechain input
    float out[2][kQuantum];         // post-fader output
    bool hasSide = false;
    struct SlotRt { Plugin* plugin = nullptr; PdcDelay* dry = nullptr; int latency = 0; };
    void rtProcess(const SlotRt* slots, double sampleRate, double tempo, double tick, bool playing);
    std::atomic<float> peakL{0.0f}, peakR{0.0f};
    void resetPeaks() { peakL.store(0.0f); peakR.store(0.0f); }
    void reset();           // back to a fresh state (no ramps in progress, filters cleared); not while audio runs

private:
    dsp::Biquad eq_[3][2];
    float eqSeen_[3][3] = {};       // freq, gain, q last used for the coefficients
    Ramp gL_, gR_, sep_;
    bool primed_ = false;
};

// Real-time worker pool. run() executes job(i) for i in [0, count) across the workers and the
// calling thread and returns when all are done. No allocation, no locks.
class WorkerPool {
public:
    explicit WorkerPool(int threads);
    ~WorkerPool();
    int threads() const { return (int)workers_.size() + 1; }
    template <class F> void run(int count, F&& job) {
        if (count <= 0) return;
        if (workers_.empty() || count == 1) { for (int i = 0; i < count; ++i) job(i); return; }
        using Fn = std::remove_reference_t<F>;
        struct Ctx { Fn* f; };
        Ctx ctx{&job};
        fn_.store([](void* c, int i) { (*static_cast<Ctx*>(c)->f)(i); });
        ctx_.store(&ctx);
        remaining_.store(count, std::memory_order_release);
        // generation, job count and next index live in one word, so a worker can only ever
        // claim a job that belongs to the run it is looking at
        const std::uint64_t gen = (state_.load(std::memory_order_relaxed) >> 40) + 1;
        state_.store((gen << 40) | ((std::uint64_t)count << 20), std::memory_order_release);
        wake_.fetch_add(1, std::memory_order_release);
        wake_.notify_all();
        work();
        while (remaining_.load(std::memory_order_acquire) > 0) std::this_thread::yield();
    }
private:
    void work();
    void loop();
    std::vector<std::thread> workers_;
    std::atomic<std::uint64_t> state_{0};
    std::atomic<unsigned> wake_{0};
    std::atomic<int> remaining_{0};
    std::atomic<bool> quit_{false};
    std::atomic<void (*)(void*, int)> fn_{nullptr};
    std::atomic<void*> ctx_{nullptr};
};

} // namespace pulp
