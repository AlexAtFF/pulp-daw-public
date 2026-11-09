#pragma once
// Automation: curves in automation clips, events inside patterns, and links from controllers
// (hardware MIDI controls and the controller plugins) to parameters.
// A parameter is named by an address:
//   tempo, mastervol, masterpitch
//   ch:<channel id>:<id>          a channel setting          chp:<channel id>:<id>     its instrument
//   mix:<track>:<id>              a mixer strip control      fx:<track>:<slot>:<id>    an effect in a slot
//   send:<from>:<to>              a send level
#include "pulp/Formula.h"
#include "pulp/Plugin.h"

namespace pulp {

enum AutoShape { ShapeCurve = 0, ShapeDouble, ShapeHold, ShapeStairs, ShapePulse, ShapeWave, NumAutoShapes };
const char* autoShapeName(int shape);

// A point of an automation curve. Shape and tension describe the stretch that starts at this point.
struct AutoPoint {
    int64 tick = 0;
    float value = 0.5f;         // 0..1
    float tension = 0.0f;       // -1..1: how the curve bends; for stairs, pulse and wave it sets how many
    int shape = ShapeCurve;
};
// The value part-way (x in 0..1) along a stretch from a to b.
float autoSegment(float a, float b, double x, int shape, float tension);

struct AutoLfo {
    bool on = false;
    int shape = 0;              // 0 sine, 1 triangle, 2 square, 3 saw
    float periodSteps = 16.0f;  // length of one cycle in steps
    float depth = 0.25f;        // how far it swings each side of the curve
    float phase = 0.0f;         // 0..1
};

// The data behind automation clips, as a pattern is the data behind pattern clips.
struct AutoClip {
    int id = 0;
    std::string name;
    uint32 color = 0xff9a6fb0;
    std::string target;                 // parameter address
    std::vector<AutoPoint> points;      // sorted by tick, at least one
    AutoLfo lfo;
    float min = 0.0f, max = 1.0f;       // the part of the control's travel the curve covers
    int64 length() const { return points.empty() ? 0 : points.back().tick; }
};
float evalAuto(const std::vector<AutoPoint>& points, const AutoLfo& lfo, double tick);     // 0..1

// Event automation inside a pattern: a value from its tick on; with ramp set it glides to the next event.
struct AutoEvent {
    int tick = 0;
    float value = 0.0f;
    bool ramp = false;
};
float evalEvents(const std::vector<AutoEvent>& events, double tick);                        // 0..1
void sortEvents(std::vector<AutoEvent>& events);
// Thins recorded movement: drops points that lie within `tolerance` of the line between their neighbours.
void simplifyEvents(std::vector<AutoEvent>& events, float tolerance);

// A link from a controller to a parameter.
struct ParamLink {
    enum Kind { Midi = 0, Controller = 1 };
    std::string target;
    int kind = Midi;
    int midiChannel = -1, cc = 0;       // Midi: channel 0..15 or -1 for any, and the controller number
    int track = 0, slot = 0, output = 0; // Controller: which plugin in the mixer, and which of its outputs
    float min = 0.0f, max = 1.0f;       // the part of the control's travel the link covers (min > max inverts)
    float smoothMs = 0.0f;
    std::string formula;                // optional mapping: x is the controller's value 0..1
};

// ---- compiled for the audio thread ----

struct AutoSourceRt {
    std::vector<AutoPoint> points;
    AutoLfo lfo;
    std::vector<AutoEvent> events;
    bool isEvents = false;
    float min = 0.0f, max = 1.0f;
};
struct AutoSegmentRt {
    int64 start = 0, length = 0, offset = 0;    // where it sits in the song (or pattern), and where in its source it begins
    std::shared_ptr<const AutoSourceRt> src;
};
// Everything that drives one parameter. Its value is a function of the song position alone: the stretch
// that started last holds the control, and keeps it at its end value once it is over; before the first
// one the control already sits at that stretch's opening value.
struct AutoTargetRt {
    Param* param = nullptr;
    std::shared_ptr<void> keep;                 // keeps the parameter's owner alive
    std::vector<AutoSegmentRt> segments;        // sorted by start
    float valueAt(double tick) const;           // normalised 0..1
};
struct LinkStateRt {
    float value = 0.0f;
    bool primed = false;
};
struct LinkRt {
    Param* param = nullptr;
    std::shared_ptr<void> keep;
    int kind = ParamLink::Midi;
    int midiChannel = -1, cc = 0;
    std::shared_ptr<Plugin> controller;
    int output = 0;
    float min = 0.0f, max = 1.0f, smoothMs = 0.0f;
    std::shared_ptr<const Formula> formula;
    std::shared_ptr<LinkStateRt> state;
};

} // namespace pulp
