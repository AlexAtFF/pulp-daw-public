#include "pulp/Render.h"

namespace pulp {

std::shared_ptr<SampleData> renderOffline(Project& project, const RenderOptions& opt,
                                          const std::function<bool(double)>& progress) {
    project.prepare(opt.sampleRate);
    Engine engine;
    engine.prepare(opt.sampleRate);
    auto state = project.compile();
    engine.setState(state);
    engine.setThreads(opt.threads);
    engine.setLooping(opt.loop);
    engine.setOffline(true);            // streamed audio is read from disk as it is needed: nothing is ever missing from an export
    // Plugin delay compensation makes the whole mix late by a fixed amount: drop that much
    // from the start so the file lines up with the song.
    const int latency = state->latency;
    {
        std::vector<float> skipL((size_t)std::max(1, latency)), skipR((size_t)std::max(1, latency));
        float* skip[2] = {skipL.data(), skipR.data()};
        if (opt.startTick > 0.0) engine.seek(opt.startTick);
        engine.play();
        if (latency > 0) engine.process(nullptr, 0, skip, 2, latency);
    }
    auto out = std::make_shared<SampleData>();
    out->sampleRate = opt.sampleRate;
    out->channels = 2;
    const int block = std::max(1, opt.blockSize);
    std::vector<float> l((size_t)block), r((size_t)block);
    float* chans[2] = {l.data(), r.data()};

    const double tempo = project.global->tempo.get();
    const double spt = 60.0 * opt.sampleRate / (tempo * kPPQ);
    const double lengthTicks = project.songMode ? (double)project.songLengthTicks()
                                                : (double)project.patternLengthTicks(project.pattern());
    // Where the transport stops, and how the tail is cut: both are worked out per sample,
    // so the result does not depend on the block size.
    int64 expected = opt.lengthSamples > 0 ? opt.lengthSamples : (int64)std::llround((lengthTicks - opt.startTick) * spt);
    bool tempoAutomated = false;
    for (auto& t : state->autoTargets) tempoAutomated = tempoAutomated || t.param == &project.global->tempo;
    for (auto& l : state->links) tempoAutomated = tempoAutomated || l.param == &project.global->tempo;
    const int64 maxTail = (int64)(opt.maxTailSeconds * opt.sampleRate);
    const int64 quietNeeded = (int64)(0.2 * opt.sampleRate);
    const float silence = 1.0e-5f;      // -100 dB
    int64 lastLoud = -1;

    for (;;) {
        int n = block;
        if (opt.lengthSamples > 0) {
            const int64 left = opt.lengthSamples - out->frames();
            if (left <= 0) break;
            n = (int)std::min<int64>(n, left);
        }
        const int64 base = out->frames();
        engine.process(nullptr, 0, chans, 2, n);
        out->ch[0].insert(out->ch[0].end(), l.begin(), l.begin() + n);
        out->ch[1].insert(out->ch[1].end(), r.begin(), r.begin() + n);
        if (progress && !progress(std::min(1.0, (double)out->frames() / (double)std::max<int64>(1, expected)))) return nullptr;
        if (opt.lengthSamples > 0) continue;
        for (int i = 0; i < n; ++i)
            if (std::fabs(l[(size_t)i]) >= silence || std::fabs(r[(size_t)i]) >= silence) lastLoud = base + i;
        // with the tempo automated the song's length in samples is only known once the transport reaches its end
        // (the transport ends at that count of samples; what it played last comes out `latency` samples later, and
        // that many were dropped from the start, so the song's last sample is the file's sample of the same number)
        if (tempoAutomated && engine.endedAtClock() >= 0) expected = engine.endedAtClock();
        else if (tempoAutomated) continue;
        const int64 frames = out->frames();
        if (frames < expected) continue;
        const int64 hardEnd = expected + maxTail;
        const int64 quietEnd = std::max(expected, lastLoud + 1 + quietNeeded);
        const int64 end = opt.tail == RenderOptions::Cut ? expected : std::min(hardEnd, quietEnd);
        if (frames >= end) {
            out->ch[0].resize((size_t)end);
            out->ch[1].resize((size_t)end);
            break;
        }
    }
    if (opt.tail == RenderOptions::Wrap && opt.lengthSamples <= 0 && out->frames() > expected && expected > 0) {
        // fold what rings on after the last bar back onto the beginning, so the file loops seamlessly
        for (int c = 0; c < 2; ++c) {
            auto& d = out->ch[c];
            for (int64 i = expected; i < (int64)d.size(); ++i) d[(size_t)((i - expected) % expected)] += d[(size_t)i];
            d.resize((size_t)expected);
        }
    }
    return out;
}

} // namespace pulp
