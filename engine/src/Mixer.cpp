#include "pulp/Mixer.h"

namespace pulp {

static ParamInfo pinf(const std::string& id, const std::string& name, float mn, float mx, float def, const char* unit = "", float skew = 1.0f) {
    ParamInfo i; i.id = id; i.name = name; i.min = mn; i.max = mx; i.def = def; i.unit = unit; i.skew = skew;
    return i;
}
static ParamInfo ptog(const std::string& id, const std::string& name, const char* off, const char* on) {
    ParamInfo i; i.id = id; i.name = name; i.min = 0; i.max = 1; i.def = 0; i.steps = 2; i.choices = {off, on};
    return i;
}
static std::string num(const char* base, int i) { return std::string(base) + std::to_string(i + 1); }

static const float kEqFreq[3] = {100.0f, 1000.0f, 8000.0f};
static const char* kEqName[3] = {"Low", "Mid", "High"};

TrackLive::TrackLive()
    : volume(pinf("volume", "Track volume", 0, 1, 0.8f, "%")),
      pan(pinf("pan", "Track panning", -1, 1, 0)),
      stereoSep(pinf("stereo", "Stereo separation (right = mono)", -1, 1, 0)),
      mute(ptog("mute", "Mute", "On", "Muted")),
      phase(ptog("phase", "Invert phase", "Off", "Inverted")),
      swap(ptog("swap", "Swap left and right", "Off", "Swapped")),
      eqFreq{Param(pinf("eq1freq", "EQ low frequency", 20, 20000, kEqFreq[0], "Hz", 3.0f)),
             Param(pinf("eq2freq", "EQ mid frequency", 20, 20000, kEqFreq[1], "Hz", 3.0f)),
             Param(pinf("eq3freq", "EQ high frequency", 20, 20000, kEqFreq[2], "Hz", 3.0f))},
      eqGain{Param(pinf("eq1gain", "EQ low gain", -18, 18, 0, "dB")), Param(pinf("eq2gain", "EQ mid gain", -18, 18, 0, "dB")),
             Param(pinf("eq3gain", "EQ high gain", -18, 18, 0, "dB"))},
      eqQ{Param(pinf("eq1q", "EQ low width", 0.3f, 4, 0.7f)), Param(pinf("eq2q", "EQ mid width", 0.3f, 8, 0.7f)),
          Param(pinf("eq3q", "EQ high width", 0.3f, 4, 0.7f))},
      slotMix{Param(pinf(num("mix", 0), "Slot 1 mix", 0, 1, 1, "%")), Param(pinf(num("mix", 1), "Slot 2 mix", 0, 1, 1, "%")),
              Param(pinf(num("mix", 2), "Slot 3 mix", 0, 1, 1, "%")), Param(pinf(num("mix", 3), "Slot 4 mix", 0, 1, 1, "%")),
              Param(pinf(num("mix", 4), "Slot 5 mix", 0, 1, 1, "%")), Param(pinf(num("mix", 5), "Slot 6 mix", 0, 1, 1, "%")),
              Param(pinf(num("mix", 6), "Slot 7 mix", 0, 1, 1, "%")), Param(pinf(num("mix", 7), "Slot 8 mix", 0, 1, 1, "%")),
              Param(pinf(num("mix", 8), "Slot 9 mix", 0, 1, 1, "%")), Param(pinf(num("mix", 9), "Slot 10 mix", 0, 1, 1, "%"))},
      slotBypass{Param(ptog(num("bypass", 0), "Slot 1", "On", "Bypassed")), Param(ptog(num("bypass", 1), "Slot 2", "On", "Bypassed")),
                 Param(ptog(num("bypass", 2), "Slot 3", "On", "Bypassed")), Param(ptog(num("bypass", 3), "Slot 4", "On", "Bypassed")),
                 Param(ptog(num("bypass", 4), "Slot 5", "On", "Bypassed")), Param(ptog(num("bypass", 5), "Slot 6", "On", "Bypassed")),
                 Param(ptog(num("bypass", 6), "Slot 7", "On", "Bypassed")), Param(ptog(num("bypass", 7), "Slot 8", "On", "Bypassed")),
                 Param(ptog(num("bypass", 8), "Slot 9", "On", "Bypassed")), Param(ptog(num("bypass", 9), "Slot 10", "On", "Bypassed"))} {
    (void)kEqName;
    std::memset(pre, 0, sizeof pre);
    std::memset(in, 0, sizeof in);
    std::memset(side, 0, sizeof side);
    std::memset(out, 0, sizeof out);
}

std::vector<Param*> TrackLive::params() {
    std::vector<Param*> v = {&volume, &pan, &stereoSep, &mute, &phase, &swap};
    for (int i = 0; i < 3; ++i) { v.push_back(&eqFreq[i]); v.push_back(&eqGain[i]); v.push_back(&eqQ[i]); }
    for (int i = 0; i < kNumSlots; ++i) { v.push_back(&slotMix[i]); v.push_back(&slotBypass[i]); }
    return v;
}
Param* TrackLive::param(const std::string& id) {
    for (Param* p : params()) if (p->info.id == id) return p;
    return nullptr;
}

void TrackLive::reset() {
    primed_ = false;
    for (auto& band : eq_) for (auto& c : band) c.reset();
    for (auto& b : eqSeen_) for (float& v : b) v = -1.0e9f;
    std::memset(in, 0, sizeof in);
    std::memset(pre, 0, sizeof pre);
    std::memset(side, 0, sizeof side);
    std::memset(out, 0, sizeof out);
    hasSide = false;
    resetPeaks();
}

float TrackLive::faderToGain(float v) { return v <= 0.0f ? 0.0f : std::pow(v / 0.8f, 2.89f); }
float TrackLive::gainToFader(float g) { return g <= 0.0f ? 0.0f : clampT(0.8f * std::pow(g, 1.0f / 2.89f), 0.0f, 1.0f); }

void TrackLive::rtProcess(const SlotRt* slots, double sr, double tempo, double tick, bool playing) {
    float* L = out[0];
    float* R = out[1];
    std::memcpy(L, in[0], sizeof(float) * kQuantum);
    std::memcpy(R, in[1], sizeof(float) * kQuantum);

    // effect slots, each with its own bypass and wet/dry mix
    for (int s = 0; s < kNumSlots; ++s) {
        Plugin* p = slots[s].plugin;
        if (!p) continue;
        const bool bypass = slotBypass[s].getBool();
        const float mix = slotMix[s].get();
        const int lat = std::min(slots[s].latency, kMaxPdcSamples);
        float dry[2][kQuantum];
        const bool needDry = bypass || mix < 1.0f;
        if (needDry || (lat > 0 && slots[s].dry)) {
            std::memcpy(dry[0], L, sizeof(float) * kQuantum);
            std::memcpy(dry[1], R, sizeof(float) * kQuantum);
            // the dry path waits as long as the plugin takes, so wet and dry stay in phase
            if (lat > 0 && slots[s].dry) slots[s].dry->process(dry[0], dry[1], kQuantum, lat);
        }
        if (!bypass) {
            ProcessContext c;
            c.numSamples = kQuantum;
            c.sampleRate = sr;
            c.tempo = tempo;
            c.tickPos = tick;
            c.playing = playing;
            c.io[0] = L;
            c.io[1] = R;
            if (hasSide) { c.sidechain[0] = side[0]; c.sidechain[1] = side[1]; }
            p->process(c);
            if (mix < 1.0f)
                for (int i = 0; i < kQuantum; ++i) { L[i] = dry[0][i] + (L[i] - dry[0][i]) * mix; R[i] = dry[1][i] + (R[i] - dry[1][i]) * mix; }
        } else {
            std::memcpy(L, dry[0], sizeof(float) * kQuantum);
            std::memcpy(R, dry[1], sizeof(float) * kQuantum);
        }
    }

    // three-band EQ after the slots; skipped entirely while every band is flat
    bool anyEq = false;
    for (int b = 0; b < 3; ++b) anyEq = anyEq || eqGain[b].get() != 0.0f;
    if (anyEq) {
        for (int b = 0; b < 3; ++b) {
            const float f = eqFreq[b].get(), g = eqGain[b].get(), q = eqQ[b].get();
            if (f != eqSeen_[b][0] || g != eqSeen_[b][1] || q != eqSeen_[b][2]) {
                eqSeen_[b][0] = f; eqSeen_[b][1] = g; eqSeen_[b][2] = q;
                for (int c = 0; c < 2; ++c) {
                    if (b == 0) eq_[b][c].lowShelf(f, q, g, sr);
                    else if (b == 1) eq_[b][c].peak(f, q, g, sr);
                    else eq_[b][c].highShelf(f, q, g, sr);
                }
            }
            if (g == 0.0f) continue;
            for (int i = 0; i < kQuantum; ++i) { L[i] = eq_[b][0].process(L[i]); R[i] = eq_[b][1].process(R[i]); }
        }
    }

    if (swap.getBool()) for (int i = 0; i < kQuantum; ++i) std::swap(L[i], R[i]);

    // stereo separation, pan, fader, mute, phase
    const float sign = phase.getBool() ? -1.0f : 1.0f;
    const float g = mute.getBool() ? 0.0f : faderToGain(volume.get());
    const float p = pan.get();
    const float tl = sign * g * (p > 0.0f ? 1.0f - p : 1.0f), tr = sign * g * (p < 0.0f ? 1.0f + p : 1.0f);
    const float ts = 1.0f - stereoSep.get();        // gain of the side signal
    if (!primed_) { gL_.snap(tl); gR_.snap(tr); sep_.snap(ts); primed_ = true; }
    gL_.target(tl, kQuantum);
    gR_.target(tr, kQuantum);
    sep_.target(ts, kQuantum);
    const bool unity = gL_.left == 0 && gR_.left == 0 && sep_.left == 0 && tl == 1.0f && tr == 1.0f && ts == 1.0f;
    float pl = 0.0f, pr = 0.0f;
    if (unity) {
        for (int i = 0; i < kQuantum; ++i) { pl = std::max(pl, std::fabs(L[i])); pr = std::max(pr, std::fabs(R[i])); }
    } else {
        for (int i = 0; i < kQuantum; ++i) {
            const float sg = sep_.next();
            if (sg != 1.0f) {
                const float m = 0.5f * (L[i] + R[i]), sd = 0.5f * (L[i] - R[i]) * sg;
                L[i] = m + sd; R[i] = m - sd;
            }
            L[i] *= gL_.next();
            R[i] *= gR_.next();
            pl = std::max(pl, std::fabs(L[i]));
            pr = std::max(pr, std::fabs(R[i]));
        }
    }
    if (pl > peakL.load(std::memory_order_relaxed)) peakL.store(pl, std::memory_order_relaxed);
    if (pr > peakR.load(std::memory_order_relaxed)) peakR.store(pr, std::memory_order_relaxed);
}

// ------------------------------------------------------------------ WorkerPool

WorkerPool::WorkerPool(int threads) {
    for (int i = 1; i < threads; ++i) workers_.emplace_back([this] { loop(); });
}

WorkerPool::~WorkerPool() {
    quit_.store(true);
    wake_.fetch_add(1);
    wake_.notify_all();
    for (auto& t : workers_) t.join();
}

void WorkerPool::work() {
    const std::uint64_t mask = (1u << 20) - 1;
    for (;;) {
        std::uint64_t s = state_.load(std::memory_order_acquire);
        const std::uint64_t next = s & mask, count = (s >> 20) & mask;
        if (next >= count) break;
        if (!state_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel)) continue;
        fn_.load(std::memory_order_acquire)(ctx_.load(std::memory_order_acquire), (int)next);
        remaining_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void WorkerPool::loop() {
    unsigned seen = wake_.load();
    while (!quit_.load(std::memory_order_relaxed)) {
        // spin briefly before sleeping: the next quantum usually arrives within microseconds
        unsigned now = seen;
        for (int spin = 0; spin < 300 && (now = wake_.load(std::memory_order_acquire)) == seen; ++spin) std::this_thread::yield();
        if (now == seen) { wake_.wait(seen); now = wake_.load(std::memory_order_acquire); }
        seen = now;
        if (quit_.load(std::memory_order_relaxed)) break;
        Rt::Scope rt;
        work();
    }
}

} // namespace pulp
