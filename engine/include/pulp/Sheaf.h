#pragma once
// SHEAF: an additive / resynthesis synthesizer.
//
// Every voice is a bank of up to 256 sine partials. Nothing else makes sound: there is no filter and
// no waveform, so nothing can alias. A partial whose frequency would reach half the sample rate is
// simply not made.
//
// The spectrum is designed, not drawn: two spectra, A and B, each a base shape (saw, square,
// triangle, pulse, vowel, bell, organ, or "user" from an analysed recording) bent by a tilt, an
// odd / even balance and a harmonic comb; a morph between the two; then, per voice, two formant
// peaks, a "filter" that is only a gain curve over the partials, damping of the high partials over
// time, and brightness from velocity, the second envelope and the LFOs.
//
// Resynthesis: analyse() takes a recording of one note, finds its pitch and measures every harmonic
// over time. The result (a small table, frames x partials) is the "user" spectrum; it is state, saved
// with the project as numbers, so the recording itself is not needed again.
#include "pulp/Audio.h"
#include "pulp/Dsp.h"
#include "pulp/Effects.h"
#include "pulp/Plugin.h"
#include "pulp/SynthParts.h"

namespace pulp {

class Sheaf : public Instrument {
public:
    static constexpr int kPartials = 256;       // sine oscillators per voice
    static constexpr int kMaxNotes = 32;        // highest polyphony setting
    static constexpr int kSlots = 40;           // voice slots: the notes plus ones that are fading out after being stolen
    static constexpr int kMaxClones = 8;        // prism: detuned copies of every partial
    static constexpr int kMaxFrames = 64;       // time frames in an analysis
    enum Shape { Saw, Square, Triangle, Pulse, Vowel, Bell, Organ, User, NumShapes };
    enum FilterType { FilterOff, LowPass, HighPass, BandPass };

    // What analyse() makes of a recording: the level of each harmonic at a number of moments.
    struct Analysis {
        static constexpr int kSilent = 1800;    // a stored level of this or more means "nothing there" (90 dB down)
        static constexpr double kStepDb = 0.05; // one step of a stored level
        std::string name;
        double hz = 0.0;            // the pitch of the recording
        double seconds = 0.0;       // how long the analysed sound is
        double offset = 0.0;        // where in the recording it begins, in seconds
        double ref = 1.0;           // the amplitude that level 0 stands for: the strongest partial at its strongest
        bool holds = false;         // the sound keeps its level (bowed, blown, sung) rather than dying away
        int frames = 0, partials = 0;
        std::vector<unsigned short> q;      // frames x partials: how far below ref, in steps of kStepDb
        std::vector<float> amp;             // the same as amplitudes relative to ref (made from q)
        int loudest = 0;                    // the frame with the most energy
        // Frames sit closer together at the start, where sounds change fastest.
        double frameTime(int frame) const { const double u = frames > 1 ? (double)frame / (frames - 1) : 0.0; return seconds * u * u; }
        float level(int frame, int partial) const { return amp[(size_t)(frame * partials + (partial - 1))] * (float)ref; }   // partial 1 is the fundamental
        void derive();              // fills amp and loudest from q
        Json toJson() const;
        static std::shared_ptr<Analysis> fromJson(const Json& j);
    };

    Sheaf();
    const char* typeId() const override { return "additive"; }
    const char* displayName() const override { return "Sheaf"; }
    void prepare(double sampleRate) override;
    void reset() override;
    int activeVoices() const override { return voicesActive_.load(std::memory_order_relaxed); }
    std::vector<Preset> factoryPresets() const override;
    std::vector<ParamGroup> paramGroups() const override;
    int readScope(float* dst, int n) const override { return scope_.read(dst, n); }
    Json saveExtra() const override;
    void loadExtra(const Json& j, const std::string& projectDir) override;

    // ---- resynthesis (control thread) ----
    // Analyses a recording of a single note and makes it the "user" spectrum. Returns false and says why
    // when the recording is silent, too short or has no clear pitch; what was there before then stays.
    bool analyse(SamplePtr sample, std::string* error = nullptr);
    bool hasAnalysis() const;
    std::string analysisName() const;
    bool analysisHolds() const;                 // true: a sound that sustains, so looping it makes sense
    std::shared_ptr<const Analysis> analysis() const;
    void clearAnalysis();
    // The analysis on its own, without touching an instrument.
    static std::shared_ptr<Analysis> analyseSample(const SampleData& s, std::string* error = nullptr);

    // How many partials each prism clone gets: 256 shared out, in fours.
    static int partialsPerClone(int clones) { return (kPartials / clampT(clones, 1, (int)kMaxClones)) & ~3; }
    // Stiff-string stretch for the "inharm" parameter: partial ratio r becomes r * sqrt((1 + B r^2) / (1 + B)).
    static double stiffness(float inharm) { return 0.01 * (double)inharm; }
    static double stretched(double ratio, double b) { return ratio * std::sqrt((1.0 + b * ratio * ratio) / (1.0 + b)); }
    // The bell shape's partial ratios for a colour setting. Partial 1 is the played pitch at every colour; partial 2
    // is the hum an octave below; from partial 3 they rise, from a tuned bell's (colour 0) to a struck bar's (colour 1).
    static double bellRatio(int partial, float colour);

    // Tests only: use the plain C++ oscillator loop instead of the SSE one (the two give the same samples).
    bool forceScalar = false;

    // ---- parameters ----
    struct Spec { Param *shape, *colour, *tilt, *oddEven, *comb, *combDepth; };
    Spec spec[2];                                           // A and B
    Param *pMorph, *pPartials, *pInharm, *pDamp, *pPhases;
    Param *pPrism, *pPrismDetune, *pPrismBlur, *pPrismSpread;
    Param *pFormFixed, *pFormFreq[2], *pFormWidth[2], *pFormGain[2];
    Param *pFilterType, *pCutoff, *pSlope, *pReso, *pFilterEnv, *pFilterKey;
    Param *pUserSpeed, *pUserStart, *pUserLoop, *pUserLoopStart, *pUserLoopEnd;
    Param *pAttack, *pDecay, *pSustain, *pRelease;
    Param *pE2Attack, *pE2Decay, *pE2Sustain, *pE2Release, *pE2Morph, *pE2Tilt;
    struct Lfo { Param *wave, *rate, *fade, *pitch, *morph, *filter, *tilt, *amp; };
    Lfo lfo[2];
    Param *pVolume, *pVelSens, *pVelBright, *pGlide, *pVoices, *pCoarse, *pFine;

protected:
    void beginBlock(ProcessContext& ctx) override;
    void handleEvent(const NoteEvent& e, ProcessContext& ctx) override;
    void render(float* L, float* R, int n, ProcessContext& ctx) override;

private:
    static constexpr int kGridPerOct = 96, kGridSize = 10 * kGridPerOct + 1;    // 20 Hz .. 20.48 kHz
    static constexpr int kFiltPerOct = 32, kFiltSize = 20 * kFiltPerOct + 1;    // -10 .. +10 octaves about the cutoff

    struct Voice {
        bool active = false, fresh = false, released = false;
        uint32 id = 0, age = 0;
        int key = 60, group = 0;
        int clones = 1, stride = kPartials;
        float velGain = 1.0f, velN = 1.0f, panL = 1.0f, panR = 1.0f;
        float fine = 0.0f, bend = 0.0f, bendTarget = 0.0f, bendStep = 0.0f;
        float modX = 0.5f, modY = 0.5f, relScale = 1.0f;
        parts::Env amp, env2;
        float kill = 1.0f, killStep = 0.0f;
        float declick = 1.0f, declickStep = 0.0f;
        double time = 0.0;                      // seconds since the note began
        float lfoPhase[2] = {0.0f, 0.0f}, lfoRnd[2] = {0.0f, 0.0f};
        uint32 rnd = 1;
        double userPos = 0.0;                   // where in the analysed sound this voice is, in seconds
        float userDir = 1.0f;
        Ramp ampMod;
        double lastW0 = -1.0;
        float lastMorph = -1.0f;
        uint32 serial = 0;
        int harmonics = 0;                      // partials per clone that were sounding last time
        int nyquist = 0;                        // partials per clone below half the sample rate
        float cloneL[kMaxClones], cloneR[kMaxClones];
        float spreadSeen = -1.0f;
        float sig[8] = {};                      // what the levels were last worked out from
        uint32 tables = 0;
        float ratio[kPartials];                 // frequency of partial i over the fundamental
        float fade[kPartials];                  // 1, falling to 0 as partial i nears half the sample rate
        // oscillator j = clone * stride + partial
        float c[kPartials], s[kPartials], cw[kPartials], sw[kPartials], cur[kPartials];
    };

    // The parameter values as of the start of the quantum.
    struct Block {
        float morph, partials, inharm, damp;
        int phases, prism;
        float prismDetune, prismBlur, prismSpread;
        bool formFixed, formOn;
        int filterType;
        float cutoff, filterEnv, filterKey;
        float userSpeed, userStart, userLoopStart, userLoopEnd;
        bool userLoop;
        float attack, decay, sustain, release;
        float e2Attack, e2Decay, e2Sustain, e2Release, e2Morph, e2Tilt;
        int lfoWave[2];
        float lfoRate[2], lfoFade[2], lfoPitch[2], lfoMorph[2], lfoFilter[2], lfoTilt[2], lfoAmp[2];
        float velSens, velBright, glide, coarse, fine;
        int voices;
    };

    void buildSpectrum(int which);
    void buildFormants();
    void buildFilter();
    void startVoice(const NoteEvent& e);
    void renderVoice(Voice& v, float* L, float* R, int n);
    void countVoices();

    std::unique_ptr<Voice[]> voices_;
    Block b_{};
    double sr_ = 48000.0;
    float bend_ = 0.0f;
    uint32 ageCounter_ = 0, noteCounter_ = 0;
    float lastKey_ = -1.0f;
    bool fresh_ = true;
    Ramp vol_;
    std::atomic<int> voicesActive_{0};
    ScopeTap scope_;

    // The two spectra as tables over the partial number, rebuilt when their parameters change.
    float amp_[2][kPartials];           // level (signed), with tilt, odd / even and comb in it. For "user": the gain to apply to the analysed level
    float ratio_[2][kPartials];         // frequency ratio, stretched
    float log2Ratio_[2][kPartials];
    float phaseC_[kPartials], phaseS_[kPartials];   // where spectrum A's partials start
    bool phased_ = false;               // some partial starts away from zero
    bool user_[2] = {false, false}, vowel_[2] = {false, false};
    float vowelEnv_[2][kGridSize + 1];  // the vowel shape's formant curve over frequency (gain)
    float formant_[kGridSize + 1];      // the two formant peaks over frequency (log2 of gain)
    double gridHz_[kGridSize];          // the frequencies those two curves are laid out on
    float filter_[kFiltSize + 1];       // the filter curve over octaves from the cutoff (log2 of gain)
    bool ratiosDiffer_ = false;
    bool harmonic_ = true;              // every partial of both spectra is a whole-number harmonic
    int maxPartial_ = 0;                // partials beyond this are silent in both spectra
    uint32 serial_ = 1;                 // bumped whenever partial frequencies may have changed
    uint32 tables_ = 1;                 // bumped whenever a table that sets levels is rebuilt
    float seenSpec_[2][8], seenForm_[6], seenFilt_[3], seenPrism_[2];
    const Analysis* seenUser_ = nullptr;

    RtShared<Analysis> analysis_;
    const Analysis* cur_ = nullptr;
};

} // namespace pulp
