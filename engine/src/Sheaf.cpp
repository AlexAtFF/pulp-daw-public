#include "pulp/Sheaf.h"
#include "pulp/Multisample.h"

#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define PULP_SHEAF_SSE 1
#endif

namespace pulp {

namespace {

constexpr float kDbPerOct = 6.0206f;            // 20 log10(2): turns dB into powers of two
constexpr float kLevel = 0.5f;                  // the volume knob times this is the output gain
constexpr double kNyquistStop = 0.98;           // of half the sample rate: no partial at or above this
constexpr double kNyquistFade = 0.90;           // partials fade out between here and the stop
const float kLog2Of20 = (float)std::log2(20.0);
const float kLog2OfRef = (float)std::log2(keyToHz(kRootKey));     // formants that follow the pitch are placed for this key

// 2^x for the audio thread: a table of the fractional part, the whole part straight into the exponent.
struct Exp2Table {
    float t[1025];
    Exp2Table() { for (int i = 0; i <= 1024; ++i) t[i] = (float)std::pow(2.0, i / 1024.0); }
};
const Exp2Table kExp2;
inline float fexp2(float x) {
    x = clampT(x, -100.0f, 30.0f);
    int whole = (int)x;
    if ((float)whole > x) --whole;
    const float f = (x - (float)whole) * 1024.0f;
    const int j = std::min(1023, (int)f);
    const float m = kExp2.t[j] + (kExp2.t[j + 1] - kExp2.t[j]) * (f - (float)j);
    const uint32 bits = (uint32)(whole + 127) << 23;
    float p;
    std::memcpy(&p, &bits, sizeof p);
    return m * p;
}

// Sine and cosine of an angle in 0..pi, exact to well under one part in 10^9 (plain arithmetic, so
// the same on every thread and machine).
inline void sincosSmall(double x, double& s, double& c) {       // |x| <= pi / 4
    const double x2 = x * x;
    s = x * (1.0 + x2 * (-1.0 / 6.0 + x2 * (1.0 / 120.0 + x2 * (-1.0 / 5040.0 + x2 * (1.0 / 362880.0 + x2 * (-1.0 / 39916800.0))))));
    c = 1.0 + x2 * (-0.5 + x2 * (1.0 / 24.0 + x2 * (-1.0 / 720.0 + x2 * (1.0 / 40320.0 + x2 * (-1.0 / 3628800.0 + x2 * (1.0 / 479001600.0))))));
}
inline void fsincos(double w, float& so, float& co) {
    const bool upper = w > kPi * 0.5;
    if (upper) w = kPi - w;
    double s, c;
    if (w > kPi * 0.25) sincosSmall(kPi * 0.5 - w, c, s);
    else sincosSmall(w, s, c);
    so = (float)s;
    co = (float)(upper ? -c : c);
}

// Fixed "random" numbers per clone and partial: the prism's blur and the starting phases.
struct CloneTables {
    float rnd[Sheaf::kMaxClones][Sheaf::kPartials];
    float phC[Sheaf::kMaxClones][Sheaf::kPartials], phS[Sheaf::kMaxClones][Sheaf::kPartials];
    CloneTables() {
        dsp::Noise n(0x51EAF001u);
        for (int c = 0; c < Sheaf::kMaxClones; ++c)
            for (int i = 0; i < Sheaf::kPartials; ++i) {
                rnd[c][i] = n.next();
                const double ph = kTwoPi * n.uni();
                phC[c][i] = (float)std::cos(ph);
                phS[c][i] = (float)std::sin(ph);
            }
    }
};
const CloneTables kClone;

inline float tableAt(const float* t, float pos, int last) {
    pos = clampT(pos, 0.0f, (float)last);
    const int i = (int)pos;
    return t[i] + (t[i + 1] - t[i]) * (pos - (float)i);
}
inline float bellCurve(float x) { const float d = 1.0f + x * x; return 1.0f / (d * d); }

// ---- the oscillator bank -------------------------------------------------------------------------
// Each oscillator is a point going round the unit circle: (c, s) is turned by (cw, sw) every sample and
// s is the sine. Its level runs in a straight line from cur to tgt over the n samples. count is a
// multiple of four. The two versions do the same arithmetic in the same order, so they give the same
// samples; the SSE one does four oscillators per instruction and two such groups per pass.
void bankScalar(float* c, float* s, const float* cw, const float* sw, float* cur, const float* tgt, int count, int n, float* out) {
    float acc[kQuantum][4];
    for (int t = 0; t < n; ++t) acc[t][0] = acc[t][1] = acc[t][2] = acc[t][3] = 0.0f;
    const float invN = 1.0f / (float)n;
    for (int j = 0; j < count; j += 4) {
        float C[4], S[4], A[4], D[4];
        for (int l = 0; l < 4; ++l) {
            const float cc = c[j + l], ss = s[j + l];
            const float k = 1.5f - 0.5f * (cc * cc + ss * ss);      // pulls the point back onto the circle
            C[l] = cc * k;
            S[l] = ss * k;
            A[l] = cur[j + l];
            D[l] = (tgt[j + l] - A[l]) * invN;
        }
        for (int t = 0; t < n; ++t)
            for (int l = 0; l < 4; ++l) {
                A[l] = A[l] + D[l];
                acc[t][l] = acc[t][l] + A[l] * S[l];
                const float nc = C[l] * cw[j + l] - S[l] * sw[j + l];
                S[l] = S[l] * cw[j + l] + C[l] * sw[j + l];
                C[l] = nc;
            }
        for (int l = 0; l < 4; ++l) { c[j + l] = C[l]; s[j + l] = S[l]; cur[j + l] = tgt[j + l]; }
    }
    for (int t = 0; t < n; ++t) out[t] = (acc[t][0] + acc[t][2]) + (acc[t][1] + acc[t][3]);
}

#ifdef PULP_SHEAF_SSE
void bankSse(float* c, float* s, const float* cw, const float* sw, float* cur, const float* tgt, int count, int n, float* out) {
    __m128 acc[kQuantum];
    for (int t = 0; t < n; ++t) acc[t] = _mm_setzero_ps();
    const __m128 invN = _mm_set1_ps(1.0f / (float)n), k15 = _mm_set1_ps(1.5f), k05 = _mm_set1_ps(0.5f);
    int j = 0;
    // sixteen oscillators at a time: each group of four waits on its own last result, so four groups in
    // step keep the processor busy where one would leave it waiting
    for (; j + 16 <= count; j += 16) {
        __m128 c0 = _mm_loadu_ps(c + j), c1 = _mm_loadu_ps(c + j + 4), c2 = _mm_loadu_ps(c + j + 8), c3 = _mm_loadu_ps(c + j + 12);
        __m128 s0 = _mm_loadu_ps(s + j), s1 = _mm_loadu_ps(s + j + 4), s2 = _mm_loadu_ps(s + j + 8), s3 = _mm_loadu_ps(s + j + 12);
        const __m128 cw0 = _mm_loadu_ps(cw + j), cw1 = _mm_loadu_ps(cw + j + 4), cw2 = _mm_loadu_ps(cw + j + 8), cw3 = _mm_loadu_ps(cw + j + 12);
        const __m128 sw0 = _mm_loadu_ps(sw + j), sw1 = _mm_loadu_ps(sw + j + 4), sw2 = _mm_loadu_ps(sw + j + 8), sw3 = _mm_loadu_ps(sw + j + 12);
        const __m128 k0 = _mm_sub_ps(k15, _mm_mul_ps(k05, _mm_add_ps(_mm_mul_ps(c0, c0), _mm_mul_ps(s0, s0))));
        const __m128 k1 = _mm_sub_ps(k15, _mm_mul_ps(k05, _mm_add_ps(_mm_mul_ps(c1, c1), _mm_mul_ps(s1, s1))));
        const __m128 k2 = _mm_sub_ps(k15, _mm_mul_ps(k05, _mm_add_ps(_mm_mul_ps(c2, c2), _mm_mul_ps(s2, s2))));
        const __m128 k3 = _mm_sub_ps(k15, _mm_mul_ps(k05, _mm_add_ps(_mm_mul_ps(c3, c3), _mm_mul_ps(s3, s3))));
        c0 = _mm_mul_ps(c0, k0); s0 = _mm_mul_ps(s0, k0);
        c1 = _mm_mul_ps(c1, k1); s1 = _mm_mul_ps(s1, k1);
        c2 = _mm_mul_ps(c2, k2); s2 = _mm_mul_ps(s2, k2);
        c3 = _mm_mul_ps(c3, k3); s3 = _mm_mul_ps(s3, k3);
        __m128 a0 = _mm_loadu_ps(cur + j), a1 = _mm_loadu_ps(cur + j + 4), a2 = _mm_loadu_ps(cur + j + 8), a3 = _mm_loadu_ps(cur + j + 12);
        const __m128 t0 = _mm_loadu_ps(tgt + j), t1 = _mm_loadu_ps(tgt + j + 4), t2 = _mm_loadu_ps(tgt + j + 8), t3 = _mm_loadu_ps(tgt + j + 12);
        const __m128 d0 = _mm_mul_ps(_mm_sub_ps(t0, a0), invN), d1 = _mm_mul_ps(_mm_sub_ps(t1, a1), invN);
        const __m128 d2 = _mm_mul_ps(_mm_sub_ps(t2, a2), invN), d3 = _mm_mul_ps(_mm_sub_ps(t3, a3), invN);
        for (int t = 0; t < n; ++t) {
            a0 = _mm_add_ps(a0, d0);
            a1 = _mm_add_ps(a1, d1);
            a2 = _mm_add_ps(a2, d2);
            a3 = _mm_add_ps(a3, d3);
            acc[t] = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_add_ps(acc[t], _mm_mul_ps(a0, s0)), _mm_mul_ps(a1, s1)), _mm_mul_ps(a2, s2)), _mm_mul_ps(a3, s3));
            const __m128 n0 = _mm_sub_ps(_mm_mul_ps(c0, cw0), _mm_mul_ps(s0, sw0));
            const __m128 n1 = _mm_sub_ps(_mm_mul_ps(c1, cw1), _mm_mul_ps(s1, sw1));
            const __m128 n2 = _mm_sub_ps(_mm_mul_ps(c2, cw2), _mm_mul_ps(s2, sw2));
            const __m128 n3 = _mm_sub_ps(_mm_mul_ps(c3, cw3), _mm_mul_ps(s3, sw3));
            s0 = _mm_add_ps(_mm_mul_ps(s0, cw0), _mm_mul_ps(c0, sw0));
            s1 = _mm_add_ps(_mm_mul_ps(s1, cw1), _mm_mul_ps(c1, sw1));
            s2 = _mm_add_ps(_mm_mul_ps(s2, cw2), _mm_mul_ps(c2, sw2));
            s3 = _mm_add_ps(_mm_mul_ps(s3, cw3), _mm_mul_ps(c3, sw3));
            c0 = n0; c1 = n1; c2 = n2; c3 = n3;
        }
        _mm_storeu_ps(c + j, c0); _mm_storeu_ps(s + j, s0); _mm_storeu_ps(cur + j, t0);
        _mm_storeu_ps(c + j + 4, c1); _mm_storeu_ps(s + j + 4, s1); _mm_storeu_ps(cur + j + 4, t1);
        _mm_storeu_ps(c + j + 8, c2); _mm_storeu_ps(s + j + 8, s2); _mm_storeu_ps(cur + j + 8, t2);
        _mm_storeu_ps(c + j + 12, c3); _mm_storeu_ps(s + j + 12, s3); _mm_storeu_ps(cur + j + 12, t3);
    }
    for (; j + 8 <= count; j += 8) {
        __m128 c0 = _mm_loadu_ps(c + j), s0 = _mm_loadu_ps(s + j), c1 = _mm_loadu_ps(c + j + 4), s1 = _mm_loadu_ps(s + j + 4);
        const __m128 cw0 = _mm_loadu_ps(cw + j), sw0 = _mm_loadu_ps(sw + j), cw1 = _mm_loadu_ps(cw + j + 4), sw1 = _mm_loadu_ps(sw + j + 4);
        const __m128 k0 = _mm_sub_ps(k15, _mm_mul_ps(k05, _mm_add_ps(_mm_mul_ps(c0, c0), _mm_mul_ps(s0, s0))));
        const __m128 k1 = _mm_sub_ps(k15, _mm_mul_ps(k05, _mm_add_ps(_mm_mul_ps(c1, c1), _mm_mul_ps(s1, s1))));
        c0 = _mm_mul_ps(c0, k0); s0 = _mm_mul_ps(s0, k0);
        c1 = _mm_mul_ps(c1, k1); s1 = _mm_mul_ps(s1, k1);
        __m128 a0 = _mm_loadu_ps(cur + j), a1 = _mm_loadu_ps(cur + j + 4);
        const __m128 t0 = _mm_loadu_ps(tgt + j), t1 = _mm_loadu_ps(tgt + j + 4);
        const __m128 d0 = _mm_mul_ps(_mm_sub_ps(t0, a0), invN), d1 = _mm_mul_ps(_mm_sub_ps(t1, a1), invN);
        for (int t = 0; t < n; ++t) {
            a0 = _mm_add_ps(a0, d0);
            a1 = _mm_add_ps(a1, d1);
            acc[t] = _mm_add_ps(_mm_add_ps(acc[t], _mm_mul_ps(a0, s0)), _mm_mul_ps(a1, s1));
            const __m128 n0 = _mm_sub_ps(_mm_mul_ps(c0, cw0), _mm_mul_ps(s0, sw0));
            const __m128 n1 = _mm_sub_ps(_mm_mul_ps(c1, cw1), _mm_mul_ps(s1, sw1));
            s0 = _mm_add_ps(_mm_mul_ps(s0, cw0), _mm_mul_ps(c0, sw0));
            s1 = _mm_add_ps(_mm_mul_ps(s1, cw1), _mm_mul_ps(c1, sw1));
            c0 = n0;
            c1 = n1;
        }
        _mm_storeu_ps(c + j, c0); _mm_storeu_ps(s + j, s0); _mm_storeu_ps(cur + j, t0);
        _mm_storeu_ps(c + j + 4, c1); _mm_storeu_ps(s + j + 4, s1); _mm_storeu_ps(cur + j + 4, t1);
    }
    for (; j < count; j += 4) {
        __m128 c0 = _mm_loadu_ps(c + j), s0 = _mm_loadu_ps(s + j);
        const __m128 cw0 = _mm_loadu_ps(cw + j), sw0 = _mm_loadu_ps(sw + j);
        const __m128 k0 = _mm_sub_ps(k15, _mm_mul_ps(k05, _mm_add_ps(_mm_mul_ps(c0, c0), _mm_mul_ps(s0, s0))));
        c0 = _mm_mul_ps(c0, k0); s0 = _mm_mul_ps(s0, k0);
        __m128 a0 = _mm_loadu_ps(cur + j);
        const __m128 t0 = _mm_loadu_ps(tgt + j);
        const __m128 d0 = _mm_mul_ps(_mm_sub_ps(t0, a0), invN);
        for (int t = 0; t < n; ++t) {
            a0 = _mm_add_ps(a0, d0);
            acc[t] = _mm_add_ps(acc[t], _mm_mul_ps(a0, s0));
            const __m128 n0 = _mm_sub_ps(_mm_mul_ps(c0, cw0), _mm_mul_ps(s0, sw0));
            s0 = _mm_add_ps(_mm_mul_ps(s0, cw0), _mm_mul_ps(c0, sw0));
            c0 = n0;
        }
        _mm_storeu_ps(c + j, c0); _mm_storeu_ps(s + j, s0); _mm_storeu_ps(cur + j, t0);
    }
    for (int t = 0; t < n; ++t) {
        const __m128 h = _mm_add_ps(acc[t], _mm_movehl_ps(acc[t], acc[t]));        // lanes 0+2 and 1+3
        out[t] = _mm_cvtss_f32(_mm_add_ss(h, _mm_shuffle_ps(h, h, 1)));
    }
}
#endif

// ---- the shapes ----------------------------------------------------------------------------------

// A bell with a minor-third "tierce", and a struck bar. Partial 1 is the note that is heard (the bell's
// "prime") in both, so the played key stays in tune whatever the colour; partial 2 is the bell's hum an
// octave below it, which a bar does not have; from partial 3 up the two series run side by side.
const double kBellTuned[12] = {1.0, 0.5, 1.2, 1.5, 2.0, 2.5, 2.667, 3.0, 4.0, 5.333, 6.667, 8.0};
const double kBellLevel[12] = {1.0, 0.45, 0.7, 0.3, 0.85, 0.25, 0.3, 0.45, 0.35, 0.2, 0.15, 0.1};

// Organ stops: which harmonics the drawbars stand on, and five registrations from one stop to all of them.
const int kOrganHarmonic[10] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 16};
const double kOrganStops[5][10] = {
    {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {1.0, 0.8, 0.0, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {1.0, 1.0, 0.9, 0.7, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {1.0, 0.9, 0.8, 0.9, 0.5, 0.6, 0.5, 0.0, 0.0, 0.0},
    {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.7, 0.6, 0.5},
};

// The first three resonances of five sung vowels (ah, eh, ee, oh, oo), in Hz. Averages for a low voice.
const double kVowel[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
const double kVowelUpper[2] = {3400.0, 4500.0};
const double kVowelQ[5] = {6.0, 12.0, 14.0, 14.0, 12.0};

std::string baseName(const std::string& path) {
    std::string n = path;
    const size_t cut = n.find_last_of("/\\");
    if (cut != std::string::npos) n = n.substr(cut + 1);
    const size_t dot = n.find_last_of('.');
    if (dot != std::string::npos && dot > 0) n = n.substr(0, dot);
    return n;
}

} // namespace

double Sheaf::bellRatio(int partial, float colour) {
    const int k = clampT(partial, 1, (int)kPartials);
    if (k <= 2) return kBellTuned[k - 1];
    const double tuned = k <= 12 ? kBellTuned[k - 1] : 8.0 * std::pow(k / 12.0, 1.5);
    const double bar = ((k - 0.5) / 1.5) * ((k - 0.5) / 1.5);          // a free bar: 1, 2.78, 5.44, 9 ... from partial 1
    const double t = clampT((double)colour, 0.0, 1.0);
    return std::pow(tuned, 1.0 - t) * std::pow(bar, t);
}

// ------------------------------------------------------------------------------------ construction

Sheaf::Sheaf() {
    voices_.reset(new Voice[kSlots]);
    for (int g = 0; g < kGridSize; ++g) gridHz_[g] = 20.0 * std::pow(2.0, (double)g / kGridPerOct);
    const std::vector<std::string> shapes = {"Saw", "Square", "Triangle", "Pulse", "Vowel", "Bell", "Organ", "User (analysed)"};
    for (int x = 0; x < 2; ++x) {
        const std::string n = x == 0 ? "a" : "b", d = x == 0 ? "A " : "B ";
        spec[x].shape = addChoice((n + "shape").c_str(), (d + "shape").c_str(), shapes, x == 0 ? Saw : Square);
        spec[x].colour = addFloat((n + "colour").c_str(), (d + "width / colour").c_str(), 0, 1, 0.5f, "%");
        spec[x].tilt = addFloat((n + "tilt").c_str(), (d + "tilt per octave").c_str(), -12, 12, 0, "dB");
        spec[x].oddEven = addFloat((n + "oddeven").c_str(), (d + "odd / even").c_str(), -1, 1, 0, "");
        spec[x].comb = addFloat((n + "comb").c_str(), (d + "comb spacing").c_str(), 1, 32, 3, "", 2.0f);
        spec[x].combDepth = addFloat((n + "combdepth").c_str(), (d + "comb depth").c_str(), 0, 1, 0, "%");
    }
    pMorph = addFloat("morph", "Morph A to B", 0, 1, 0, "%");
    pPartials = add({"partials", "Partials", 1, (float)kPartials, 128, 1, kPartials, "", {}});
    pInharm = addFloat("inharm", "Inharmonicity", 0, 1, 0, "%", 3.0f);
    pDamp = addFloat("damp", "High partial damping", 0, 60, 0, "dB", 3.0f);
    pPhases = addChoice("phases", "Partial phases", {"Aligned", "Random"}, 0);

    pPrism = add({"prism", "Prism clones", 1, (float)kMaxClones, 1, 1, kMaxClones, "", {}});
    pPrismDetune = addFloat("prismdetune", "Prism detune", 0, 100, 12, "ct", 2.0f);
    pPrismBlur = addFloat("prismblur", "Prism blur", 0, 1, 0, "%");
    pPrismSpread = addFloat("prismspread", "Prism stereo spread", 0, 1, 0.5f, "%");

    pFormFixed = addBool("formfixed", "Formants fixed", true);
    const float formDef[2] = {700.0f, 2400.0f};
    for (int f = 0; f < 2; ++f) {
        const std::string n = "f" + std::to_string(f + 1), d = "Formant " + std::to_string(f + 1) + " ";
        pFormFreq[f] = addFloat((n + "freq").c_str(), (d + "frequency").c_str(), 100, 8000, formDef[f], "Hz", 3.0f);
        pFormWidth[f] = addFloat((n + "width").c_str(), (d + "width").c_str(), 0.1f, 2, 0.5f, "oct", 2.0f);
        pFormGain[f] = addFloat((n + "gain").c_str(), (d + "gain").c_str(), -24, 24, 0, "dB");
    }

    pFilterType = addChoice("ftype", "Filter", {"Off", "Low pass", "High pass", "Band pass"}, LowPass);
    pCutoff = addFloat("cutoff", "Cutoff", 20, 20000, 20000, "Hz", 3.0f);
    pSlope = addFloat("slope", "Slope per octave", 6, 96, 24, "dB", 2.0f);
    pReso = addFloat("reso", "Resonance bump", 0, 18, 0, "dB");
    pFilterEnv = addFloat("fenv", "Envelope 2 to cutoff", -1, 1, 0, "");
    pFilterKey = addFloat("fkey", "Cutoff key tracking", 0, 1, 0, "%");

    pUserSpeed = addFloat("uspeed", "Evolution speed", 0, 4, 1, "x", 2.0f);
    pUserStart = addFloat("ustart", "Start point", 0, 1, 0, "%");
    pUserLoop = addBool("uloop", "Loop back and forth", true);
    pUserLoopStart = addFloat("uloopstart", "Loop start", 0, 1, 0.35f, "%");
    pUserLoopEnd = addFloat("uloopend", "Loop end", 0, 1, 0.75f, "%");

    pAttack = addFloat("attack", "Attack", 0, 5000, 2, "ms", 3.0f);
    pDecay = addFloat("decay", "Decay", 1, 5000, 400, "ms", 3.0f);
    pSustain = addFloat("sustain", "Sustain", 0, 1, 0.8f, "%");
    pRelease = addFloat("release", "Release", 1, 5000, 200, "ms", 3.0f);

    pE2Attack = addFloat("e2attack", "Envelope 2 attack", 0, 5000, 2, "ms", 3.0f);
    pE2Decay = addFloat("e2decay", "Envelope 2 decay", 1, 5000, 600, "ms", 3.0f);
    pE2Sustain = addFloat("e2sustain", "Envelope 2 sustain", 0, 1, 0, "%");
    pE2Release = addFloat("e2release", "Envelope 2 release", 1, 5000, 300, "ms", 3.0f);
    pE2Morph = addFloat("e2morph", "Envelope 2 to morph", -1, 1, 0, "");
    pE2Tilt = addFloat("e2tilt", "Envelope 2 to tilt", -12, 12, 0, "dB");

    const float rateDef[2] = {5.0f, 0.3f};
    for (int k = 0; k < 2; ++k) {
        const std::string n = "l" + std::to_string(k + 1), d = "LFO " + std::to_string(k + 1) + " ";
        lfo[k].wave = addChoice((n + "wave").c_str(), (d + "shape").c_str(), parts::lfoNames(), 0);
        lfo[k].rate = addFloat((n + "rate").c_str(), (d + "rate").c_str(), 0.02f, 30, rateDef[k], "Hz", 3.0f);
        lfo[k].fade = addFloat((n + "fade").c_str(), (d + "fade in").c_str(), 0, 5000, 0, "ms", 3.0f);
        lfo[k].pitch = addFloat((n + "pitch").c_str(), (d + "to pitch").c_str(), 0, 1200, 0, "ct", 3.0f);
        lfo[k].morph = addFloat((n + "morph").c_str(), (d + "to morph").c_str(), -1, 1, 0, "");
        lfo[k].filter = addFloat((n + "filter").c_str(), (d + "to cutoff").c_str(), -4, 4, 0, "oct");
        lfo[k].tilt = addFloat((n + "tilt").c_str(), (d + "to tilt").c_str(), -12, 12, 0, "dB");
        lfo[k].amp = addFloat((n + "amp").c_str(), (d + "to level").c_str(), 0, 1, 0, "%");
    }

    pVolume = addFloat("volume", "Level", 0, 1, 0.5f, "%");
    pVelSens = addFloat("velsens", "Velocity to level", 0, 1, 1, "%");
    pVelBright = addFloat("velbright", "Velocity to brightness", 0, 24, 0, "dB");
    pGlide = addFloat("glide", "Glide", 0, 2000, 0, "ms", 3.0f);
    pVoices = add({"voices", "Polyphony", 1, (float)kMaxNotes, 16, 1, kMaxNotes, "", {}});
    pCoarse = add({"coarse", "Coarse tune", -24, 24, 0, 1, 49, "st", {}});
    pFine = addFloat("fine", "Fine tune", -100, 100, 0, "ct");

    prepare(48000.0);
}

std::vector<ParamGroup> Sheaf::paramGroups() const {
    std::vector<ParamGroup> g;
    g.push_back({"Spectrum A", {"ashape", "acolour", "atilt", "aoddeven", "acomb", "acombdepth"}});
    g.push_back({"Spectrum B", {"bshape", "bcolour", "btilt", "boddeven", "bcomb", "bcombdepth"}});
    g.push_back({"Morph and partials", {"morph", "partials", "inharm", "damp", "phases"}});
    g.push_back({"Prism", {"prism", "prismdetune", "prismblur", "prismspread"}});
    g.push_back({"Formants", {"formfixed", "f1freq", "f1width", "f1gain", "f2freq", "f2width", "f2gain"}});
    g.push_back({"Filter", {"ftype", "cutoff", "slope", "reso", "fenv", "fkey"}});
    g.push_back({"Resynthesis", {"uspeed", "ustart", "uloop", "uloopstart", "uloopend"}});
    g.push_back({"Amplitude", {"attack", "decay", "sustain", "release"}});
    g.push_back({"Envelope 2", {"e2attack", "e2decay", "e2sustain", "e2release", "e2morph", "e2tilt"}});
    for (int k = 1; k <= 2; ++k) {
        const std::string n = "l" + std::to_string(k);
        g.push_back({"LFO " + std::to_string(k), {n + "wave", n + "rate", n + "fade", n + "pitch", n + "morph", n + "filter", n + "tilt", n + "amp"}});
    }
    g.push_back({"Global", {"volume", "velsens", "velbright", "glide", "voices", "coarse", "fine"}});
    return g;
}

void Sheaf::prepare(double sampleRate) {
    sr_ = sampleRate;
    const float nan = std::nanf("");
    for (auto& row : seenSpec_) for (float& v : row) v = nan;      // every table is rebuilt at the next quantum
    for (float& v : seenForm_) v = nan;
    for (float& v : seenFilt_) v = nan;
    for (float& v : seenPrism_) v = nan;
    seenUser_ = nullptr;
    reset();
}

void Sheaf::reset() {
    for (int i = 0; i < kSlots; ++i) voices_[i].active = false;
    voicesActive_.store(0, std::memory_order_relaxed);
    ageCounter_ = noteCounter_ = 0;
    lastKey_ = -1.0f;
    fresh_ = true;
}

void Sheaf::countVoices() {
    int n = 0;
    for (int i = 0; i < kSlots; ++i) n += voices_[i].active ? 1 : 0;
    voicesActive_.store(n, std::memory_order_relaxed);
}

// ------------------------------------------------------------------------------------ the tables

// One spectrum as a table over the partial number: level, frequency ratio and (for A) starting phase.
// The levels are scaled so that their squares add up to one, so no setting of shape, tilt, balance or
// comb changes the loudness much; only the colour.
void Sheaf::buildSpectrum(int x) {
    const int shape = spec[x].shape->getInt();
    const float colour = spec[x].colour->get(), tilt = spec[x].tilt->get(), oe = spec[x].oddEven->get();
    const float comb = std::max(1.0f, spec[x].comb->get()), depth = spec[x].combDepth->get();
    const int count = pPartials->getInt();
    const double stiff = stiffness(pInharm->get());
    const Analysis* an = cur_ && cur_->frames > 0 && cur_->partials > 0 ? cur_ : nullptr;
    user_[x] = shape == User && an != nullptr;
    vowel_[x] = shape == Vowel;

    if (vowel_[x]) {
        // the mouth: five resonances in a row; the first three move with the vowel
        const double pos = clampT(colour, 0.0f, 1.0f) * 4.0;
        const int v0 = std::min(3, (int)pos);
        const double vt = pos - v0;
        double f[5];
        for (int k = 0; k < 3; ++k) f[k] = kVowel[v0][k] * std::pow(kVowel[v0 + 1][k] / kVowel[v0][k], vt);
        f[3] = kVowelUpper[0];
        f[4] = kVowelUpper[1];
        double top = 0.0;
        double env[kGridSize];
        for (int g = 0; g < kGridSize; ++g) {
            const double hz = gridHz_[g];
            double r = 1.0;
            for (int k = 0; k < 5; ++k) {
                const double u = hz / f[k], a = 1.0 - u * u, bq = u / kVowelQ[k];
                r /= std::sqrt(a * a + bq * bq);
            }
            env[g] = r;
            top = std::max(top, r);
        }
        for (int g = 0; g < kGridSize; ++g) vowelEnv_[x][g] = (float)(env[g] / top);
        vowelEnv_[x][kGridSize] = vowelEnv_[x][kGridSize - 1];
    }

    double level[kPartials], gain[kPartials], ratio[kPartials];
    const double organPos = clampT(colour, 0.0f, 1.0f) * 4.0;
    const int organ0 = std::min(3, (int)organPos);
    const double organT = organPos - organ0;
    const double duty = clampT((double)colour, 0.02, 0.98);
    double energy = 0.0;
    for (int i = 0; i < kPartials; ++i) {
        const int k = i + 1;
        double a = 0.0, r = (double)k, phase = 0.0;
        switch (shape) {
            case Square: a = (k & 1) ? 1.0 / k : 0.0; break;
            case Triangle: a = (k & 1) ? (((k - 1) / 2) & 1 ? -1.0 : 1.0) / ((double)k * k) : 0.0; break;
            case Pulse: a = std::sin(kPi * k * duty) / k; phase = kPi * 0.5 - kPi * k * duty; break;
            case Bell: {
                r = bellRatio(k, colour);
                const double tuned = k <= 12 ? kBellLevel[k - 1] : 0.1 * std::pow(12.0 / k, 1.5);
                const double bar = k == 1 ? 1.0 : (k == 2 ? 0.0 : std::pow((double)(k - 1), -0.8));
                a = tuned + (bar - tuned) * clampT((double)colour, 0.0, 1.0);
                break;
            }
            case Organ:
                for (int h = 0; h < 10; ++h)
                    if (kOrganHarmonic[h] == k) a = kOrganStops[organ0][h] + (kOrganStops[organ0 + 1][h] - kOrganStops[organ0][h]) * organT;
                break;
            case User:
                if (an) a = i < an->partials ? (double)an->amp[(size_t)(an->loudest * an->partials + i)] : 0.0;
                else a = 1.0 / k;           // nothing analysed yet: a saw stands in
                break;
            default: a = 1.0 / k; break;    // Saw, and the source of the vowel
        }
        double g = std::pow(r, (double)tilt / kDbPerOct);
        if (k & 1) g *= oe > 0.0f ? 1.0 - oe : 1.0;
        else g *= oe < 0.0f ? 1.0 + oe : 1.0;
        g *= 1.0 - depth * (0.5 - 0.5 * std::cos(kTwoPi * k / comb));
        if (k > count) g = 0.0;
        level[i] = a;
        gain[i] = g;
        ratio[i] = stretched(r, stiff);
        double heard = a * g;
        if (vowel_[x]) heard *= tableAt(vowelEnv_[x], ((float)std::log2(ratio[i]) + kLog2OfRef - kLog2Of20) * kGridPerOct, kGridSize - 1);
        energy += heard * heard;
        if (x == 0) {
            phaseC_[i] = phase == 0.0 ? 1.0f : (float)std::cos(phase);
            phaseS_[i] = phase == 0.0 ? 0.0f : (float)std::sin(phase);
        }
    }
    const double norm = energy > 1.0e-18 ? 1.0 / std::sqrt(energy) : 0.0;
    for (int i = 0; i < kPartials; ++i) {
        amp_[x][i] = (float)((user_[x] ? gain[i] : level[i] * gain[i]) * norm);
        if (user_[x] && i >= an->partials) amp_[x][i] = 0.0f;
        ratio_[x][i] = (float)ratio[i];
        log2Ratio_[x][i] = (float)std::log2(ratio[i]);
    }
    if (x == 0) phased_ = shape == Pulse;
}

// The two formant peaks as one curve over frequency. A peak's width is how far from its centre, in
// octaves, its gain in dB has fallen to a quarter.
void Sheaf::buildFormants() {
    float centre[2], width[2], gain[2];
    for (int f = 0; f < 2; ++f) {
        centre[f] = (float)std::log2(pFormFreq[f]->get() / 20.0);
        width[f] = std::max(0.05f, pFormWidth[f]->get());
        gain[f] = pFormGain[f]->get() / kDbPerOct;
    }
    for (int g = 0; g < kGridSize; ++g) {
        const float oct = (float)g / kGridPerOct;
        formant_[g] = gain[0] * bellCurve((oct - centre[0]) / width[0]) + gain[1] * bellCurve((oct - centre[1]) / width[1]);
    }
    formant_[kGridSize] = formant_[kGridSize - 1];
}

// The filter as a gain curve over octaves from the cutoff: flat on the passing side, falling in a
// straight line (in dB per octave) on the other, with an optional bump at the cutoff.
void Sheaf::buildFilter() {
    const int type = pFilterType->getInt();
    const float slope = pSlope->get() / kDbPerOct, bump = pReso->get() / kDbPerOct;
    for (int g = 0; g < kFiltSize; ++g) {
        const float oct = (float)(g - 10 * kFiltPerOct) / kFiltPerOct;
        float v = 0.0f;
        if (type == LowPass) v = -slope * std::max(0.0f, oct);
        else if (type == HighPass) v = -slope * std::max(0.0f, -oct);
        else if (type == BandPass) v = -slope * std::fabs(oct);
        if (type != FilterOff) v += bump * bellCurve(oct / 0.2f);
        filter_[g] = v;
    }
    filter_[kFiltSize] = filter_[kFiltSize - 1];
}

void Sheaf::beginBlock(ProcessContext& ctx) {
    bend_ = ctx.pitchBendSemis;
    cur_ = analysis_.rtAcquire();

    Block& b = b_;
    b.morph = pMorph->get();
    b.partials = pPartials->get();
    b.inharm = pInharm->get();
    b.damp = pDamp->get();
    b.phases = pPhases->getInt();
    b.prism = clampT(pPrism->getInt(), 1, (int)kMaxClones);
    b.prismDetune = pPrismDetune->get();
    b.prismBlur = pPrismBlur->get();
    b.prismSpread = pPrismSpread->get();
    b.formFixed = pFormFixed->getBool();
    b.formOn = pFormGain[0]->get() != 0.0f || pFormGain[1]->get() != 0.0f;
    b.filterType = pFilterType->getInt();
    b.cutoff = pCutoff->get();
    b.filterEnv = pFilterEnv->get();
    b.filterKey = pFilterKey->get();
    b.userSpeed = pUserSpeed->get();
    b.userStart = pUserStart->get();
    b.userLoop = pUserLoop->getBool();
    b.userLoopStart = pUserLoopStart->get();
    b.userLoopEnd = pUserLoopEnd->get();
    b.attack = pAttack->get(); b.decay = pDecay->get(); b.sustain = pSustain->get(); b.release = pRelease->get();
    b.e2Attack = pE2Attack->get(); b.e2Decay = pE2Decay->get(); b.e2Sustain = pE2Sustain->get(); b.e2Release = pE2Release->get();
    b.e2Morph = pE2Morph->get();
    b.e2Tilt = pE2Tilt->get();
    for (int k = 0; k < 2; ++k) {
        b.lfoWave[k] = lfo[k].wave->getInt();
        b.lfoRate[k] = lfo[k].rate->get();
        b.lfoFade[k] = lfo[k].fade->get();
        b.lfoPitch[k] = lfo[k].pitch->get();
        b.lfoMorph[k] = lfo[k].morph->get();
        b.lfoFilter[k] = lfo[k].filter->get();
        b.lfoTilt[k] = lfo[k].tilt->get();
        b.lfoAmp[k] = lfo[k].amp->get();
    }
    b.velSens = pVelSens->get();
    b.velBright = pVelBright->get();
    b.glide = pGlide->get();
    b.coarse = pCoarse->get();
    b.fine = pFine->get();
    b.voices = clampT(pVoices->getInt(), 1, (int)kMaxNotes);

    // rebuild whatever table a moved parameter belongs to
    bool spectra = false;
    for (int x = 0; x < 2; ++x) {
        const float key[8] = {spec[x].shape->get(), spec[x].colour->get(), spec[x].tilt->get(), spec[x].oddEven->get(),
                              spec[x].comb->get(), spec[x].combDepth->get(), b.partials, b.inharm};
        bool same = true;
        for (int i = 0; i < 8; ++i) same = same && key[i] == seenSpec_[x][i];
        if (spec[x].shape->getInt() == User && cur_ != seenUser_) same = false;
        if (same) continue;
        for (int i = 0; i < 8; ++i) seenSpec_[x][i] = key[i];
        buildSpectrum(x);
        spectra = true;
        ++tables_;
    }
    seenUser_ = cur_;
    if (spectra) {
        ratiosDiffer_ = false;
        harmonic_ = true;
        maxPartial_ = 0;
        for (int i = 0; i < kPartials; ++i) {
            if (ratio_[0][i] != ratio_[1][i]) ratiosDiffer_ = true;
            if (ratio_[0][i] != (float)(i + 1) || ratio_[1][i] != (float)(i + 1)) harmonic_ = false;
            if (amp_[0][i] != 0.0f || amp_[1][i] != 0.0f) maxPartial_ = i + 1;
        }
        ++serial_;
    }
    {
        const float key[6] = {pFormFreq[0]->get(), pFormWidth[0]->get(), pFormGain[0]->get(), pFormFreq[1]->get(), pFormWidth[1]->get(), pFormGain[1]->get()};
        bool same = true;
        for (int i = 0; i < 6; ++i) same = same && key[i] == seenForm_[i];
        if (!same) { for (int i = 0; i < 6; ++i) seenForm_[i] = key[i]; buildFormants(); ++tables_; }
    }
    {
        const float key[3] = {(float)b.filterType, pSlope->get(), pReso->get()};
        bool same = true;
        for (int i = 0; i < 3; ++i) same = same && key[i] == seenFilt_[i];
        if (!same) { for (int i = 0; i < 3; ++i) seenFilt_[i] = key[i]; buildFilter(); ++tables_; }
    }
    if (b.prismDetune != seenPrism_[0] || b.prismBlur != seenPrism_[1]) {
        seenPrism_[0] = b.prismDetune;
        seenPrism_[1] = b.prismBlur;
        ++serial_;
    }

    const float vol = pVolume->get() * kLevel;
    if (fresh_) vol_.snap(vol);
    else vol_.target(vol, kQuantum);
    fresh_ = false;
    countVoices();
}

// ------------------------------------------------------------------------------------ notes

void Sheaf::startVoice(const NoteEvent& e) {
    const Block& b = b_;
    const float killStep = (float)(1.0 / (0.003 * sr_));
    // the polyphony limit: the note that has been going longest fades out (one that was already let go first)
    int sounding = 0;
    for (int i = 0; i < kSlots; ++i) sounding += voices_[i].active && voices_[i].killStep == 0.0f ? 1 : 0;
    while (sounding >= b.voices) {
        Voice* old = nullptr;
        for (int i = 0; i < kSlots; ++i) {
            Voice& c = voices_[i];
            if (!c.active || c.killStep != 0.0f) continue;
            if (!old || (c.released && !old->released) || (c.released == old->released && c.age < old->age)) old = &c;
        }
        if (!old) break;
        old->killStep = killStep;
        --sounding;
    }
    Voice* v = nullptr;
    for (int i = 0; i < kSlots && !v; ++i) if (!voices_[i].active) v = &voices_[i];
    if (!v) {                                   // every slot is busy: take the one that has faded furthest
        v = &voices_[0];
        for (int i = 1; i < kSlots; ++i) if (voices_[i].kill < v->kill || (voices_[i].kill == v->kill && voices_[i].age < v->age)) v = &voices_[i];
    }

    v->active = true;
    v->fresh = true;
    v->released = false;
    v->id = e.id;
    v->age = ++ageCounter_;
    v->key = e.key;
    v->group = e.group;
    v->clones = b.prism;
    v->stride = partialsPerClone(b.prism);
    v->velN = e.velocity / kDefaultVelocity;
    v->velGain = std::max(0.0f, 1.0f + (v->velN * v->velN - 1.0f) * b.velSens);
    parts::panGains(e.pan, v->panL, v->panR);
    v->fine = e.fineCents;
    v->modX = e.modX;
    v->modY = e.modY;
    v->relScale = 0.25f + 1.5f * e.release;
    v->bend = v->bendTarget = v->bendStep = 0.0f;
    if (e.glideFromKey >= 0.0f && e.slideSamples > 0.0f) {            // the channel's portamento
        v->bend = e.glideFromKey - (float)e.key;
        v->bendStep = -v->bend / e.slideSamples;
    } else if (b.glide > 0.0f && lastKey_ >= 0.0f && lastKey_ != (float)e.key) {     // its own glide, from the last note played
        v->bend = lastKey_ - (float)e.key;
        v->bendStep = -v->bend / (float)(b.glide * 0.001 * sr_);
    }
    lastKey_ = (float)e.key;
    v->amp.set(b.attack, b.decay, b.sustain, b.release * v->relScale, sr_);
    v->env2.set(b.e2Attack, b.e2Decay, b.e2Sustain, b.e2Release * v->relScale, sr_);
    v->amp.start();
    v->env2.start();
    v->kill = 1.0f;
    v->killStep = 0.0f;
    v->time = 0.0;
    v->rnd = 0x9E3779B9u * (++noteCounter_) + 0x7F4A7C15u;
    if (v->rnd == 0) v->rnd = 1;
    for (int k = 0; k < 2; ++k) {
        v->lfoPhase[k] = 0.0f;
        v->rnd ^= v->rnd << 13; v->rnd ^= v->rnd >> 17; v->rnd ^= v->rnd << 5;
        v->lfoRnd[k] = (float)((double)v->rnd / 2147483648.0 - 1.0);
    }
    v->userPos = cur_ ? (double)b.userStart * cur_->seconds : 0.0;
    v->userDir = 1.0f;
    v->lastW0 = -1.0;
    v->lastMorph = -1.0f;
    v->serial = 0;
    v->harmonics = 0;
    v->nyquist = 0;
    v->spreadSeen = -1.0f;
    v->tables = 0;

    // every oscillator starts at its phase with no level; the first stretch sets the levels outright
    const bool random = b.phases == 1;
    for (int c = 0; c < v->clones; ++c) {
        const int base = c * v->stride;
        const float* pc = c == 0 && !random ? phaseC_ : kClone.phC[c];
        const float* ps = c == 0 && !random ? phaseS_ : kClone.phS[c];
        for (int i = 0; i < v->stride; ++i) {
            v->c[base + i] = pc[i];
            v->s[base + i] = ps[i];
            v->cw[base + i] = 1.0f;
            v->sw[base + i] = 0.0f;
            v->cur[base + i] = 0.0f;
        }
    }
    // Partials that all start at zero make a sound that starts at zero. Any other start is faded in over a millisecond.
    const bool fromZero = !random && v->clones == 1 && !phased_;
    v->declick = fromZero ? 1.0f : 0.0f;
    v->declickStep = (float)(1.0 / (0.001 * sr_));
}

void Sheaf::handleEvent(const NoteEvent& e, ProcessContext&) {
    const float killStep = (float)(1.0 / (0.003 * sr_));
    auto release = [](Voice& v) { v.amp.release(); v.env2.release(); v.released = true; };
    switch (e.type) {
        case NoteEvent::On: startVoice(e); break;
        case NoteEvent::Off:
            for (int i = 0; i < kSlots; ++i) if (voices_[i].active && voices_[i].id == e.id) release(voices_[i]);
            break;
        case NoteEvent::AllOff:
            for (int i = 0; i < kSlots; ++i) if (voices_[i].active) release(voices_[i]);
            break;
        case NoteEvent::Kill:
            for (int i = 0; i < kSlots; ++i) if (voices_[i].active && voices_[i].killStep == 0.0f) voices_[i].killStep = killStep;
            break;
        case NoteEvent::KillNote:
            for (int i = 0; i < kSlots; ++i) if (voices_[i].active && voices_[i].id == e.id && voices_[i].killStep == 0.0f) voices_[i].killStep = killStep;
            break;
        case NoteEvent::SlideTo:
            for (int i = 0; i < kSlots; ++i) {
                Voice& v = voices_[i];
                if (v.active && v.group == e.group && !v.released && v.killStep == 0.0f) {
                    v.bendTarget = (float)(e.key - v.key);
                    v.bendStep = (v.bendTarget - v.bend) / std::max(1.0f, e.slideSamples);
                }
            }
            break;
    }
}

// ------------------------------------------------------------------------------------ one voice

void Sheaf::renderVoice(Voice& v, float* L, float* R, int n) {
    const Block& b = b_;
    const float invSr = (float)(1.0 / sr_);

    // the envelopes follow their knobs while the note sounds
    v.amp.set(b.attack, b.decay, b.sustain, b.release * v.relScale, sr_);
    v.env2.set(b.e2Attack, b.e2Decay, b.e2Sustain, b.e2Release * v.relScale, sr_);

    // The modulators as they will stand at the end of this stretch: the partials' levels are run there
    // in a straight line, so nothing steps.
    for (int i = 0; i < n; ++i) v.env2.next();
    const float env2 = v.env2.value;
    v.time += (double)n / sr_;
    float lf[2];
    for (int k = 0; k < 2; ++k) {
        v.lfoPhase[k] += b.lfoRate[k] * (float)n * invSr;
        while (v.lfoPhase[k] >= 1.0f) {
            v.lfoPhase[k] -= 1.0f;
            v.rnd ^= v.rnd << 13; v.rnd ^= v.rnd >> 17; v.rnd ^= v.rnd << 5;
            v.lfoRnd[k] = (float)((double)v.rnd / 2147483648.0 - 1.0);
        }
        const float fade = b.lfoFade[k] > 0.0f ? clampT((float)(v.time * 1000.0 / b.lfoFade[k]), 0.0f, 1.0f) : 1.0f;
        lf[k] = parts::lfoShape(b.lfoWave[k], v.lfoPhase[k], v.lfoRnd[k]) * fade;
    }

    // pitch: constant over the stretch (at most 64 samples), phase always continuous
    const double semis = (double)v.key + (double)v.bend + (double)bend_ + (double)v.fine * 0.01 + (double)b.coarse + (double)b.fine * 0.01
                       + (double)(lf[0] * b.lfoPitch[0] + lf[1] * b.lfoPitch[1]) * 0.01;
    if (v.bendStep != 0.0f) {
        v.bend += v.bendStep * (float)n;
        if ((v.bendStep > 0.0f) == (v.bend >= v.bendTarget)) { v.bend = v.bendTarget; v.bendStep = 0.0f; }
    }
    const double f0 = keyToHz(semis);
    const double w0 = kTwoPi * f0 / sr_;
    const float log2f0 = (float)std::log2(f0);

    const float m = clampT(b.morph + b.e2Morph * env2 + lf[0] * b.lfoMorph[0] + lf[1] * b.lfoMorph[1] + (v.modY - 0.5f) * 2.0f, 0.0f, 1.0f);

    const int C = v.clones, stride = v.stride;
    const float* l2A = log2Ratio_[0];
    const float* l2B = log2Ratio_[1];
    const int want = std::min(stride, maxPartial_);
    const int span = std::max(want, v.harmonics);          // the partials wanted now and the ones still sounding from before
    const float detOct = b.prismDetune / 1200.0f;

    // ---- frequencies, when anything that sets them has moved
    const bool retune = v.fresh || w0 != v.lastW0 || v.serial != serial_ || (ratiosDiffer_ && m != v.lastMorph);
    if (retune) {
        const double wStop = kPi * kNyquistStop, wFade = kPi * kNyquistFade;
        const double widest = C > 1 ? (double)fexp2(detOct) : 1.0;     // the sharpest clone decides whether a partial fits
        int fit = 0;
        for (int i = 0; i < span; ++i) {
            const float r = ratiosDiffer_ ? fexp2(l2A[i] + m * (l2B[i] - l2A[i])) : ratio_[0][i];
            const double w = w0 * (double)r * widest;
            if (w >= wStop) break;
            v.ratio[i] = r;
            v.fade[i] = w <= wFade ? 1.0f : (float)((wStop - w) / (wStop - wFade));
            fit = i + 1;
        }
        v.nyquist = fit;
        // Whole-number harmonics of one fundamental (per clone) are powers of one turn: each is the last one
        // turned once more, which costs four multiplications. Anything else gets its own sine and cosine.
        const bool chain = harmonic_ && (C == 1 || b.prismBlur == 0.0f);
        for (int c = 0; c < C; ++c) {
            const int base = c * stride;
            const float place = C > 1 ? -1.0f + 2.0f * (float)c / (float)(C - 1) : 0.0f;
            if (chain) {
                const double wc = C > 1 ? w0 * (double)fexp2(detOct * place) : w0;
                const double c1 = std::cos(wc), s1 = std::sin(wc);
                double zr = c1, zi = s1;
                for (int i = 0; i < fit; ++i) {
                    v.cw[base + i] = (float)zr;
                    v.sw[base + i] = (float)zi;
                    const double nr = zr * c1 - zi * s1;
                    zi = zi * c1 + zr * s1;
                    zr = nr;
                }
                continue;
            }
            for (int i = 0; i < fit; ++i) {
                double w = w0 * (double)v.ratio[i];
                if (C > 1) w *= (double)fexp2(detOct * ((1.0f - b.prismBlur) * place + b.prismBlur * kClone.rnd[c][i]));
                fsincos(w, v.sw[base + i], v.cw[base + i]);
            }
        }
        v.lastW0 = w0;
        v.lastMorph = m;
        v.serial = serial_;
    }
    const int live = std::min(span, v.nyquist);            // partials per clone that sound in this stretch
    const int count = (live + 3) & ~3;

    // ---- where the analysed sound has got to
    const float* frame0 = nullptr;
    const float* frame1 = nullptr;
    float frameT = 0.0f;
    int frameAt = 0, userPartials = 0;
    if ((user_[0] || user_[1]) && cur_) {
        const Analysis& an = *cur_;
        const double T = std::max(1.0e-6, an.seconds);
        double pos = v.userPos + (double)v.userDir * (double)b.userSpeed * (double)n / sr_;
        if (b.userLoop) {
            const double ls = (double)std::min(b.userLoopStart, b.userLoopEnd) * T, le = (double)std::max(b.userLoopStart, b.userLoopEnd) * T;
            if (v.userDir > 0.0f && pos >= le) { pos = 2.0 * le - pos; v.userDir = -1.0f; }
            if (v.userDir < 0.0f && pos <= ls) { pos = 2.0 * ls - pos; v.userDir = 1.0f; }
            if (pos > le) pos = le;
            if (v.userDir < 0.0f && pos < ls) pos = ls;
        }
        pos = clampT(pos, 0.0, T);
        v.userPos = pos;
        const double at = (double)(an.frames - 1) * std::sqrt(pos / T);
        frameAt = clampT((int)at, 0, an.frames - 1);
        const int next = std::min(frameAt + 1, an.frames - 1);
        frameT = (float)(at - (double)frameAt);
        frame0 = an.amp.data() + (size_t)frameAt * (size_t)an.partials;
        frame1 = an.amp.data() + (size_t)next * (size_t)an.partials;
        userPartials = an.partials;
    }

    // ---- what shapes the levels in this stretch
    const float gridOff = ((b.formFixed ? log2f0 : kLog2OfRef) - kLog2Of20) * (float)kGridPerOct;
    // brightness from velocity, envelope 2 and the LFOs, in dB per octave
    const float tiltDb = clampT(b.velBright * (v.velN - 1.0f) + b.e2Tilt * env2 + lf[0] * b.lfoTilt[0] + lf[1] * b.lfoTilt[1], -24.0f, 24.0f);
    const float dampT = b.damp / kDbPerOct * (float)v.time;
    const bool filt = b.filterType != FilterOff;
    float filtOff = 0.0f;
    if (filt) {
        const float oct = b.filterEnv * 8.0f * env2 + (v.modX - 0.5f) * 10.0f + b.filterKey * (float)(semis - (double)kRootKey) / 12.0f
                        + lf[0] * b.lfoFilter[0] + lf[1] * b.lfoFilter[1];
        const float log2fc = clampT((float)std::log2(b.cutoff) + oct, 3.0f, 15.5f);      // 8 Hz .. 46 kHz
        filtOff = (log2f0 - log2fc + 10.0f) * (float)kFiltPerOct;
    }
    // If none of it has moved since the last stretch, the levels are where they already stand.
    const float sig[8] = {m, tiltDb, dampT, filtOff, gridOff, frameT, (float)frameAt, (float)live};
    bool still = !retune && !v.fresh && v.tables == tables_;
    for (int i = 0; i < 8; ++i) still = still && sig[i] == v.sig[i];
    float level[kPartials];
    if (!still) {
        for (int i = 0; i < 8; ++i) v.sig[i] = sig[i];
        v.tables = tables_;

        // A and B
        float lin[kPartials], partB[kPartials], l2v[kPartials];
        const bool anyVowel = vowel_[0] || vowel_[1];
        double energyA = 0.0, energyB = 0.0;
        for (int i = 0; i < live; ++i) {
            float a = amp_[0][i], bb = amp_[1][i];
            const float l2 = l2A[i] + m * (l2B[i] - l2A[i]);
            l2v[i] = l2;
            if (frame0) {
                const float u = i < userPartials ? frame0[i] + (frame1[i] - frame0[i]) * frameT : 0.0f;
                if (user_[0]) a *= u;
                if (user_[1]) bb *= u;
            }
            if (anyVowel) {
                const float pos = clampT(l2 * (float)kGridPerOct + gridOff, 0.0f, (float)(kGridSize - 1));
                const int g = (int)pos;
                const float t = pos - (float)g;
                if (vowel_[0]) a *= vowelEnv_[0][g] + (vowelEnv_[0][g + 1] - vowelEnv_[0][g]) * t;
                if (vowel_[1]) bb *= vowelEnv_[1][g] + (vowelEnv_[1][g + 1] - vowelEnv_[1][g]) * t;
                energyA += (double)a * a;
                energyB += (double)bb * bb;
            }
            lin[i] = a;
            partB[i] = bb;
        }
        // A vowel's resonances stay where they are while the harmonics move with the key, so how much of the
        // sound falls under them changes from note to note. Its level is evened out here, note by note (by up
        // to 24 dB), so a vowel is as loud at the top of the keyboard as at the bottom.
        float evenA = 1.0f, evenB = 1.0f;
        if (vowel_[0] && energyA > 1.0e-12) evenA = (float)std::min(16.0, 1.0 / std::sqrt(energyA));
        if (vowel_[1] && energyB > 1.0e-12) evenB = (float)std::min(16.0, 1.0 / std::sqrt(energyB));
        // morphed
        for (int i = 0; i < live; ++i) {
            const float a = lin[i] * evenA, bb = partB[i] * evenB;
            lin[i] = a + m * (bb - a);
        }

        // the tilt, which leaves the energy where it was
        float scale = 1.0f / std::sqrt((float)C);              // the clones share the level between them
        if (tiltDb != 0.0f) {
            const float t = tiltDb / kDbPerOct;
            double before = 0.0, after = 0.0;
            for (int i = 0; i < live; ++i) {
                before += (double)lin[i] * lin[i];
                lin[i] *= fexp2(t * l2v[i]);
                after += (double)lin[i] * lin[i];
            }
            if (after > 1.0e-30 && before > 0.0) scale *= (float)std::sqrt(before / after);
        }

        // damping, formants and the filter: all gains over the partials, added up in powers of two
        for (int i = 0; i < live; ++i) {
            float lg = -dampT * std::max(0.0f, v.ratio[i] - 1.0f);
            if (b.formOn) lg += tableAt(formant_, l2v[i] * (float)kGridPerOct + gridOff, kGridSize - 1);
            if (filt) lg += tableAt(filter_, l2v[i] * (float)kFiltPerOct + filtOff, kFiltSize - 1);
            level[i] = lin[i] * fexp2(clampT(lg, -40.0f, 4.0f)) * scale * v.fade[i];
        }
        for (int i = live; i < count; ++i) level[i] = 0.0f;
    }

    // ---- the oscillators, a clone at a time
    float accL[kQuantum], accR[kQuantum], mono[kQuantum];
    if (C > 1) for (int t = 0; t < n; ++t) accL[t] = accR[t] = 0.0f;
    const bool respread = b.prismSpread != v.spreadSeen;
    const int dropFrom = count, dropTo = std::min(stride, (v.harmonics + 3) & ~3);
    for (int c = 0; c < C; ++c) {
        const int base = c * stride;
        if (v.fresh) for (int i = 0; i < count; ++i) v.cur[base + i] = level[i];
        const float* target = still ? v.cur + base : level;
        if (count > 0) {
#ifdef PULP_SHEAF_SSE
            if (!forceScalar) bankSse(v.c + base, v.s + base, v.cw + base, v.sw + base, v.cur + base, target, count, n, mono);
            else
#endif
                bankScalar(v.c + base, v.s + base, v.cw + base, v.sw + base, v.cur + base, target, count, n, mono);
        } else {
            for (int t = 0; t < n; ++t) mono[t] = 0.0f;
        }
        for (int i = dropFrom; i < dropTo; ++i) v.cur[base + i] = 0.0f;        // partials that have gone past half the sample rate
        if (C == 1) {
            for (int t = 0; t < n; ++t) accL[t] = accR[t] = mono[t];
        } else if (!respread) {
            const float l = v.cloneL[c], r = v.cloneR[c];
            for (int t = 0; t < n; ++t) { accL[t] += mono[t] * l; accR[t] += mono[t] * r; }
        } else {
            // the clones are laid out from left to right (equal power); a moved spread is ramped
            const float place = (-1.0f + 2.0f * (float)c / (float)(C - 1)) * b.prismSpread;
            const double a = (double)(place + 1.0f) * kPi * 0.25;
            const float gl = (float)(std::cos(a) * 1.4142135623730951), gr = (float)(std::sin(a) * 1.4142135623730951);
            if (v.spreadSeen < 0.0f) { v.cloneL[c] = gl; v.cloneR[c] = gr; }           // a new note starts in place
            const float dl = (gl - v.cloneL[c]) / (float)n, dr = (gr - v.cloneR[c]) / (float)n;
            float l = v.cloneL[c], r = v.cloneR[c];
            for (int t = 0; t < n; ++t) {
                l += dl; r += dr;
                accL[t] += mono[t] * l;
                accR[t] += mono[t] * r;
            }
            v.cloneL[c] = gl;
            v.cloneR[c] = gr;
        }
    }
    v.spreadSeen = b.prismSpread;
    v.harmonics = live;

    // ---- amplitude envelope, tremolo, velocity, pan
    const float trem = (1.0f - b.lfoAmp[0] * (0.5f - 0.5f * lf[0])) * (1.0f - b.lfoAmp[1] * (0.5f - 0.5f * lf[1]));
    if (v.fresh) v.ampMod.snap(trem);
    else v.ampMod.target(trem, n);
    v.fresh = false;
    for (int t = 0; t < n; ++t) {
        const float e = v.amp.next();
        const float g = e * v.ampMod.next() * v.velGain * v.kill * v.declick;
        if (v.declick < 1.0f) v.declick = std::min(1.0f, v.declick + v.declickStep);
        L[t] += accL[t] * g * v.panL;
        R[t] += accR[t] * g * v.panR;
        if (v.killStep > 0.0f) { v.kill -= v.killStep; if (v.kill <= 0.0f) { v.kill = 0.0f; v.active = false; break; } }
        if (!v.amp.active()) { v.active = false; break; }
    }
}

void Sheaf::render(float* L, float* R, int n, ProcessContext&) {
    float mixL[kQuantum], mixR[kQuantum];
    for (int t = 0; t < n; ++t) mixL[t] = mixR[t] = 0.0f;
    for (int i = 0; i < kSlots; ++i)
        if (voices_[i].active) renderVoice(voices_[i], mixL, mixR, n);
    for (int t = 0; t < n; ++t) {
        const float g = vol_.next();
        mixL[t] *= g;
        mixR[t] *= g;
        L[t] += mixL[t];
        R[t] += mixR[t];
    }
    scope_.push(mixL, mixR, n);
    countVoices();
}

// ------------------------------------------------------------------------------------ resynthesis

void Sheaf::Analysis::derive() {
    amp.resize(q.size());
    for (size_t i = 0; i < q.size(); ++i) amp[i] = q[i] >= kSilent ? 0.0f : (float)std::pow(10.0, -(double)q[i] * kStepDb / 20.0);
    loudest = 0;
    double best = -1.0;
    for (int f = 0; f < frames; ++f) {
        double e = 0.0;
        for (int p = 0; p < partials; ++p) { const double a = amp[(size_t)(f * partials + p)]; e += a * a; }
        if (e > best) { best = e; loudest = f; }
    }
}

Json Sheaf::Analysis::toJson() const {
    Json j = Json::object();
    j.set("v", 1);
    j.set("name", name);
    j.set("hz", hz);
    j.set("seconds", seconds);
    j.set("offset", offset);
    j.set("ref", ref);
    j.set("holds", holds);
    j.set("frames", frames);
    j.set("partials", partials);
    Json t = Json::array();
    for (unsigned short v : q) t.push(Json((int)v));
    j.set("q", t);
    return j;
}

std::shared_ptr<Sheaf::Analysis> Sheaf::Analysis::fromJson(const Json& j) {
    if (!j.isObject()) return nullptr;
    auto a = std::make_shared<Analysis>();
    a->name = j["name"].str("");
    a->hz = j["hz"].num(0.0);
    a->seconds = j["seconds"].num(0.0);
    a->offset = j["offset"].num(0.0);
    a->ref = j["ref"].num(1.0);
    a->holds = j["holds"].b(false);
    a->frames = j["frames"].i(0);
    a->partials = j["partials"].i(0);
    const Json& t = j["q"];
    if (a->frames < 1 || a->frames > kMaxFrames || a->partials < 1 || a->partials > kPartials) return nullptr;
    if (!t.isArray() || t.size() != (size_t)a->frames * (size_t)a->partials) return nullptr;
    if (!(a->seconds > 0.0) || !(a->hz > 0.0) || !std::isfinite(a->seconds) || !std::isfinite(a->hz) || !std::isfinite(a->ref)) return nullptr;
    a->q.resize(t.size());
    for (size_t i = 0; i < t.size(); ++i) a->q[i] = (unsigned short)clampT(t.at(i).i(kSilent), 0, (int)kSilent);
    a->derive();
    return a;
}

std::shared_ptr<Sheaf::Analysis> Sheaf::analyseSample(const SampleData& s, std::string* error) {
    auto fail = [&](const char* why) -> std::shared_ptr<Analysis> { if (error) *error = why; return nullptr; };
    const int64 total = s.frames();
    const double sr = s.sampleRate;
    if (total < 64 || !(sr >= 4000.0)) return fail("the recording is too short to analyse");
    std::vector<float> mono((size_t)total);
    float top = 0.0f;
    for (int64 i = 0; i < total; ++i) {
        const float v = s.channels > 1 ? 0.5f * (s.ch[0][(size_t)i] + s.ch[1][(size_t)i]) : s.ch[0][(size_t)i];
        mono[(size_t)i] = v;
        top = std::max(top, std::fabs(v));
    }
    if (!(top >= 1.0e-5f) || !std::isfinite(top)) return fail("the recording is silent");
    const ms::Range sound = ms::findSound(s);
    const int64 len = sound.end - sound.start;
    if ((double)len < sr * 0.04) return fail("the recording is too short to analyse");
    const ms::Pitch heard = ms::detectPitch(s);
    if (!(heard.hz > 0.0) || heard.clarity < 0.5) return fail("no clear pitch was found: analyse a recording of one steady note");
    double f0 = heard.hz;
    if (f0 < 20.0 || f0 > 0.2 * sr) return fail("the pitch of the recording is out of range");

    // A windowed spectrum around a moment in the recording. Near either end the window is slid inwards so
    // that all of it lies on the recording: a window cut off by the edge would smear every strong harmonic
    // across the whole spectrum. (So the first and last half-window of a recording read as one moment each.)
    std::vector<float> re, im;
    auto spectrumAt = [&](int64 centre, int win, int size, double& wsum) {
        re.assign((size_t)size, 0.0f);
        im.assign((size_t)size, 0.0f);
        wsum = 0.0;
        if (total >= win) centre = clampT<int64>(centre, win / 2, total - win + win / 2);
        for (int i = 0; i < win; ++i) {
            const int64 at = centre - win / 2 + i;
            if (at < 0 || at >= total) continue;
            const double x = kTwoPi * ((double)i + 0.5) / win;
            const double w = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) - 0.01168 * std::cos(3.0 * x);   // Blackman-Harris: side lobes 92 dB down
            re[(size_t)i] = (float)(w * mono[(size_t)at]);
            wsum += w;
        }
        dsp::fft(re.data(), im.data(), size);
    };
    // The strongest peak within range of hz: its height and where exactly it is.
    auto peakNear = [&](int size, double hz, double range, double* where) -> double {
        const double binHz = sr / size;
        const int lo = std::max(1, (int)std::floor((hz - range) / binHz)), hi = std::min(size / 2 - 2, (int)std::ceil((hz + range) / binHz));
        if (where) *where = hz;
        if (hi < lo) return 0.0;
        auto mag = [&](int b) { return std::hypot((double)re[(size_t)b], (double)im[(size_t)b]); };
        int best = lo;
        double bestMag = -1.0;
        for (int bin = lo; bin <= hi; ++bin) { const double v = mag(bin); if (v > bestMag) { bestMag = v; best = bin; } }
        const double a = mag(best - 1), c = mag(best + 1);
        if (bestMag <= 0.0 || a <= 0.0 || c <= 0.0 || a > bestMag || c > bestMag) { if (where) *where = best * binHz; return std::max(0.0, bestMag); }
        const double la = std::log(a), lb = std::log(bestMag), lc = std::log(c), den = la - 2.0 * lb + lc;
        if (den >= -1.0e-12) { if (where) *where = best * binHz; return bestMag; }
        const double d = clampT(0.5 * (la - lc) / den, -1.0, 1.0);
        if (where) *where = (best + d) * binHz;
        return std::exp(lb - 0.25 * (la - lc) * d);
    };

    // ---- the pitch, exactly: from the first harmonics of a long window in the body of the note
    {
        int win = (int)clampT<int64>((int64)std::llround(16.0 * sr / f0), 1024, std::min<int64>(len, 65536));
        win = (int)std::min<int64>(win, len);
        int size = 1024;
        while (size < 4 * win) size <<= 1;
        const int64 centre = sound.start + (int64)(len * 0.4);
        double wsum = 0.0;
        spectrumAt(centre, win, size, wsum);
        auto refine = [&](double reach) {
            double sum = 0.0, weight = 0.0;
            for (int k = 1; k <= 8; ++k) {
                if (k * f0 > 0.45 * sr) break;
                double at = 0.0;
                const double pk = peakNear(size, k * f0, std::min(0.4 * f0, reach * k * f0), &at);
                sum += pk * pk * at / k;
                weight += pk * pk;
            }
            if (weight > 0.0) f0 = sum / weight;
        };
        auto energyAt = [&](double firstMultiple, double step, int howMany) {
            double e = 0.0;
            for (int k = 0; k < howMany; ++k) {
                const double hz = (firstMultiple + step * k) * f0;
                if (hz > 0.45 * sr) break;
                const double pk = peakNear(size, hz, 0.1 * f0, nullptr);
                e += pk * pk;
            }
            return e;
        };
        refine(0.03);
        if ((double)win >= 12.0 * sr / f0) {
            // an octave too high (something sounds half way between the "harmonics") or too low (only the even ones sound)?
            if (energyAt(0.5, 1.0, 8) > 0.05 * energyAt(1.0, 1.0, 8) && f0 * 0.5 >= 20.0) { f0 *= 0.5; refine(0.03); }
            else if (energyAt(1.0, 2.0, 8) < 1.0e-4 * energyAt(2.0, 2.0, 8)) { f0 *= 2.0; refine(0.03); }
        }
        refine(0.015);
    }
    if (!(f0 >= 20.0) || f0 > 0.2 * sr) return fail("the pitch of the recording is out of range");

    // ---- every harmonic at every frame
    const double seconds = (double)len / sr;
    const int frames = clampT((int)std::sqrt(seconds / 0.001) + 1, 8, (int)kMaxFrames);
    // Eight periods under a window whose main lobe is four bins either side: each harmonic stands clear of its
    // neighbours, and what it is measured over is about two periods long in effect.
    const int win = (int)std::min<int64>(65536, std::max<int64>(std::llround(8.0 * sr / f0), std::llround(0.02 * sr)));
    int size = 1024;
    while (size < 4 * win) size <<= 1;
    const double binHz = sr / size;
    int partials = clampT((int)(0.45 * sr / f0), 1, (int)kPartials);
    std::vector<double> level((size_t)frames * (size_t)partials, 0.0);
    double ref = 0.0;
    for (int f = 0; f < frames; ++f) {
        const double u = (double)f / (frames - 1);
        const int64 centre = sound.start + (int64)std::llround(seconds * u * u * sr);
        double wsum = 0.0;
        spectrumAt(centre, win, size, wsum);
        if (wsum < 1.0e-9) continue;
        double* row = level.data() + (size_t)f * (size_t)partials;
        for (int k = 1; k <= partials; ++k) {
            const double range = std::min(0.25 * f0, 0.03 * k * f0 + 2.0 * binHz);
            row[k - 1] = 2.0 * peakNear(size, k * f0, range, nullptr) / wsum;
            ref = std::max(ref, row[k - 1]);
        }
    }
    if (!(ref > 0.0) || !std::isfinite(ref)) return fail("the recording is silent");

    auto out = std::make_shared<Analysis>();
    std::vector<int> q(level.size());
    int used = 1;
    for (int f = 0; f < frames; ++f)
        for (int p = 0; p < partials; ++p) {
            const double a = level[(size_t)f * (size_t)partials + (size_t)p];
            int v = Analysis::kSilent;
            if (a > 0.0) v = clampT((int)std::lround(-20.0 * std::log10(a / ref) / Analysis::kStepDb), 0, (int)Analysis::kSilent);
            q[(size_t)f * (size_t)partials + (size_t)p] = v;
            if (v < Analysis::kSilent) used = std::max(used, p + 1);
        }
    out->name = s.path.empty() ? std::string("Recording") : baseName(s.path);
    out->hz = f0;
    out->seconds = seconds;
    out->offset = (double)sound.start / sr;
    out->ref = ref;
    out->holds = ms::holds(s, sound);
    out->frames = frames;
    out->partials = used;                       // partials above the last one with anything in it are not kept
    out->q.resize((size_t)frames * (size_t)used);
    for (int f = 0; f < frames; ++f)
        for (int p = 0; p < used; ++p) out->q[(size_t)f * (size_t)used + (size_t)p] = (unsigned short)q[(size_t)f * (size_t)partials + (size_t)p];
    out->derive();
    return out;
}

bool Sheaf::analyse(SamplePtr sample, std::string* error) {
    if (!sample) { if (error) *error = "no recording given"; return false; }
    auto a = analyseSample(*sample, error);
    if (!a) return false;
    analysis_.set(a);
    return true;
}
bool Sheaf::hasAnalysis() const { return analysis_.get() != nullptr; }
std::string Sheaf::analysisName() const { auto a = analysis_.get(); return a ? a->name : std::string(); }
bool Sheaf::analysisHolds() const { auto a = analysis_.get(); return a && a->holds; }
std::shared_ptr<const Sheaf::Analysis> Sheaf::analysis() const { return analysis_.get(); }
void Sheaf::clearAnalysis() { analysis_.set(nullptr); }

Json Sheaf::saveExtra() const {
    auto a = analysis_.get();
    if (!a) return Json();
    Json j = Json::object();
    j.set("analysis", a->toJson());
    return j;
}

// A state without a table (a factory preset, a project saved before anything was analysed) leaves the
// analysed sound alone.
void Sheaf::loadExtra(const Json& j, const std::string&) {
    if (j.isNull()) return;
    auto a = Analysis::fromJson(j["analysis"]);
    if (a) analysis_.set(a);
}

// ------------------------------------------------------------------------------------ presets

std::vector<Preset> Sheaf::factoryPresets() const {
    // Shapes: 0 saw, 1 square, 2 triangle, 3 pulse, 4 vowel, 5 bell, 6 organ, 7 user. Filter: 0 off, 1 low, 2 high, 3 band.
    // LFO shapes: 0 sine, 1 triangle, 2 saw up, 3 saw down, 4 square, 5 random steps.
    return buildPresets(*this, {
        {"Plain Saw", {}},
        // ---- basses
        {"Round Bass", {{"ashape", 2}, {"bshape", 0}, {"morph", 0.3f}, {"e2morph", 0.6f}, {"e2decay", 220}, {"partials", 64}, {"cutoff", 1200}, {"slope", 18}, {"fenv", 0.2f},
                        {"coarse", -12}, {"attack", 1}, {"decay", 500}, {"sustain", 0.75f}, {"release", 120}, {"velbright", 6}, {"volume", 0.64f}}},
        {"Hollow Bass", {{"ashape", 1}, {"bshape", 3}, {"bcolour", 0.18f}, {"morph", 0.35f}, {"l2morph", 0.3f}, {"l2rate", 0.4f}, {"partials", 64}, {"cutoff", 1800}, {"reso", 5},
                         {"fenv", 0.25f}, {"e2decay", 300}, {"e2sustain", 0.2f}, {"coarse", -12}, {"attack", 1}, {"sustain", 0.8f}, {"release", 100}, {"velbright", 4}, {"volume", 0.49f}}},
        {"Growl Bass", {{"ashape", 4}, {"acolour", 0.0f}, {"bshape", 4}, {"bcolour", 0.85f}, {"morph", 0.5f}, {"l1wave", 1}, {"l1rate", 2.5f}, {"l1morph", 0.5f}, {"coarse", -12},
                        {"prism", 2}, {"prismdetune", 10}, {"prismspread", 0.4f}, {"sustain", 0.9f}, {"release", 140}, {"velbright", 5}, {"volume", 0.31f}}},
        {"Sub Thump", {{"ashape", 2}, {"partials", 6}, {"e2tilt", 10}, {"e2decay", 90}, {"coarse", -12}, {"attack", 0}, {"decay", 450}, {"sustain", 0.4f}, {"release", 160}, {"volume", 0.46f}}},
        {"Reed Bass", {{"ashape", 3}, {"acolour", 0.12f}, {"atilt", -2}, {"cutoff", 2600}, {"slope", 12}, {"fkey", 0.5f}, {"f1freq", 500}, {"f1width", 0.6f}, {"f1gain", 6},
                       {"coarse", -12}, {"attack", 4}, {"sustain", 0.85f}, {"release", 110}, {"velbright", 6}, {"volume", 0.28f}}},
        {"Rubber Bass", {{"ashape", 0}, {"aoddeven", -0.6f}, {"bshape", 2}, {"morph", 1.0f}, {"e2morph", -1.0f}, {"e2decay", 180}, {"glide", 40}, {"coarse", -12},
                         {"attack", 1}, {"decay", 600}, {"sustain", 0.7f}, {"release", 90}, {"velbright", 5}, {"volume", 0.72f}}},
        // ---- leads
        {"Glass Lead", {{"ashape", 1}, {"bshape", 0}, {"morph", 0.3f}, {"partials", 96}, {"prism", 3}, {"prismdetune", 9}, {"prismspread", 0.6f}, {"glide", 50}, {"l1pitch", 12},
                        {"l1rate", 5.5f}, {"l1fade", 400}, {"cutoff", 9000}, {"attack", 3}, {"sustain", 0.85f}, {"release", 180}, {"velbright", 6}, {"volume", 0.41f}}},
        {"Talking Lead", {{"ashape", 4}, {"acolour", 0.0f}, {"bshape", 4}, {"bcolour", 0.5f}, {"e2morph", 1.0f}, {"e2attack", 300}, {"e2sustain", 1.0f}, {"l1pitch", 10}, {"l1fade", 500},
                          {"glide", 30}, {"attack", 15}, {"sustain", 0.9f}, {"release", 200}, {"velbright", 4}, {"volume", 0.45f}}},
        {"Comb Lead", {{"ashape", 0}, {"acomb", 3}, {"acombdepth", 0.9f}, {"bshape", 0}, {"bcomb", 5}, {"bcombdepth", 0.9f}, {"morph", 0.5f}, {"l2wave", 1}, {"l2rate", 0.25f},
                       {"l2morph", 0.5f}, {"release", 160}, {"velbright", 8}, {"volume", 0.48f}}},
        {"Soft Whistle", {{"ashape", 2}, {"partials", 3}, {"l1pitch", 18}, {"l1rate", 5.0f}, {"l1fade", 300}, {"coarse", 12}, {"attack", 40}, {"sustain", 0.9f}, {"release", 220}, {"volume", 0.64f}}},
        {"Nasal Lead", {{"ashape", 3}, {"acolour", 0.3f}, {"f1freq", 1400}, {"f1width", 0.35f}, {"f1gain", 12}, {"f2freq", 3200}, {"f2width", 0.3f}, {"f2gain", 8}, {"glide", 25},
                        {"l1pitch", 9}, {"l1rate", 5.8f}, {"l1fade", 350}, {"attack", 5}, {"sustain", 0.85f}, {"release", 150}, {"velbright", 5}, {"volume", 0.42f}}},
        // ---- pads
        {"Morning Pad", {{"ashape", 0}, {"atilt", -3}, {"bshape", 4}, {"bcolour", 0.3f}, {"morph", 0.4f}, {"l2rate", 0.12f}, {"l2morph", 0.35f}, {"partials", 64}, {"prism", 4},
                         {"prismdetune", 16}, {"prismspread", 0.9f}, {"phases", 1}, {"cutoff", 5000}, {"slope", 12}, {"attack", 700}, {"decay", 1500}, {"sustain", 0.85f}, {"release", 1400}, {"volume", 0.43f}}},
        {"Choir Pad", {{"ashape", 4}, {"acolour", 0.0f}, {"bshape", 4}, {"bcolour", 0.75f}, {"morph", 0.3f}, {"l2rate", 0.1f}, {"l2morph", 0.3f}, {"partials", 64}, {"prism", 4},
                       {"prismdetune", 14}, {"prismblur", 0.5f}, {"prismspread", 1.0f}, {"phases", 1}, {"l1pitch", 8}, {"l1rate", 4.8f}, {"l1fade", 900}, {"attack", 500}, {"sustain", 0.9f},
                       {"release", 1100}, {"volume", 0.36f}}},
        {"Glass Pad", {{"ashape", 5}, {"acolour", 0.1f}, {"atilt", -3}, {"bshape", 5}, {"bcolour", 0.7f}, {"btilt", -3}, {"morph", 0.3f}, {"l2rate", 0.07f}, {"l2morph", 0.25f}, {"partials", 24},
                       {"prism", 2}, {"prismdetune", 6}, {"prismspread", 0.8f}, {"attack", 400}, {"decay", 3000}, {"sustain", 0.7f}, {"release", 2000}, {"volume", 0.38f}}},
        {"Prism Strings", {{"ashape", 0}, {"atilt", -2}, {"prism", 6}, {"prismdetune", 22}, {"prismblur", 0.4f}, {"prismspread", 1.0f}, {"phases", 1}, {"cutoff", 7000}, {"slope", 12},
                           {"l1pitch", 6}, {"l1rate", 5.2f}, {"l1fade", 700}, {"attack", 260}, {"decay", 800}, {"sustain", 0.9f}, {"release", 600}, {"velbright", 4}, {"volume", 0.35f}}},
        {"Slow Bloom", {{"ashape", 2}, {"bshape", 0}, {"e2morph", 1.0f}, {"e2attack", 3000}, {"e2decay", 2000}, {"e2sustain", 0.6f}, {"e2release", 1500}, {"cutoff", 600}, {"slope", 18},
                        {"fenv", 0.45f}, {"prism", 3}, {"prismdetune", 12}, {"prismspread", 0.7f}, {"attack", 900}, {"sustain", 1.0f}, {"release", 1800}, {"volume", 0.32f}}},
        // ---- bells
        {"Church Bell", {{"ashape", 5}, {"acolour", 0.0f}, {"atilt", -2}, {"partials", 24}, {"damp", 4}, {"attack", 0}, {"decay", 5000}, {"sustain", 0.0f}, {"release", 4000}, {"velbright", 6}, {"volume", 0.39f}}},
        {"Chime Bar", {{"ashape", 5}, {"acolour", 1.0f}, {"partials", 10}, {"damp", 2}, {"attack", 0}, {"decay", 3500}, {"sustain", 0.0f}, {"release", 3000}, {"velbright", 8}, {"volume", 0.29f}}},
        {"Tine Bell", {{"ashape", 5}, {"acolour", 0.85f}, {"partials", 6}, {"damp", 9}, {"l1rate", 5.0f}, {"l1amp", 0.2f}, {"attack", 0}, {"decay", 2200}, {"sustain", 0.0f}, {"release", 1800},
                       {"velbright", 8}, {"volume", 0.45f}}},
        // ---- organs
        {"Drawbar Organ", {{"ashape", 6}, {"acolour", 0.7f}, {"l1rate", 6.3f}, {"l1amp", 0.12f}, {"attack", 3}, {"decay", 100}, {"sustain", 1.0f}, {"release", 60}, {"velsens", 0.2f}, {"volume", 0.40f}}},
        {"Full Organ", {{"ashape", 6}, {"acolour", 1.0f}, {"prism", 2}, {"prismdetune", 4}, {"prismspread", 0.5f}, {"attack", 5}, {"sustain", 1.0f}, {"release", 80}, {"velsens", 0.1f}, {"volume", 0.30f}}},
        {"Flute Stop", {{"ashape", 6}, {"acolour", 0.2f}, {"l1pitch", 5}, {"l1rate", 4.5f}, {"l1fade", 600}, {"attack", 35}, {"decay", 200}, {"sustain", 0.9f}, {"release", 120}, {"velsens", 0.4f}, {"volume", 0.63f}}},
        // ---- vowels
        {"Aah", {{"ashape", 4}, {"acolour", 0.0f}, {"l1pitch", 14}, {"l1rate", 5.2f}, {"l1fade", 600}, {"attack", 90}, {"sustain", 0.9f}, {"release", 250}, {"velbright", 4}, {"volume", 0.43f}}},
        {"Ooh to Eee", {{"ashape", 4}, {"acolour", 1.0f}, {"bshape", 4}, {"bcolour", 0.5f}, {"e2morph", 1.0f}, {"e2attack", 1800}, {"e2sustain", 1.0f}, {"e2release", 600}, {"l1pitch", 10},
                        {"l1fade", 800}, {"attack", 120}, {"sustain", 0.9f}, {"release", 300}, {"volume", 0.76f}}},
        // ---- glassy keys
        {"Glass Keys", {{"ashape", 0}, {"aoddeven", -0.5f}, {"atilt", -1}, {"inharm", 0.12f}, {"damp", 12}, {"partials", 48}, {"attack", 0}, {"decay", 1800}, {"sustain", 0.0f}, {"release", 900},
                        {"velbright", 10}, {"volume", 0.53f}}},
        {"Stiff String Keys", {{"ashape", 0}, {"atilt", -2}, {"inharm", 0.35f}, {"damp", 7}, {"partials", 64}, {"e2tilt", 6}, {"e2decay", 300}, {"attack", 0}, {"decay", 2600}, {"sustain", 0.0f},
                               {"release", 700}, {"velbright", 9}, {"volume", 0.32f}}},
        {"Mellow Keys", {{"ashape", 2}, {"bshape", 3}, {"bcolour", 0.3f}, {"e2morph", 0.5f}, {"e2decay", 350}, {"e2sustain", 0.05f}, {"l1rate", 4.0f}, {"l1amp", 0.1f}, {"cutoff", 6000},
                         {"attack", 1}, {"decay", 1500}, {"sustain", 0.15f}, {"release", 400}, {"velbright", 8}, {"volume", 0.52f}}},
        // ---- evolving textures
        {"Harmonic Drift", {{"ashape", 0}, {"acomb", 3}, {"acombdepth", 1.0f}, {"bshape", 0}, {"bcomb", 7}, {"bcombdepth", 1.0f}, {"morph", 0.5f}, {"l2rate", 0.08f}, {"l2morph", 0.5f},
                            {"l1rate", 0.19f}, {"l1tilt", 2}, {"prism", 4}, {"prismdetune", 10}, {"prismblur", 1.0f}, {"prismspread", 1.0f}, {"phases", 1}, {"attack", 1500}, {"sustain", 1.0f},
                            {"release", 2500}, {"partials", 48}, {"volume", 0.29f}}},
        {"Spectral Rain", {{"ashape", 5}, {"acolour", 0.0f}, {"bshape", 5}, {"bcolour", 1.0f}, {"morph", 0.5f}, {"l1wave", 5}, {"l1rate", 7.0f}, {"l1morph", 0.5f}, {"l2wave", 5}, {"l2rate", 3.1f},
                           {"l2tilt", 2.5f}, {"partials", 20}, {"prism", 2}, {"prismdetune", 8}, {"prismspread", 1.0f}, {"phases", 1}, {"attack", 300}, {"sustain", 0.9f}, {"release", 2500}, {"atilt", -3}, {"btilt", -3}, {"volume", 0.35f}}},
        {"Metal Bloom", {{"ashape", 5}, {"acolour", 1.0f}, {"bshape", 5}, {"bcolour", 0.0f}, {"e2morph", 1.0f}, {"e2attack", 4000}, {"e2sustain", 1.0f}, {"e2release", 2000}, {"inharm", 0.15f}, {"partials", 32},
                         {"cutoff", 3000}, {"fenv", 0.3f}, {"phases", 1}, {"attack", 800}, {"sustain", 1.0f}, {"release", 2500}, {"volume", 0.32f}}},
        {"Odd Even Shimmer", {{"ashape", 0}, {"aoddeven", -1.0f}, {"bshape", 0}, {"boddeven", 1.0f}, {"morph", 0.5f}, {"l1rate", 3.5f}, {"l1morph", 0.5f}, {"prism", 2}, {"prismdetune", 7},
                              {"prismspread", 0.8f}, {"cutoff", 8000}, {"attack", 200}, {"sustain", 0.9f}, {"release", 900}, {"volume", 0.50f}}},
    });
}

} // namespace pulp
