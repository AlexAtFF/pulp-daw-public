#pragma once
// Piano-roll tools: scales and chords, and the edits in the tools menu. Each works on the
// selected notes when any are selected, otherwise on all of them. They are plain functions
// on note lists so the tests can check them without a window.
#include "pulp/Project.h"

namespace pulp::tools {

struct NamedIntervals { std::string name; std::vector<int> intervals; };
const std::vector<NamedIntervals>& scales();
const std::vector<NamedIntervals>& chords();
bool inScale(int key, int root, const NamedIntervals& scale);
int snapToScale(int key, int root, const NamedIntervals& scale);
std::string keyName(int key);                       // 60 -> "C5"

enum Prop { Velocity, Pan, Release, ModX, ModY, Fine, NumProps };
const char* propName(int prop);
float getProp(const Note& n, int prop);             // normalised 0..1
void setProp(Note& n, int prop, float v);
float defaultProp(int prop);

bool anySelected(const std::vector<Note>& notes);

struct QuantizeOpts {
    int grid = kTicksPerStep;
    float strength = 1.0f;      // 0..1: how far each note moves toward the grid
    bool starts = true;         // move start times
    bool ends = false;          // also snap note ends (durations)
    bool keepEnds = false;      // when starts move, leave the end where it was
};
void quantize(std::vector<Note>& notes, const QuantizeOpts& o);
void legato(std::vector<Note>& notes);                              // each note lasts until the next one starts
void chop(std::vector<Note>& notes, int grid);                      // cut notes into grid-sized pieces
void glue(std::vector<Note>& notes);                                // join touching notes of the same key
enum ArpPattern { ArpUp, ArpDown, ArpUpDown, ArpRandom };
void arpeggiate(std::vector<Note>& notes, int grid, int pattern, int octaves, float gate, uint32 seed = 1);
void strum(std::vector<Note>& notes, int ticksPerNote, bool downward, float velocityTaper);
void flam(std::vector<Note>& notes, int ticksBefore, float velocityScale);
void articulate(std::vector<Note>& notes, float lengthMultiply, int gapTicks);   // lengthMultiply 0 = legato minus gap
struct RandomizeOpts {
    float velocity = 0.0f, pan = 0.0f, length = 0.0f;   // 0..1 amounts
    int pitchRange = 0;                                  // semitones either way
    int scaleRoot = -1, scaleIndex = 0;                  // keep randomised pitches in a scale (root < 0 = off)
    uint32 seed = 1;
};
void randomize(std::vector<Note>& notes, const RandomizeOpts& o);
void scaleLevels(std::vector<Note>& notes, int prop, float multiply, float offset, float centre = 0.5f);
void flipVertical(std::vector<Note>& notes);
void reverseTime(std::vector<Note>& notes);
void transpose(std::vector<Note>& notes, int semitones);
void shiftTime(std::vector<Note>& notes, int ticks);
// Keeps keepTicks, removes dropTicks and closes the gap, over and over (for odd-length loops).
void trimPattern(std::vector<Note>& notes, int keepTicks, int dropTicks);
enum LfoShape { LfoSine, LfoTriangle, LfoSquare, LfoSawUp, LfoSawDown };
void lfo(std::vector<Note>& notes, int prop, int shape, int periodTicks, float lo, float hi, float phase);
// The same wave as a list of events for a control (values 0..1 of its range), from tick 0 to lengthTicks:
// straight lines for the triangle and the saws, steps for the square, sixteen lines a cycle for the sine.
std::vector<AutoEvent> lfoEvents(int lengthTicks, int shape, int periodTicks, float lo, float hi, float phase);
void duplicateAfter(std::vector<Note>& notes);                      // Ctrl+B: copies placed straight after the originals and selected

struct RiffOpts {
    enum Kind { Progression, Arpeggio, Groove };
    int kind = Progression;
    int root = 60;              // key
    int scaleIndex = 0;         // index into scales()
    int bars = 4;
    int ticksPerBar = 16 * kTicksPerStep;
    float density = 0.5f;       // 0..1
    bool sevenths = false;
    uint32 seed = 1;
};
std::vector<Note> riff(const RiffOpts& o);

} // namespace pulp::tools
