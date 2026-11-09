#pragma once
// Core constants and lock-free helpers shared by the whole engine.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulp {

using int64 = std::int64_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

constexpr int kPPQ = 960;            // ticks per quarter note
constexpr int kTicksPerStep = 240;   // one step is a sixteenth note
constexpr int kQuantum = 64;         // every DSP object is processed in blocks of exactly this many samples
constexpr int kNumInserts = 125;
constexpr int kMasterTrack = 0;
constexpr int kCurrentTrack = 126;   // the "current" mixer track
constexpr int kNumMixerTracks = 127;
constexpr int kNumPlaylistTracks = 500;
constexpr int kMaxEventsPerQuantum = 256;
constexpr int kRootKey = 60;         // shown as C5, as in the reference program
constexpr float kDefaultVelocity = 100.0f / 127.0f;   // velocity 100, the level a new note gets
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

inline float dbToGain(float db) { return db <= -144.0f ? 0.0f : std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g) { return g <= 1.0e-7f ? -144.0f : 20.0f * std::log10(g); }
inline double keyToHz(double key) { return 440.0 * std::pow(2.0, (key - 69.0) / 12.0); }
template <class T> inline T clampT(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Set while a thread is inside the audio callback. The tests replace operator new and
// fail if anything allocates while this is set.
struct Rt {
    static thread_local int depth;
    static std::atomic<int64> allocationsInRt;
    struct Scope { Scope() { ++depth; } ~Scope() { --depth; } };
    // An offline render may read files from inside process(); that is not real time, so the marker is lifted.
    struct Suspend { int saved; Suspend() : saved(depth) { depth = 0; } ~Suspend() { depth = saved; } };
    static bool inRt() { return depth > 0; }
};

// Single-producer single-consumer ring. Capacity must be a power of two.
template <class T, size_t N>
class SpscQueue {
public:
    bool push(const T& v) {
        const size_t w = w_.load(std::memory_order_relaxed);
        if (w - r_.load(std::memory_order_acquire) >= N) return false;
        buf_[w & (N - 1)] = v;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        const size_t r = r_.load(std::memory_order_relaxed);
        if (r == w_.load(std::memory_order_acquire)) return false;
        v = buf_[r & (N - 1)];
        r_.store(r + 1, std::memory_order_release);
        return true;
    }
    size_t size() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
private:
    std::array<T, N> buf_{};
    std::atomic<size_t> r_{0}, w_{0};
};

// A value published by a control thread and read by the audio thread without locks.
// The audio thread calls rtAcquire() once per block; the pointer stays valid until its
// next rtAcquire(). Control threads call set() (serialised between themselves) and the
// old values are freed on a control thread once the audio thread has let go (hazard pointer).
template <class T>
class RtShared {
public:
    void set(std::shared_ptr<const T> v) {
        std::lock_guard<std::mutex> g(m_);
        alive_.push_back(v);
        latest_.store(v.get(), std::memory_order_seq_cst);
        collectLocked();
    }
    std::shared_ptr<const T> get() const {     // control threads only
        std::lock_guard<std::mutex> g(m_);
        const T* p = latest_.load();
        for (auto& a : alive_) if (a.get() == p) return a;
        return {};
    }
    const T* rtAcquire() {
        const T* p;
        do {
            p = latest_.load(std::memory_order_seq_cst);
            hazard_.store(p, std::memory_order_seq_cst);
        } while (latest_.load(std::memory_order_seq_cst) != p);
        return p;
    }
    void collect() { std::lock_guard<std::mutex> g(m_); collectLocked(); }
private:
    void collectLocked() {
        const T* cur = latest_.load(std::memory_order_seq_cst);
        const T* hz = hazard_.load(std::memory_order_seq_cst);
        alive_.erase(std::remove_if(alive_.begin(), alive_.end(),
                                    [&](const std::shared_ptr<const T>& a) { return a.get() != cur && a.get() != hz; }),
                     alive_.end());
    }
    mutable std::mutex m_;
    std::vector<std::shared_ptr<const T>> alive_;
    std::atomic<const T*> latest_{nullptr};
    std::atomic<const T*> hazard_{nullptr};
};

// Linear ramp used for click-free parameter changes. One target per quantum.
struct Ramp {
    float cur = 0.0f, step = 0.0f;
    int left = 0;
    void snap(float v) { cur = v; step = 0; left = 0; }
    void target(float v, int n) {
        if (n <= 0 || v == cur) { if (n <= 0) cur = v; step = 0; left = 0; if (v == cur) return; cur = v; return; }
        step = (v - cur) / (float)n; left = n; tgt = v;
    }
    inline float next() {
        if (left > 0) { cur += step; if (--left == 0) cur = tgt; }
        return cur;
    }
    float tgt = 0.0f;
};

inline void flushDenormal(float& v) { if (std::fabs(v) < 1.0e-20f) v = 0.0f; }

} // namespace pulp
