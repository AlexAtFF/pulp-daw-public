// The list of built-in plugin types.
#include "pulp/Controllers.h"
#include "pulp/Editor.h"
#include "pulp/Effects.h"
#include "pulp/Multisample.h"
#include "pulp/Plugin.h"
#include "pulp/Sampler.h"
#include "pulp/Trio.h"
#include "pulp/Dynamics.h"
#include "pulp/Pads.h"
#include "pulp/Space.h"
#include "pulp/Sextet.h"
#include "pulp/Ripple.h"
#include "pulp/Zest.h"
#include "pulp/Analysis.h"
#include "pulp/MidiOut.h"
#include "pulp/Tone.h"
#include "pulp/Clatter.h"
#include "pulp/LowFour.h"
#include "pulp/Squelch.h"
#include "pulp/Thud.h"
#include "pulp/Twine.h"
#include "pulp/Vocal.h"
#include "pulp/Creative.h"
#include "pulp/Sheaf.h"

namespace pulp {

PluginRegistry::PluginRegistry() {
    add({"sampler", "Sampler", "Samplers", true, [] { return std::make_shared<Sampler>(); }});
    add({"trio", "Trio", "Synths", true, [] { return std::make_shared<Trio>(); }});
    add({"zest", "Zest", "Synths", true, [] { return std::make_shared<Zest>(); }});
    add({"fm6", "Sextet", "Synths", true, [] { return std::make_shared<Sextet>(); }});
    add({"wavetable", "Ripple", "Synths", true, [] { return std::make_shared<Ripple>(); }});
    add({"acid", "Squelch", "Synths", true, [] { return std::make_shared<Squelch>(); }});
    add({"pluck", "Twine", "Synths", true, [] { return std::make_shared<Twine>(); }});
    add({"bass", "Low Four", "Synths", true, [] { return std::make_shared<LowFour>(); }});
    add({"kick", "Thud", "Drums", true, [] { return std::make_shared<Thud>(); }});
    add({"drumsynth", "Clatter", "Drums", true, [] { return std::make_shared<Clatter>(); }});
    add({"multi", "Timbre", "Samplers", true, [] { return std::make_shared<Timbre>(); }});
    add({"pads", "Sixteen", "Drums", true, [] { return std::make_shared<PadKit>(); }});
    add({"slicer", "Chop", "Samplers", true, [] { return std::make_shared<Chop>(); }});
    add({"grains", "Motes", "Samplers", true, [] { return std::make_shared<Motes>(); }});
    add({"additive", "Sheaf", "Synths", true, [] { return std::make_shared<Sheaf>(); }});
    add({"layer", "Layer", "Utility", true, [] { return std::make_shared<LayerInstrument>(); }});
    add({"midiout", "MIDI Out", "Utility", true, [] { return std::make_shared<MidiOut>(); }});
    // effects
    add({"eq7", "Contour EQ", "EQ and filter", false, [] { return std::make_shared<ContourEq>(); }});
    add({"filter", "Sweep", "EQ and filter", false, [] { return std::make_shared<Sweep>(); }});
    add({"comp", "Squeeze", "Dynamics", false, [] { return std::make_shared<Squeeze>(); }});
    add({"limiter", "Lid", "Dynamics", false, [] { return std::make_shared<Lid>(); }});
    add({"multiband", "Triband", "Dynamics", false, [] { return std::make_shared<Triband>(); }});
    add({"transient", "Snap", "Dynamics", false, [] { return std::make_shared<Snap>(); }});
    add({"clip", "Curb", "Dynamics", false, [] { return std::make_shared<Curb>(); }});
    add({"enhance", "Glow", "Dynamics", false, [] { return std::make_shared<Glow>(); }});
    add({"deess", "Sibilant", "Dynamics", false, [] { return std::make_shared<Sibilant>(); }});
    add({"gate", "Doorman", "Dynamics", false, [] { return std::make_shared<Doorman>(); }});
    add({"reverb", "Chamber", "Time and space", false, [] { return std::make_shared<Chamber>(); }});
    add({"delay", "Repeater", "Time and space", false, [] { return std::make_shared<Repeater>(); }});
    add({"convolve", "Spaces", "Time and space", false, [] { return std::make_shared<Spaces>(); }});
    add({"multitap", "Taps", "Time and space", false, [] { return std::make_shared<Taps>(); }});
    add({"chorus", "Shimmer", "Modulation", false, [] { return std::make_shared<Shimmer>(); }});
    add({"flanger", "Jet", "Modulation", false, [] { return std::make_shared<Jet>(); }});
    add({"phaser", "Swirl", "Modulation", false, [] { return std::make_shared<Swirl>(); }});
    add({"tremolo", "Flutter", "Modulation", false, [] { return std::make_shared<Flutter>(); }});
    add({"vibrato", "Waver", "Modulation", false, [] { return std::make_shared<Waver>(); }});
    add({"rotary", "Rotor", "Modulation", false, [] { return std::make_shared<Rotor>(); }});
    add({"drive", "Grit", "Distortion", false, [] { return std::make_shared<Grit>(); }});
    add({"stereo", "Stereo Kit", "Stereo and utility", false, [] { return std::make_shared<StereoKit>(); }});
    add({"geq", "Ten Band", "EQ and filter", false, [] { return std::make_shared<TenBand>(); }});
    add({"modfilter", "Motion Filter", "EQ and filter", false, [] { return std::make_shared<MotionFilter>(); }});
    add({"tape", "Quarter Inch", "Distortion", false, [] { return std::make_shared<QuarterInch>(); }});
    add({"tube", "Glass", "Distortion", false, [] { return std::make_shared<Glass>(); }});
    add({"amp", "Stack", "Distortion", false, [] { return std::make_shared<Stack>(); }});
    add({"fuzz", "Splat", "Distortion", false, [] { return std::make_shared<Splat>(); }});
    add({"crush", "Coarse", "Distortion", false, [] { return std::make_shared<Coarse>(); }});
    add({"shaper", "Bend", "Distortion", false, [] { return std::make_shared<Bend>(); }});
    add({"midside", "Middle and Sides", "Stereo and utility", false, [] { return std::make_shared<MiddleSides>(); }});
    add({"tune", "True Note", "Vocal", false, [] { return std::make_shared<TrueNote>(); }});
    add({"vocoder", "Talker", "Vocal", false, [] { return std::make_shared<Talker>(); }});
    add({"harmony", "Second Voice", "Vocal", false, [] { return std::make_shared<SecondVoice>(); }});
    add({"denoise", "Lint", "Vocal", false, [] { return std::make_shared<Lint>(); }});
    add({"timewarp", "Rewind", "Creative", false, [] { return std::make_shared<Rewind>(); }});
    add({"glitch", "Hiccup", "Creative", false, [] { return std::make_shared<Hiccup>(); }});
    add({"xyfx", "Thumb Pad", "Creative", false, [] { return std::make_shared<ThumbPad>(); }});
    add({"scratch", "Platter", "Creative", false, [] { return std::make_shared<Platter>(); }});
    add({"editor", "Splice", "Tools", false, [] { return std::make_shared<Splice>(); }});
    add({"gauge", "Gauge", "Analysis", false, [] { return std::make_shared<Gauge>(); }});
    add({"ctl_peak", "Follower", "Controllers", false, [] { return std::make_shared<Follower>(); }});
    add({"ctl_lfo", "Wobble", "Controllers", false, [] { return std::make_shared<Wobble>(); }});
    add({"ctl_xy", "Pad XY", "Controllers", false, [] { return std::make_shared<PadXy>(); }});
    add({"ctl_formula", "Formula", "Controllers", false, [] { return std::make_shared<FormulaCtl>(); }});
}

} // namespace pulp
