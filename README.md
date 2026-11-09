# PULP

A native Windows music program in the FL Studio mould: Channel Rack with a step sequencer, Piano Roll,
Playlist, Mixer with effects, Browser, undo history, autosave and WAV export. It is a real program
(C++20, JUCE, its own `PULP.exe`, ASIO and Windows Audio), not a web page.

PULP is not affiliated with or endorsed by Image-Line; it contains no FL Studio code, names, graphics, presets
or samples. It is free software under the GNU Affero General Public License version 3 (see `LICENSE`).

This is an early, unfinished build: `PROGRESS.md` is the development log and says exactly what is built, what
is partial, what is missing and what has not been verified. In short: the planned feature set is in, except a
note-by-note pitch editor, a modular patcher and CLAP plugin hosting, plus the gaps PROGRESS.md lists. You
can program drums on pads or steps, slice loops, play 39 sampled instruments and ten synthesizers (or build an
instrument from your own recordings), write melodies, arrange a song, mix it with some forty-five effects,
automate any control, duck a bass under a kick, play and record from a MIDI keyboard, record vocals into the
playlist (with loop takes, comping and punch in / out), stretch loops to the tempo, edit audio, host VST3
plugins, import MIDI files and SoundFonts, save, undo, and export WAV, FLAC, OGG, MP3 and stems. There is a demo song,
starter templates and a first-run tour. **Nobody has listened to any of it yet: everything is verified by
measurement, not by ear.** The vocal effects were tried on sustained sung notes only.

## Run it

Double-click `dist\PULP\PULP.exe`. The `factory` folder beside it holds the drum kits.

To rebuild everything from source: run `build.bat` (it downloads portable CMake and Ninja into `tools\` on
first use, fetches JUCE 9.0.4, builds, runs the tests, builds the factory library if it is not there yet
(that step needs Python and the network, and takes a minute) and refills `dist\PULP`). You need the Visual
Studio 2022 C++ Build Tools (the free "Desktop development with C++" workload) and Python 3.

| Command | What it does |
|---------|--------------|
| `build.bat` | release build of everything, runs the tests, fills `dist\PULP` |
| `build.bat quick` | engine, renderer and tests only (a few seconds), runs the tests |
| `build.bat test` | the tests again in a Debug build with the checked heap |
| `build.bat app` | the program only |
| `build.bat installer` | packs `dist\PULP` and an install script into `dist\PULP-setup.zip` |
| `build.bat testplugins` | two small VST3 plugins used only by the hosting check (`scripts\m10_vst3.txt`) |
| `build.bat library` | downloads the recordings for the factory instruments (about 420 MB, once) and rebuilds the library |
| `dist\PULP\pulp_render.exe song.pulp out.wav` | render a project with no window and no sound card |

## Pick the audio device

Press **F10** (or the AUDIO button). Choose the driver, the output device, sample rate and buffer size, and
tick which outputs and inputs are active.

- On its first run PULP opens the ASIO driver that belongs to your Windows default output if there is one,
  otherwise Windows Audio on the default device.
- ASIO gives the lowest delay. "Windows Audio" shares the device with other programs.
- The toolbar shows CPU load, sounding voices and underruns. If underruns climb, pick a bigger buffer.

## Make something in a minute

1. **Drums.** In the Channel Rack (F6), left-click steps to set them, right-click to clear. Space plays.
2. **More sounds.** Drag a sound from the Browser onto the rack, or use the `+` button. Double-click a
   sound to add it; click once to hear it.
3. **A melody.** `+` > Trio adds a synth. Right-click its channel button > "Send to piano roll" (or F7).
   Click to place notes, drag to move, drag the right edge to resize, right-click to delete.
4. **A second pattern.** F4 gives the next empty pattern; numpad + and - step between them.
5. **Arrange.** In the Playlist (F5), pick a pattern on the left and click on a track to place it.
   Press **L** to switch from pattern mode to song mode so Space plays the arrangement.
6. **Mix.** Select channels and press Ctrl+L to give them mixer tracks. In the Mixer (F9) click a slot to
   add an effect; select a track, then click another track's arrow to send to it.
7. **Save and export.** Ctrl+S saves, Ctrl+N saves a new version, Ctrl+R exports a WAV.

The hint bar under the buttons at the top left describes whatever the mouse is over.

## Shortcuts

The program-wide keys below (transport, windows, patterns, files, undo) can be changed in Settings (F10) >
SHORTCUTS; the keys inside the Piano Roll, Playlist and Channel Rack are fixed.
`docs\SHORTCUTS.md` compares every shortcut in FL Studio's manual with PULP, line by line.

These follow FL Studio's defaults.

| Keys | Action |
|------|--------|
| Space | play / stop |
| Ctrl+Space | pause |
| L | pattern mode / song mode |
| R | record notes (while playing) |
| Ctrl+M | metronome |
| Ctrl+T | typing keyboard (Z S X D C ... plays notes) |
| F5, F6, F7, F9 | Playlist, Channel Rack, Piano Roll, Mixer |
| F8 | Plugin picker: the list of instruments to add |
| Alt+F8 | Browser |
| F10 | audio and MIDI settings |
| F4 | next empty pattern |
| numpad + / - | next / previous pattern |
| Ctrl+S, Shift+Ctrl+S | save, save as |
| Ctrl+N | save new version |
| Ctrl+O | open |
| Ctrl+R | export WAV |
| Ctrl+Z | undo / redo the last step |
| Ctrl+Alt+Z, Shift+Ctrl+Z | step back, step forward through the history |

Channel Rack: Up / Down select a channel, Alt+Up / Alt+Down move it, Alt+C clone, Alt+Delete delete,
Shift+Ctrl+Left / Right rotate the steps, Ctrl+L route the selected channels to free mixer tracks.

Piano Roll: P draw, B paint, D delete, T mute, C slice, E select, Z zoom, Y play from here; Ctrl+A select all,
Ctrl+D deselect, Ctrl+C / X / V, Ctrl+B duplicate, Delete; Shift+Up / Down a semitone, Ctrl+Up / Down an octave,
Shift+Left / Right nudge; Ctrl+Q quick quantize, Alt+Q quantize, Ctrl+L legato, Alt+L articulate, Alt+U chop,
Ctrl+G glue, Alt+A arpeggiate, Alt+S strum, Alt+F flam, Alt+R randomize, Alt+X levels, Alt+Y flip, Alt+O LFO,
Alt+W trimmer, Alt+E riff generator, Ctrl+M import MIDI, Shift+Ctrl+M export MIDI, Ctrl+0 zoom to fit.
(Tool letters are off while the typing keyboard is on.)

Playlist: P draw, B paint, D delete, T mute, S slip, C slice, E select, Z zoom, Y play from here; Ctrl+A, Ctrl+B
duplicate, Ctrl+C / X / V, Delete, Shift+arrows nudge, Shift+G group, Alt+G ungroup, Alt+T add a time marker.
Tracks: right-click a track's name to rename, colour, size, lock or group it with the track above; the top
track of a group has a triangle beside its name that folds the group away (the hidden tracks still play).

Mixer: Left / Right select a track, Alt+Left / Alt+Right move it one place (with its effects, routing and
automation), S solo, F2 rename, Ctrl+L send the Channel Rack's selected channels to it.

Mouse, everywhere: right-click deletes (notes, clips, steps), the wheel scrolls, Ctrl+wheel zooms, Shift+wheel
scrolls sideways, middle-drag pans, Ctrl+right-drag zooms. Knobs move on a vertical drag, Ctrl for fine
control, Alt-click or a double right-click resets, right-click opens the knob menu. Alt bypasses snap.

## Recording a vocal

1. Press **F10** and check that the input device is your audio interface or microphone and that its
   inputs are ticked under Active inputs.
2. Open the Mixer (**F9**) and click an empty insert track, for example Insert 1.
3. In the panel on the right, click **IN: none** and choose the input your microphone is plugged into.
   Most interfaces present each microphone as one channel of a multi-channel device, so pick
   **Input 1 (mono)** or **Input 2 (mono)** rather than recording a single mic as stereo (which would put
   you in one ear only). **Inputs 1+2** records both as stereo. The small meter beside the button shows the
   input level.
4. If your interface lets you hear yourself directly (most have a "direct monitor" switch or mix), leave
   **MONITOR off**. Switch it on only if you want to hear yourself through the track's effects, and then
   only on headphones.
5. On the toolbar, click **IN 0** to choose a count-in of one or two bars, and click the record button
   (or press **R**) so it is lit.
6. Press **Space**. PULP switches to song mode, counts in, and records while the song plays.
7. Press **Space** again to stop. The take appears in the Playlist on a track named "Recording", as an
   audio clip that plays through the mixer track you recorded on, with that track's effects live.
8. Move, trim, slice or mute the clip like any other. The arrow in its corner opens gain, normalize,
   fades, reverse, stretch and pitch, and mixer track.

Where the audio goes: a `recordings` folder beside the project file, written to disk as you sing, so a
crash does not lose the take. Save the project first; until it has been saved, takes go to
`%APPDATA%\PULP\recordings`.

### If the meter does not move when you sing

If the input meter shows a low, steady level while you sing, check the following settings and connections.

- Windows: Settings > Privacy & security > Microphone, "Let desktop apps access your microphone" is on;
- the interface's **mute button** for that mic, if it has one;
- the **gain** knob for that input;
- the cable, and **48V** phantom power if the microphone is a condenser.

Hover over the small input meter in the Mixer panel to see the level in dB. It should rise when you sing.

### Timing, and measuring the real latency

The take is shifted so it sits where you sang it. Out of the box PULP uses the input and output latency the
driver reports plus its own 64 samples. Drivers do not always tell the truth, so you can measure it:

1. Connect a cable from one of your interface's outputs back into an input (or hold the microphone against
   a headphone).
2. Press **F10** and click **Measure latency**. PULP plays three quiet clicks (about a second) and listens.
3. The window then says "Recording offset: N samples, measured on this device". It is remembered for that
   driver, device, sample rate and buffer size. **Forget** goes back to the driver's figures.

Tests prove the placement arithmetic and the measurement to the exact sample; a real interface's round trip
has not been measured with a cable yet.

### Takes, punching in, and starting hands-free

Right-click the record button for these:

- **Loop recording.** Ctrl-drag in the Playlist's ruler to mark a loop region, then record. The region
  loops and every pass is kept as a take on its own track ("Take 1", "Take 2" ...). The last complete take
  plays; the others are muted. To hear another, open its clip menu (the arrow in its corner) and choose
  **Use this take here**. To build one good take from several: cut a take with the slice tool (C) where the
  good part starts and ends, then choose "Use this take here" on that piece. The other takes are cut at the
  same places and silenced there.
- **Punch in / out.** Mark the lines you want to redo as the loop region, switch this on, put the song
  position a bar or two earlier (click the ruler) and record. You hear the song from where you started;
  only what you sing inside the region is kept. With loop recording off the song plays straight through.
- **Wait for input.** Press play with Record lit: the play button turns amber and nothing moves until you
  sing, play a key or hit a pad. The take starts with that sound.
- **Overdub or replace** for notes: overdub adds to the notes already in the pattern; replace wipes the
  notes the playhead passes while you record (pattern mode only).
- **Notes in song mode.** With Record lit and the song playing, what you play goes into the pattern under
  the play position; where there is none, PULP makes a new pattern and puts its clip on a free track,
  starting on the bar line and growing as you play on.

Right-click the metronome button to choose its sound.

## Fitting loops to the tempo, and changing pitch

Drop a loop on the Playlist. PULP listens for its tempo and, if it is not the project's, asks whether to
stretch it. You can change your mind any time from the clip menu > **Stretch and pitch**:

- **Follow the project tempo**: the clip stays in time whatever the tempo is (change the tempo and it is
  re-fitted a moment later).
- Three ways of stretching: **Resample** (like a record deck: faster is higher), **rhythmic** (for drums and
  loops: every hit is kept sharp), **tonal** (for vocals, chords and pads).
- **Length** as a percentage and **Pitch** in semitones, independent of each other in the two stretch modes.
- **Tempo of this audio** if PULP guessed wrong, or **Detect** to listen again.

In the Browser, the **SYNC** switch plays previews of loops at the project tempo.

Files longer than about 45 seconds are played from disk rather than loaded, so a long vocal or a whole
backing track costs almost no memory.

## Automation: making controls move by themselves

**Right-click any knob, fader or switch** (anywhere, including inside instruments and effects) for its menu:

- **Create automation clip** puts a curve for that control on the Playlist. Drag its points; right-click the
  curve to add a point; right-click a point to delete it or to choose the shape of the stretch after it
  (curve, double curve, hold, stairs, pulse, wave). The little ring between two points bends the stretch.
  The clip's own menu (the arrow in its corner) has LFO mode and the range. It plays in song mode (press L).
- **Edit events** opens the event editor: draw the control's movement inside the current pattern. Drag for
  steps, Shift-drag for a straight glide, right-drag to erase. Events loop with the pattern.
- **Link to controller** ties the control to a knob on your MIDI hardware (choose "learn", then move the
  knob) or to a controller plugin. FROM and TO set how far the control travels.
- **Record it instead:** light Record, press play and move the control. In pattern mode the movement is
  written into the pattern; in song mode it becomes an automation clip.

Sidechain ducking, the usual way: put **Follower** (mixer slot > Controllers) on the kick's mixer track and
pick its preset "Ducking". Right-click the bass track's fader > Link to controller > choose the Follower.
The bass now dips on every kick. The other controllers are **Wobble** (an LFO), **Pad XY** (two values you
drag by hand) and **Formula** (type an expression such as `0.5 + 0.5 * sin(2 * pi * beat / 4)`).

The tempo can be automated: right-click the tempo box > Create automation clip.

## MIDI keyboards and controllers

Plug one in: PULP finds it, and its keys play the selected channel. With Record lit and the pattern playing,
what you play is written into the pattern. **F10 > MIDI** lists the devices; each can play notes, act as a
controller, or both. (No MIDI device was available when this was built, so this has only been tested with
simulated messages. If a keyboard does nothing, that page shows the last message received.)

## Editing audio (Splice)

In the Mixer, click an empty slot > Tools > **Splice**. It lets the track's sound through untouched and can
record it: switch **Record** on, and choose whether it starts now, when the song plays, or when sound
arrives. Put it on the track you record your voice on, or on the master to capture the whole mix.

The recording appears as a waveform. Drag to select a part (double-click selects everything), then:
TRIM, CUT, MUTE, FADE IN, FADE OUT, NORM (normalize), REV (reverse). To remove hiss or hum, select a moment
that holds only the noise and press LEARN, then select what to clean (or nothing, for all of it) and press
DENOISE. UNDO steps back. LOAD brings a WAV file in; SAVE writes one. **TO PLAYLIST** puts the selection on
the playlist as an audio clip, or drag from the grey bar under the waveform onto a playlist track.

## Instruments from recordings

The Browser's Factory library > **Instruments** holds 39 instruments: piano, electric pianos, organs, guitars,
basses, strings, brass, woodwinds, a voice, harp, plucked instruments, mallets, bells. Click one to hear a
note, double-click it (or drag it to the Channel Rack) to play it. Each is a handful of single recorded notes
that PULP trimmed, tuned, looped and spread across the keyboard; nine are synthesized and say so. The
instrument that plays them is **Timbre**; its presets (Pad from anything, Plucked, Dark, Swell ...) reshape
whichever instrument is loaded, and its channel settings show which recording plays which keys.

**Your own instrument, from your own sound:** add a Timbre channel (`+` > Timbre), open its settings, and drop
one or more recordings of single notes onto the window (or click BUILD FROM SAMPLES). One note is enough: sing
one "aah" into the microphone, drop the take on a Timbre, and play it as a choir pad. PULP listens to each
file, works out its pitch (it does not need to be named), cuts the silence, loops it if it is a held sound,
and puts every note where it belongs. Several notes a few keys apart make a better instrument than one.
SFZ instruments (`.sfz`) can be opened the same way as the factory ones.

## Adding your own samples

- Drag a `.wav` file from Windows Explorer onto the Channel Rack to make a sampler channel, or onto a
  sampler channel's button (or the waveform in its settings) to replace its sample.
- In the Browser choose My folders > "Add a folder..." to keep a folder of sounds in the tree. Right-click a
  sound to add it to Favourites.
- In the channel settings you can trim the sample with the two markers, reverse it, normalize it, switch
  on an envelope and a filter, and set its root key.
- WAV (8, 16, 24, 32-bit and float), FLAC, AIFF, OGG and MP3 are read.
- Drag a `.wav` onto a Playlist track (from the Browser or from Explorer) to place it as an audio clip.

## Instruments, effects and what came with milestones 11 to 14

**Starting points.** File > **Open the demo song** loads a finished 20-bar song to play and take apart.
File > **New from template** starts a project with a drum kit, bass and chords already set up for a style
(trap, house, lo-fi, synthwave). File > **Take the tour** walks through the windows in a minute; it also
runs by itself the first time PULP starts.

**Instruments** (Channel Rack `+`, or F8):

| Instrument | What it is |
|---|---|
| Sampler | one sample, played across the keys |
| Timbre | instruments made of recorded notes: the factory library (LIBRARY in its settings), your own recordings, SFZ and SoundFont presets |
| Sixteen | sixteen drum pads. Comes with the 808 kit; LOAD A FOLDER OF DRUMS fills the pads from a folder; drop a sound on a pad to replace it. Pad 1 is the key C5, so the step squares play it; use the piano roll for the rest |
| Chop | cuts a drum loop into its hits. Drop a loop on its settings, then WRITE NOTES puts the slices into the piano roll for rearranging |
| Trio | simple three-oscillator synth |
| Zest | the big subtractive synth (37 presets) |
| Sextet | six-operator FM synth (33 presets) |
| Ripple | wavetable synth (31 presets); LOAD WAVETABLE takes a table of your own |
| Squelch | 303-style acid bass with slide and accent (22 presets) |
| Twine | plucked strings (24 presets) |
| Low Four | a simple bass (23 presets) |
| Thud | kick drum synthesizer (23 presets) |
| Clatter | drum synthesizer: eleven drum models, or a whole kit across the keys (25 presets) |
| Motes | granular sampler: drop a recording on its settings and play it as a cloud of grains (24 presets) |
| Sheaf | additive synthesizer: sounds built from up to 256 sine waves, two spectra with a morph; drop a recording of one note on its settings and it plays that sound at any pitch (32 presets) |
| Layer | plays several channels from one |
| MIDI Out | sends its notes to a MIDI output for external hardware |

Every instrument has a **Presets** button at the top of its settings (click for the list, right-click for the
next one).

**Effects** (Mixer: click an empty slot on the right): Contour EQ, Sweep (filter), Squeeze (compressor with
sidechain), Lid (limiter, with a compressor stage you can switch on), Triband (multiband compressor and maximizer), Snap (transient shaper), Curb (soft
clipper), Glow (one-knob loudness), Sibilant (de-esser), Doorman (gate), Chamber (reverb), Spaces
(convolution reverb), Repeater (delay), Taps (multitap delay), Shimmer (chorus), Jet (flanger), Swirl
(phaser), Flutter (tremolo / auto-pan), Waver (vibrato), Rotor (rotary speaker), Grit (distortion), Stereo
Kit, Splice (audio editor), Gauge (analyser: spectrum, spectrogram, oscilloscope, vectorscope, loudness in
LUFS, tuner), Ten Band (graphic EQ), Motion Filter, Quarter Inch (tape), Glass (tube), Stack (amp and cabinet), Splat (fuzz), Coarse (bit crusher), Bend (waveshaper you draw), Middle and Sides, the vocal set: True Note (pitch correction), Talker (vocoder), Second Voice (harmonizer), Lint (noise reduction that learns), the creative set: Rewind (time and volume curves you draw), Hiccup (glitch), Thumb Pad (effects on an XY pad), Platter (scratching), the controllers Follower, Wobble, Pad XY and Formula, and any VST3 effect.

**Files from other programs.** File > **Import a MIDI file** turns a .mid into channels with instruments and
a pattern in the playlist (tempo changes in the file become a tempo automation clip). Channel Rack `+` > **SoundFont preset** opens a preset of an .sf2 file as a Timbre
instrument. Audio files that can be used anywhere: WAV, FLAC, AIFF, OGG, MP3.

**Export** (Ctrl+R): WAV, FLAC, OGG, MP3; "Split mixer tracks" writes a file per mixer track (stems).
File > Export a MIDI file writes the notes.

**Shortcuts** can be changed in Settings (F10) > SHORTCUTS: click a command, press the new key.

**Installing on another PC:** `build.bat installer` makes `dist\PULP-setup.zip`; unpack it there and run
`Install PULP.cmd` (no administrator rights needed).

## VST3 plugins

PULP hosts VST3 instruments and effects from other makers.

1. **Find them, once:** Settings (F10) > PLUGINS > **FIND PLUGINS**. PULP looks in
   `C:\Program Files\Common Files\VST3` (ADD FOLDER... adds another place). Each plugin is examined by a
   separate process, so one that crashes cannot take PULP down; it is shown in red and left alone. A scan of
   a typical plugin folder takes a few seconds. Press it again after installing a new plugin.
2. **An instrument:** Channel Rack `+` > VST3 instruments, or the Browser > Plugin database > VST3
   instruments (double-click, or drag to the rack). It is a channel like any other: steps, piano roll,
   mixer track.
3. **An effect:** in the Mixer, click an empty effect slot > VST3 plugins.
4. **Its window:** the plugin's own editor opens in a window of its own. Click the channel's name or the
   effect slot to bring it back. Space and the other PULP shortcuts still work while it has the focus.
5. **Automating it:** move a control in the plugin's window; the strip at the top of the window then names
   it. Click the strip > Create automation clip (or Link to controller). PARAMETERS shows the plugin's
   controls as PULP knobs, which have the same right-click menu. With record on and the song playing, a
   control you move in the plugin's window is recorded.
6. **Saving:** the plugin's settings are saved inside the project. If a project is opened on a PC without
   the plugin, its place is kept ("Missing: name"), nothing is lost, and it comes back when the plugin does.

Limits: VST3 only (no VST2, no CLAP); the first 256 parameters of a plugin can be automated; one stereo
output per plugin. PROGRESS.md has the full list.

## Your files

- Projects are `.pulp` files. File > "Pack project and audio into one file" makes a `.pulpz` you can move
  or back up; opening it unpacks it beside the file.
- While there are unsaved changes PULP autosaves every two minutes into `%APPDATA%\PULP\autosave`. If it ever
  closes badly, the next start offers the newest backup; File > "Recover an autosaved backup" lists them.
- `%APPDATA%\PULP\settings.xml` holds the audio device, browser folders and recent files;
  `%APPDATA%\PULP\plugins.xml` holds the list of VST3 plugins that were found.

## Where things are

| Path | What |
|------|------|
| `engine\` | everything that makes sound: plain C++20, no JUCE, no window, no sound card |
| `app\` | the program: windows, drawing, input, devices |
| `render\` | the command-line renderer |
| `tests\` | renders audio and asserts on it |
| `factory\` | the factory library and its manifests: `sources.json` (where every recording comes from), `manifest.json` (every file) |
| `factory-cache\` | the downloaded recordings (not in git; filled by `tools\fetch-samples.py`) |
| `docs\` | the project file format, the plugin interface, scripting |
| `scripts\`, `out\` | the scripts used to check each milestone and what they produced |
| `PROGRESS.md` | the development log: what is done, partial, missing, unverified, and every deviation from FL Studio |
| `CREDITS.md` | licences for third-party code and every factory sound |
| `LICENSE` | GNU Affero General Public License version 3 |
