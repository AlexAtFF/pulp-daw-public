#pragma once
// Synthesized drum sounds for the factory library. The 808 and 909 were analogue
// synthesizers, so these are built from oscillators, noise and envelopes, not recordings.
#include "pulp/Audio.h"

namespace pulp::drums {

struct KickSpec {
    double startHz = 160.0, endHz = 48.0;
    double pitchDecay = 0.035;      // seconds for the pitch sweep to settle
    double ampDecay = 0.22;         // seconds
    double click = 0.25;            // 0..1 transient level
    double drive = 0.0;             // 0..1 saturation
    double length = 0.45;           // seconds
};
std::shared_ptr<SampleData> makeKick(const KickSpec& spec, double sampleRate = 48000.0);

struct SnareSpec {
    double bodyHz = 185.0, body2Hz = 330.0;
    double bodyDecay = 0.09, noiseDecay = 0.16;
    double noiseLevel = 0.75, tone = 6500.0, snap = 0.3;
    double length = 0.4;
};
std::shared_ptr<SampleData> makeSnare(const SnareSpec& spec, double sampleRate = 48000.0);

struct ClapSpec {
    double centreHz = 1100.0, q = 1.6, tailDecay = 0.12, spread = 0.011;
    int bursts = 4;
    double length = 0.45;
};
std::shared_ptr<SampleData> makeClap(const ClapSpec& spec, double sampleRate = 48000.0);

struct HatSpec {
    double decay = 0.045;           // closed about 0.045, open about 0.35
    double highpass = 7000.0, bandHz = 9500.0;
    double noiseMix = 0.0;          // 0 = all metallic oscillators (808), toward 1 = noisier (909)
    double length = 0.2;
};
std::shared_ptr<SampleData> makeHat(const HatSpec& spec, double sampleRate = 48000.0);

struct TomSpec {
    double hz = 120.0, sweep = 1.5, pitchDecay = 0.03, ampDecay = 0.2, noise = 0.05, length = 0.5;
};
std::shared_ptr<SampleData> makeTom(const TomSpec& spec, double sampleRate = 48000.0);

std::shared_ptr<SampleData> makeCowbell(double sampleRate = 48000.0);
std::shared_ptr<SampleData> makeRim(double sampleRate = 48000.0, bool bright = false);
std::shared_ptr<SampleData> makeClave(double sampleRate = 48000.0);
std::shared_ptr<SampleData> makeMaracas(double sampleRate = 48000.0);
std::shared_ptr<SampleData> makeCymbal(double decay, double brightness, double sampleRate = 48000.0);

struct FactoryFile {
    std::string path;       // relative to the factory folder
    std::string source;     // how it was made
    std::string licence;
    std::shared_ptr<SampleData> audio;
};
// Everything PULP synthesizes for the factory library.
std::vector<FactoryFile> makeFactoryDrums(double sampleRate = 48000.0);

} // namespace pulp::drums
