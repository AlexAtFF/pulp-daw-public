# Driving PULP without clicking

`PULP.exe --script file.txt --log out.log [--no-audio] [--data-dir folder] [--size 1500x860]`

- `--no-audio` runs the engine from a silent background thread: nothing is played. Use it for every
  automated check that does not specifically test the sound card.
- A scripted run never writes the user's settings, recent files or autosaves. Give it `--data-dir <folder>` to
  let it keep those in a sandbox (needed to test autosave and crash recovery).
- Paths in a script are relative to the script file. One command per line; `#` starts a comment.
- `tools/loopback_capture.py` records what reaches the Windows default output device and compares it with a
  render, for the few tests that do play sound. Keep those quiet (`set master 0.2`) and short.

## Commands

| Command | Does |
|---------|------|
| `wait <ms>` | pause |
| `quit` | close the program |
| `key <keys>` / `keyto <target> <keys>` | a key press (`ctrl + S`, `F6`, `space`, `shift + cursor up`), to the focused window or to a named one |
| `click <target> x y [right] [ctrl] [shift] [alt]`, `dblclick ...` | a synthetic mouse click at a point inside the target |
| `drag <target> x1 y1 x2 y2 [right] [ctrl] [shift] [alt]` | press, move in ten steps, release |
| `shot <target> <file.png> [scale]` | screenshot of a target |
| `hint <target> x y` | log the hint-bar text of whatever is at that point |
| `show <window>`, `hide <window>`, `move <window> x y w h`, `detach <window>`, `dock <window>`, `windows` | window management |
| `state`, `steps`, `piano`, `playlist`, `mixer`, `history`, `backups`, `devices` | log a one-line description of that part of the program |
| `play`, `stop`, `set tempo|master|song|pattern|channel <value>` | transport and selection |
| `param <channel> <id> <value>` | set a channel-settings or instrument parameter |
| `preset <channel> <index>`, `addchannel <type>`, `drop <file> [x y]`, `preview <file>` | channels and samples |
| `note on <key> [velocity]`, `note off <key>`, `record on|off` | live notes (as from a keyboard) |
| `tool <n>` (-2 accepts the open tool panel, -1 cancels it), `toolset <setting> <value>` (a setting of the open tool panel), `ptool <n>`, `pianoset stamp|scaleroot|scale|wave <n>`, `midi import|export <file>` | piano roll: tools menu entry, mouse tool, settings, MIDI files |
| `pltool <n>` | playlist mouse tool |
| `pltrack <track> group\|ungroup\|collapse\|expand` | groups a playlist track with the one above; hides or shows a group's tracks (tracks count from 0) |
| `pltracks [n]` | logs where the first n playlist tracks are drawn: `track@top+height`, `g` grouped, `c` collapsed, `h` hidden |
| `mixsel <track>`, `fx <track> <slot> <type|none>`, `openfx <track> <slot>`, `fxparam <track> <slot> <id> <value>`, `mixparam <track> <id> <value>`, `route <from> <to> [sc|off]`, `mixmove <track> left\|right` (the track changes places with its neighbour), `fxmove <track> <slot> <to slot>` (two slots change places) | mixer |
| `save <file>`, `open <file>`, `new`, `saveversion`, `pack <file>` | project files |
| `undo`, `undostep`, `redo` | undo history |
| `autosave`, `recover`, `crash` | backups; `crash` kills the program on purpose |
| `export <file> [song|pattern] [rate] [bits] [leave|cut|wrap]`, `waitexport` | WAV export |
| `device <driver> <name> [rate] [buffer]` | open an audio device |
| `audio`, `testinput on|off`, `testloop <frames>`, `countin <bars>`, `mixinput <track> <0..3> [monitor]` | recording: log the input level, takes and clips; feed a test tone into input 1; loop the output back into input 1 through a "cable" of that many frames (silent mode only, at least 480) |
| `recopt punch|loop|wait|replace on|off`, `metrosound <0..3>`, `loopregion <start> <end>`, `seek <tick>` | record options, metronome sound, the playlist's loop region, song position |
| `calibrate`, `waitbusy` | measure the round trip (logs `calibration frames=...`); `waitbusy` holds the script until it has answered |
| `addaudio <file> [track] [tick]`, `clipstretch <clip> <mode> <sync 0|1> <length multiplier> <cents>`, `audiotempo <audio id> <bpm|detect>` | audio clips: place a file, stretch settings, the audio's tempo |
| `splitclip <clip> <tick>`, `usetake <clip>` | cut a clip; make a take the one that plays |
| `vstscan [all] [file ...]` then `waitbusy`, `vstlist` | scan VST3 files (no files: the standard folders; `all`: again, even the known and failed ones); list what is known |
| `vstchan <name>`, `vstfx <track> <slot> <name>` | a hosted instrument as a channel, a hosted effect in a mixer slot, by part of the plugin's name |
| `vstinfo <track> <slot>` or `vstinfo ch <channel>`, `vsttouch <track or ch> <slot or channel> <parameter index> <value>` | what a hosted plugin reports; make it report a control dragged in its own window |
| `openchan <channel>`, `vstwin`, `vstshot <file> [window]`, `vstclose` | open a channel (a hosted instrument's own window); list, picture (rendered by Windows) and close plugin windows |
| `fxparam <track> <slot> #<n> <value>` | the nth parameter of an effect, for plugins whose parameter names are numbers |
| `new [n or demo]` | a new project; from template number n; the demo song |
| `padload <pad> <file> [layer]`, `padkit <folder>`, `padinfo` | Sixteen (the current channel): a sound on a pad, a kit from a folder, what is on the pads |
| `chop <file>`, `chop detect`, `chop write`, `chop info` | a new Chop channel slicing a loop; slice again; write its notes into the pattern; list the slices |
| `midisong import <file>`, `midisong export <file>` | a whole MIDI file in (channels, a pattern, a clip) and out |
| `sf2 <file>`, `sf2 <file> <n>` | list a SoundFont's presets; preset n as a Timbre channel |
| `export <file> [song or pattern] [rate] [bits] [leave, cut or wrap] [stems]` | the file's ending picks the format (wav, flac, mp3, ogg); `stems` writes a file per mixer track |
| `curve info`, `curve add <x> <y>`, `curve drag <x> <y> <to x> <to y>` | the open waveshaper's curve, in -1..1 (the mouse is moved for real) |
| `paintbench <target> [times]` | how long a window takes to draw itself |
| `fxpreset <track> <slot> <n>` | an effect's factory preset |
| `rewind info`, `rewind add time or volume <place> <height>` | the open Rewind's curves (0..1 across and up) |
| `browse instrument or effect <type> [preset]`, `browseopen <item> [<item> ...]` | as if double-clicked in the Browser's plugin database; open part of the Browser's tree |
| `timesig <beats> <unit>` | the time signature |
| `gaugeinfo` | the readings of the analysers that are open |
| `midiout` | what MIDI Out channels have sent |
| `shortcut list`, `shortcut reset`, `shortcut <command> <key or none>` | the program-wide shortcuts |
| `tour start`, `tour next`, `tour back`, `tour close` | the first-run tour |
| `buildinst <file> [file ...]`, `loadinst <file.pulpinst or .sfz>` | a multisample instrument built from recordings of single notes (logs what was heard in each); a channel playing an instrument file |
| `menuitem <address> <0|1|2|3>` | what the control's right-click menu does: 0 create automation clip, 1 link to controller, 2 edit events, 3 remove the link |
| `autoclip <address> [track] [start] [length]`, `autopoints <curve> tick,value[,shape[,tension]] ...`, `autolfo <curve> <on> <shape> <steps> <depth>` | automation clips |
| `events <address> tick,value[,ramp] ...`, `showevents <address>`, `eventinfo` | event automation in the current pattern, the event editor |
| `link <address> ctl <track> <slot> <output> [min max smooth formula]`, `link <address> midi <channel|-1> <cc> [min max]`, `unlink <address>`, `learn <address>`, `showlink <address>` | controller links |
| `midiin cc <channel> <controller> <value>`, `midiin on <key> <velocity>`, `midiin off <key>`, `midiin bend <-1..1>`, `mididevices` | as if a MIDI device had sent it; list the devices |
| `knobdown <address>`, `knobset <address> <0..1>`, `knobup <address>` | a control being grabbed, moved and let go by hand (for recording movements) |
| `paramval <address>`, `autos`, `settingspage <0|1>` | log a control's value; log curves, links and events; show the audio or MIDI settings page |
| `ed sel <from s> <to s>`, `ed op <trim|cut|silence|fadein|fadeout|normalize|reverse|learn|denoise|undo|new|all|play>`, `ed load <file>`, `ed save <file>`, `ed toplaylist`, `ed info` | the newest open Splice editor (open one with `fx <track> <slot> editor` and `openfx`; start and stop recording with `fxparam <track> <slot> rec 1|0`) |
| `dump <file>` | write the project JSON |
| `log <text>` | write a line to the log |

Targets: `main`, `toolbar`, `browser`, a window's content (`rack`, `piano`, `playlist`, `mixer`, `chan`,
`settings`, `export`, `events`, `link`), the window frame itself (`rackwin`, `pianowin`, ...), or `fx` for the newest effect editor.

The scripts used to check each milestone are in `scripts\`; their logs and screenshots land in `out\`.
