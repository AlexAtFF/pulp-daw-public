// pulp_render --stress: builds a large project in memory and plays it the way the sound card would
// (fixed small blocks, the mixer's worker threads, no look-ahead), timing every block. It answers the
// question "would this project play without dropouts at this buffer size on this PC".
#include "pulp/Engine.h"
#include "pulp/Project.h"
#include "pulp/Sampler.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace pulp;

namespace {
SamplePtr burst(double hz, double seconds, double decay) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = 48000.0;
    s->channels = 1;
    s->ch[0].resize((size_t)(seconds * 48000.0));
    for (size_t i = 0; i < s->ch[0].size(); ++i) {
        const double t = (double)i / 48000.0;
        s->ch[0][i] = (float)(0.6 * std::sin(kTwoPi * hz * t) * std::exp(-t * decay));
    }
    return s;
}
}

int runStress(int numChannels, int numTracks, int numEffects, double seconds, int threads, int block) {
    numChannels = clampT(numChannels, 1, 500);
    numTracks = clampT(numTracks, 1, kNumInserts);
    numEffects = clampT(numEffects, 0, numTracks * kNumSlots);
    const double sr = 48000.0;
    Project p;
    p.sampleRate = sr;
    p.global->tempo.set(128.0f);
    // ---- channels: every kind of instrument, in rotation, each on a mixer track ----
    const char* kinds[] = {"sampler", "trio", "zest", "fm6", "wavetable", "acid", "pluck", "bass", "kick", "drumsynth"};
    const int numKinds = (int)(sizeof kinds / sizeof kinds[0]);
    const int chord[4][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}};
    p.pattern().steps = 64;
    int perKind[16] = {};
    for (int i = 0; i < numChannels; ++i) {
        const int kind = i % numKinds;
        std::shared_ptr<Plugin> plug;
        if (kind == 0) { auto s = std::make_shared<Sampler>(); s->setSample(burst(80.0 + 40.0 * (i % 7), 0.4, 9.0)); plug = s; }
        else plug = PluginRegistry::global().create(kinds[kind]);
        if (!plug) continue;
        ++perKind[kind];
        // a different preset on each, so that the load is not one patch a hundred times
        const auto presets = plug->factoryPresets();
        if (!presets.empty()) plug->loadState(presets[(size_t)(i / numKinds) % presets.size()].state);
        auto& c = p.addChannel(plug, std::string(kinds[kind]) + " " + std::to_string(i + 1));
        c.mixerTrack = 1 + i % numTracks;
        c.live->volume.set(0.25f);
        auto& notes = p.pattern().notesFor(c.id);
        const bool drum = kind == 0 || kind == 8 || kind == 9;
        for (int bar = 0; bar < 4; ++bar) {
            if (drum) {
                for (int s = (i % 4); s < 16; s += 4) { Note n; n.tick = bar * 3840 + s * kTicksPerStep; n.length = kTicksPerStep; n.key = kRootKey; notes.push_back(n); }
            } else if (kind == 5 || kind == 7) {
                for (int s = 0; s < 16; s += 2) { Note n; n.tick = bar * 3840 + s * kTicksPerStep; n.length = kTicksPerStep + 120; n.key = chord[bar][0] + 12; notes.push_back(n); }
            } else {
                // a held three-note chord, and a moving line on top
                for (int k : chord[bar]) { Note n; n.tick = bar * 3840; n.length = 3800; n.key = k + 12 * (i % 2); n.velocity = 0.6f; notes.push_back(n); }
                for (int s = 0; s < 16; s += 4) { Note n; n.tick = bar * 3840 + s * kTicksPerStep + 120; n.length = 400; n.key = chord[bar][s / 4 % 3] + 24; n.velocity = 0.5f; notes.push_back(n); }
            }
        }
        sortNotes(notes);
    }
    // ---- effects: every kind, in rotation, spread over the tracks ----
    std::vector<std::string> fx;
    for (auto& t : PluginRegistry::global().types())
        if (!t.instrument && t.id != "editor" && t.id.rfind("ctl_", 0) != 0) fx.push_back(t.id);
    int placed = 0;
    for (int e = 0; e < numEffects; ++e) {
        const int track = 1 + e % numTracks, slot = e / numTracks;
        if (slot >= kNumSlots) break;
        auto plug = PluginRegistry::global().create(fx[(size_t)e % fx.size()]);
        if (plug && p.setSlot(track, slot, plug)) ++placed;
    }
    p.setSlot(kMasterTrack, 0, PluginRegistry::global().create("limiter"));

    p.prepare(sr);
    Engine engine;
    engine.prepare(sr);
    auto state = p.compile();
    engine.setState(state);
    engine.setThreads(threads);
    engine.setLooping(true);
    engine.play();
    std::vector<float> l((size_t)block), r((size_t)block);
    float* out[2] = {l.data(), r.data()};
    const double budget = (double)block / sr;
    const int64 blocks = (int64)(seconds * sr / block);
    std::vector<double> times;
    times.reserve((size_t)blocks);
    int maxVoices = 0;
    double peak = 0.0;
    bool finite = true;
    // the first second is a warm-up (caches, the first notes of every channel) and is timed with the rest
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (int64 b = 0; b < blocks; ++b) {
        const auto t0 = clock::now();
        engine.process(nullptr, 0, out, 2, block);
        const auto t1 = clock::now();
        times.push_back(std::chrono::duration<double>(t1 - t0).count());
        maxVoices = std::max(maxVoices, engine.activeVoices());
        for (int i = 0; i < block; ++i) { if (!std::isfinite(l[(size_t)i])) finite = false; peak = std::max(peak, (double)std::abs(l[(size_t)i])); }
        // keep to the pace of a sound card, so that the worker threads sleep and wake as they would
        next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(budget));
        std::this_thread::sleep_until(next);
        if (clock::now() - next > std::chrono::milliseconds(200)) next = clock::now();
    }
    std::vector<double> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    int over = 0;
    for (double t : times) { sum += t; over += t > budget ? 1 : 0; }
    const double avg = sum / (double)times.size();
    auto pct = [&](double q) { return sorted[(size_t)std::min<double>((double)sorted.size() - 1, q * (double)sorted.size())]; };
    printf("stress: %d channels (", (int)p.channels.size());
    for (int k = 0; k < numKinds; ++k) printf("%s%d %s", k ? ", " : "", perKind[k], kinds[k]);
    printf("), %d mixer tracks, %d effects of %d kinds, %d threads, %d-sample blocks at %.0f Hz, %.0f s\n",
           numTracks, placed, (int)fx.size(), threads, block, sr, seconds);
    printf("  the block has %.3f ms; it took %.3f ms on average (%.1f %% of the time available), median %.3f, 99th percentile %.3f, 99.9th %.3f, worst %.3f ms\n",
           budget * 1000.0, avg * 1000.0, 100.0 * avg / budget, pct(0.5) * 1000.0, pct(0.99) * 1000.0, pct(0.999) * 1000.0, sorted.back() * 1000.0);
    printf("  blocks that took longer than they had (each would be a dropout): %d of %d; most voices at once %d; delay compensation %d samples; peak %.3f%s\n",
           over, (int)times.size(), maxVoices, state->latency, peak, finite ? "" : "  NOT FINITE");
    return over == 0 && finite ? 0 : 3;
}
