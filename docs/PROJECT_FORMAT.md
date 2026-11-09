# PULP project format

A `.pulp` file is one UTF-8 JSON document. Current version: **2**. Code: `engine/src/Project.cpp`,
`engine/src/ProjectMixer.cpp`, `engine/src/ProjectFiles.cpp`.

## Rules

- `format` is always `"pulp-project"`. `version` is an integer.
- A reader refuses a file whose `version` is higher than it knows.
- Older files are upgraded in memory by `Project::migrate`, one version step at a time, and are written
  back in the current version on the next save.
- Unknown keys are ignored. Missing keys take their defaults. So adding an optional key does not need a
  new version; changing the meaning or shape of an existing key does. (Audio clips, stretch settings and
  takes were added this way and files stayed at version 2.)
- A file reference beginning with `mem:` only ever appears in undo snapshots inside the running program:
  it is audio an editor holds that has not been written to a file. Saving writes it to `edits/` first.
- Time is in ticks, 960 per quarter note. A step is 240 ticks.
- File references live under the keys `sample`, `file`, `ir` or `wavetable`. A value is either
  `factory:<path inside the factory library>`, a path relative to the project file's folder, or an
  absolute path. Saving turns absolute paths inside the project folder into relative ones.

## Top level

| Key | Type | Meaning |
|-----|------|---------|
| `format`, `version` | string, int | as above |
| `name` | string | project name |
| `tempo` | number | BPM, 10 to 522 |
| `masterVolume` | number | 0..1, 0.8 is unity |
| `masterPitch` | number | cents |
| `timeSig` | [num, den] | time signature |
| `songMode` | bool | song mode or pattern mode |
| `currentPattern` | int | index of the selected pattern |
| `loop` | [start, end] | song loop region in ticks; end <= start means none |
| `channels` | array | see Channels |
| `patterns` | array | see Patterns |
| `playlist` | object | `clips`, `tracks`, `markers` |
| `mixer` | array | only tracks that differ from a fresh track |
| `recoveredFrom` | string | only in autosave backups: the project file the backup belongs to |

## Channels

`{id, name, color, mixerTrack, group?, layer?, strip, plugin}`

- `id`: stable integer; patterns refer to channels by it.
- `mixerTrack`: 0 is the master, 1..125 are inserts.
- `group`: channel group name. `layer`: ids of the channels a Layer channel plays.
- `strip`: the channel settings by parameter id (`volume`, `pan`, `mute`, `pitch`, `pitchrange`, `poly`, `mono`,
  `porta`, `portatime`, `cut`, `cutby`, `gate`, `shift`, `swingmix`, `arpdir`, `arprange`, `arptime`, `arpgate`,
  `arpchord`, `arprepeat`, `echocount`, `echotime`, `echofeed`, `echopan`, `echopitch`, `echomodx`, `echomody`,
  `veltrack`, `keypan`, `keymodx`).
- `plugin`: `{type, params: {id: value}, extra?}` as written by `Plugin::saveState`.

## Patterns

`{id, name, color, steps, swing, notes, markers?}`

- `notes` maps a channel id (as a string) to an array of notes. A note is an array:
  `[tick, length, key, velocity, pan, release, modX, modY, fineCents, colour, flags]`
  with flags 1 = slide, 2 = portamento, 4 = muted. Key 60 is shown as C5.
- `markers`: `[{tick, name}]`.

## Playlist

- `clips`: `{type, track, start, length, offset, pattern, muted, group?}`. `type` 0 is a pattern clip;
  `pattern` is a pattern id; `offset` is where in the pattern the clip starts.
- `type` 1 is an audio clip. It adds `audio` (an id from the top-level `audio` list), `mixerTrack`, `gain`,
  `fadeIn` and `fadeOut` (ticks), `reverse`, `sourceOffsetFrames` (frames of the file skipped before the
  clip's own offset: recording latency, or where a take starts in a shared file), and, only when they are
  not at their defaults: `stretch` (0 resample, 1 rhythmic, 2 tonal), `sync` (follow the project tempo),
  `timeMul` (length multiplier when not synced), `pitch` (cents), `takeGroup` and `take` (loop-recorded takes).
- `tracks`: only tracks that differ from the default: `{index, name, color, mute, solo, locked, height, grouped}`.
- `markers`: `[{tick, name}]`.

## Automation

- Top-level `automation`: the curves behind automation clips: `[{id, name, color, target, points, lfo?, min?, max?}]`.
  `target` is a parameter address (below). A point is `[tick, value 0..1, tension -1..1, shape]` with shape
  0 single curve, 1 double curve, 2 hold, 3 stairs, 4 pulse, 5 wave; shape and tension describe the stretch
  after the point. `lfo` is `{on, shape, period (steps), depth, phase}`. `min` and `max` are the part of
  the control's travel the curve covers.
- A playlist clip of `type` 2 is an automation clip; its `auto` key is a curve id.
- A pattern's `events`: `{address: [[tick, value 0..1, ramp 0|1], ...]}`: a held value from its tick, or with
  ramp a glide to the next event.
- Top-level `links`: `[{target, kind, channel, cc, track, slot, output, min, max, smooth?, formula?}]`.
  `kind` 0 is a MIDI controller (`channel` 0..15 or -1 for any, `cc`); `kind` 1 is a controller plugin in mixer
  `track`, `slot`, and which of its outputs. `smooth` is in milliseconds; `formula` maps `x` (0..1).
- Parameter addresses: `tempo`, `mastervol`, `masterpitch`; `ch:<channel id>:<id>` (channel setting);
  `chp:<channel id>:<id>` (the channel's instrument); `mix:<track>:<id>` (mixer strip);
  `fx:<track>:<slot>:<id>` (effect in a slot); `send:<from>:<to>` (send level).

All of these are optional keys, so files stay at version 2.

## Audio

Top-level `audio`: `[{id, name, file, bpm?}]`. `file` follows the file-reference rules above; `bpm` is the
audio's own tempo when it is known. Files longer than about 45 seconds are played from disk; that is decided
when the project is loaded and is not stored.

## Mixer

Each entry: `{index, name?, color?, dock?, input?, monitor?, output?, params?, slots?, routes?}`

- `input`: recording input, 0 none, 1 = input 1 (mono), 2 = input 2 (mono), 3 = inputs 1+2 (stereo).
  `monitor`: hear the input through the track. `output`: also play on a hardware output pair
  (1 = outputs 1+2, 2 = outputs 3+4 ...).

- `params`: the strip parameters that are not at their defaults (`volume`, `pan`, `stereo`, `mute`, `phase`,
  `swap`, `eq1freq` .. `eq3q`, `mix1` .. `mix10`, `bypass1` .. `bypass10`).
- `slots`: `[{slot, plugin}]` with `plugin` as for channels.
- `routes`: present only when they differ from the default (an insert sends to the master at full level):
  `[{to, level, sidechain}]`. Routes that would close a loop are dropped on load.

## Instruments made of samples

A Timbre channel's `plugin.extra` is either `{file: <path to a .pulpinst or .sfz>}` or, for an instrument
built from loose recordings, `{built: {name, sample: [paths]}}` (it is built again from those files on load).

A `.pulpinst` file is JSON beside its samples: `{format: "pulp-instrument", version: 1, name, category, sustain,
credit, params, zones}`. `params` are Timbre settings that suit the instrument. A zone is `{file, root, cents,
lo, hi, lovel, hivel, group?, of?, turn?, loop?: [start, end], gain}`: `root` is the key the note was recorded
on and `cents` how sharp the recording is; `lo`..`hi` and `lovel`..`hivel` are the keys and velocities it
plays; `group` / `of` / `turn` make takes alternate; `loop` is in frames, with the crossfade already written
into the audio.

A plugin of a type the program cannot create is kept as it was read (type, params, extra) and written back
unchanged.

## Pads, slices and other plugin state

- Sixteen (`pads`): `extra` is `{pads: [{name, layers: [{sample}]}, ... 16 of them]}`; a layer's `sample` is a
  path like any other (`factory:` for the factory kits). Per-pad settings are ordinary parameters `p<n>_level`,
  `_pan`, `_tune`, `_length`, `_group`.
- Chop (`slicer`): `extra` is `{sample, slices: [frame, ...], beats}`: where each slice starts and how many beats
  long the loop is.
- Spaces (`convolve`) keeps a user impulse response under `ir`; Ripple (`wavetable`) keeps a user table under
  `wavetable`. Both are file paths and are packed with the project like samples.
- Presets carry no `extra` for these plugins, so choosing one never changes the loaded kit, loop or file.

## Hosted plugins

A hosted VST3 plugin is stored like a built-in one, with `type` `vst3:<identifier>` (the identifier names the
plugin and its maker's ids, not its file). `params` holds up to 256 values from 0 to 1 keyed by `v<the plugin's
own parameter id>`. `extra` is `{state, name, maker, plugin}`: `state` is the plugin's whole state as the
plugin wrote it, in base64; `name` and `maker` are for the "Missing: ..." stand-in; `plugin` is the file it
was loaded from, for information. On load the state is applied first, then any parameter whose saved value
differs from what the state gave.

## Version history

| Version | Change | Upgrade |
|---------|--------|---------|
| 1 | First format (milestones 1 to 4): no mixer. | n/a |
| 2 | Added `mixer`. | A version 1 file gets an empty `mixer` (every track at its defaults). |

## Packed projects

A `.pulpz` file is a ZIP archive with stored entries: `project.pulp` plus every non-factory file it uses under
`samples/`, with the references rewritten to those paths. Opening one unpacks it into a folder of the same
name beside the archive.
