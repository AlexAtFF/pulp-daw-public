# Writing a built-in instrument or effect for PULP

PULP is a native Windows DAW in the FL Studio mould (C++20; the engine has no dependencies at all, not
even JUCE). Built-in plugins live in the engine and sit behind one frozen interface. This page is what a
plugin author needs; the contract itself is `docs/PLUGIN_INTERFACE.md` and `engine/include/pulp/Plugin.h`.

## Read first

- `engine/include/pulp/Plugin.h`, `docs/PLUGIN_INTERFACE.md`: the interface.
- `engine/include/pulp/Core.h` (`kQuantum`, `Ramp`, `flushDenormal`, `keyToHz`, `dbToGain`), `engine/include/pulp/Dsp.h`
  (`Biquad`, `Svf`, `OnePole`, `DelayLine`, `Noise`, `fft`).
- An instrument: `engine/include/pulp/Trio.h` + `engine/src/Trio.cpp` (voices, `Adsr`, `BlepOsc`, presets, groups).
- Effects: `engine/include/pulp/Effects.h` + `engine/src/EffectsDynamics.cpp`, `EffectsSpace.cpp`, `EffectsTone.cpp`
  (`buildPresets`, `ScopeTap`, latency, sidechain, wet/dry).
- Tests: `tests/Test.h`, `tests/Util.h`, `tests/test_m3_notes.cpp` (synth tuning, aliasing, presets),
  `tests/test_m5_effects.cpp` (effects against known responses), `tests/test_m8_automation.cpp` (controllers).

## Rules

1. **No Image-Line names.** Every plugin has an original product name and nothing in code, presets or
   comments uses an FL Studio plugin or preset name. No third-party code or samples.
2. **`process` is real-time code**: always exactly `kQuantum` (64) samples; no allocation, locks, file access
   or system calls; allocate everything in `prepare(sampleRate)` (it may be called again with another rate).
   `reset()` clears state on the audio thread without allocating.
3. **Deterministic**: output depends only on the input audio, the note events and the parameter values, never
   on wall-clock time, thread, or uninitialised memory. Random sources are seeded (`dsp::Noise`). The same
   project must render bit-identically at any block size and thread count.
4. **Parameters** are created in the constructor (`addFloat`, `addChoice`, `addBool`, `add`) with stable ids,
   never added or removed later. Every one is automatable, so anything audible is ramped across the quantum
   (`Ramp`, or recompute coefficients per quantum for filters); a ramp starts at its target on the first quantum
   after `prepare()` / `reset()`, so a render never fades in from a default.
5. **No clicks, no aliasing, no runaway**: band-limit oscillators (polyBLEP as in `BlepOsc`, wavetables with
   mip levels, or additive); oversample non-linearities (see `Grit` for a 4x scheme and how it reports its
   latency); declick voice starts, stops and steals (see `kill` in `Trio`); flush denormals in feedback paths;
   clamp feedback so nothing can blow up at any parameter setting.
6. **Latency**: an effect that delays its output reports it in `latencySamples()` (constant while processing).
7. **Instruments** derive from `Instrument` (it splits the quantum at note offsets): implement `handleEvent`
   for every `NoteEvent::Type` (`On`, `Off`, `AllOff`, `Kill`, `KillNote`, `SlideTo`; honour `glideFromKey` /
   `slideSamples` for portamento, per-note `pan`, `modX` (filter cutoff), `modY` (resonance), `fineCents`,
   `release`), `ctx.pitchBendSemis`, velocity (100/127 is the reference level), a voice limit with stealing,
   and `activeVoices()`. Level: a single note at velocity 100 should peak around -12 to -6 dBFS.
8. **Presets**: instruments ship at least **20** named factory presets that sound good straight away and are
   clearly different from each other (basses, leads, pads, keys, plucks, effects ...); effects ship at least 5.
   Use `buildPresets(*this, {{"Name", {{"id", value}, ...}}, ...})`. Preset names are original and descriptive.
9. **Panel**: there is no hand-made editor. The program builds the panel from the parameter list: continuous
   parameters become knobs, choices become selectors, on/off become lamps, grouped by `paramGroups()`. So choose
   clear parameter names, units (`"Hz"`, `"ms"`, `"dB"`, `"%"`, `"st"`, `"ct"`), sensible ranges and skew
   (`skew` 3 for frequencies and times), and group them. `meter(0)` may report gain reduction in dB;
   `readScope` may expose output for a spectrum. If a plugin really needs a custom display or a control the
   generic panel cannot give (a drawable curve, a file to load), say so in your notes: do not build UI.
10. State beyond parameters (a drawn curve, a loaded file) goes through `saveExtra` / `loadExtra`; file
    references sit under a JSON key named `file`, `sample`, `ir` or `wavetable` so project saving finds them.

## Where your files go, and how to build

Work in a staging folder of your own so nobody else's build sees half-written code:

```
staging\<yourname>\pulp\<Header>.h     headers, included as "pulp/<Header>.h"
staging\<yourname>\<Source>.cpp        implementation
staging\<yourname>\test_<topic>.cpp    tests (TEST(snake_case_sentence) { CHECK(...); })
staging\<yourname>\NOTES.md            see below
```

Build and test (Git Bash; use the Write tool for source files, shell heredocs mangle C++ here):

```
cd /c/path/to/pulp-daw
cmd //c "tools\\env.bat && cmake -S tools\\staging -B build\\staging_<yourname> -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTAGING_DIR=C:/path/to/pulp-daw/staging/<yourname> && cmake --build build\\staging_<yourname> && build\\staging_<yourname>\\staging_tests.exe"
```

and once at the end in Debug with the checked heap (`-B build\\staging_<yourname>_dbg -DCMAKE_BUILD_TYPE=Debug`).
That build compiles the engine as it stands plus your folder. Never run `build.bat`, never use `build\quick`,
`build\release` or `build\debug`, never edit anything outside your staging folder, never touch git. If the
engine itself fails to compile, it is someone else's edit in progress: wait a few minutes and retry.

Your tests cannot use the plugin registry (your plugin is not registered yet): construct your classes directly
(`std::make_shared<YourSynth>()`, `project.addChannel(synth, "name")`, `project.setSlot(track, slot, effect)`).

## What the tests must show

- Instruments: every oscillator / voice mode is in tune (within 2 cents at several keys and at 44.1, 48 and
  96 kHz); aliases are well below the signal for the brightest settings; note-on lands on the exact sample;
  portamento and slide notes glide; velocity, per-note pan / mod X / mod Y do something measurable; the voice
  limit holds; every preset sounds, stays finite and below +6 dBFS, and survives `saveState` / `loadState`;
  the render is bit-identical at block sizes 64, 33 and 4096 and with 1 and 4 threads; nothing allocates on
  the audio thread (`Rt::allocationsInRt` unchanged across a render); no clicks at note ends (the last samples
  of a released note fall smoothly to silence).
- Effects: each against a known response: an EQ's measured curve, a compressor's static ratio, a delay's tap
  time to the sample, a limiter's ceiling, a filter's slope, distortion harmonics and alias rejection, a
  modulation effect's rate and depth, reported latency equals measured latency (null a delayed dry copy),
  bypass-equivalent settings are transparent, silence in gives silence out (no denormal crawl, no DC), every
  preset stable for noise, a sine and silence, state round trip, block-size independence, no allocation.

## NOTES.md

For each plugin: its `typeId`, display name, category, whether it is an instrument, a two-line description, the
exact registry line to add, e.g.
`add({"fm6", "Sextet", "Synths", true, [] { return std::make_shared<Sextet>(); }});`
then what was tested, what was not, any custom display it would benefit from, and every known shortfall.
