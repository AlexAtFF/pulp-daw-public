#include "pulp/DrumSynth.h"
#include "pulp/Dsp.h"

namespace pulp::drums {

using dsp::Biquad;
using dsp::Noise;

static std::shared_ptr<SampleData> blank(double seconds, double sr) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = 1;
    s->ch[0].assign((size_t)(seconds * sr), 0.0f);
    return s;
}

static void finish(SampleData& s, double peak = 0.9) {
    auto& d = s.ch[0];
    const int n = (int)d.size();
    const int fade = std::min(n, (int)(0.012 * s.sampleRate));
    for (int i = 0; i < fade; ++i) d[(size_t)(n - 1 - i)] *= (float)i / (float)fade;
    float pk = 0.0f;
    for (float v : d) pk = std::max(pk, std::fabs(v));
    if (pk > 1.0e-6f) for (float& v : d) v = (float)(v * peak / pk);
}

std::shared_ptr<SampleData> makeKick(const KickSpec& k, double sr) {
    auto s = blank(k.length, sr);
    double phase = 0.0;
    for (size_t i = 0; i < s->ch[0].size(); ++i) {
        const double t = (double)i / sr;
        const double hz = k.endHz + (k.startHz - k.endHz) * std::exp(-t / k.pitchDecay);
        phase += kTwoPi * hz / sr;
        double v = std::sin(phase) * std::exp(-t / k.ampDecay);
        v += k.click * std::sin(kTwoPi * 1800.0 * t) * std::exp(-t / 0.002);     // beater click
        if (k.drive > 0.0) { const double g = 1.0 + k.drive * 6.0; v = std::tanh(v * g) / std::tanh(g); }
        s->ch[0][i] = (float)v;
    }
    finish(*s);
    return s;
}

std::shared_ptr<SampleData> makeSnare(const SnareSpec& p, double sr) {
    auto s = blank(p.length, sr);
    Noise nz(0x51A7E001u);
    Biquad hp, lp;
    hp.highpass(700.0, 0.7, sr);
    lp.lowpass(p.tone, 0.7, sr);
    double ph1 = 0, ph2 = 0;
    for (size_t i = 0; i < s->ch[0].size(); ++i) {
        const double t = (double)i / sr;
        const double sweep = 1.0 + p.snap * std::exp(-t / 0.012);
        ph1 += kTwoPi * p.bodyHz * sweep / sr;
        ph2 += kTwoPi * p.body2Hz * sweep / sr;
        const double body = (std::sin(ph1) + 0.55 * std::sin(ph2)) * std::exp(-t / p.bodyDecay);
        const double noise = lp.process(hp.process(nz.next())) * std::exp(-t / p.noiseDecay);
        s->ch[0][i] = (float)(0.7 * body + p.noiseLevel * noise);
    }
    finish(*s);
    return s;
}

std::shared_ptr<SampleData> makeClap(const ClapSpec& p, double sr) {
    auto s = blank(p.length, sr);
    Noise nz(0xC1A9C1A9u);
    Biquad bp, hp;
    bp.bandpass(p.centreHz, p.q, sr);
    hp.highpass(500.0, 0.7, sr);
    for (size_t i = 0; i < s->ch[0].size(); ++i) {
        const double t = (double)i / sr;
        double env = 0.0;
        for (int b = 0; b < p.bursts - 1; ++b) {                 // quick retriggers: several hands
            const double bt = t - b * p.spread;
            if (bt >= 0.0 && bt < p.spread) env = std::max(env, std::exp(-bt / 0.0045));
        }
        const double tt = t - (p.bursts - 1) * p.spread;        // then the tail
        if (tt >= 0.0) env = std::max(env, std::exp(-tt / p.tailDecay));
        s->ch[0][i] = (float)(hp.process(bp.process(nz.next())) * env);
    }
    finish(*s);
    return s;
}

// Six square oscillators at inharmonic ratios: the classic metallic source.
static double metal(double t, double scale) {
    static const double f[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
    double v = 0.0;
    for (double hz : f) v += std::fmod(t * hz * scale, 1.0) < 0.5 ? 1.0 : -1.0;
    return v / 6.0;
}

std::shared_ptr<SampleData> makeHat(const HatSpec& p, double sr) {
    auto s = blank(p.length, sr);
    Noise nz(0x4A7D00D5u);
    Biquad bp, hp, hp2;
    bp.bandpass(p.bandHz, 1.2, sr);
    hp.highpass(p.highpass, 0.8, sr);
    hp2.highpass(p.highpass, 0.8, sr);
    for (size_t i = 0; i < s->ch[0].size(); ++i) {
        const double t = (double)i / sr;
        const double src = (1.0 - p.noiseMix) * metal(t, 1.0) * 1.6 + p.noiseMix * nz.next();
        const double v = hp2.process(hp.process(bp.process((float)src) + 0.3f * (float)src));
        const double env = std::exp(-t / p.decay) * std::min(1.0, t / 0.0003);
        s->ch[0][i] = (float)(v * env);
    }
    finish(*s, 0.8);
    return s;
}

std::shared_ptr<SampleData> makeCymbal(double decay, double brightness, double sr) {
    auto s = blank(std::min(4.0, decay * 5.0), sr);
    Noise nz(0xC7B0A111u);
    Biquad lo, hi, hp;
    lo.bandpass(3600.0, 1.0, sr);
    hi.bandpass(8500.0 * brightness, 0.9, sr);
    hp.highpass(2200.0, 0.7, sr);
    for (size_t i = 0; i < s->ch[0].size(); ++i) {
        const double t = (double)i / sr;
        const float src = (float)(metal(t, 1.0) * 1.4 + metal(t, 1.513) * 0.9 + 0.35 * nz.next());
        const double v = lo.process(src) * std::exp(-t / (decay * 0.5)) + hi.process(src) * std::exp(-t / decay);
        s->ch[0][i] = (float)(hp.process((float)v) * std::min(1.0, t / 0.0005));
    }
    finish(*s, 0.8);
    return s;
}

std::shared_ptr<SampleData> makeTom(const TomSpec& p, double sr) {
    auto s = blank(p.length, sr);
    Noise nz(0x70A570A5u);
    Biquad lp;
    lp.lowpass(p.hz * 6.0, 0.7, sr);
    double ph = 0.0;
    for (size_t i = 0; i < s->ch[0].size(); ++i) {
        const double t = (double)i / sr;
        const double hz = p.hz * (1.0 + (p.sweep - 1.0) * std::exp(-t / p.pitchDecay));
        ph += kTwoPi * hz / sr;
        const double v = std::sin(ph) * std::exp(-t / p.ampDecay) + p.noise * lp.process(nz.next()) * std::exp(-t / 0.03);
        s->ch[0][i] = (float)v;
    }
    finish(*s);
    return s;
}

std::shared_ptr<SampleData> makeCowbell(double sr) {
    auto s = blank(0.45, sr);
    Biquad bp;
    bp.bandpass(820.0, 2.2, sr);
    for (size_t i = 0; i < s->ch[0].size(); ++i) {
        const double t = (double)i / sr;
        const double sq = (std::fmod(t * 540.0, 1.0) < 0.5 ? 1.0 : -1.0) + (std::fmod(t * 800.0, 1.0) < 0.5 ? 1.0 : -1.0);
        const double env = 0.7 * std::exp(-t / 0.012) + 0.3 * std::exp(-t / 0.11);
        s->ch[0][i] = (float)(bp.process((float)sq) * env);
    }
    finish(*s, 0.8);
    return s;
}

std::shared_ptr<SampleData> makeRim(double sr, bool bright) {
    auto s = blank(0.12, sr);
    Biquad hp;
    hp.highpass(bright ? 600.0 : 350.0, 0.7, sr);
    for (size_t i = 0; i < s->ch[0].size(); ++i) {
        const double t = (double)i / sr;
        const double v = std::sin(kTwoPi * (bright ? 1900.0 : 1667.0) * t) * std::exp(-t / 0.006)
                       + 0.6 * std::sin(kTwoPi * (bright ? 520.0 : 455.0) * t) * std::exp(-t / 0.012)
                       + 0.4 * std::sin(kTwoPi * 3200.0 * t) * std::exp(-t / 0.002);
        s->ch[0][i] = hp.process((float)v);
    }
    finish(*s, 0.85);
    return s;
}

std::shared_ptr<SampleData> makeClave(double sr) {
    auto s = blank(0.15, sr);
    for (size_t i = 0; i < s->ch[0].size(); ++i) {
        const double t = (double)i / sr;
        s->ch[0][i] = (float)(std::sin(kTwoPi * 2500.0 * t) * std::exp(-t / 0.018));
    }
    finish(*s, 0.8);
    return s;
}

std::shared_ptr<SampleData> makeMaracas(double sr) {
    auto s = blank(0.09, sr);
    Noise nz(0x3A2AC455u);
    Biquad hp;
    hp.highpass(6000.0, 0.8, sr);
    for (size_t i = 0; i < s->ch[0].size(); ++i) {
        const double t = (double)i / sr;
        const double env = std::min(1.0, t / 0.008) * std::exp(-std::max(0.0, t - 0.008) / 0.012);
        s->ch[0][i] = (float)(hp.process(nz.next()) * env);
    }
    finish(*s, 0.6);
    return s;
}

std::vector<FactoryFile> makeFactoryDrums(double sr) {
    std::vector<FactoryFile> out;
    const std::string src = "Synthesized by PULP (engine/src/DrumSynth.cpp)";
    const std::string lic = "CC0-1.0";
    // Each sound is levelled so a kit sits together without clipping when several hit at once.
    auto levelFor = [](const std::string& path) {
        static const std::pair<const char*, float> table[] = {
            {"Kick", 0.85f}, {"Snare", 0.62f}, {"Clap", 0.5f}, {"Hat", 0.34f}, {"Cymbal", 0.4f}, {"Ride", 0.38f}, {"Crash", 0.42f},
            {"Tom", 0.65f}, {"Conga", 0.55f}, {"Cowbell", 0.42f}, {"Rim", 0.5f}, {"Clave", 0.45f}, {"Maracas", 0.3f}};
        for (auto& t : table) if (path.find(t.first) != std::string::npos) return t.second;
        return 0.6f;
    };
    auto add = [&](const std::string& path, std::shared_ptr<SampleData> a) {
        const float pk = a->peak(), want = levelFor(path);
        if (pk > 1.0e-6f) for (float& v : a->ch[0]) v *= want / pk;
        out.push_back({path, src, lic, std::move(a)});
    };

    // ---- 808-style kit: long sine kick, metallic oscillator hats ----
    const std::string k8 = "Drums/808 Kit/";
    { KickSpec k; k.startHz = 130; k.endHz = 47; k.pitchDecay = 0.045; k.ampDecay = 0.42; k.click = 0.18; k.length = 0.95; add(k8 + "808 Kick.wav", makeKick(k, sr)); }
    { KickSpec k; k.startHz = 120; k.endHz = 44; k.pitchDecay = 0.05; k.ampDecay = 0.9; k.click = 0.12; k.drive = 0.12; k.length = 2.0; add(k8 + "808 Kick Long.wav", makeKick(k, sr)); }
    { SnareSpec s; add(k8 + "808 Snare.wav", makeSnare(s, sr)); }
    { ClapSpec c; add(k8 + "808 Clap.wav", makeClap(c, sr)); }
    { HatSpec h; h.decay = 0.04; h.length = 0.18; add(k8 + "808 Hat Closed.wav", makeHat(h, sr)); }
    { HatSpec h; h.decay = 0.3; h.length = 0.9; add(k8 + "808 Hat Open.wav", makeHat(h, sr)); }
    add(k8 + "808 Cymbal.wav", makeCymbal(0.55, 1.0, sr));
    { TomSpec t; t.hz = 90; t.ampDecay = 0.28; t.length = 0.7; add(k8 + "808 Tom Low.wav", makeTom(t, sr)); }
    { TomSpec t; t.hz = 130; t.ampDecay = 0.24; t.length = 0.6; add(k8 + "808 Tom Mid.wav", makeTom(t, sr)); }
    { TomSpec t; t.hz = 180; t.ampDecay = 0.2; t.length = 0.5; add(k8 + "808 Tom High.wav", makeTom(t, sr)); }
    { TomSpec t; t.hz = 230; t.sweep = 1.15; t.ampDecay = 0.12; t.noise = 0.0; t.length = 0.3; add(k8 + "808 Conga.wav", makeTom(t, sr)); }
    add(k8 + "808 Cowbell.wav", makeCowbell(sr));
    add(k8 + "808 Rimshot.wav", makeRim(sr, false));
    add(k8 + "808 Clave.wav", makeClave(sr));
    add(k8 + "808 Maracas.wav", makeMaracas(sr));

    // ---- 909-style kit: punchier driven kick, noisier hats, brighter snare ----
    const std::string k9 = "Drums/909 Kit/";
    { KickSpec k; k.startHz = 260; k.endHz = 52; k.pitchDecay = 0.022; k.ampDecay = 0.2; k.click = 0.45; k.drive = 0.35; k.length = 0.5; add(k9 + "909 Kick.wav", makeKick(k, sr)); }
    { SnareSpec s; s.bodyHz = 210; s.body2Hz = 390; s.bodyDecay = 0.06; s.noiseDecay = 0.2; s.noiseLevel = 0.9; s.tone = 9000; s.snap = 0.6; add(k9 + "909 Snare.wav", makeSnare(s, sr)); }
    { ClapSpec c; c.centreHz = 1300; c.tailDecay = 0.2; c.length = 0.7; add(k9 + "909 Clap.wav", makeClap(c, sr)); }
    { HatSpec h; h.decay = 0.035; h.noiseMix = 0.55; h.bandHz = 10500; h.length = 0.16; add(k9 + "909 Hat Closed.wav", makeHat(h, sr)); }
    { HatSpec h; h.decay = 0.4; h.noiseMix = 0.55; h.bandHz = 10500; h.length = 1.2; add(k9 + "909 Hat Open.wav", makeHat(h, sr)); }
    add(k9 + "909 Ride.wav", makeCymbal(0.7, 0.8, sr));
    add(k9 + "909 Crash.wav", makeCymbal(0.9, 1.25, sr));
    { TomSpec t; t.hz = 85; t.sweep = 2.2; t.pitchDecay = 0.04; t.ampDecay = 0.3; t.length = 0.8; add(k9 + "909 Tom Low.wav", makeTom(t, sr)); }
    { TomSpec t; t.hz = 125; t.sweep = 2.2; t.pitchDecay = 0.035; t.ampDecay = 0.25; t.length = 0.7; add(k9 + "909 Tom Mid.wav", makeTom(t, sr)); }
    { TomSpec t; t.hz = 175; t.sweep = 2.2; t.pitchDecay = 0.03; t.ampDecay = 0.2; t.length = 0.6; add(k9 + "909 Tom High.wav", makeTom(t, sr)); }
    add(k9 + "909 Rimshot.wav", makeRim(sr, true));
    return out;
}

} // namespace pulp::drums
