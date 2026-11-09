# PULP internal plugin interface

Frozen at milestone 5 (2026-10-16). Every built-in instrument and effect, and later every hosted
VST3 plugin, sits behind this interface. Code: `engine/include/pulp/Plugin.h`.

Changing anything here means touching every plugin, so additions must be optional virtuals with a
default, never a change to an existing signature.

## Identity

| Member | Meaning |
|--------|---------|
| `typeId()` | Stable id stored in project files, e.g. `"reverb"`. Never renamed. |
| `displayName()` | PULP's own product name shown to the user, e.g. `"Chamber"`. |
| `isInstrument()` | True for generators (Channel Rack), false for effects (mixer slots). |
| `wantsSidechain()` | True if the plugin reads `ProcessContext::sidechain`. |

Types are registered in `engine/src/Registry.cpp` with an id, name, category and factory.

## Parameters

- A plugin creates its parameters in its constructor with `add`, `addFloat`, `addChoice`, `addBool`.
  The list never changes after construction.
- `ParamInfo`: `id` (stable, used in files and automation), `name`, `min`, `max`, `def`, `skew`,
  `steps` (0 = continuous), `unit`, `choices`.
- `Param::get()` / `set()` are atomic and may be called from any thread. Values are plain (not normalised);
  `getNorm` / `setNorm` map through the skew for knobs.
- The engine and the interface both write parameters. A plugin reads them at the start of each quantum
  and must ramp anything audible (use `pulp::Ramp`). Ramps start at their target on the first quantum after
  `prepare()` or `reset()`, so a render never fades in from a default.

## Audio

- `prepare(sampleRate)`: called off the audio thread before the plugin is published, and again if the rate
  changes. Allocate everything here. Leaves the plugin in a clean state.
- `process(ProcessContext&)`: always exactly `kQuantum` (64) samples, on the audio thread or a render or
  worker thread. No allocation, locks, file access or blocking calls.
  - `io[0]`, `io[1]`: stereo float buffers. Instruments receive them cleared and add their sound.
    Effects process in place.
  - `sidechain[0..1]`: the track's sidechain input, or null when nothing is routed to it.
  - `events`, `numEvents`: note events for instruments, sorted by `offset` (sample within the quantum).
  - `tempo`, `tickPos`, `playing`, `sampleRate`, `pitchBendSemis` (channel pitch plus master pitch).
- `reset()`: audio thread; drop voices, clear delay lines.
- `latencySamples()`: samples of delay the plugin adds. Read when the project is compiled; the mixer delays
  parallel paths to match and a slot's dry signal is delayed by the same amount for wet/dry mixing and
  bypass. Capped at 16384 samples per path.
- `tailSeconds()`: how long the plugin rings after its input stops.
- `activeVoices()`: for the voice counter.

Output must be the same whatever thread calls `process`, and must depend only on the input, the events
and the parameter values: that is what makes an export identical to playback.

## Note events (instruments)

`NoteEvent::type`:

| Type | Meaning |
|------|---------|
| `On` | Start a note: `key`, `velocity` (100/127 is the default level), `pan`, `modX`, `modY`, `fineCents`, `release`, `id`, `group`. If `glideFromKey >= 0`, start at that key and glide to `key` over `slideSamples` (portamento). |
| `Off` | Release the note with this `id`. |
| `SlideTo` | Bend every sounding note of colour `group` to `key` over `slideSamples` and stay there. |
| `AllOff` | Release everything. |
| `Kill` | Fade everything out in about 3 ms (cut groups, stop). |
| `KillNote` | Fade out the note with this `id` (mono, polyphony limit). |

Native instruments derive from `Instrument`, which splits each quantum at event offsets so notes start
on exact samples; they implement `handleEvent` and `render`.

## State and presets

- `saveState()` returns `{type, params: {id: value}, extra}`; `loadState()` restores it. Unknown or missing
  parameters fall back to their defaults, so older files keep loading.
- `saveExtra()` / `loadExtra()` carry anything that is not a parameter (a sample path, a drawn curve).
- `factoryPresets()` returns named states. `buildPresets()` in `Effects.h` builds them from id/value tables.

## Controllers (added at milestone 8, optional)

- `numControlOutputs()`, `controlOutput(i)`, `controlOutputName(i)`: a plugin can put out control values in
  0..1 that parameters are linked to. `controlOutput` returns the value as of the last quantum the plugin
  processed and is safe to call from any thread. A track holding such a plugin is always processed, even
  when it carries no sound. The engine applies links at the start of the next quantum.
- While the song plays, a controller should derive anything periodic from `tickPos` rather than counting
  samples, so that an export matches playback.

## Editor panel contract

- `paramGroups()` says how the parameters are grouped on the panel. Empty means one group. The interface
  builds the panel from the parameter list: continuous parameters become knobs, stepped ones with names
  become choice boxes, two-state ones become lamps.
- `readScope(dst, n)`: the latest `n` mono output samples, for spectrum displays (lock-free).
- `meter(i)`: plugin-defined read-outs. By convention `meter(0)` is gain reduction in dB.
- A plugin never touches interface classes. Custom displays live in `app/PluginEditor.cpp` and use only
  the calls above plus the plugin's public parameters.

## Hosted plugins (milestone 10)

`app/PluginHost.cpp` wraps a VST3 plugin in this interface (`HostedPlugin`); nothing in the engine knows.

- The engine asks `PluginRegistry::create` for a type it does not know; the registry passes the id to a
  function the program registered (`setExternalFactory`). No such plugin: `createOrMissing` gives a
  `MissingPlugin`, which keeps the saved state.
- `prepare` sets the plugin up for blocks of exactly `kQuantum` samples; `process` copies the quantum into
  the plugin's buffer (and the sidechain into its second input when it has one), turns note events into MIDI
  at their sample offsets, and copies the result back with NaN and infinity replaced by silence.
- Parameters are created once, in the constructor, from the plugin's automatable parameters. Each quantum,
  values that PULP changed are handed to the plugin; values the plugin changed arrive through its listener.
- `latencySamples()` is what the plugin reports; `tailSeconds()` likewise, capped at 30 s.
- Plugins are created and destroyed on the interface thread, whichever thread asks.

