#include "pulp/NoteSynth.h"
#include "pulp/Dsp.h"

namespace pulp::notesynth {

namespace {

std::shared_ptr<SampleData> blank(double seconds, double sr, int channels = 1) {
    auto s = std::make_shared<SampleData>();
    s->sampleRate = sr;
    s->channels = channels;
    for (int c = 0; c < channels; ++c) s->ch[c].assign((size_t)(seconds * sr), 0.0f);
    return s;
}
void normalise(SampleData& s, float peak = 0.8f) {
    const float p = s.peak();
    if (p <= 0.0f) return;
    for (int c = 0; c < s.channels; ++c) for (auto& v : s.ch[c]) v *= peak / p;
}
void fadeOut(SampleData& s, double seconds) {
    const int64 n = s.frames(), f = std::min<int64>(n, (int64)(seconds * s.sampleRate));
    for (int c = 0; c < s.channels; ++c)
        for (int64 i = 0; i < f; ++i) s.ch[c][(size_t)(n - 1 - i)] *= (float)i / (float)f;
}

// A plucked string (Karplus-Strong): a burst of noise goes round a delay line through a gentle low-pass.
//   bright 0..1: how sharp the pluck is.  decay: seconds for the note to fall by 60 dB at middle C.
//   pick 0..0.5: where along the string it is plucked (thins some overtones, as a real string does).
std::shared_ptr<SampleData> pluck(int key, double sr, double seconds, double bright, double decay, double pick, uint32 seed) {
    auto s = blank(seconds, sr);
    const double hz = keyToHz(key);
    const double period = sr / hz - 0.5;                    // the averaging filter in the loop is half a sample long
    const int len = (int)period;
    const double frac = period - len;
    std::vector<float> line((size_t)len + 2, 0.0f);
    dsp::Noise noise(seed);
    dsp::OnePole tone;
    tone.setHz(400.0 + bright * bright * 12000.0, sr);
    std::vector<float> burst((size_t)len + 2);
    for (auto& v : burst) v = tone.lp(noise.next());
    const int comb = std::max(1, (int)(pick * len));
    for (size_t i = 0; i < burst.size(); ++i) line[i] = burst[i] - (i >= (size_t)comb ? burst[i - (size_t)comb] : 0.0f) * 0.7f;
    // loss per trip round the loop so that the note decays in `decay` seconds (higher notes die sooner)
    const double t60 = decay * std::pow(2.0, -(key - 60) / 18.0);
    const float loss = (float)std::pow(10.0, -3.0 / (t60 * hz));
    const float damp = (float)(0.5 - 0.42 * (1.0 - bright));   // 0.5 is the plain average; less leaves the tone brighter for longer
    int w = 0;
    float last = 0.0f;
    const int n = (int)s->frames();
    for (int i = 0; i < n; ++i) {
        const int r0 = w, r1 = (w + 1) % (len + 1);
        // r0 is the oldest sample (len + 1 back), r1 the next: together a delay of len + frac
        const float x = (float)(line[(size_t)r0] * frac + line[(size_t)r1] * (1.0 - frac));
        const float y = loss * ((1.0f - damp) * x + damp * last);
        last = x;
        line[(size_t)w] = y;
        w = (w + 1) % (len + 1);
        s->ch[0][(size_t)i] = x;
    }
    return s;
}

// Additive tone: partials with their own ratio, level and decay time. For bars, bells, tines and pipes.
struct Partial { double ratio, level, decay; };
std::shared_ptr<SampleData> partials(int key, double sr, double seconds, const std::vector<Partial>& ps, double attack, bool held, double stretch = 1.0) {
    auto s = blank(seconds, sr);
    const double hz = keyToHz(key);
    const int n = (int)s->frames();
    for (size_t k = 0; k < ps.size(); ++k) {
        const double f = hz * ps[k].ratio;
        if (f > sr * 0.45) continue;
        // higher notes ring for less time
        const double decay = ps[k].decay * std::pow(2.0, -(key - 60) / (24.0 * stretch));
        const double w = kTwoPi * f / sr, ph0 = 0.37 * (double)k;
        for (int i = 0; i < n; ++i) {
            const double t = i / sr;
            const double env = held ? 1.0 : std::exp(-t * 6.9 / decay);
            s->ch[0][(size_t)i] += (float)(ps[k].level * env * std::sin(w * i + ph0));
        }
    }
    const int a = std::max(1, (int)(attack * sr));
    for (int i = 0; i < a && i < n; ++i) s->ch[0][(size_t)i] *= (float)i / (float)a;
    return s;
}

// Two-operator FM: an electric piano tine, bells. index falls away quickly, as the "bark" of a tine does.
std::shared_ptr<SampleData> fm(int key, double sr, double seconds, double ratio, double index, double indexDecay, double decay, double attack) {
    auto s = blank(seconds, sr);
    const double hz = keyToHz(key);
    const int n = (int)s->frames();
    const double keyScale = std::pow(2.0, -(key - 60) / 30.0);
    for (int i = 0; i < n; ++i) {
        const double t = i / sr;
        const double mod = index * keyScale * std::exp(-t * 6.9 / indexDecay) * std::sin(kTwoPi * hz * ratio * t);
        const double env = std::min(1.0, t / attack) * std::exp(-t * 6.9 / (decay * keyScale));
        s->ch[0][(size_t)i] = (float)(env * std::sin(kTwoPi * hz * t + mod));
    }
    return s;
}

// A voice singing a vowel: a buzzing source through the vowel's resonances, several singers slightly apart.
std::shared_ptr<SampleData> choir(int key, double sr, double seconds, bool high, uint32 seed) {
    auto s = blank(seconds, sr, 2);
    const int n = (int)s->frames();
    // "aah": resonances for lower and higher voices (Hz, width, level)
    static const double lowF[5][3] = {{650, 80, 1.0}, {1080, 90, 0.5}, {2650, 120, 0.35}, {2900, 130, 0.25}, {3250, 140, 0.1}};
    static const double highF[5][3] = {{800, 80, 1.0}, {1150, 90, 0.5}, {2900, 120, 0.3}, {3900, 130, 0.12}, {4950, 140, 0.04}};
    const auto& F = high ? highF : lowF;
    dsp::Noise rnd(seed);
    const int singers = 5;
    for (int v = 0; v < singers; ++v) {
        const double detune = (rnd.next()) * 0.09;                  // up to 9 cents apart
        const double vibRate = 5.0 + rnd.next() * 0.7, vibDepth = 0.12 + 0.05 * rnd.next(), vibPhase = rnd.uni() * kTwoPi;
        const float pan = (float)v / (float)(singers - 1);
        dsp::Biquad bq[5];
        for (int k = 0; k < 5; ++k) bq[k].bandpass(F[k][0] * (1.0 + 0.02 * rnd.next()), F[k][0] / F[k][1], sr);
        dsp::OnePole breathTone;
        breathTone.setHz(2500.0, sr);
        double phase = rnd.uni();
        const double hz0 = keyToHz(key + detune);
        for (int i = 0; i < n; ++i) {
            const double t = i / sr;
            // the vibrato comes in after a moment, as a singer's does
            const double vib = vibDepth * std::min(1.0, t / 0.6) * std::sin(kTwoPi * vibRate * t + vibPhase);
            const double hz = hz0 * std::pow(2.0, vib / 12.0);
            const double dt = hz / sr;
            phase += dt;
            if (phase >= 1.0) phase -= 1.0;
            // a rounded pulse: rich in overtones that fall away steadily (the sawtooth's step is smoothed by polyBLEP)
            double saw = 2.0 * phase - 1.0;
            if (phase < dt) { const double x = phase / dt; saw -= x + x - x * x - 1.0; }
            else if (phase > 1.0 - dt) { const double x = (phase - 1.0) / dt; saw -= x * x + x + x + 1.0; }
            const float src = (float)saw + 0.04f * breathTone.lp(rnd.next());
            float out = 0.0f;
            for (int k = 0; k < 5; ++k) out += (float)F[k][2] * bq[k].process(src);
            const float env = (float)(std::min(1.0, t / 0.18));
            s->ch[0][(size_t)i] += out * env * (1.0f - 0.6f * pan);
            s->ch[1][(size_t)i] += out * env * (0.4f + 0.6f * pan);
        }
    }
    return s;
}

// Several slightly detuned sawtooths through a low-pass: string and brass sections of a synthesizer.
std::shared_ptr<SampleData> saws(int key, double sr, double seconds, int voices, double detuneCents, double cutoffHz, double attack, uint32 seed) {
    auto s = blank(seconds, sr, 2);
    const int n = (int)s->frames();
    dsp::Noise rnd(seed);
    for (int v = 0; v < voices; ++v) {
        const double cents = voices > 1 ? detuneCents * (2.0 * v / (voices - 1) - 1.0) : 0.0;
        const double dt = keyToHz(key + cents / 100.0) / sr;
        double phase = rnd.uni();
        const float pan = voices > 1 ? (float)v / (float)(voices - 1) : 0.5f;
        dsp::Svf f;
        f.set((float)std::min(cutoffHz * std::pow(2.0, (key - 60) / 24.0), sr * 0.4), 0.8f, (float)sr);
        for (int i = 0; i < n; ++i) {
            phase += dt;
            if (phase >= 1.0) phase -= 1.0;
            double saw = 2.0 * phase - 1.0;
            if (phase < dt) { const double x = phase / dt; saw -= x + x - x * x - 1.0; }
            else if (phase > 1.0 - dt) { const double x = (phase - 1.0) / dt; saw -= x * x + x + x + 1.0; }
            const float y = f.lowpass((float)saw) * (float)std::min(1.0, (i / sr) / attack);
            s->ch[0][(size_t)i] += y * (1.0f - 0.7f * pan);
            s->ch[1][(size_t)i] += y * (0.3f + 0.7f * pan);
        }
    }
    return s;
}

// A sub-bass drum voice played as a bass: a sine that drops onto its note, driven a little so it has overtones.
std::shared_ptr<SampleData> bass808(int key, double sr) {
    auto s = blank(2.6, sr);
    const double hz = keyToHz(key);
    const int n = (int)s->frames();
    double phase = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = i / sr;
        const double f = hz * std::pow(2.0, 1.0 * std::exp(-t / 0.018));      // starts an octave up, lands in 40 ms
        phase += kTwoPi * f / sr;
        const double env = std::min(1.0, t / 0.002) * std::exp(-t * 6.9 / 2.4);
        const double click = 0.25 * std::exp(-t / 0.0015) * std::sin(kTwoPi * 1800.0 * t);
        s->ch[0][(size_t)i] = (float)(std::tanh(1.8 * env * std::sin(phase)) / std::tanh(1.8) * 0.9 + click);
    }
    return s;
}

struct Def {
    const char* name;
    const char* category;
    bool sustain;
    int lowKey, highKey, step;
    const char* how;
};
const Def kDefs[] = {
    {"808 Bass", "Bass", false, 24, 48, 4, "a sine that drops onto its note, lightly driven: the 808's bass drum played as a bass"},
    {"Electric Piano", "Keys", false, 36, 96, 4, "two-operator FM, the way a tine piano is usually synthesized"},
    {"Electric Bass", "Bass", false, 28, 55, 3, "a plucked-string model with a dark pick"},
    {"Electric Guitar", "Guitars", false, 40, 79, 3, "a plucked-string model with a bright pick near the bridge"},
    {"Acoustic Guitar", "Guitars", false, 40, 79, 3, "a plucked-string model with a body resonance"},
    {"Choir Aah", "Voices", true, 43, 84, 4, "five buzzing voices through the resonances of the vowel \"aah\""},
    {"Kalimba", "Plucked", false, 55, 91, 4, "a short sine with one inharmonic overtone, like a struck tine"},
    {"Tonewheel Organ", "Keys", true, 36, 96, 6, "seven sine drawbars"},
    {"Vibraphone", "Mallets", false, 53, 89, 4, "a bar's first three partials, long ringing"},
    {"Marimba", "Mallets", false, 45, 96, 4, "a bar's first three partials, short and woody"},
    {"Glockenspiel", "Mallets", false, 79, 108, 4, "a metal bar's inharmonic partials"},
    {"Bells", "Bells", false, 60, 96, 4, "FM with a non-integer ratio"},
    {"Harp", "Plucked", false, 36, 96, 5, "a plucked-string model, soft pick, long ring"},
    {"Nylon Pluck", "Plucked", false, 48, 84, 4, "a plucked-string model, very soft pick"},
    {"Synth Pluck", "Plucked", false, 36, 84, 6, "two detuned sawtooths through a closing filter"},
    {"Synth Strings", "Strings", true, 36, 96, 6, "seven detuned sawtooths through a low-pass, slow attack"},
    {"Synth Brass", "Brass", true, 36, 84, 6, "three detuned sawtooths through a brighter low-pass"},
};

} // namespace

std::vector<std::string> names() {
    std::vector<std::string> out;
    for (auto& d : kDefs) out.push_back(d.name);
    return out;
}

NoteSet make(const std::string& name, double sr, int every) {
    NoteSet set;
    const Def* def = nullptr;
    for (auto& d : kDefs) if (name == d.name) def = &d;
    if (!def) return set;
    set.name = def->name;
    set.category = def->category;
    set.sustain = def->sustain;
    set.how = def->how;
    for (int key = def->lowKey; key <= def->highKey; key += def->step * std::max(1, every)) {
        std::shared_ptr<SampleData> s;
        const uint32 seed = 1000u + (uint32)key * 31u;
        if (name == "808 Bass") s = bass808(key, sr);
        else if (name == "Electric Piano") {
            s = fm(key, sr, 4.5, 14.0, 0.9, 0.12, 3.6, 0.002);
            auto body = fm(key, sr, 4.5, 1.0, 1.6, 1.4, 3.6, 0.002);
            for (size_t i = 0; i < s->ch[0].size(); ++i) s->ch[0][i] = 0.35f * s->ch[0][i] + body->ch[0][i];
        }
        else if (name == "Electric Bass") s = pluck(key, sr, 3.5, 0.32, 4.5, 0.18, seed);
        else if (name == "Electric Guitar") s = pluck(key, sr, 4.0, 0.75, 4.0, 0.12, seed);
        else if (name == "Acoustic Guitar") {
            s = pluck(key, sr, 3.5, 0.6, 3.0, 0.2, seed);
            dsp::Biquad b1, b2;                             // the box: two broad resonances
            b1.peak(105.0, 2.0, 6.0, sr);
            b2.peak(210.0, 2.5, 4.0, sr);
            for (auto& v : s->ch[0]) v = b2.process(b1.process(v));
        }
        else if (name == "Choir Aah") s = choir(key, sr, 3.4, key >= 60, seed);
        else if (name == "Kalimba") s = partials(key, sr, 2.2, {{1.0, 1.0, 1.6}, {5.4, 0.25, 0.25}, {2.0, 0.08, 0.6}}, 0.001, false);
        else if (name == "Tonewheel Organ") s = partials(key, sr, 2.4, {{1.0, 1.0, 1}, {2.0, 0.7, 1}, {3.0, 0.5, 1}, {4.0, 0.3, 1}, {5.0, 0.14, 1}, {6.0, 0.12, 1}, {8.0, 0.1, 1}}, 0.006, true);
        else if (name == "Vibraphone") s = partials(key, sr, 5.0, {{1.0, 1.0, 5.0}, {4.0, 0.3, 1.6}, {10.0, 0.08, 0.5}}, 0.002, false, 2.0);
        else if (name == "Marimba") s = partials(key, sr, 1.6, {{1.0, 1.0, 0.9}, {4.0, 0.3, 0.25}, {9.2, 0.08, 0.1}}, 0.001, false, 1.5);
        else if (name == "Glockenspiel") s = partials(key, sr, 3.5, {{1.0, 1.0, 3.2}, {2.76, 0.45, 1.6}, {5.4, 0.25, 0.9}, {8.93, 0.1, 0.5}}, 0.0005, false, 3.0);
        else if (name == "Bells") s = fm(key, sr, 5.0, 3.51, 2.4, 2.8, 4.5, 0.001);
        else if (name == "Harp") s = pluck(key, sr, 4.5, 0.45, 5.5, 0.4, seed);
        else if (name == "Nylon Pluck") s = pluck(key, sr, 3.0, 0.3, 3.2, 0.3, seed);
        else if (name == "Synth Pluck") {
            s = saws(key, sr, 2.0, 2, 7.0, 20000.0, 0.001, seed);
            // a filter that closes as the note dies
            dsp::Svf fl, fr;
            const int n = (int)s->frames();
            for (int i = 0; i < n; ++i) {
                const double t = i / sr;
                if ((i & 31) == 0) { const float c = (float)(200.0 + 9000.0 * std::exp(-t / 0.18)); fl.set(c, 1.2f, (float)sr); fr.set(c, 1.2f, (float)sr); }
                const float env = (float)std::exp(-t * 6.9 / 1.6);
                s->ch[0][(size_t)i] = fl.lowpass(s->ch[0][(size_t)i]) * env;
                s->ch[1][(size_t)i] = fr.lowpass(s->ch[1][(size_t)i]) * env;
            }
        }
        else if (name == "Synth Strings") s = saws(key, sr, 3.2, 7, 14.0, 2600.0, 0.25, seed);
        else if (name == "Synth Brass") s = saws(key, sr, 2.6, 3, 8.0, 3600.0, 0.05, seed);
        if (!s) continue;
        normalise(*s);
        fadeOut(*s, set.sustain ? 0.15 : 0.02);
        set.notes.push_back({key, s});
    }
    return set;
}

} // namespace pulp::notesynth
