# PULP progress

This file records what each milestone delivered, what still needs testing, and where PULP differs
from FL Studio. References to "the spec" mean the project's design brief.

Last updated: 2026-10-16, milestones 11 to 14 complete; of the stretch goals the additive synth is built, the pitch editor, patcher and CLAP are not.

## Milestones

| # | Milestone | State |
|---|-----------|-------|
| 1 | Toolchain, engine and transport | **Done** (the settings window arrived with milestone 2: F10) |
| 2 | Channel Rack and Browser | **Done** (gaps listed below) |
| 3 | Piano Roll | **Done** (gaps listed below) |
| 4 | Playlist and song mode | **Done** (gaps listed below) |
| 5 | Mixer and core effects | **Done** (gaps listed below) |
| 6 | Save, load, autosave, crash recovery, undo, WAV export | **Done** (gaps listed below) |
| 7 | Audio clips and recording | **Done** (gaps and what is unverified are listed below) |
| 8 | Automation, controllers, MIDI hardware | **Done** (physical MIDI device testing is pending) |
| 9 | Multisample engine and factory library | **Done** (nobody has listened to the library yet; see below) |
| 10 | VST3 hosting | **Done** (third-party VST3 instrument testing is pending) |
| 11 | Remaining synthesizers | **Done**, with the stretch goal: ten synthesizers (the additive / resynthesis one among them), the pad sampler, the slicer, the granular sampler, MIDI Out; nobody has listened |
| 12 | Remaining effects | **Done** but for the stretch goals: 30 effects (dynamics, EQ, space, modulation, distortion, vocal, creative) and the analyser; nobody has listened |
| 13 | Imports, extra export formats, templates, demo song | **Done**: MIDI and SoundFont import, MP3 / OGG / FLAC / AIFF reading, MP3 / OGG / FLAC and stem export, MIDI export, templates, demo song; nobody has listened |
| 14 | Polish | **Done** in part: remappable shortcuts, the audit against the manual (docs\SHORTCUTS.md), performance measured against the target, first-run tour, installer package, and gaps from earlier milestones closed (listed in the section); per-window keys are fixed |

## Milestone 1: toolchain, engine and transport

Built:

- Portable CMake 4.4.4 and Ninja 1.13.2 in `tools\` (downloaded by `tools\bootstrap.ps1`, checksums pinned).
  JUCE 9.0.4 fetched by CMake from the release tag, pinned by SHA-256. Compiles with MSVC 14.44.
- Four targets: `pulp_engine` (static library), `pulp_app` (PULP.exe), `pulp_render`, `pulp_tests`.
- The engine has no JUCE in it at all: it is plain C++20, so the renderer and the tests build in seconds
  and nothing in it can reach a window or a sound card.
- Project model (channels, patterns, notes, playlist clips, tracks, markers), JSON project file with a version number.
- Clock and sequencer: 960 ticks per quarter note. Every DSP object runs in fixed 64-sample quanta whatever
  buffer size the sound card asks for, so playback and export produce the same samples.
- Sampler instrument (one sample, repitched by key, optional envelope, reverse, trim, normalize).
- Device output through JUCE: ASIO and Windows Audio (shared, exclusive, low latency).
  On first run PULP picks the ASIO driver that matches the Windows default output, else Windows Audio.
- Program window with toolbar: pattern/song switch, play, stop, position, tempo, master volume and pitch,
  CPU / voices / underrun counters, oscilloscope, peak meter, hint bar.
- Script and screenshot harness: `PULP.exe --script file --log file [--no-audio]`.

Tests (all pass, `build.bat quick`):

- Four-on-the-floor kick lands on exactly the expected sample at 140, 60, 97.3, 174.25, 522, 33 and 10 BPM,
  at 44.1, 48 and 96 kHz; a render shifted by one sample is shown to fail the same comparison.
- Renders are bit-identical at block sizes 1, 17, 64, 127, 256, 512 and 4096.
- Nothing allocates memory inside the audio callback (operator new is instrumented in the test build).
- Pattern loop wraps on the exact sample. Sampler tuning within 0.5 cent across keys and sample rates.
- Project save and reload gives the same document and the same audio. A damaged file is refused with a message.

PULP opened the ASIO driver on first run. In the Windows Audio playback test, WASAPI loopback captured
kick intervals of 428.56 to 428.58 ms at 140 BPM. The expected interval was 428.57 ms
(`tools\loopback_capture.py`, `scripts\m1_audible.txt`).

Not verified: sound through the ASIO driver was not captured (loopback only sees Windows Audio). No person has
listened to anything yet.

## Milestone 2: Channel Rack and Browser

Built:

- Floating tool windows inside the workspace: move, resize, close, double-click the title to fill the workspace,
  detach onto the desktop (second screen) and dock back. Toolbar buttons and F-keys toggle them.
- Channel Rack: mute lamp (right-click solo), pan and volume knobs, channel button (click opens the channel
  settings, right-click opens the menu), selector lamp, step buttons (left sets, right clears, drags paint),
  per-pattern step count and swing, graph editor for velocity, pan, pitch, release, mod X, mod Y and shift,
  channel groups with a filter, rename, colour, clone, delete, replace instrument, fill each 2/4/8 steps,
  rotate, clear, move up and down, add channel. Playing step is highlighted.
- Channel settings window (same for every instrument): the instrument's own panel (the sampler shows its
  waveform with draggable trim markers), then levels, polyphony, mono and portamento, cut and cut-by groups,
  gate, shift and swing mix, arpeggiator (direction, chord, range, time, gate, repeat), echo delay (count, time,
  feedback, pan, pitch, mod X, mod Y), tracking, and a preview keyboard. Layer channels pass notes to children.
- Browser: factory library, current project's sounds, folders you add, favourites, plugin database, search,
  click to preview with a waveform, double-click or drag to the rack, drop on a sampler to replace its sample.
- Pattern selector in the toolbar: list, next empty (F4), numpad + and -, clone, rename, delete.
- Audio settings window (F10): driver, output and input device, sample rate, buffer size, active channels,
  latency read-out, driver control panel.
- Factory drums: 808-style and 909-style kits (26 sounds) synthesized by PULP and written as 24-bit WAV with
  `factory\manifest.json` (source, licence CC0, SHA-256). The new-project template is kick, clap, hat, snare.
- Hint bar text for the controls built so far.

Tests added (22 tests, 412 checks, all pass): factory drums are clean and reproducible; step model; cut groups
silence the other channel without a click; polyphony limit and mono; arpeggiator order, timing and gate;
echo timing and decay; swing, shift, gate; layer channels; portamento glide; mod X/Y drive the sampler filter;
block-size invariance and no audio-thread allocation with all of it switched on; preview player; master volume
applies from the first sample (this caught and fixed a real bug: the first 64 samples of an export ramped in
from unity when the master volume was not at its default).

Verified by script (`scripts\m2_rack.txt`, screenshots in `out\`): clicking and dragging steps, right-click
clearing, F4 and numpad pattern keys, dropping a file on the rack, the graph editor, clone / move / delete
keys, detach and dock, window toggles. Verified on the real device (`scripts\m2_audible.txt`): a beat clicked
into the rack played for 2.2 s at about -45 dBFS, and the loopback recording of the interface's output lines up
with an offline render of the same project with correlation 0.986.

Partial or missing in this milestone:

- Browser preview in sync with the tempo arrived with milestone 7 (the SYNC switch).
- The mixer track number in each rack row arrives with the mixer (milestone 5).
- WAV, FLAC and AIFF files are listed and loaded (since milestone 9); MP3 and OGG since milestone 13.
- Factory instruments are stored as FLAC (since milestone 9); the drum hits stay as small WAV files.
- Dragging from Windows Explorer and real mouse drags from the Browser use JUCE's drop targets and were
  exercised only through the script's drop command, not with a real mouse.
- A full kit at default levels can exceed 0 dBFS on the master (the meter turns red). The default project gets
  a limiter on the master in milestone 5, as the reference program's template has.

## Milestone 3: Piano Roll

Built:

- Piano Roll window (F7): keyboard, note grid, ruler, per-note property lane, both scrollbars, playhead that
  follows playback. Opens from the rack's "Send to piano roll" or by clicking a channel's mini preview.
- Tools with the reference program's keys: draw (P), paint (B), delete (D), mute (T), slice (C), select (E),
  zoom (Z), playback (Y). Left-click places a note with the last note's length and properties, drag moves,
  right edge resizes, right-click deletes, Shift-drag clones, Ctrl-drag selects, Alt bypasses snap,
  Ctrl+wheel and Ctrl+right-drag zoom, Alt+wheel zooms rows, middle-drag pans.
- Snap: Main (follows the toolbar), Line (follows zoom), none, 1/6 to 3/2 step, 1/6 to 3/2 beat, bar.
- Property lane: velocity, pan, release, mod X, mod Y, fine pitch. Note colour groups 1 to 16.
- Slide notes (bend the sounding notes of their colour group and hold) and portamento notes.
- Ghost notes from the other channels, double-click to jump. Scale highlighting (root + 14 scales).
  Stamp tool (21 chords). Waveform backdrop for sampler channels. Time markers. Loop a range (Ctrl-drag the ruler).
- Tools menu: quick quantize, quantize (grid, strength, starts, ends), quick legato, articulate, chop, glue,
  arpeggiate, strum, flam, randomize (in scale), scale levels, LFO for any property lane, flip, reverse,
  trimmer, riff generator (progressions, arpeggios, bass grooves), each with live preview and Accept / Cancel.
- Editing keys: Ctrl+A, Ctrl+D, Ctrl+C / X / V, Ctrl+B, Delete, Shift+Up/Down, Ctrl+Up/Down, Shift+Left/Right,
  Ctrl+Q, Alt+Q, Ctrl+L, Alt+U, Ctrl+G, Alt+A, Alt+S, Alt+F, Alt+L, Alt+R, Alt+X, Alt+Y, Alt+O, Alt+W, Alt+E,
  Ctrl+M (import MIDI), Shift+Ctrl+M (export MIDI), Ctrl+0 (zoom to fit), PageUp / PageDown.
- MIDI file import and export for the channel being edited (formats 0 and 1, any resolution).
- TRIO, the default three-oscillator synth: band-limited sine, triangle, square, saw, soft saw and noise;
  per-oscillator coarse, fine, level, pan, phase and stereo detune; amp envelope; low-pass filter with its own
  envelope and key tracking; ring-mod option; 22 presets with a preset selector in the channel settings.
- Typing keyboard (Ctrl+T, the two-row layout), record button (R) with input quantize to the snap setting,
  step recording, metronome (Ctrl+M) that never reaches an export, main snap selector in the toolbar.

Tests added (31 tests, 718 checks, all pass): every Trio waveform within 1 cent at five keys; coarse and fine
tuning; the saw's aliases are more than 55 dB below the fundamental; all 22 presets sound, stay finite and
survive save / load; slide notes bend only their colour group; every note tool; riff generator stays in key and
is reproducible; MIDI round trip plus a hand-written 96 PPQ running-status file; metronome clicks on exact
beats and is absent from renders; loop range repeats exactly.

Verified by script (`scripts\m3_piano.txt`, `scripts\m3_piano2.txt`, screenshots in `out\`): drawing, moving,
resizing, right-click delete, select all, transpose, duplicate, tool panels (accept and cancel), marquee,
riff generator, property lane, tool keys, MIDI export then import, step recording, live recording, clone,
slice, paint, mute, Alt bypass, stamp, scale highlight, ghost notes and their double-click, waveform backdrop.
Verified on the real device (`scripts\m3_audible.txt`): a TRIO riff played 1.8 s at about -55 dBFS and the
loopback recording matches an offline render with correlation 0.993.

Partial or missing in this milestone:

- Recording from a MIDI keyboard and the count-in arrived with milestones 8 and 7.
- Live recording worked in pattern mode only when this milestone was finished; song mode was added later
  (see milestone 14, gaps closed).
- The typing keyboard was exercised through the same note path by script, but not with real key presses.
- The per-note properties window on double-click arrived with milestone 14.

## Milestone 4: Playlist and song mode

Built:

- Playlist window (F5): 500 tracks, any clip on any track, picker panel (click selects a pattern, drag places
  it), track headers, ruler, both scrollbars, playhead that follows the song.
- Pattern clips with a mini preview of their notes. Tools: draw (P), paint (B), delete (D), mute (T), slip (S),
  slice (C), select (E), zoom (Z), playback (Y). Move, resize from both edges (the left edge keeps the content
  in place), Shift-drag clone, Ctrl-drag select, right-click delete, Alt bypasses snap, double-click opens the
  pattern, Ctrl+B duplicate, Ctrl+C / X / V, Shift+arrows nudge, Delete.
- Clip menu (the corner arrow): make unique, mute, rename pattern, pattern colour, group (Shift+G), ungroup (Alt+G).
- Tracks: mute lamp, solo (right-click the lamp), lock, colour, name, four sizes, group with the track above.
- Time markers (ruler menu, Alt+T), loop region (Ctrl-drag the ruler, double-click clears), song end marker.
- Song mode plays the playlist and loops the whole song or the loop region; pattern mode loops the pattern.

Tests added (37 tests, 747 checks, all pass): clips play on exact samples across tracks and overlaps; clip offset
and length trim the pattern and cut held notes at the clip end; track mute, solo, track groups and muted clips;
whole-song loop and loop region; song render identical at any block size; make unique; switching modes.
These tests found and fixed two real bugs: the length of an export depended on the block size, and a seek
issued before the first block of song-mode playback was thrown away.

Verified by script (`scripts\m4_playlist.txt`, screenshots in `out\`): placing, moving, resizing, cloning,
slicing, muting, slipping, loop region, song playback position, track mute lamp, select all, duplicate, group,
marker key.

Partial or missing in this milestone:

- Audio clips and automation clips arrive in milestones 7 and 8; the picker lists patterns only until then.
- Grouped tracks follow the top track's mute and solo. They did not collapse when this milestone was
  finished; that was added later (see milestone 14, gaps closed).
- Not played through the real device in this milestone (song mode runs the same engine path that milestones 1
  to 3 verified on the device).

## Milestone 5: Mixer and core effects

Built:

- Mixer engine: master, 125 inserts and the "current" track. Each track: ten effect slots with per-slot
  wet/dry mix and bypass, three-band EQ after the slots, swap left/right, stereo separation, pan, fader
  (80% is unity, 100% is +5.6 dB), mute, phase invert, peak meters.
- Routing: any track to any other at a send level, sidechain-only routes, feedback loops refused (also when
  loading a damaged file). Only tracks that carry sound are processed.
- Plugin delay compensation across the whole graph: every path is delayed to match the slowest, a slot's dry
  path is delayed for wet/dry and bypass, and renders drop the leading latency so files start on time.
- Multi-core: instruments and independent mixer tracks run on a pool of worker threads (cores minus one).
  The result is bit-identical for any thread count.
- Effects, each with presets and all parameters exposed: Contour EQ (seven bands, six band types, curve over a
  live spectrum with draggable handles), Squeeze (compressor with sidechain input and gain-reduction display),
  Lid (5 ms look-ahead brickwall limiter with a gate and gain-reduction display), Chamber (reverb: pre-delay,
  size, damping, early/late balance, width), Repeater (stereo delay: tempo sync, ping-pong, filtered and
  saturated feedback, modulation), Shimmer (chorus), Grit (distortion, 4x oversampled, four characters),
  Sweep (multimode filter, 12 or 24 dB), Stereo Kit (width, balance, gain, phase, swap, mono bass).
- Mixer window (F9): strips with name, pan, stereo separation, meter and fader, mute lamp (right-click solo),
  send arrows with send-level knobs, four layouts, track colours and names, docks left and right, track menu;
  side panel with the selected track's slots, EQ and switches and a delay-compensation read-out.
- Effect editor windows with preset selector. Channel Rack rows show their mixer track; Ctrl+L routes the
  selected channels to free mixer tracks. New projects start with Lid on the master.
- The plugin interface is frozen and written down in `docs\PLUGIN_INTERFACE.md`.

Tests added (54 tests, 1372 checks, all pass in release and in a Debug build with the checked heap):
default routing is bit-transparent; fader law, pan, mute, phase, swap, separation, strip EQ; sends, buses,
loop refusal, save / load; delay compensation nulls a latent parallel path at 1, 64, 240 and 1000 samples
(and the test is shown to fail without compensation); bypass and wet/dry stay aligned; sidechain ducking;
identical output for 1, 3, 4 and 8 threads and several block sizes with no audio-thread allocation; the
current track; EQ measured against its own curve at nine frequencies; compressor static ratio and knee;
limiter never exceeds its ceiling and passes quiet signal untouched; reverb decay time within 30% of its
setting; delay tap time exact, synced and ping-pong; chorus, stereo tools; distortion latency, harmonics and
alias rejection; filter slopes; every preset of every effect stable and reloadable.

Verified by script (`scripts\m5_mixer.txt`): Ctrl+L, adding effects, clicking a send arrow, loop refused,
sidechain route, editors open. Verified on the real device (`scripts\m5_audible.txt`): a beat through mixer
tracks, compressor, EQ, a delay bus and the master limiter for 2 s at about -48 dBFS; the loopback recording
matches a single-threaded offline render with correlation 0.9999, with no underruns.

Partial or missing in this milestone:

- Dragging strips to reorder them is not built (tracks are addressed by number). Docks are. Moving a track
  one place by key or menu was added later (see milestone 14, gaps closed).
- The mixer's input selector (for recording) and output selector (extra hardware outputs) arrive with
  recording in milestone 7; they are not shown until then.
- Worker threads run at normal priority; the audio thread is the device's own.
- Lid was a limiter with a gate and no compressor mode when this milestone was finished; the compressor
  stage was added later (see milestone 14, gaps closed).
- `build.bat test` runs a Debug build with the checked heap and checked iterators.
  `build.bat asan` requires the AddressSanitizer component.
- Hard clipping through Grit: 4x oversampling lowers aliasing by more than 8 dB against none, but a square
  wave's slow harmonics still fold; soft clipping measures below -60 dB.

## Milestone 6: save, load, autosave, crash recovery, undo history, WAV export

Built:

- `.pulp` project files (JSON, versioned; schema in `docs\PROJECT_FORMAT.md`). Samples inside the project's
  folder are stored by relative path, so the folder can be moved. Saving writes a temporary file and swaps it in.
- Format migration: version 1 files (written before the mixer existed) upgrade to version 2 on load. Files from
  a newer PULP, damaged files and non-project files are refused with a message.
- Pack: project plus its audio in one `.pulpz` archive (a plain ZIP); opening one unpacks it beside the file.
- File menu: new, open, recent files, save (Ctrl+S), save as (Shift+Ctrl+S), save new version (Ctrl+N, adds or
  bumps `_2`, `_3`...), pack, recover a backup, export WAV (Ctrl+R), register the `.pulp` file type for this
  Windows user (only when the user picks that menu item), exit. Unsaved changes are asked about before closing,
  opening or starting a new project. `PULP.exe file.pulp` opens that file. The title bar shows the name and `*`.
- Undo history: every finished edit is a step. Ctrl+Z flips between the last step and the one before
  (FL's behaviour), Ctrl+Alt+Z steps back, Shift+Ctrl+Z steps forward. The history is listed in the Browser
  under Current project > History and in the Edit menu; click a step to go to it.
- Autosave: every two minutes while there are unsaved changes, into `%APPDATA%\PULP\autosave` (ten newest kept
  per project). A lock file detects a run that did not close cleanly; the next start offers the newest backup.
  A crash handler writes `%APPDATA%\PULP\crash.log` with a stack trace.
- WAV export: song or pattern, 44.1 / 48 / 88.2 / 96 kHz, 16-bit, 24-bit or 32-bit float, tail leave / cut /
  wrap, optional dither. It renders a copy of the project on background threads (all cores) with a progress
  bar and a cancel button; the interface stays live.

Tests added (60 tests, 1422 checks, all pass): ZIP store round trip, checksum failure and path-escape refusal;
a full project (samples, synth, patterns, clips, mixer routing, sidechain, effects) saves, reloads and renders
identically, re-saves to the same bytes, and still loads after its folder is moved; pack then unpack renders
identically with the originals deleted; a version 1 file upgrades; newer / foreign / too-old files are refused;
tail modes; single-thread 64-sample-block playback is bit-identical to a six-thread 4096-block export; 16-bit,
24-bit and float files hold the render to within their resolution.

Verified by script (`scripts\m6_files.txt`, `scripts\m6_recover.txt`, sandboxed with `--data-dir`): save, undo
toggle, step back and forward, a knob move as a step, save new version, two exports, pack, new, open, autosave,
a deliberate crash (crash log written), and on the next run the lock was seen and the backup restored the
unsaved edit. The program's WAV export is byte-identical to `pulp_render` on the same project.

Partial or missing in this milestone:

- Undo restores a whole-project snapshot: voices are cut when a step is undone. (Effect editors used to close too;
  since milestone 12 they are reopened in place.)
- FLAC export, stems ("split mixer tracks") and MIDI export of the whole project arrived with milestone 13 (MP3 and OGG too).
- The crash-recovery dialog, the unsaved-changes dialog and the file choosers were not clicked through by a
  person; the script exercised the same session calls underneath them.
- The file-type registration menu item was not run (it changes a Windows user setting, so it is left for the user).

## Milestone 7: audio clips and recording

Built:

- Audio clips on the playlist: any WAV placed on any track, playing on an exact sample through a chosen mixer
  track. Trim from both edges, slip, slice, move, clone, mute, group, delete (the same tools as pattern clips),
  gain, normalize, fade in, fade out, reverse, mixer track. Files at another sample rate are resampled.
  A clip cut in the middle of its file gets a 2 ms fade so it does not click. The picker lists audio; the draw
  tool places the chosen one; `.wav` files can be dragged onto a track from the Browser or from Explorer.
- Disk streaming: a file longer than about 45 seconds is not loaded. A loader thread keeps the pages around
  each playing position in memory (and the pages every clip starts on, so pressing play never waits); the
  audio thread only copies from pages that are there, and a page that is not plays as silence and is counted.
  Exports read from disk on the spot, so nothing is ever missing from a rendered file. The playlist draws long
  files from an overview built once when the file is added.
- Time-stretching and pitch (clip menu > Stretch and pitch): three algorithms. Resample (speed and pitch move
  together, band-limited), rhythmic (overlap-add that carries every drum hit across untouched and stretches
  only the time between hits), tonal (phase vocoder with phase locking). Length from 6% to 1600%, pitch
  -24 to +24 semitones without changing the length, and "Follow the project tempo".
- Tempo detection: when a file is dropped on the playlist PULP listens for its tempo. A file cut to whole bars
  gets its exact tempo from its length. If it differs from the project's, PULP asks whether to stretch it to
  the tempo. The tempo can also be typed in or detected again from the clip menu.
- Recording: each mixer insert has an input selector (none, input 1 mono, input 2 mono, inputs 1+2 stereo;
  nothing is recorded until one is chosen) and a monitor switch with a feedback warning. With Record lit,
  play records a take from every track that has an input. The dry signal goes straight from the driver to
  32-bit float WAV files in a `recordings` folder beside the project, written by a background thread as it
  happens and flushed twice a second (a file is readable even if the program dies mid-take).
- The take becomes an audio clip where it was performed: it is shifted by the measured round trip if the
  device has been calibrated, otherwise by the engine's 64-sample input delay plus the input and output
  latency the driver reports. A take recorded with no loop region runs on past the end of the song.
- Loop recording with takes: with a loop region set (Ctrl-drag the playlist ruler) the region loops while
  recording and every pass is kept as a take, stacked on the next free tracks ("Take 1", "Take 2" ...). The
  last complete take plays; the others are muted.
- Comping: a take's clip menu has "Use this take here". It un-mutes that clip and cuts and mutes the group's
  other takes over the same stretch, so slicing a take and choosing pieces builds a comp; joins get the
  2 ms fade.
- Punch in / out (record button, right-click): only the part of the performance inside the loop region is
  kept, exactly from the region's start to its end. With loop recording on as well, every pass is a take.
- Wait for input (record button, right-click): the transport stays still, the play button amber, until the
  input goes above -34 dBFS or a note is played; the take begins with that sound.
- Latency calibration (Audio settings > Measure latency): three quiet noise bursts (peak -16 dBFS, 1.2 s in
  all) go out, PULP finds them on either input and stores the real round trip for that driver, device, sample
  rate and buffer size. "Forget" goes back to the driver's figures. The window shows which is in use.
- Count-in of one or two bars. Four metronome sounds (beep, tick, wood block, bell; right-click the
  metronome button). Input level meter, with the level in dB in the hint bar.
- Note recording: overdub (notes are added) or replace (the notes the playhead passes are wiped).
- Per-track hardware output: a mixer insert can also play on outputs 1+2, 3+4, 5+6 or 7+8 of the sound card.
- Splice, the audio editor and recorder (effect slot, Tools): passes the track's sound through untouched and
  records it (now, while the song plays, or when sound arrives). The recording shows as a waveform with a
  drag selection. Trim, cut, silence, fade in, fade out, normalize, reverse, noise removal (learn the noise
  from a selection, then remove it), its own undo, load and save WAV, play the selection, send to the playlist
  or drag out onto the playlist. Audio that is not in a file yet is written into the project's `edits` folder
  when the project is saved or autosaved, and survives the program's undo.
- Browser: SYNC switch. With it on, a sound with a steady beat is previewed stretched to the project tempo.
- The audio device is opened with two inputs so a two-microphone interface's inputs are both available.

Tests added (89 tests, 1977 checks, all pass in release and in Debug with the checked heap):

- Audio clips: bit-exact at their tick; gain, mixer track, head offset, the declick fade; trim, fades, slip,
  reverse, resampling; with a latent effect they stay identical across block sizes and thread counts, loop
  correctly, save and reload.
- Streaming: a project playing a file from disk renders bit-identically to the same audio held in memory, at
  four block sizes and thread counts; 16-bit and 24-bit files decode to the same values either way; in live
  mode with the loader keeping up there are no misses, **no allocation on the audio thread**, and less than
  half the file is ever in memory; a jump to an unloaded place plays silence instead of blocking, is counted,
  and recovers; a prefetched jump has no gap.
- Stretching: resampling moves pitch by the right ratio and keeps a 15 kHz tone read twice as fast below
  -54 dB instead of folding it; the tonal stretch keeps 440 Hz within 0.5 Hz and level within 0.5 dB at four
  ratios and keeps a three-note chord's partials at their levels; the rhythmic stretch puts all eight hits of
  a loop within 12 ms of where they belong at three ratios, still sharp, and leaves a tone's pitch alone;
  pitch shift by +7, -5 and +12 semitones is within 0.4% in both stretch modes with the length unchanged.
- Tempo detection: loops at 90, 120, 128, 140 and 174 BPM come out exact; un-trimmed recordings at 97 and 133
  within 0.5 BPM; noise and silence have no tempo.
- A one-bar 100 BPM loop synced into a 125 BPM project plays its four hits on the project's beats, and again
  at 80 BPM, in all three modes; stretched clips render identically at any block size and survive save / load.
- The streaming WAV writer is readable before and after closing; monitoring follows the track's input mode
  with exactly 64 samples of delay; count-in clicks four times then starts on the exact sample.
- **A take recorded through 0, 137 and 1024 samples of simulated round-trip latency plays back
  sample-for-sample where it was performed, and is shown to be wrong without the compensation.**
- Loop recording: three and a half passes give four takes, and each take played alone is sample-for-sample
  what was performed during its pass. Comping: the right take sounds in each stretch, nothing doubles, the
  join is not a gap, other clips are untouched, a split leaves both parts playing what they did.
- Punch: only the range is written to disk and it plays back sample-exact from punch-in to punch-out; punch
  inside a loop gives one take per pass.
- Wait for input: nothing moves below the threshold; the take starts with the first loud sample; stop
  cancels; a played note starts it.
- Calibration: through simulated cables of 64, 500, 2048 and 7001 samples, at full level and at -26 dB with
  hiss, the measurement is exact to the sample; with nothing connected, or only noise, it reports failure.
- Metronome sounds click on every beat, accent the first, fall silent between beats and differ from each
  other. A mixer track sent to outputs 3+4 appears there bit-identically.
- Editor: every operation changes only its selection; noise removal lowers hiss by more than 20 dB, leaves a
  tone within 0.5 dB and halves the noise under it; Splice in a slot is bit-transparent, records exactly what
  passed, never allocates on the audio thread, undoes, survives a project snapshot unsaved, and is written
  into the project folder on save; the three record modes start when they should.

Verified by script (all silent):

- `scripts\m7_record.txt`: arm a track, count-in, record, stop; the clip and its file appear; a placed audio
  file; undo and redo of both.
- `scripts\m7_takes.txt` (test tone on input 1): a looped recording made three stacked takes with the second
  one playing; auditioning and comping changed the right clips; punch on bar two gave a clip from tick 3840
  to 7680; wait-for-input held the transport until the tone started; calibration through a simulated
  1000-frame cable measured 1064 (1000 + the engine's 64); a one-bar loop exported at 120 BPM was detected as
  120.00 and fitted to 150 (length ratio 0.8); the editor on the master recorded the song, trimmed,
  normalized, faded, reversed, undid, learned and removed noise, sent its audio to the playlist, and after
  save, new, open everything was back (`out\m7_takes.png`, `m7_editor.png`, `m7_calibration.png`).
- `scripts\m7_stream.txt`: a 90-second file played from disk by the real loader thread at real-time pace,
  including a 40-second jump while playing and a jump while stopped, with no missing page.

Recording tests with audio hardware:

- `scripts\m7_device.txt` recorded a Windows Audio take and placed it using the driver's reported latency.
- `scripts\m7_input.txt` checked input metering through Windows Audio and ASIO.

Not verified:

- Recording an audible voice or instrument.
- Measuring round-trip latency with a physical loopback cable.
- How the stretch algorithms sound to a person. Their numbers are tested; their artefacts are not judged.
- Dragging from the editor onto the playlist and the "Stretch to the tempo?" question were not exercised with
  a real mouse (the script calls the same functions).
- Routing to hardware outputs 3 and above.
- Recording through ASIO: the input was read, a take was not recorded through it.

Partial or missing in this milestone:

- A stretched or pitch-shifted clip is rendered into memory; only unprocessed long audio streams from disk.
- Changing the tempo re-renders every clip that follows it: instant for loops, a pause for long files.
- Recording knob movements while recording audio belongs to automation (milestone 8).
- Splice records at most 30 minutes, keeps every unsaved version in memory until the program closes, and has
  no zoom: the whole recording always fits the window.
- FLAC and AIFF are read since milestone 9 (MP3 and OGG since milestone 13).

## Milestone 8: automation, controllers and MIDI hardware

Built:

- The control menu. Right-click any knob, fader, selector or switch, in the toolbar, the Channel Rack, the
  channel settings, the mixer and inside every built-in instrument and effect: reset, type in value, copy
  value, paste value, **create automation clip**, **link to controller**, **edit events**, remove the link.
  (Mute lamps keep right-click for solo; the tempo box keeps tap tempo and adds the automation items.)
- Automation clips on the playlist: a curve for one control, on its own (taller) track. Drag a point to move
  it, right-click the curve to add a point, right-click a point to delete it, type its value or choose the
  shape of the stretch after it: single curve, double curve, hold, stairs, pulse, wave. The small ring in the
  middle of a stretch is its tension (for stairs, pulse and wave: how many). The clip menu has rename, make
  unique, range (which part of the control's travel the curve covers) and **LFO mode** (sine, triangle,
  square, saw; cycle length; depth; it swings around the curve). Automation clips move, resize, slice, mute
  and group like any clip; the picker lists the curves so the draw tool can place another clip of one.
- Event automation inside patterns, with an **event editor** window: drag to draw held steps on the snap
  grid, Shift-drag for a straight glide, right-drag to erase. Events loop with their pattern in pattern
  mode and play wherever the pattern's clips are in song mode.
- A control's value is a function of the song position alone: the clip (or pattern clip) that started last
  holds the control and keeps it at its end value once it is over; before the first one the control already
  sits at that clip's opening value. So an export, a play from the start and a play from the middle agree.
- Tempo, master volume, master pitch, every channel setting, every instrument parameter, every mixer strip
  control, every effect parameter and every send level can be automated. Exports whose tempo is automated
  come out at the right length.
- Link to controller: a window per control with the source (a hardware MIDI control, found by **learn**:
  move it and it is linked; or any controller plugin in the mixer), the range (FROM above TO inverts it),
  smoothing and a mapping formula such as `1 - x`.
- Four controller plugins (mixer slot > Controllers); each lets the sound through:
  **Follower** (envelope follower: listens to its track or its sidechain input, with base, amount, attack,
  release, and a switch that silences what passes through), **Wobble** (LFO: six shapes, tempo-synced or
  free, base, depth, phase; its phase follows the song position), **Pad XY** (drag a dot: two outputs,
  with smoothing), **Formula** (a typed expression of three knobs, time, beat and tempo).
  Sidechain ducking: Follower on the kick's track with the "Ducking" preset, then link the bass's volume to it.
- Recording knob movements: with Record lit and the song playing, moving any control (or a linked hardware
  control) is recorded. In pattern mode it becomes events in the pattern; in song mode it becomes an
  automation clip where it happened, so it works while a take is being recorded. A control you are holding
  is left alone by automation and links until you let go.
- MIDI hardware: every MIDI input is opened and devices are found as they are plugged in or removed.
  Settings (F10) has a MIDI page where each device can play notes, act as a controller, or both. Notes play
  the selected channel at once and are recorded like the typing keyboard; the pitch wheel bends the channel.

Tests added (102 tests, 101579 checks, all pass in release and in Debug with the checked heap):

- Formulas: precedence, powers, comparisons, every function, division by zero, and ten malformed inputs refused.
- Curve shapes: each of the six starts on its first point and stays between its two points at five tensions;
  specific values for each; LFO mode; events hold and glide; thinning recorded movement keeps the shape
  within its tolerance.
- Parameter addresses lead back to the same parameter for every kind of control, and bad ones are refused.
- **An automation clip moves a mixer fader exactly**: at the end of every 64-sample quantum the gain is the
  curve's value to within 0.001%, in between it ramps with no step, a hold's jump is complete within one
  quantum of its tick, a clip's range maps the curve, and nothing allocates on the audio thread.
- **Automation depends only on the song position**: a render is bit-identical wherever the knobs were left;
  rendering from four positions in the middle matches the full render there; block size and thread count
  make no difference.
- Overlapping, finished, muted, trimmed and orphaned clips follow the rule above; pattern events drive
  controls in pattern mode (and loop), in song mode through pattern clips, and yield to a later clip.
- **Sidechain ducking**: a muted trigger with a Follower ducks a bass to under 30% on every beat and lets it
  back; without the link the bass is steady; the same through a sidechain route ducks only where the kick is.
- Hardware links: nothing moves until the control does; any-channel and single-channel links; formula;
  inverted range; smoothing reaches 63% after one time constant; a held control is left alone; renders
  ignore hardware.
- Tempo automation: two bars at 120 and 240 BPM render to exactly 3.0 s with every hit on its sample at any
  block size; a tempo gliding from 100 to 200 over a bar takes the time the integral says.
- Controllers: each is bit-transparent to the sound; Wobble matches a sine of the song position; Pad XY
  with and without smoothing; Formula evaluates, refuses bad text and keeps the old; Follower rises and falls.
- A project with a curve (all point fields, LFO, range), pattern events, a controller link with formula and
  smoothing and a MIDI link saves, reloads identically, re-saves to the same bytes and renders identically
  at three block sizes and thread counts with no allocation on the audio thread.

Verified by script (`scripts\m8_automation.txt`, silent; pictures in `out\m8_*.png`): a clip made from a
fader's menu drove the fader while the song played; dragging a point, right-clicking to add one and dragging
the tension ring changed the curve; steps and a glide drawn in the event editor moved the pan in pattern
mode; a Follower on the kick ducked the synth's volume to 0 just after the hit and back to 51% before the
next; the pad and formula windows; MIDI learn linked the next controller that moved and the fader followed
it; a MIDI note sounded and, with Record on, was written into the pattern; a recorded knob movement became
events in pattern mode and an automation clip in song mode; a tempo clip from the tempo box; save, new,
open brought everything back.

Not verified:

- The script injected MIDI messages through the device-message handler. Physical MIDI input,
  hot-plugging and the device switches still need testing.
- The right-click menus, the link window's controls and the pad were driven through their handlers, not
  with a real mouse.
- How automation, ducking and the LFO sound to a person.

Partial or missing in this milestone:

- MIDI output arrived with milestone 11 (the MIDI Out channel: notes, program, pitch wheel); there is no MIDI clock.
- Notes from a MIDI keyboard were recorded in pattern mode only (as with the typing keyboard) when this
  milestone was finished; song mode was added later (see milestone 14, gaps closed).
- The piano roll's LFO tool wrote note properties only when this milestone was finished; it writes events for
  the channel's volume, pan and pitch since milestone 14 (gaps closed).
- An automated control shows where automation left it, and that position is what gets saved. Playing again
  always puts it back where the song says, so nothing audible depends on it.
- Clips that follow the tempo are not re-fitted while a tempo automation moves (see D-STRETCH).
- Links to controllers lag one 64-sample quantum behind the controller.

## Milestone 9: the multisample engine and the factory library

Built:

- The multisample builder (`engine/src/MultisampleBuild.cpp`). Given recordings of single notes it:
  finds each one's pitch (and how many cents out of tune the recording is, which playback then corrects);
  trims the silence before and after; decides whether the sound holds or dies away; for sounds that hold,
  finds a loop in the steady part whose two ends match (a whole number of periods that is also close to a
  whole number of samples) and writes a crossfade in, so a held key sounds for as long as it is held; lays
  the notes out across the keyboard, each taking the keys nearest to it; splits velocity layers evenly;
  makes takes of the same note take turns (round robin); and evens out the level between notes.
  One note is enough. A stated key (from a manifest) is trusted; a note name in a file name is checked
  against what is heard; with neither, the ear decides.
- Timbre, the instrument that plays the result (Channel Rack `+` > Timbre): repitching with four-point
  interpolation, sustain loop on / off, amplitude envelope, low-pass filter with its own envelope and key
  tracking, LFO to pitch (vibrato), level (tremolo) and filter with a delay, pitch envelope, velocity
  sensitivity, transpose, fine tune, sample start, 40 voices, slide and portamento notes, 20 presets that
  reshape any instrument (pad from anything, plucked, staccato, dark, swell ...).
- Making your own: the channel settings of a Timbre show the keyboard with each recording's stretch of keys.
  **BUILD FROM SAMPLES** (or dropping one or more WAV / FLAC / AIFF files on the window) builds an instrument
  from them, for example from your own recorded voice. LOAD INSTRUMENT opens an instrument file.
- Instrument files: `.pulpinst` (a JSON description beside its samples, stored as FLAC) and **SFZ** files
  (regions, key and velocity ranges, root key, tune, volume, loop points, round robin, `#define`,
  `default_path`). The Browser shows an instrument's folder as one instrument: click to hear a note,
  double-click or drag to the rack to play it.
- Audio files: the engine now reads WAV, **FLAC** (its own decoder and encoder, written for PULP and checked
  against ffmpeg and libsndfile) and **AIFF / AIFC**, anywhere a sample can be used.
- The factory library, built by `build.bat library`: `tools\fetch-samples.py` downloads the recordings
  listed in `factory\sources.json` (282 files, 416 MB, every one with its URL, source, licence and SHA-256,
  URLs pinned to commits) into `factory-cache\`, and `pulp_render --build-library` turns them into
  instruments with the builder above. What comes out (in `factory\`, 212 MB, copied to `dist\PULP\factory`):
  - 30 instruments from recordings: grand piano, two electric pianos (Pianet, Wurlitzer), pipe organ,
    acoustic and electric guitar, electric and upright bass, violin, viola, cello, string ensemble,
    trumpet, trombone, French horn, tenor saxophone, flute, clarinet, solo voice "aah", harp, harpsichord,
    pizzicato strings, banjo, psaltery, dan tranh, kalimba, marimba, vibraphone, glockenspiel, tubular bells.
  - 9 synthesized instruments, for sounds with no licence-clean recording or that were synthesizers anyway:
    808 bass, FM electric piano, choir "aah", tonewheel organ, FM bells, nylon pluck, synth pluck,
    synth strings, synth brass. (The synthesizer can also make eight more that recordings replaced.)
  - An acoustic drum kit of 18 hits (kick, snare, rimshot, sidestick, three hi-hats, three toms, crash,
    ride, ride bell, tambourine, shaker, clap, cowbell, woodblock) and 11 orchestral effects (impacts,
    crescendo risers, wind chimes), trimmed and levelled; plus the synthesized 808 and 909 kits from before.
- Sources: eleven are CC0 (VSCO 2 Community Edition, VCSL, six Karoryfer libraries, Ganjo, Virtuosity Drums,
  a Karoryfer vocal set); the two electric pianos are Greg Sullivan's, CC BY 3.0 (credited); the viola is from
  the University of Iowa Musical Instrument Samples, whose page says they "may be downloaded and used for any
  projects, without restrictions". `CREDITS.md` is generated from the manifests and lists every file.
  Rejected on licence: jRhodes3d (non-commercial), a GPL banjo.
- When a plugin is unavailable, a stand-in preserves its saved state. A missing effect passes audio
  through and a missing instrument stays silent.

Tests added (131 tests, 102927 checks, all pass in release and in Debug with the checked heap):

- Pitch: notes at ten keys from E1 to C7, held and struck, at 44.1, 48 and 96 kHz, are heard within 4 cents;
  a note 27 cents flat is reported as that; vibrato averages out; a weak fundamental is still the pitch;
  noise is "unclear" and silence has none.
- Trimming finds the start within 12 ms before the onset and cuts the inaudible tail; held and struck notes
  are told apart. Loops: at four keys with and without vibrato the join is no steeper than the wave itself.
- Note names in file names (sharps, flats, negative octaves, other octave conventions, bare MIDI numbers,
  and words that are not notes).
- One note builds an instrument that plays within 3 cents on five keys and corrects a flat recording.
  Five notes given out of order, two unlabelled, are laid out with every key in exactly one zone; eight keys
  play in tune from the nearest recording; **a key held for ten seconds keeps its level within 1 dB with no
  click at the loop**; released, it stops; uneven recordings come out within a factor of 1.3 in level.
- Velocity layers and round robin choose the right recording; soft playing is measurably duller.
- Timbre: attack, release, filter, transpose, tremolo; 48 notes at once render bit-identically at four block
  sizes and thread counts with no allocation on the audio thread; all 20 presets sound.
- Instrument files round-trip (zones, loops, tuning, gain, audio to 24 bits); a project that uses an
  instrument file, and one that built its instrument from loose files, save, reload and render identically.
  AIFF is read. SFZ: a hand-written file with groups, defines, a round robin, a loop, a tune correction and
  missing samples loads correctly and plays in tune.
- FLAC (17 tests): bit-exact round trips at 16 and 24 bits, mono and stereo, seven lengths and three rates;
  streams made by ffmpeg at several compression levels decode to the right samples; damaged and truncated
  input is refused or cut short without reading out of bounds; a non-ASCII path.
- Every synthesized instrument is finite, at the expected level, builds with its stated keys, and plays in
  tune between its notes. A project with unknown plugins loads, plays, and saves them back unchanged.

Verified by script (`scripts\m9_instruments.txt`, silent; pictures `out\m9_*.png`): an instrument built from
one "voice" recording 20 cents sharp (the report says +20.0, and it plays); three notes given out of order
laid out as C4 / C5 / C6 zones; a note recorded into the pattern and exported measures 329.59 Hz for an E
(329.63); a recorded factory instrument (violin, grand piano) and a synthesized one (808 bass) loaded from
the library and sounded; after save, new, open, all ten channels were back and the pattern exported.

The library build's own report (`out\m9_library.log`): of 253 pitched recordings, 247 were heard on the key
the source states. The six that were not are inharmonic or have a weak fundamental (one piano note, a
psaltery, a kalimba tine, three tubular bells); for those the stated key was kept.

Not verified:

- **Nobody has listened to the factory instruments.** Their pitch, levels and loops are measured, not heard.
  The sourcing notes list doubts worth a listen: the Wurlitzer samples are short and stop abruptly, the
  banjo is narrow and short, the solo voice is one deep male voice and not a choir, some sources are quiet.
- Dropping files on the channel settings and the BUILD FROM SAMPLES chooser were not used with a real mouse
  (the script calls the same function). Building from a real recording of a voice was not tried.
- A clean checkout fetching the library from the network (`build.bat library`) was run here once with the
  cache already filled; the fetch script itself was tested by the agent that wrote it (fresh download of all
  282 files, checksum failure, 404, resume).

Partial or missing in this milestone:

- No recording was found under a usable licence for: a choir, a Rhodes, a tonewheel organ, a flat-top
  acoustic guitar (the guitar is an archtop), mandolin, ukulele, koto, music box, vinyl noise, synthetic
  risers. The first three are synthesized; the others are absent. There is no trap or lo-fi drum kit yet.
- The library is one velocity layer and one take per note (the engine supports more; the download was kept
  near 400 MB). Instruments are loaded whole when a channel uses them, not streamed.
- An instrument built from loose files is rebuilt from those files each time the project opens.
- SoundFont (.sf2) import arrived with milestone 13. SFZ opcodes beyond those listed are ignored.
- The preset-browser instrument with macro knobs (FL: FLEX) is not built: the Browser, Timbre's LIBRARY menu
  (milestone 11) and Timbre's presets do that job.

## Milestone 10: VST3 hosting

Built:

- PULP hosts VST3 instruments and effects (`app/PluginHost.cpp`). A hosted plugin sits behind the same
  internal interface as a built-in one, so the engine, the mixer, delay compensation, automation, links,
  undo and project files treat it like any other. The engine library still has no JUCE in it: the program
  hands the engine a function that makes plugins whose type starts with `vst3:`.
- Scanning (Settings > PLUGINS > FIND PLUGINS): every `.vst3` in `C:\Program Files\Common Files\VST3` and in
  folders the user adds is examined by a separate process (`PULP.exe --scan-vst3 <file> <result>`). A plugin that
  crashes, hangs for 45 seconds or holds nothing takes only that process down and goes on a blacklist, shown
  in red and left alone until RESCAN ALL or FORGET FAILED. The list is kept in `%APPDATA%\PULP\plugins.xml`.
- A hosted instrument is a Channel Rack channel (`+` > VST3 instruments, the Browser's Plugin database >
  VST3 instruments, or Replace instrument); a hosted effect is a mixer slot (slot menu > VST3 plugins).
  Notes reach an instrument as MIDI, sample-accurately; a note's colour is its MIDI channel; the channel
  pitch knob is the pitch wheel. An effect with a second input gets the mixer's sidechain on it.
- Each plugin's own editor opens in a floating window of its own (a plugin with no editor gets a plain list
  of its parameters). A strip of PULP's above the editor shows the control touched last; clicking it opens
  the usual control menu (create automation clip, link to controller, edit events). PARAMETERS shows the
  plugin's parameters as PULP knobs. Keys the plugin does not use (space to play, and so on) reach PULP.
- Parameters: up to 256 of a plugin's automatable parameters are ordinary PULP parameters (0 to 1), named by
  the plugin's own parameter identifiers, so automation clips, event automation, controller links, MIDI
  learn and recording of knob movements all work on them. A control dragged in the plugin's own window is
  recorded like a knob and is a step in the undo history.
- State: the plugin's whole state is stored in the project, with its name, maker and file. Undo and redo
  give the running instance the older state: the plugin is not loaded again and its window stays open.
- Latency: what the plugin reports goes into delay compensation like a built-in effect's (changes are picked
  up the next time the mixer is rebuilt). The song position, tempo and play state are passed to the plugin.
- Projects open even when a required plugin is missing. A stand-in retains the plugin's name and state,
  passes audio through for an effect, and stays silent for an instrument.
- A plugin that returns NaN or infinity is silenced for that block, so it cannot poison the mix.
- Two small VST3 plugins of PULP's own for checking all this on any PC (`tools\testplugins`, built by
  `build.bat testplugins`, not shipped): PULP Test Tone (an instrument, a sine per note) and PULP Test Delay
  (an effect that delays by exactly 256 samples and reports it).
- A fault from milestone 9, found and fixed here: every build of `dist` rewrote `factory\manifest.json` with
  only the 26 synthesized drum files, forgetting the 415 library files. The step now keeps the entries of
  files that exist, and the manifest in the repository and in `dist` lists all 441 again.
- Also in this milestone: the FLAC decoder now refuses a block whose "wasted bits" count leaves no bits (a
  hardening fix from the codec's author; the 17 FLAC tests pass).

Verified by script (`scripts\m10_vst3.txt` and `scripts\m10_missing.txt`, silent; log `out\m10_vst3.log`,
pictures `out\m10_*.png`, measurements by `numpy` on the exported files):

- The scanner found the test plugins and a third-party effect. It flagged the crashing test file in red
  and PULP kept running.
- The test instrument in the Channel Rack plays the pattern: 261.66 Hz for C5 (261.63), its two notes start within
  one sample of sample 0 and sample 48000 (step 9 at 120 BPM).
- A parameter set from PULP reaches the plugin (level 0.25 against 0.5: the export is exactly half as loud).
  A control moved from the plugin's side shows in PULP, names itself in the window strip ("Level: 0.90"),
  appears in the history as "Plugin: Level", and undo and redo move it back and forth with the window open.
- Delay compensation: with the 256-sample delay plugin on the instrument's mixer track, the export is
  sample-for-sample identical to the export without it (the kick on another track stays aligned); flipping
  the plugin's polarity changes the export by exactly the tone, which proves the plugin is in the path.
- An automation clip on the delay plugin's gain, rising over one bar, scales the tone accordingly.
- A third-party effect on the master reported its parameters and latency, and its editor opened.
  Changing a band gain altered the exported audio. After saving and reopening the project, all three
  plugins retained their settings and the export matched the original.
- Playing live (silent device) with both plugin windows open: no dropouts, clean exit with the windows open.
- With an empty plugin list (as on another PC) the same project opens, the kick still plays, the channel
  settings say the plugin is missing, and saving writes all three plugins' state back byte for byte.

Tests: the engine tests are unchanged in number (131 tests, 102927 checks, all pass in release and in Debug
with the checked heap). Hosted plugins need JUCE and real plugin files, so they are covered by the scripts
above and not by `pulp_tests`; the engine side (the stand-in for a missing plugin, the external factory) has
had tests since milestone 9.

Not verified:

- Instrument hosting was tested with PULP's test instrument. Third-party instruments and listening
  checks for hosted effects still need testing.
- A simulated drag in the test plugin reached PULP's touched-control, undo and recording handlers.
  Manual interaction with a third-party editor remains untested.
- How a hosted plugin sounds and behaves on the real sound card under load. The silent device ran it live.
- Plugins that take a long time to start, show licence windows, or need an iLok.

Partial or missing in this milestone:

- VST3 only. No VST2 (its SDK is no longer licensed), no CLAP (the spec's stretch goal).
- At most 256 parameters per plugin are exposed, as 0 to 1 knobs; the plugin's own window shows real values.
- Slide notes and per-note pan, mod X, mod Y and fine pitch are not sent to hosted instruments. The pitch
  wheel assumes the usual range of two semitones each way.
- A preset chosen inside a plugin's own window is saved with the project but is not an undo step and does
  not mark the project as changed until something else does (plugins do not announce it reliably).
- No preset browsing for hosted plugins from PULP's Browser, no per-plugin favourites or categories. Hosted
  effects were listed in the mixer slot menu only; since milestone 14 the Browser's plugin database lists
  them too (Effects > VST3 effects).
- One stereo input (plus a sidechain) and one stereo output per plugin: extra outputs of multi-output
  instruments are not routed. A plugin that only works in mono hears the sum of left and right.
- Exports tell hosted plugins that the render is not in real time (since milestone 14; not checked with a plugin
  that behaves differently offline).
- A plugin's latency that changes while it runs is taken into account at the next edit, not immediately.
- A plugin that crashes while it is running (not while being scanned) takes PULP down with it; the autosave
  and crash recovery from milestone 6 are the safety net.

## Milestone 11: the remaining synthesizers

Built (Channel Rack `+`, or F8):

- **Zest**, the flagship subtractive synth: two band-limited oscillators (five shapes, unison up to 7 with
  detune and spread, hard sync), sub and noise, two multimode filters in series or parallel, three envelopes,
  three LFOs (tempo sync, per note or global), an eight-slot modulation matrix, built-in drive, chorus, delay
  and reverb, mono / legato modes. 37 presets. (Written under the working name "Citrus" and renamed: that name
  said aloud is too close to an Image-Line product.)
- **Sextet**, a six-operator FM synth: seven operator shapes, a full 6 x 6 routing matrix with feedback, twelve
  ready-made algorithms, ratio or fixed frequencies, per-operator envelopes and key scaling, two LFOs, a pitch
  envelope, a filter, an alias guard. 33 presets.
- **Ripple**, a wavetable synth: two oscillators that morph through a table (22 built-in tables), position
  moved by envelope, LFOs and mod Y, unison, sub and noise, a per-voice filter, chorus and delay. A wavetable
  of your own loads with LOAD WAVETABLE in the channel settings. 31 presets.
- **Sixteen**, the pad drum sampler: sixteen pads on the keys C5 to D#6 (so the step sequencer's steps play
  pad 1), up to four velocity layers a pad, level, pan, tuning, length and cut group per pad (hats choke each
  other). It arrives with the 808 kit; ten kit presets; LOAD A FOLDER OF DRUMS puts kicks, snares, hats and
  the rest on their usual pads by the words in their file names; drop a sound on a pad to replace it, Ctrl-drop
  to add a layer. The pads light up as they play.
- **Chop**, the loop slicer: finds the hits in a loop (also a hat that lands on a ringing kick), puts each
  slice on its own key from C5 upward, shows the loop with its slices (click a slice to hear it, right-click
  to cut or join), and WRITE NOTES writes one note per slice into the piano roll so the pattern plays the
  loop at the project's tempo, ready to be rearranged. Also from the Browser: right-click an audio file >
  "Slice it in a new channel". Ten presets (gated, reversed, octave down ...).
- **Squelch**, a 303-style acid bass: saw or square into a resonant four-pole filter, with its own slide (a
  note that overlaps the one before glides to it without retriggering) and accent (notes above velocity 110
  are louder and open the filter further). 22 presets. Steps play it two octaves down, where a bass belongs.
- **Twine**, a plucked-string model: damping, brightness, pick position, stiffness, body, an even decay time
  across the keyboard, 32 notes at once. 24 presets.
- **Low Four**, a simple bass with seven controls (style, body, bite, sustain, tone, octave, level). 23 presets.
- **Thud**, a kick drum synthesizer: pitch sweep (start, end, time, curve), decay, click, drive, sub; the key
  transposes it. 23 presets.
- **Clatter**, a drum synthesizer with eleven models (kick, snare, clap, hats, tom, cymbal, rimshot, cowbell,
  clave, shaker) and a kit mode in which keys choose the model; a closed hat chokes an open one. 25 presets.
- **Sheaf**, an additive synthesizer (the spec's stretch goal for this milestone): each voice is up to 256
  sine waves and nothing else, so it cannot alias. Two spectra, A and B, each built from a shape (saw, square,
  triangle, pulse, vowel, bell, organ, or "User") with tilt, odd / even balance and a comb, a morph between
  them (by knob, envelope, LFO or mod Y), stiff-string stretch, two formant peaks that can stay put as the
  pitch moves, a filter that only scales partials, up to eight detuned clones of every partial, two
  envelopes, two LFOs; 32 presets. **Resynthesis**: drop a recording of one note on its settings (or
  ANALYSE RECORDING...) and it measures the note's harmonics at up to 64 moments, then plays that sound as
  the "User" shape at any pitch and length, with speed, start and loop controls. The measured table is
  saved in the project as numbers (the recording is not needed again). CLEAR forgets it.
- **Motes**, a granular sampler: drop any recording on its settings and each held note plays it as a stream
  of grains (position, spread, scan speed from frozen to normal to backwards, grain size, density, pitch
  spread, pan spread, reverse chance, envelope, filter). Scan speed 1 with short grains time-stretches the
  recording at any key. 24 presets that work on any sound. (Written as "Dust"; renamed, as that is another
  maker's granular plugin.)
- **MIDI Out**: a channel whose notes leave as MIDI (notes, program, bank, pitch wheel) to one of the PC's
  MIDI outputs, for external hardware.
- In Timbre's channel settings, **LIBRARY** lists every factory instrument by family and loads the chosen one
  into the channel: the preset browser over the factory library.
- The three-oscillator synth (Trio) and the Layer channel were there already.

Everything in the list above is in the program.

Tests (all pass in release and in Debug with the checked heap; the totals are at the end of milestone 14):

- Zest (25 tests), Sextet (17), Ripple (19), plus two speed tests: tuning within a fraction of a cent at
  44.1, 48 and 96 kHz, aliasing bounds, note timing to the sample, glide and slide notes, velocity, pan, mod
  X / Y, envelopes, LFOs, filters against their expected responses, the FM spectrum against Bessel functions,
  wavetable morphing and user tables, voice limits and stealing without clicks, every preset sounding at a
  sane level and reloading, identical renders at any block size and thread count with nothing allocated on
  the audio thread. These were written by the agents that wrote the synths; they were re-run here unchanged.
- Squelch, Twine, Low Four, Thud and Clatter (46 tests, by the agent that wrote them, re-run here unchanged):
  tuning (worst 0.72 cents), aliasing bounds, the acid slide measured cycle by cycle against an exponential
  glide and the accent's level and filter difference, the string's decay time within 1.2 % from key 36 to
  108, the kick's sweep within 0.11 % and its decay within 0.26 %, every drum model's level, length and
  character, hats choking, no clicks on retrigger or steal, all 117 presets at a sane level and reloading
  identically, identical renders at any block size.
- Sheaf (32 tests, by the agent that wrote it, re-run here unchanged in release and in Debug): every
  partial within 0.0004 cents of its frequency at three sample rates; nothing that is not a partial above
  -121 dB on held notes, and energy below the fundamental at most -86 dB while a note is bent up two
  octaves; the shapes' harmonics within 0.02 dB of the textbook; tilt, balance, comb, filter and formants
  within 0.15 dB of their stated curves; a synthetic note with known harmonics analysed to within 0.03 dB
  from 60 ms on and replayed an octave up within 0.05 dB; the analysis survives save and load to the
  identical render and is kept by presets; 16 notes of the heaviest preset use about 6 % of one core;
  identical renders at any block size, with and without the SSE2 oscillator loop, no allocation while
  playing. (Figures are the agent's from its notes; the pass / fail was re-run here.)
- Motes (9 tests, by the agent that wrote it): grain pitch within 0.001 cent of the key; at scan speed 1 the
  loudness curve follows the sample's (0.90 to 0.98) at five keys; a frozen position holds for as long as the
  note; the same notes render identically twice; silent without a sample; the sample survives a project
  save and load; stealing without clicks.
- Sixteen and Chop (7 tests, `tests\test_m11_pads.cpp`): pads play their own samples by key and velocity
  layer; level, pan, tune, length and cut groups measured; kit loading by file name; the factory kits; hits
  found within 3 ms (and a tick on a ringing thud); each key plays exactly its slice; slices played in time
  rebuild the loop's loudness curve (0.985 or better); save and load; identical renders at any block size.

Verified by script (`scripts\m11_beats.txt`, `scripts\m11_midiout.txt`, `scripts\m11_synths.txt`, silent;
pictures `out\m11_*.png`):

- Each of the eight synthesizers of this milestone was added to a new project, given two steps and exported:
  all sound on both steps, peak between -10.8 and -6.6 dBFS, and play the right pitch (261.7 Hz for C5 on
  Zest, Sextet, Ripple and Twine; 65.4 Hz for Squelch and 65.2 for Low Four, which sit two octaves down; 52 Hz
  where Thud's and Clatter's kicks settle). A project holding them saved and reopened.

- A two-bar 909 loop at 100 BPM was cut into its 16 hits, each slice starting 1 ms before its hit; its notes
  were written into the piano roll; the pattern's export matches the loop's loudness curve by 0.998 (sample
  for sample the residual is -13.9 dB, because a note can only start on a tick, 25 samples apart there); at
  125 BPM the hits come every 0.24 s as they should.
- A pad sampler arrived with the 808 kit, took the 909 kit from its folder, had one pad replaced and a layer
  added, was played from the step sequencer and by recording a note; after save, new and open the pads and the
  slices were back and the export identical.
- A MIDI Out channel on channel 3 with program 5 sent the program change and the notes of its steps (in
  silent mode the messages are counted and listed, never sent).
- (`scripts\m11_sheaf.txt`, pictures `out\m11_sheaf*.png`) Sheaf with **real recordings**, which its own
  tests never met: as it arrives it plays a saw whose first twelve harmonics are 1/n within 0.01 dB at
  261.66 Hz for C5. The factory library's sung "Aah" (129.3 Hz, 5.7 s) dropped on the channel was analysed
  into 153 harmonics at 64 moments and played at C5 in tune (0.2 cents); at 0.3 s into the note its nine
  harmonics above -30 dB are within 0.8 dB of the recording's (5.4 dB at 0.15 s, while the vowel opens).
  The cello's C (65.7 Hz) came back with its first sixteen harmonics within 1.9 dB at 0.15 s and 1.4 dB at
  0.3 s. Saved, a new project, opened again: the export is the same file byte for byte (the project file
  is 63 kB with the table in it). A hand clap was refused with a message and nothing changed. CLEAR gave
  exactly the saw again, and undo brought the cello back to the byte.

Not verified:

- **Nobody has listened to any of the new synths or their 273 presets.** Their levels, tuning and spectra are
  measured; whether they "sound good straight away", as the spec asks, is unknown.
- Sending MIDI Out to a physical instrument remains untested.
- The pads and the slice view were not operated with a real mouse (the script calls the same functions and
  the pictures show them drawn).

Partial or missing in this milestone:

- Sheaf's resynthesis keeps harmonics only: no noise (breath, bow, hammer), no partials further than a
  quarter of the fundamental from a harmonic (a piano's top, bells), one pitch for the whole note (vibrato
  is averaged out); an attack shorter than about 10 ms is flattened; chords and drums are refused or
  misread. Two real recordings were tried (above), both sustained. One analysed sound per instrument; its
  loop runs back and forth only and the loop points are not chosen for you. No drawn spectrum or table
  display: it uses the generic panel of 72 knobs. Its filter's resonance is a bump, not a ring; its LFOs
  are not tempo-synced. The SSE2 implementation and the scalar loop produced identical samples in testing.
- Motes reads grains with four-point interpolation and no band-limiting, so it aliases when transposed far
  up (not measured); at its extreme settings (200 long grains a second) eight notes cost a fifth of a core.
- Low Four is a string model with tone settings: that it "sounds like an electric bass" is the least supported
  claim here. Squelch is shaped like a 303 (ladder filter, slide, accent) but is not a circuit model; with the
  channel's Mono switch on it does not slide on overlapping notes. Twine plays flat above about 5 kHz
  fundamentals. Clatter's cowbell aliases audibly two octaves up (-42 dB); hat and cymbal aliasing was not
  measured. Clatter ignores note-off and slides, as drums do.
- Sixteen has no per-pad outputs to the mixer: the plugin interface gives an instrument one stereo output.
  (A kick that other parts duck under is a separate channel in the templates for that reason.)
- No preset-browser instrument with macro knobs of its own: Timbre with its LIBRARY menu and its 20 presets
  does that job.
- No automation-only channel: any control can have an automation clip directly (milestone 8), so there is
  nothing for such a channel to do. No speech / vocal-chop channel (optional in the spec).
- The synths use the generic panel of knobs. Sextet's matrix (36 knobs), Zest's modulation matrix and the
  envelopes would be far easier with drawn displays; none was built. (Sextet's "copy algorithm into the
  matrix" had no button; it has one since milestone 14: EDIT ALGORITHM AS MATRIX.)
- Chop does not stretch slices: at a slower tempo than the loop's there are gaps between slices, at a faster
  one they are cut short. Slice notes sit on the tick grid, not on the exact sample.
- Sixteen's kit presets are ten arrangements of three kits (808, 909, acoustic), not twenty different kits.
- MIDI Out: timing is within about a millisecond of the engine, not sample-exact; no control changes, no
  MIDI clock.

## Milestone 12: the remaining effects

Built (Mixer slot menu):

- Dynamics: **Triband** (three-band compressor and maximizer), **Snap** (transient shaper), **Curb** (soft
  clipper, oversampled), **Glow** (one-knob "louder and fuller"), **Sibilant** (de-esser), **Doorman** (noise
  gate / expander with look-ahead and sidechain; written as "Gatekeeper" and renamed, as that is another
  maker's product). The compressor with sidechain and the limiter with gate were there already; the gate and
  the de-esser show a scrolling gain-reduction display like theirs.
- Time and space: **Spaces** (convolution reverb, zero latency, twelve built-in impulse responses that PULP
  synthesizes itself, LOAD IMPULSE for your own), **Taps** (multitap delay, eight taps on a grid).
- Modulation: **Jet** (flanger), **Swirl** (phaser), **Flutter** (tremolo and auto-pan), **Waver**
  (vibrato), **Rotor** (rotary speaker). Chorus was there already.
- EQ and filter: **Ten Band** (graphic EQ, ten octave bands), **Motion Filter** (multimode filter with its
  own envelope follower and an LFO that can follow the tempo).
- Distortion: **Quarter Inch** (tape saturation with wow, flutter and optional hiss), **Glass** (tube
  stages), **Stack** (guitar amp voicings, tone stack and cabinets), **Splat** (fuzz with an octave), **Coarse**
  (bit depth and sample-rate reduction), **Bend** (a waveshaper whose curve is drawn in its window: drag the
  points, double-click to add one, right-click to remove one). All oversampled where they distort.
- Stereo: **Middle and Sides** (level and shelves for the middle and the sides separately, width, balance).
- Vocal: **True Note** (automatic pitch correction: key, nine scales or your own set of notes, retune time
  from instant to 400 ms, amount, formants kept or shifted, transpose), **Talker** (vocoder: the carrier comes
  from another mixer track through the sidechain, or from its own built-in carrier when nothing is routed; 8
  to 40 bands), **Second Voice** (harmonizer and doubler: up to four added voices by semitones or by scale
  steps so thirds stay in key, each with level, pan, delay and detune), **Lint** (noise reduction that learns:
  switch Learn on over a stretch of pure noise, then off). The noise gate is Doorman.
- Creative: **Rewind** (time and volume manipulation locked to the song: a time curve says how far behind
  "now" the sound is across each bar, a volume curve gates and pumps; both are drawn in its window: drag a
  point, double-click to add one, right-click for its shape; 24 presets from half speed and tape stop to
  stutters and trance gates), **Hiccup** (a glitch tool: on a grid, each step may repeat, reverse, tape-stop,
  crush or gate, by chance but the same way every time the bar comes round), **Thumb Pad** (eleven effects
  played from an XY pad: press to bring the effect in, drag to play it, let go and it fades out), **Platter**
  (a scratcher: press and drag the record sideways; it carries on when let go).
- An effect's window now stays open, in place, when a step is undone or redone (it used to close).
- Analysis: **Gauge**, one plugin with six displays: spectrum (with a held peak line), spectrogram,
  oscilloscope, vectorscope with a correlation meter, loudness (momentary, short-term and integrated LUFS to
  ITU-R BS.1770 / EBU R 128 with gating, peak and RMS), and a chromatic tuner. It changes nothing in the sound.

Everything in the list above is in the program.

Tests:

- Dynamics (35 tests) and space / modulation (36 tests), written by the agents that wrote the effects and
  re-run here unchanged: frequency and level responses against their designs, time constants, latency
  confirmed by nulling against a delayed copy and constant for every setting, convolution against direct
  convolution (-132 dB), exact tap times, aliasing bounds, presets stable and reloadable, silence in gives
  silence out, identical renders at any block size and thread count with no allocation on the audio thread.
- Creative effects (33 tests, by the agent that wrote them, re-run here unchanged): flat curves pass the
  input bit for bit; half speed reads 220.000 Hz for 440 Hz in; a stutter repeats the first eighth note to
  the bit; a render started from bar 3 matches that stretch of a render from the start; the glitch tool gives
  the same output for the same seed and song position; every pad mode measured at its corners; the record
  held still at speed 1 passes the input bit for bit, stops dead after its brake time, and plays an octave
  up when dragged forward at twice real time; no clicks beyond stated bounds.
- Vocal effects (51 tests, by the agent that wrote them, re-run here unchanged): a note 30 cents flat lands
  within 0.11 cents at instant speed; out-of-scale notes go to the nearest scale note; vibrato is flattened
  at instant speed and kept at 400 ms; formant peaks stay within 2 % when kept; unvoiced sound and silence
  pass bit for bit; harmony intervals within 0.14 cents and the right thirds in key; the vocoder puts 80 % of
  its output in the band the voice occupies; learned noise drops 24 dB while a tone over it changes by 0.001
  dB; four tests use six recorded sung notes from the factory library (median within 1.1 cents of the note).
- Tone effects (54 tests, by the agent that wrote them, re-run here unchanged): the graphic EQ's centres
  within 0.002 dB of its sliders; filter slopes and resonance; harmonics of each saturator (odd only for the
  tape, even with bias for the tube); wow and flutter depth; crusher steps and images; the drawn curve applied
  exactly; mid/side arithmetic; oversampling at least 30 dB better than none in all cases but one (the
  waveshaper at 4x with a hard-cornered curve, 26 dB); neutral settings bit-transparent; latencies confirmed.
- Gauge (3 tests, `tests\test_m12_gauge.cpp`): the audio passes bit for bit; a 997 Hz sine at -23 dBFS on
  both sides reads -23.0 LUFS (momentary, short-term and integrated, at 44.1, 48 and 96 kHz), full scale on
  one side reads -3.01; bass counts for less and treble for more as the weighting says; silence and passages
  more than 10 LU down are gated out of the integrated reading, which starts again when playback starts;
  peak, RMS and correlation (+1, -1, about 0) are right.

Verified by script (`scripts\m12_creative.txt`, silent; pictures `out\m12_rewind*.png`, `m12_thumbpad.png`,
`m12_platter.png`, `m12_motes.png`): Rewind on a track of the demo song had a point added to each curve by a
(scripted) double-click and then took a preset, its curves redrawn; Thumb Pad, Platter and Hiccup loaded and
their controls were pictured; the song exported with all four. A recorded voice dropped on a Motes channel
played from the step sequencer, and exported identically after save, new and open.

Verified by script (`scripts\m12_vocal.txt`, silent): a recorded sung note from the factory library (129.04
Hz, 23 cents under its C) was placed in the playlist and exported through True Note set to instant and three
semitones up: 155.56 Hz, exactly the note. Second Voice added a fifth above it (nothing at 193 Hz before, a
clear partial after). Talker made sound from its own carrier; Lint with nothing learned passed the voice
unchanged in level. The project saved and reopened.

Verified by script (`scripts\m12_effects.txt`, silent; pictures `out\m12_*.png`): Gauge on the master of the
demo song in all six displays while it played (integrated -8.5 LUFS, correlation 0.8); all twenty-two other new
effects loaded onto tracks of the demo song and several panels pictured; the waveshaper's curve was given a
point and had another dragged with the (scripted) mouse, undone and redone with its window staying open; the
song exported with all of them and, after save, new and open, exported identically.

Not verified:

- **Nobody has listened to any of these effects** or their presets. Whether the impulse responses sound like
  rooms, whether Rotor sounds like the cabinet it models, whether Glow flatters a mix: unknown.
- Loading a user impulse response through the LOAD IMPULSE button (the chooser was not operated; the function
  it calls is tested with WAV files).
- The tuner on a real instrument or voice.

Partial or missing in this milestone (see also the list of everything missing at the end):

- No linear-phase EQ mode. No frequency splitter that feeds bands to other mixer tracks; no dedicated send
  plugin (mixer routing does sends). The mono-maker below a frequency is Stereo Kit's simple one (a 12 dB
  high-pass on the sides), not a phase-coherent crossover.
- The tape, tube and amp are shaped like their models (asymmetry, sag, tone stack, cabinet curves) but are
  not circuit or tape models; the cabinets are filter networks, not impulse responses. Each anti-aliased
  stage loses about half a dB at 20 kHz (up to 1.9 dB for the amp's lead voicing).
- The creative effects assume four beats to the bar. Thumb Pad rings audibly when thrown about fast on its
  resonant modes (measured as steps ten times the test tone's own; by design, but unheard). Platter cannot
  play ahead of the newest sound. Hiccup has no triplet grids. Nobody has dragged the pad, the record or a
  curve point with a real mouse (curve points were added by scripted double-clicks).
- The vocal effects were tried on sustained sung vowels only: no speech, no consonants, no lyrics, no real
  room noise. True Note and Second Voice are 18 ms late and Lint 41 ms (compensated in the mix, too long to
  monitor through while singing). True Note lets the first 7 to 15 ms of a note through uncorrected, follows
  one voice only, and has no way to draw target notes; Talker's built-in carrier cannot be played from notes
  and its output is not level-limited; Second Voice changes level with the interval when formants are kept;
  Lint handles steady noise only, and Learn left on spoils the profile. No note-by-note pitch editor
  (stretch goal).
- No modular patcher (stretch goal).
- Triband (30 knobs), Taps (57), Rotor, Stack and Ten Band use the generic panel (the graphic EQ has knobs,
  not sliders); Hiccup's five chances likewise; Gauge measures sample peaks, not true peaks.

## Milestone 13: imports, export formats, templates, the demo song

Built:

- **MIDI files as songs** (File > Import a MIDI file as channels and a pattern): every track and MIDI channel
  becomes a channel with an instrument chosen from its General MIDI program (factory instruments where there
  is one, Trio otherwise; the drum channel becomes a Sixteen with the acoustic kit, each drum key moved to
  the pad that holds that sound), all in one pattern placed in the playlist. An untouched project takes the
  file's tempo and time signature. File > Export a MIDI file writes the song (in song mode: the playlist's
  pattern clips laid end to end) or the pattern, one track per channel, drums on channel 10 with their
  General MIDI keys. (The piano roll's own single-track import and export are from milestone 3.)
- **SoundFonts** (.sf2): Channel Rack `+` > SoundFont preset: choose a file, then a preset from its list;
  the preset's samples, key and velocity ranges, root keys, tuning, loops, levels and stereo pairs become a
  Timbre instrument, written once as FLAC into `%APPDATA%\PULP\Instruments`.
- **Export**: WAV (16, 24, 32-bit float), **FLAC** (16 or 24 bit), **OGG** (Vorbis) and **MP3**, chosen in the export window;
  sample rate, tail handling and dither as before; **Split mixer tracks** writes one file for each mixer track
  that has channels or audio on it (the other tracks silenced, the buses it feeds kept; silent tracks get no
  file).
- **Ogg Vorbis**, written from scratch for PULP like the FLAC codec (no third-party code): .ogg files are read
  anywhere an audio file can be used (Browser, playlist, samplers, pads) and the export window writes them.
  The decoder is complete Vorbis I (floors 0 and 1, residues 0 to 2, coupling, both block sizes, exact
  length); the encoder is variable bit rate with a quality setting (about 136 kbit/s at the default 0.6).
- **MP3**, also written from scratch: the decoder reads MPEG-1, 2 and 2.5 Layer III (every stereo mode,
  constant and variable bit rate, free format, the bit reservoir, all block types, ID3 tags skipped, and the
  encoder delay and padding taken off so that a file comes back at its exact length); the encoder writes
  constant bit rate MPEG-1 at 96 to 320 kbit/s (the export window offers 128, 192, 256 and 320) at 32, 44.1 or
  48 kHz, resampling anything else. Files PULP writes carry the delay and padding, so other programs also
  read them at the exact length.
- The time signature can be set (right-click the tempo box; it is shown beside the tempo), and exports tell
  hosted plugins that the render is not in real time.
- An export is refused, with a message, if its file ending names a format this build cannot write.
- **Templates** (File > New from template): Empty; Basic with limiter; Trap, House, Lo-fi and Synthwave
  starters, each with a kick of its own, a drum kit on pads, bass, chords or pad on their own mixer tracks, a
  reverb bus, a limiter, and (house, synthwave) the bass ducking under the kick.
- **The demo song** (File > Open the demo song): 20 bars at 122 BPM for seven instruments (909 kick, drum
  pads, Zest bass, electric piano from the library, Ripple pad, Sextet lead, vibraphone), with the bass
  sidechained to the kick, a reverb bus fed by five sends, chorus, EQ, a ping-pong delay, a filter opened by
  two automation clips, a fade-out automated on the master, an arrangement with an intro, a break and a fill.
  It is built in code (`engine\src\Templates.cpp`), so it is also the end-to-end test.
- Found and fixed while testing this milestone: clicking a channel's name in the Channel Rack could crash
  the program (the rack rebuilt its rows while one of them was still handling the click; rows are now kept).
  This fault dates from milestone 2 and had not shown in tests. Release builds now keep their symbols
  (`PULP.pdb` beside the build), so `crash.log` names the functions: that is how it was found.

Tests (`tests\test_m13_files.cpp`, `tests\test_m13_templates.cpp`, 4 tests; the Ogg codec's 18; the MP3
codec's 27):

- MP3 (by the agent that wrote it, re-run here unchanged): 22 embedded reference streams (from LAME, from
  libshine and written straight from the bitstream syntax to reach what no encoder here produces: intensity
  stereo, mixed blocks, every Huffman table) decode to ffmpeg's samples and the exact length; the encoder
  round-trips sines, sweeps, noise, a music-like mix, silence and inputs down to one sample at 96 to 320
  kbit/s above stated signal-to-noise floors with the right frame sizes; cut, damaged and non-MP3 input never
  crashes (the input sits between two no-access pages, so a read outside it would stop the run). The agent
  reports from its own runs: within 1.6e-6 of ffmpeg on 39 LAME files; 226 files from this encoder decoded by
  ffmpeg without warnings at the exact length; music-like stereo at 192 kbit/s 23.0 dB against LAME's 23.9;
  about 970,000 damaged-stream decodes without a crash, one of which found a real bug that is now a test.

- Ogg (by the agent that wrote it, re-run here unchanged): 23 embedded reference streams made by libvorbis,
  by ffmpeg's encoder and by a hand-written bit-level writer (floor 0, every residue type, four channels,
  odd block sizes) decode to the expected samples and the exact length; damaged, cut and non-Ogg input never
  crashes; the encoder round-trips sines, sweeps, noise and a music-like mix through the decoder above
  stated signal-to-noise floors, with exact lengths, at several qualities, mono and stereo. The agent also
  reports, from its own runs outside the test suite: 224 files from other encoders decoded within -120 dB of
  libsndfile; 879 files from this encoder accepted by ffmpeg and libsndfile without warnings at the right
  length; a fuzz run of 550,000 damaged files under AddressSanitizer with no report.

- MIDI files keep their program numbers through a write and a read; General MIDI names and drum pads.
- A SoundFont made by the test (two mono samples at different rates, one looped; a stereo pair; preset and
  instrument generators) lists its presets in order and builds instruments that play in tune on and off the
  root keys, hold a looped note past the end of its sample, apply fine tuning and attenuation, join the stereo
  pair, survive being saved as a PULP instrument; cut and wrong files are refused without a crash.
- Every template makes a project in which a note on every channel sounds, peaks at or under full scale, and
  renders identically after being written down and read back.
- The demo song renders to exactly 20 bars; the limiter holds the master under full scale; the intro is
  quieter than the verse; the low end arrives with the kick at bar 5 and leaves for the break; the last bar
  fades; the bass alone is quieter when the kick keys its compressor; eight bars from the middle render
  identically after a JSON round trip at another block size and thread count with nothing allocated.

Verified by script (`scripts\m13_midi.txt`, `scripts\m13_demo.txt`, silent; pictures `out\m13_*.png`):

- A four-part MIDI file (written by `out\m13\make_midi.py`) became four channels (Grand Piano, Electric Bass,
  Trio, acoustic drum kit) and a 4-bar pattern at its 110 BPM; the song exported at the right length; the MIDI
  export has five tracks at 960 ticks.
- (`scripts\m13_codecs.txt`) The demo song exported as MP3 at 192 kbit/s: 946 kB, which libsndfile reads as
  exactly the 1,888,525 frames of the WAV export, 37.3 dB signal to error, and ffmpeg plays without a
  warning; read back into PULP's playlist, PULP's own decode matches libsndfile's within 3.5e-6.
- (`scripts\m13_codecs.txt`) The demo song exported as OGG: 760 kB (155 kbit/s), which libsndfile reads as
  exactly the 1,888,525 frames of the WAV export, 28.3 dB signal to error. Placed in the playlist of a new
  project and exported again, PULP's own decode matches libsndfile's within 9e-7 everywhere the file stays
  under full scale (the lossy file overshoots to 1.043 on 164 samples, which a 24-bit export clips).
- With each part on its own mixer track and the master's limiter removed, the four stems add up to the full
  mix within 2e-7; FLAC at 24 bit / 48 kHz and 16 bit / 44.1 kHz were written and read back by libsndfile.
- Every template opened; the demo song opened, played for a moment with 13 voices and no dropouts, exported
  (39.3 s, peak 0.966) and exported identically after save, new and open.

Not verified:

- **Nobody has listened to the demo song, the templates, or an OGG or MP3 file PULP wrote.** The demo's arrangement and mix were designed on
  paper and checked by measurement (levels per section, the kick's entry, the sidechain, the fade).
- SoundFont coverage uses generated fixtures. A real General MIDI bank may use generators the reader ignores.
- MIDI import with real-world files (tempo maps, odd time signatures, many channels).
- The export window's new choices were not clicked (the script passes the same options).

Partial or missing in this milestone:

- MP3 export is constant bit rate only and OGG export variable bit rate only; both are mono or stereo. An OGG with more channels is read as its front left and right. MP3 inside a WAV container, Layers I and II, and Ogg files holding Opus or FLAC are refused.
- MIDI import ignores controllers, pitch bend and program changes after the first. (Tempo changes in the
  file were ignored when this milestone was finished; they now arrive as tempo automation, see milestone 14,
  gaps closed. Time-signature changes within a file are still ignored: the last one read stands.)
- SoundFont import keeps samples, ranges, tuning, loops and levels, and the release time; not the SoundFont's
  envelopes, filters or modulators. Where a preset layers several samples on one key, one of them plays.
- Stems are made by muting, one render per track; a track that has its own channels and also receives
  another track's send carries both in its stem.
- Templates and the demo song are built in code, not editable template files; there are four genre starters.

## Milestone 14: polish

Built:

- **Shortcuts**: the program-wide commands (24 at first, 51 after the audit below) (transport, windows, patterns, files, undo) are a table with a
  key each, shown in Settings > SHORTCUTS; click a command and press a key to change it (a key another
  command had is taken from it and that is said; right-click removes a key; STANDARD KEYS puts everything
  back). Changes are kept in `%APPDATA%\PULP\shortcuts.xml`. F8 now opens the plugin picker (the list of
  instruments), which was missing.
- **A first-run tour**: nine steps laid over the program, each lighting up one part (Channel Rack, transport,
  Browser, Piano Roll, Playlist, Mixer) and saying in plain words what it is for and how to start. It is shown
  once, the first time PULP starts; File > Take the tour shows it again.
- **An installer package**: `build.bat installer` packs `dist\PULP` into `dist\PULP-setup.zip` with
  `Install PULP.cmd`, which copies PULP to `%LOCALAPPDATA%\Programs\PULP`, adds a Start Menu shortcut and
  makes .pulp files open in PULP for the current user. No administrator rights, no system setting touched;
  `Uninstall PULP.cmd` in the installed folder takes it all away again and leaves projects and settings.
- **Performance, measured against the spec's target** (100 channels, 60 active mixer tracks, 150 effects, a
  256-sample buffer). `pulp_render --stress` builds a project that size in memory (ten of each of ten
  instrument kinds on their presets, every effect kind in rotation, 480 voices sounding) and plays it at the
  pace of a sound card with the mixer's worker threads, timing every block. At the target load, one of
  three 20-second runs missed a single block deadline. Reducing the worker count or doubling the project
  size produced more late blocks.
- `paintbench` repeatedly draws a window with the software renderer. With the demo song playing, the
  full window rendered within the 16.7 ms budget for 60 frames per second. The play position and
  scroll bars now update 60 times per second. Meters and scopes update 30 times per second.
- **The shortcut audit**: every line of the manual's shortcut page was compared with PULP and the result is
  `docs\SHORTCUTS.md` (same / added / no, line by line). The additions: Ctrl+H (stop all sound),
  Ctrl+I (wait for input), Ctrl+P (count-in), Ctrl+B (blend / replace), Ctrl+E (step recording), numpad * and
  / (bar forward and back), numpad 1 to 9 (patterns), Ctrl+F4, F2 (rename pattern), F1, F12, Tab, Esc,
  Ctrl+Shift+H, Ctrl+Shift+M, Ctrl+Shift+R; in the rack 1 to 0 (mute), Ctrl+1 to 0 (solo), Ctrl+C / X / V
  (steps), Shift+Up / Down; in the mixer S, Ctrl+L, Shift+Ctrl+L, F2; in the playlist Alt+M, Alt+Shift+M,
  Shift+I, Shift+Q, Shift+C, Shift+4 / 5 / 0; in the piano roll Alt+M, Alt+Shift+M, Shift+I, S, O, Alt+V, G,
  K, Shift+4. The program-wide ones joined the table of remappable commands (now 51).
- Gaps from earlier milestones that were closed here:
  - The **time signature** (the spec lists it in the toolbar; it could not be set anywhere): right-click the
    tempo box > Time signature; the box shows it beside the tempo; bars, grids and the count-in follow it.
  - The Browser's **plugin database** listed instruments only. It now lists the effects by kind, and every
    built-in instrument and effect opens to show its presets. Double-click an effect to put it in a free slot
    of the selected mixer track; double-click a preset to use it (on the selected channel or the selected
    track's effect of that kind if there is one, otherwise in a new channel or slot).
  - Effect windows no longer close when a step is undone or redone (milestone 6 listed this).
  - Double-clicking a note in the piano roll opens its properties as numbers (velocity, pan, release, mod X
    and Y, fine pitch, length); with several notes selected, what is changed goes to all of them (milestone 3
    listed this as missing).
  - **Lid**, the limiter, has the compressor stage milestone 5 listed as missing: switched on, a compressor
    with threshold, ratio, knee, attack and release works ahead of the limiter (which still holds the
    ceiling after it), with its own gain-reduction reading and four presets. It is off in every project made
    before, which therefore sound as they did (`tests\test_m14_lid.cpp`, 2 tests: the output follows the
    ratio within 0.25 dB from 30 dB under to 18 dB over the threshold; after one attack time 63 % of the
    reduction is there and after one release time 37 % is left; switched off it passes a tone within
    0.01 dB; no allocation while it runs).
  - **Groups of playlist tracks fold away** (milestone 4 listed this): the top track of a group has a small
    triangle at the left of its name; click it (or right-click the name > Hide the group's tracks) and the
    tracks grouped under it are hidden, with "(+2)" beside the name. Hidden tracks still play; Ctrl+A,
    Shift+I and the slice tool leave their clips alone, and clips cannot be dragged onto them. The state is
    saved with the project and can be undone (`tests\test_m14_tracks.cpp`: a collapsed group renders
    sample for sample what the open one does, and the flag survives save and load;
    `scripts\m14_groups.txt`: in the demo song two tracks were grouped under Bass, a click on the triangle
    hid them and moved the tracks below up by 60 pixels, select-all left the nine hidden clips unselected,
    a second click, undo, redo, save and open each gave the expected rows; two selected clips dragged
    down one row, where the second would have landed on a hidden track, stayed put, and dragged two rows
    they landed on the tracks that show; pictured in
    `out\m14_groups_shut.png`).
  - **Mixer tracks can change places** (milestone 5 listed this): Alt+Left / Alt+Right, or Move left /
    Move right in the track's menu, swap an insert track with its neighbour. Everything goes with it: its
    name, colour, fader, effects, the channels and audio clips that play through it, the routes and sends to
    and from it, sidechain feeds, automation clips, events and controller links (they are re-addressed, so a
    curve drawn for "Insert 5 - Sweep - Cutoff" follows the track to Insert 6). The master and the "current"
    track do not move. Tracks are not dragged with the mouse.
  - **A fault found on the way and put right**: moving an effect up or down a slot (the slot menu, since
    milestone 5) left its automation, events and links behind at the old slot, where they then moved
    whatever effect was there or nothing at all. They now go with the effect, as does a link that listens
    to a controller plugin that is moved.
    (`tests\test_m14_mixer_order.cpp`, 2 tests: a song with a trigger, a follower linked to a fader, an
    automated effect, a pan curve and a send whose level the pattern's events move renders sample for
    sample the same after each of four track swaps, on other block sizes and thread counts, after save and
    load, and after an effect or the follower changes slots; swapped back, the project file is what it was.
    `scripts\m14_mixorder.txt`: in the demo song Alt+Right twice moved Drums from insert 2 to 4, the menu's
    move took the kick past the bass, whose sidechain feed followed, and the Pad track's two filter curves
    followed it from insert 5 to 6. The export after the moves, and again after save and open, differs
    from the export before by at most 4 steps of the 24-bit file's last bit (-126 dB) on 8 % of samples:
    the tracks are added up in another order, which rounds differently.)
  - **Notes are recorded in song mode too** (milestones 3 and 8 listed "pattern mode only"). With Record lit
    and the song playing, a note from the typing keyboard, the piano roll's keys or a MIDI keyboard goes into
    the pattern that is playing at that place: the selected pattern, if one of its clips is under the play
    position; otherwise a new pattern in a new clip on the first free track, starting on the bar line, which
    then takes the rest of that recording and grows by whole bars as you play on. The song does not wrap at
    its end while you record (unless a loop region is set). Replace mode works in pattern mode only: in song
    mode recorded notes are always added (`tests\test_m14_recsong.cpp`: notes placed at eleven song
    positions, through clips with an offset, past clip ends and around clips that are in the way, each
    sound on its exact sample in the render; `scripts\m14_recsong.txt`: in an empty song two notes played a
    bar apart made one two-bar clip; a note played from bar 5 made a second pattern and clip there; played
    again from bar 5, the next note joined it; undo and redo took a note away and brought it back).
  - **A MIDI file's tempo changes come in with it** (milestone 13 listed "first tempo only"): they become
    an automation clip of the tempo on the playlist track under the imported pattern, each tempo held until
    the next. Into a project that already has music the notes come in at the project's tempo and the
    import message says the file's tempo changes were left out. MIDI files PULP writes still carry one
    tempo (`tests\test_m14_tempomap.cpp`, 2 tests: tempo events survive writing and reading, two on one
    tick resolve to the later, a single-tempo file makes no clip; a click on each bar line of a song at
    120, 60 and 180 BPM lands within one 64-sample step of where the map puts it.
    `scripts\m14_tempomap.txt`: a file with bars at 120, 90 and 150 BPM, made by
    `tools\make_test_midi.py`, came in with "2 tempo changes as an automation clip" and exported as
    exactly the 396,800 frames those tempos add up to).
  - **A fault found on the way and put right**: with the tempo automated, an export with the tail cut ended
    early by the mix's plugin delay (239 samples, 5 ms, with the limiter the new project has on its
    master). The end of the song is now the end of the file to the sample; the test above renders the
    same song with and without a look-ahead limiter and requires the same length.
  - Sextet has the button milestone 11 left out: **EDIT ALGORITHM AS MATRIX** copies the chosen algorithm's
    routing into the Matrix and Out knobs and switches to "Custom matrix" (`scripts\m14_sextet.txt`: the
    pattern exported before and after the click is the same file byte for byte, the saved project has
    Algorithm at "Custom matrix" with the routing in the matrix, and undo brings the algorithm back).
  - **The piano roll's LFO tool writes events too** (milestone 8 listed "note properties only"): besides
    the six note properties, its Property list has Channel volume, Channel panning and Channel pitch, which
    fill the whole pattern with events for that control of the channel (sine, triangle, square, saws; they
    replace the control's events in that pattern; Cancel puts them back, and the result is one undo step).
    (`tests\test_m14_lfo.cpp`, 2 tests: for five shapes, four periods and three phases the events stay within
    1 % of the wave for the sine and 0.4 % for the others; a square on a channel's volume and a saw on its
    pan move those controls in the running engine as the wave says. `scripts\m14_lfo.txt`: a held note
    with a four-step square on the channel volume exported at 0.07 to 0.10 RMS in the high halves and
    0.001 in the low ones; the saved pattern holds the 24 events; a cancelled second LFO left them alone;
    undo removed them.)
  - The milestone table said of milestone 1 "settings window still to come": that note was out of date. The
    settings window (F10) has existed since milestone 2 and now has four pages: audio, MIDI, plugins,
    shortcuts.
- Crash logs with function names (see milestone 13).

Verified by script (`scripts\m14_keys.txt`): F4 and numpad 1 moved between patterns; 2 muted the second
channel and Ctrl+3 soloed the third and released it; Ctrl+C / Ctrl+V copied the kick's steps onto the clap;
in the mixer Ctrl+L sent the selected channel to track 3 (checked in the saved file) and S soloed it; in the
playlist Alt+M muted two clips, Alt+Shift+M unmuted them, Shift+I inverted the selection; F12 closed every
window and Ctrl+Shift+H put them back; numpad * and / moved the song position by a bar.
`scripts\m14_paint.txt` and `scripts\m14_layout.txt` give the drawing times and check that window places
are kept. `scripts\m14_browser.txt`: a Zest preset chosen in the Browser made a new channel and a second
preset went onto that channel; an effect went into a free slot of the selected mixer track, its preset onto
it, and another effect's preset added that effect; the tree is pictured open (`out\m14_browser.png`). The
time signature was set to 3/4 by script: a bar became 2880 ticks, the next-bar key moved by that much, and
the project file kept it. `scripts/m14_noteprops.txt`: a double-click on a note opened its properties; velocity,
length and fine pitch typed there were in the saved file, and a pan typed with two notes selected went to both.

Verified by script (`scripts\m14_shortcuts.txt` with `m14_shortcuts2.txt`, `scripts\m14_tour.txt`; pictures
`out\m14_*.png`): the standard keys work; the mixer was moved to Ctrl+K (F9 then did nothing), play to P (the
space bar then did nothing); a key taken from another command left that command with none; after a restart
the changed keys were still in force; STANDARD KEYS restored them. The tour was stepped through all nine
steps, each lighting the right window. The installer package was built (221 MB), unpacked and installed into
a scratch folder with shortcuts and file association switched off, and the installed copy ran a script.

Not verified:

- The installer test skipped shortcut creation, file associations and uninstallation.
- Changing a shortcut by clicking a row and pressing a key (the script assigns keys through the same
  function); the tour's NEXT / BACK buttons and arrow keys with a real mouse and keyboard.

Partial or missing in this milestone:

- The audit left 64 lines of the manual's list (some covering several keys) that PULP does not have, against
  55 that were already the same and 39 that were added; `docs\SHORTCUTS.md`
  lists each with the reason (mostly features PULP lacks: consolidating clips, note groups, performance
  mode; and the keys that tell left Shift from right Shift). The manual page was
  read through a text extraction, so a line it dropped would be missing from the audit.
- Only the program-wide shortcuts can be changed. The keys inside the Piano Roll, the Playlist and the
  Channel Rack (tool letters, Ctrl+Q, Alt+S and the rest) are fixed.
- The stress benchmark occasionally missed a block deadline. Live playback while scrolling the interface
  remains untested, and the stress project excludes hosted VST3 plugins.
- Window places are kept between runs (layout.xml); there are no named layout presets.
- The hint bar covers what earlier milestones gave it; the new panels (pads, slices, Gauge, plugin settings,
  shortcuts) have tooltips, which the hint bar shows, but no audit of "hint text everywhere" was made.
- The installer is a zip with a script, not a signed setup program; there is no update mechanism.
- Still open from earlier milestones (each is listed where it arose): mixer tracks move by key or menu, not
  by dragging; replace-mode note recording works in pattern mode only; one time signature for the whole
  project (no changes within a song).

### The whole program at the last full run

On 2026-10-16, at commit fae677b:

- `build.bat` (release, everything): 559 engine tests, 316,929 checks, none failed.
- `build.bat test` (Debug with the checked heap): the full run was made one commit earlier, before Sheaf came
  in: 527 tests, 289,703 checks, none failed. Sheaf's 32 tests were then run in Debug on their own (3,121
  checks, none failed). An earlier Debug run the same day had one failure, in
  `denoise_profile_can_be_saved_and_loaded_while_it_runs`: its last check assumed the second thread got
  time to save ten times. The test now waits for that thread to finish.
- All 39 silent scripts in `scripts\` end with "0 errors" (`m6_files` ends in a crash on purpose, which
  `m6_recover` then recovers from). The four `_audible` scripts and the three that open the real sound
  card (`m7_device`, `m7_input`, `m7_stream`) were not run again in this last pass.
- `dist\PULP\PULP.exe` launched with audio enabled and closed cleanly from its close button.
- `dist\PULP-setup.zip` was packed again from that build (196 MB); it was not unpacked or installed again.

## Deviations from FL Studio

Every known difference, with the reason. Deviations are listed here as they are introduced.

| ID | What differs | Why |
|----|--------------|-----|
| D-TIMEBASE | Timebase is fixed at 960 PPQ. FL lets you choose 24 to 960. | One resolution keeps every editor and the file format simple; 960 is FL's finest. |
| D-QUANTUM | Parameter changes and automation take effect on 64-sample boundaries (1.3 ms at 48 kHz) with a ramp between: the value is exact at the end of each 64 samples. Notes are still placed on exact samples. | Fixed internal blocks are what make export identical to playback at any buffer size. |
| D-KNOB-LAW | Channel volume: 78% is unity gain and the curve is (v/0.78)^2. Master volume: 80% is unity. FL's exact knob curves were not measured. | Could not measure FL here; defaults sit where FL's defaults sit. |
| D-PAN-LAW | Panning is a balance law: centre is unity on both sides, the far side fades. FL's default is its circular law. | Simpler and keeps centred sounds at unity; a project option can follow later. |
| D-VELOCITY | Velocity 100 (the default) plays a sample at its recorded level; gain follows (velocity/100)^2. | FL's curve was not measured. |
| D-SWING | Swing warps the timeline so every second sixteenth is delayed, up to half a step at 100%. | FL's exact swing amount was not measured. |
| D-NAMES | Every built-in plugin has an original name and look. | Required: no Image-Line names or graphics. |
| D-CHANSET | One channel settings window follows the selected channel. FL opens a window per channel. | Simpler to manage; detach it to keep it on another screen. |
| D-ARP | Arpeggiator chords are a fixed list of nine; "Auto" walks the held notes. FL has a longer chord list and a slide option. | Kept to what is tested. |
| D-ECHO | Echo delay repeats whole notes with feedback as a velocity factor and steps for pan, pitch, mod X and mod Y. FL's echo also has a "ping pong" and fat mode. | Kept to what is tested. |
| D-TRACKING | Tracking is three controls (velocity to volume, key to pan, key to mod X). FL has separate volume / pan / mod curves with a middle key. | Simplified. |
| D-LAYER | Layering is a "Layer" instrument whose children are set from the channel menu. | Same result as FL's Layer channel; different way to pick children. |
| D-SOLO | Solo mutes the other channels (and un-solos when clicked again), as FL does; there is no separate solo state. | Matches FL. Listed for clarity. |
| D-STEPGRAPH | The graph editor's Shift property moves a step later within its own step only. | Matches the step grid; larger shifts belong to the piano roll. |
| D-TRIO | TRIO has its own amplitude and filter envelopes. FL's 3x Osc leaves envelopes to the channel settings. | A default synth should be playable without visiting another tab. |
| D-SNAPLIST | The snap list has no separate "Cell" entry; "Line" follows the grid lines at the current zoom. | Cell and Line behave alike at this grid. |
| D-STAMP | The stamp stays active until set back to None. | FL offers "only one"; kept simple. |
| D-PASTE | Paste lands on the first whole bar in view, keeping the notes' position inside the bar. | Predictable without a playhead-based rule. |
| D-RIFF | The riff generator has three kinds (progression, arpeggio, bass groove) with root, scale, bars, density, sevenths and seed. FL's Riff Machine is an eight-stage chain. | Covers "progressions, arps and grooves" from the spec. |
| D-TRIMMER | The trimmer keeps N steps and removes M, repeating. FL's Claw Machine has more options. | Same purpose: odd-length loops. |
| D-LFO | The LFO tool writes per-note property values. FL's can also write event automation. | Event automation arrives in milestone 8. |
| D-RECMODE | Recorded notes are quantized on input to the snap setting; there is no separate input-quantize switch. | One setting to learn. |
| D-METRO | The metronome has four synthesized sounds (beep, tick, wood block, bell) with an accent on beat one. FL lets you pick recorded sounds. | Nothing to licence, and it is never part of a project. |
| D-PLSNAP | "Line" snap in the playlist resolves to steps, beats or bars by zoom (coarser than the piano roll). | Arranging is done in beats and bars. |
| D-TRACKGROUP | Track groups share mute and solo with their top track and fold away under it (a triangle beside its name). A folded group's tracks are hidden, not merged into a summary lane of their clips. | Folding arrived in the polish milestone; a summary lane was not attempted. |
| D-CLIPNAME | A pattern clip shows its pattern's name and colour; renaming or recolouring from the clip menu changes the pattern. | Same as FL for pattern clips. Listed for clarity. |
| D-SENDLAW | Send level is squared (50% on the knob is a quarter of the gain, 100% is unity). | A usable taper; FL's send law was not measured. |
| D-SIDECHAIN | A track has one sidechain input: the sum of every route marked "sidechain to this track". FL lets each plugin pick among several. | One input covers the ducking recipe; simpler. |
| D-CURRENT | The "current" track listens to the selected mixer track (after its fader). | FL's follows the selected channel's track; same use (meters, analysers). |
| D-TRACKEQ | The strip EQ is low shelf, peak, high shelf with frequency, gain and width each. | Same shape as FL's strip EQ. Listed for clarity. |
| D-LIMITER-LATENCY | Lid always looks ahead 5 ms, so the default project has 239 samples of compensated latency. | A true brickwall needs look-ahead; compensation keeps everything aligned. |
| D-EFFECT-NAMES | Effects are Contour EQ, Squeeze, Lid, Chamber, Repeater, Shimmer, Grit, Sweep, Stereo Kit. | Original names, as required. |
| D-MIXDRYWET | Time-based effects use one dry/wet knob where the middle is both at full level. | Works as an insert and as a 100% wet send. |
| D-NEWKEY | Ctrl+N is "save new version", as in FL; "New" is in the File menu without a key. | Matches FL. Listed for clarity. |
| D-UNDO | Undo works on whole-project snapshots (up to 300 steps or 96 MB). Knob moves made without releasing through a control's own gesture are not steps. | Simple and cannot corrupt the project; cost is that sounding voices are cut on undo. |
| D-PACK | Packed projects are `.pulpz` ZIP files with stored (uncompressed) entries. FL uses zipped `.zip` loop packages. | Any unzip tool opens them. |
| D-HISTORY | The history list lives in the Browser (Current project > History) and the Edit menu. | Same place as FL's. |
| D-RECSONG | Recording audio switches to song mode. FL can also record into the playlist from pattern mode. | A take is a playlist clip; one rule is easier to trust. |
| D-RECARM | Choosing an input on a mixer track arms it; there is no separate arm button. The record button's right-click menu holds loop takes, punch, wait for input and overdub / replace, not FL's list of what to record. | FL arms by selecting an input too; notes and audio are always both recorded. |
| D-AUDIOCLIP | An audio clip carries its own gain, fades, reverse and mixer track; it does not sit on a Channel Rack channel. | FL puts audio clips on sampler-like channels; fewer moving parts here. |
| D-INPUTDELAY | The hardware input reaches the engine 64 samples late (one internal block); recording corrects for it and monitoring is that much later. | The fixed-block engine needs a whole block of input before it can run. |
| D-COUNTIN | The count-in button cycles none, one bar, two bars and applies to plain playback as well as recording. | Simple; FL's count-in applies to recording only. |
| D-STRETCH | Stretching and pitch shifting are worked out ahead of time, when a clip's setting or the tempo changes, and the result is played like any sample. FL stretches while it plays. | Playback and export stay the same audio. The cost: a tempo change re-renders the clips that follow it, and a tempo that is automated will not re-fit a clip continuously. |
| D-STRETCHMODES | Three algorithms: resample, rhythmic (overlap-add with the drum hits carried across untouched), tonal (phase vocoder). FL offers more modes. | These three cover speed change, loops and sustained material; each is tested. |
| D-STREAM | Files longer than about 45 seconds play from disk; shorter ones, and any clip that is stretched or pitched, are held in memory. | A stretched copy has to exist somewhere; memory is the simple place. |
| D-TEMPOASK | Dropping audio on the playlist detects its tempo and asks once whether to stretch it. FL's sample properties offer the same from a menu. | "Detect tempo on import and offer to stretch" is what the build spec asks for. |
| D-TAKES | Loop-recorded takes are separate clips on consecutive playlist tracks, sharing a take group; the last complete one plays. Comping is slicing plus "Use this take here". | FL also stacks loop takes on tracks and comps by muting; the one-click "use this take" is added. |
| D-PUNCH | Punch in / out uses the playlist's loop region as its range. FL uses the time selection. | PULP's playlist has one range, the loop region. |
| D-WAIT | Wait for input ignores the count-in and uses a fixed threshold of -34 dBFS. | A count-in makes no sense when the performer starts the transport. |
| D-CALIBRATE | Calibration uses three bursts of noise rather than a click, and stores one figure per driver, device, sample rate and buffer size. | Noise bursts are found reliably even through a quiet or noisy path, and tests show the result is exact to the sample. |
| D-HWOUT | A track's extra hardware output is taken after its fader and is not delay-compensated against the master. | It leaves the mixer graph; aligning it would need a second compensation path. |
| D-REPLACE | Replace mode wipes the current channel's notes as the playhead passes them, checked twenty times a second. | Enough for the purpose; the wipe lags the playhead by at most 50 ms. |
| D-EDITOR | Splice edits one whole recording with one selection: no regions, markers, zoom, scripting or loop tools as FL's Edison has. Noise removal has one fixed strength. | The spec's list (trim, fade, normalize, reverse, noise removal, drag to playlist) is what is built and tested. |
| D-AUTOVALUE | Where no automation clip is playing, a control takes the end value of the clip that started last, and before the first clip the first clip's opening value. FL leaves it wherever it was. | A control's value then depends on the song position alone, so an export and a play from anywhere agree. |
| D-AUTOEDIT | In an automation clip a right-click adds a point (as in FL); the clip itself is deleted from its title strip, its menu or with the delete tool. Points do not snap to each other's values. | The curve area would otherwise delete the clip on every right-click. |
| D-AUTOLFO | An automation clip's LFO swings around the drawn curve (added to it). FL multiplies the two. | Simple to predict: the curve is the centre line. |
| D-EVENTS | Events are held values, or glides to the next event; the event editor draws steps on the snap grid or one straight line. FL's event editor has more drawing modes and an LFO tool. | The spec asks for event automation and an editor; this is the tested core. |
| D-RECAUTO | Recorded knob movements become pattern events in pattern mode and an automation clip in song mode, thinned to the points that matter. FL asks what to make of them. | One rule; the result can be edited either way. |
| D-CONTROLLERS | The four controllers are Follower, Wobble, Pad XY and Formula, original names for the envelope follower, LFO, XY pad and formula controller. A link reads its controller one 64-sample quantum late. | Names as required; the lag is what keeps every thread count producing the same audio. |
| D-LINK | One link per control. A link has range, smoothing and one formula in `x`. FL also offers a list of ready-made mapping curves. | The formula covers them (`1 - x`, `x * x`, ...). |
| D-MIDIROLES | Every MIDI input is opened as soon as it appears; the settings page only chooses its roles (notes, controllers). There is no per-device port number or controller-type list. | Nothing to configure before a keyboard works. |
| D-TIMBRE | The multisample instrument is one plugin, Timbre, with one low-pass filter, one LFO and one pitch envelope; its presets reshape whatever instrument is loaded. FL spreads this over DirectWave, the channel sampler and FLEX. | One engine does what the spec asks of the multisample engine; the preset-browser instrument with macro knobs is not built. |
| D-LIBRARY | The factory library is not in the repository: `build.bat library` downloads the recordings named in `factory\sources.json` (checksums verified) and builds the instruments. It is one velocity layer and one take per note. | Hundreds of megabytes of audio do not belong in git; the manifest makes the build repeatable. |
| D-SYNTHESIZED | Nine factory instruments are synthesized (808 bass, FM electric piano, choir, tonewheel organ, FM bells, three plucks, synth strings and brass) and say so in the Browser and in CREDITS.md. | No recording under a licence that allows redistribution was found, or the sound was a synthesizer to begin with. |
| D-VSTSCAN | Plugins are looked for when the user presses FIND PLUGINS, not at start-up, and only VST3 is hosted. FL scans VST2, VST3 and CLAP. | The spec asks for VST3; a scan on demand never surprises with licence windows at start-up. |
| D-VSTWINDOW | A hosted plugin's editor is a separate window of Windows' own, with a strip of PULP's above it (last touched control, PARAMETERS). FL wraps the editor in its own window with its own toolbar and keeps "last tweaked" in a menu. | A plugin's editor is a native window and cannot live inside PULP's own floating panels. |
| D-VSTPARAMS | Up to 256 parameters per plugin are exposed to automation, shown as 0 to 1. FL exposes them all with the plugin's own value text in its browser. | A fixed parameter list is what the internal plugin interface requires; the plugin's window shows real values. |
| D-VSTNOTES | Slide notes and per-note pan, mod X / Y and fine pitch are not sent to hosted instruments; channel pitch is sent as a pitch wheel of two semitones each way. | They have no MIDI equivalent that every plugin understands. |
| D-VSTSTATE | Where a hosted plugin's saved state and the project's own list of its parameter values disagree, the project's values win. | A value set in PULP just before saving may not have reached the plugin's state yet. |
| D-PADS | Sixteen's pads are the keys C5 to D#6, with up to four velocity layers each and one stereo output for the whole kit. FL's FPC has per-pad outputs to the mixer and General MIDI key numbers. | The plugin interface gives an instrument one stereo output; C5 upward lets the step sequencer play pad 1. |
| D-CHOP | Chop cuts at hits (or evenly), plays slices at their own speed and writes notes on the tick grid. FL's slicers can also stretch slices to the tempo and have per-slice settings. | The essential job (cut, map to keys, write the notes) first. |
| D-ROMPLER | There is no separate preset-browser instrument with macro knobs: Timbre has a LIBRARY menu of the factory instruments and presets that reshape any of them. FL has FLEX. | One sample engine does both jobs. |
| D-AUTOCHANNEL | There is no automation-only channel. FL's automation clips are channels. | In PULP any control gets an automation clip directly (D-AUTOEDIT). |
| D-MIDIOUT | MIDI Out sends notes, program, bank and pitch wheel to one of eight outputs, timed to about a millisecond. FL's MIDI Out also maps controllers and syncs clocks. | PULP's MIDI Out currently covers note playback and instrument selection. |
| D-NAMES | Two plugins were renamed after they were written: Zest (written as "Citrus") and Doorman (written as "Gatekeeper"). | The first sounded like an Image-Line product name, the second is another maker's product. |
| D-GAUGE | One analyser, Gauge, with six displays (spectrum, spectrogram, oscilloscope, vectorscope, loudness, tuner). FL spreads these over several plugins. Peaks are sample peaks, not true peaks. | One slot shows whichever view is wanted. |
| D-IMPULSES | Spaces' twelve built-in impulse responses are synthesized by PULP (rooms, halls, plate, spring), not recordings of real spaces; a recording of your own can be loaded. | No licence-clean set of recorded impulse responses was sourced. |
| D-MIDISONG | Importing a MIDI file adds a channel per part and one pattern holding all of it, with instruments picked from the factory library by General MIDI program; only the file's first tempo is used. | It gives a playable song at once; tempo maps would need tempo automation written from the file. |
| D-SF2 | A SoundFont preset is converted into a Timbre instrument (samples, ranges, tuning, loops, levels); the SoundFont's own envelopes, filters, modulators and layering are not reproduced. | Timbre is the one sample engine; it has its own envelope and filter. |
| D-STEMS | "Split mixer tracks" renders once per track with the other source tracks muted, so a stem includes that track's share of the buses and the master's effects. FL taps each track's own output. | It needs no change to the engine and the stems add up to the mix when the master is linear. |
| D-TEMPLATES | Templates and the demo song are built by code in the engine, not stored as project files. | They cannot go stale against the file format, and the demo is also a test. |
| D-SHORTCUTS | Only the 24 program-wide shortcuts can be changed; keys inside the Piano Roll, Playlist and Channel Rack are fixed. | The spec asks that all be remappable; the per-window keys would each need the same treatment and did not get it. |
| D-INSTALLER | The installer is a zip holding the program and a script that installs it for the current user. | The script performs a per-user install. |
| D-MP3TAG | MP3 files PULP writes name their encoder `LAMEPULP1` in the Info frame. | Other programs (ffmpeg among them) only take the encoder delay and padding from that frame when the name starts with LAME, Lavf or Lavc; without the prefix they would add about 1100 samples of silence. The cost is that the file half-claims to come from LAME: the plain name would cost that extra silence. |
| D-CODECS | MP3 export is constant bit rate MPEG-1 only (no VBR, nothing under 96 kbit/s); OGG export is variable bit rate only. The encoders' models were tuned by measurement against LAME and libvorbis, never by ear. | Both codecs were written for PULP so that the engine keeps no third-party code. |
| D-RECNOTESONG | Notes recorded in song mode go into the pattern whose clip is playing under the play position; where none is, PULP makes a new pattern and a clip for it. FL writes them into the selected pattern at the song position and leaves placing the pattern to you. | So that what was played is heard where it was played without a further step, and an existing pattern used elsewhere in the song is not changed by accident. |
| D-MIXMOVE | Mixer tracks are moved one place at a time with Alt+Left / Alt+Right or the track menu; FL also drags them with Shift+wheel. | The key is FL's own; no mouse gesture was built. |
| D-ADDITIVE | Sheaf is PULP's additive synth: 256 sine partials a voice, two designed spectra with a morph, and resynthesis of a recording of one note into a table of harmonic levels. FL's additive synth (Harmor) also resynthesises whole audio files with their noise and pitch movement, takes images, and has its own unison, phaser, pluck and effects sections. | A measured, alias-free core that plays a recorded note at any pitch was the part worth building first; the rest is not attempted. |
| D-MISSING | A plugin that cannot be created is replaced by a stand-in that keeps its state. FL shows an error and a placeholder too. | Opening a project must never lose what it cannot play. |
| D-SYNCPREVIEW | Tempo-synced preview stretches the whole file to the project tempo and plays it from its start; it does not wait for the next bar. | Simple; it answers "does this loop fit". |

## Known bugs

None recorded yet.
