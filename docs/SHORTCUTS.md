# Shortcut audit against FL Studio's manual

Checked on 2026-10-16 against the manual's "Keyboard and mouse shortcuts" page
(image-line.com, FL Studio online manual, `basics_shortcuts.htm`), read through a text extraction of the page:
a line that the extraction dropped would not show here. Every line of that list is below with what PULP does.

- **same**: the key does the same thing in PULP.
- **added**: the same, and it was added during this audit (milestone 14).
- **no**: PULP has the feature but not on a key, or has no such feature; the note says which.

The program-wide keys (the sections File, Pattern selector, Transport, Window navigation and Global) can be
changed in Settings > SHORTCUTS. The keys that belong to one window are fixed.

## File

| Key | FL Studio | PULP |
|---|---|---|
| Alt+0..9 | open recent file | no: File > Open recent |
| Ctrl+N | save new version | same |
| Ctrl+O | open | same |
| Ctrl+R | export WAV | same (opens the export window) |
| Ctrl+S | save | same |
| Ctrl+Shift+H | arrange windows | added (puts every window back where it first was) |
| Ctrl+Shift+M | export MIDI file | added |
| Ctrl+Shift+R | export MP3 | added: opens the export window, where the format is chosen |
| Ctrl+Shift+S | save as | same |

## Pattern selector

| Key | FL Studio | PULP |
|---|---|---|
| numpad 1..9 | select pattern 1 to 9 | added |
| numpad + / - | next / previous pattern | same |
| Ctrl+F4 | next empty pattern | added |
| F2 | rename the pattern (again: random colour) | added (rename only) |
| F4 | next empty pattern, with a naming dialog | same, without the dialog (F2 names it) |

## Channel Rack and step sequencer

| Key | FL Studio | PULP |
|---|---|---|
| 1..9, 0 | mute / unmute the first ten channels | added |
| Ctrl+1..9, 0 | solo / unsolo the first ten channels | added |
| Up / Down | select the channel above / below | same |
| Shift+Up / Down | grow the channel selection | added |
| Page Up / Down | next / previous channel group | no: the group button's menu |
| Alt+C | clone | same |
| Alt+Del | delete | same |
| Alt+Up / Down | move the channel | same |
| Alt+G | group selected channels | no: channel menu > Set group |
| Alt+M | toggle the mixer track selectors | no: the selectors are always shown |
| Alt+U, Alt+Z | unzip / zip channels | no such feature |
| Ctrl+Alt+R | restretch all audio clips | no: clips re-fit by themselves when the tempo changes |
| Ctrl+C / X / V | copy / cut / paste the channel's steps | added |
| Ctrl+L | route selected channels to free mixer tracks | same |
| Ctrl+Enter | focus the Playlist | added |
| Shift+Ctrl+Left / Right | shift the steps | same |
| Shift+wheel | move the channel up / down; on the title bar, change pattern | no |

## Record, playback, transport

| Key | FL Studio | PULP |
|---|---|---|
| Space | play / stop | same |
| Ctrl+Space | play / pause | same |
| L | pattern / song mode | same |
| R | record on / off | same |
| numpad * and / | next / previous bar (song mode) | added |
| numpad 0 | fast forward | added, as a jump of four bars |
| Backspace | toggle line / none snap | no: the snap selector of each window |
| Ctrl+B | blend notes (overdub) | added: switches between blending and replacing (in the Piano Roll and Playlist the key duplicates instead, as in FL) |
| Ctrl+E | step edit mode | added |
| Ctrl+H | stop all sound | added |
| Ctrl+I | wait for input before recording | added |
| Ctrl+M | metronome | same |
| Ctrl+P | metronome precount | added (one bar of count-in, on / off) |
| Ctrl+T | typing keyboard to piano | same |

## Window navigation

| Key | FL Studio | PULP |
|---|---|---|
| F5, F6, F7, F9 | Playlist, step sequencer, Piano Roll, Mixer | same |
| F8 | plugin picker | same since milestone 14: the list of instruments, not a picture grid |
| Alt+F8 | browser | same |
| F10 | MIDI settings | same: the settings window (audio, MIDI, plugins, shortcuts) |
| F1 | help | added: starts the tour |
| F11 | song info | no such window |
| F12 | close all windows | added |
| Tab | cycle the windows | added |
| Esc | close a window | added (the window that has the keyboard) |
| Enter | maximize / restore the Playlist | no |

## Mixer

| Key | FL Studio | PULP |
|---|---|---|
| Left / Right | (select track) | same |
| S | solo the current track | added |
| Alt+S | alt solo (track and what it is routed to and from) | no |
| Ctrl+L | link selected channels to the selected track | added |
| Shift+Ctrl+L | link selected channels starting from the selected track | added |
| F2 | rename the track | added |
| Alt+L | select the channels linked to the track | no |
| Alt+Left / Right, Shift+wheel | move tracks | added: Alt+Left / Right move the selected track one place (the wheel does not) |
| Alt+R | render armed tracks to WAV | no: Export > Split mixer tracks |
| Alt+W | wave view of the meters | no such view |
| Ctrl+A, Ctrl+Shift+click | select several tracks | no: one track is selected at a time |
| Ctrl+Shift+S | save mixer track state | no such feature |
| Ctrl+Enter | track properties | no: right-click the track |

## Playlist

| Key | FL Studio | PULP |
|---|---|---|
| P, B, D, T, S, C, E, Z, Y | draw, paint, delete, mute, slip, slice, select, zoom, play tools | same |
| Del | delete the selection | same |
| Home | to the start | same |
| PgUp / PgDn | zoom in / out | same |
| Ctrl+A, Ctrl+D | select all, deselect | same |
| Ctrl+B | duplicate to the right | same |
| Ctrl+C / X / V | copy / cut / paste | same |
| Shift+arrows | move the selection | same |
| Shift+G, Alt+G | group / ungroup | same |
| Alt+T | add a time marker | same (Ctrl+T is the typing keyboard here) |
| Alt+M, Alt+Shift+M | mute / unmute the selection | added |
| Shift+I | invert the selection | added |
| Shift+Q | quick quantize clip starts | added |
| Shift+C | select all clips by source | added |
| Shift+4, Shift+5, Shift+0 | show all, zoom to selection, centre on the play position | added |
| Shift+1..3 | fixed zoom levels | no: PgUp / PgDn and Ctrl+wheel |
| right-click, double right-click | delete, mute | same (right-click deletes) |
| Ctrl+click, Ctrl+Shift+click | select, add to selection | same |
| middle-drag | pan the view | same |
| Alt (held) | bypass snap | same |
| Shift+click on a clip | clone | same |
| 5, 6 | zoom on selection / performance clips | no (Shift+5 zooms to the selection) |
| 7, 8, 9 | transpose down / up, reverse (audio clips) | no: the clip's Stretch and pitch menu |
| Insert | slice at the cursor | no: the slice tool |
| Backspace | toggle snap | no |
| Alt+* and Alt+/ | next / previous marker | no |
| Alt+P | picker panel | no: the picker is always shown |
| Alt+right-click | audition a clip | no |
| Ctrl+Alt+C and variants | consolidate clips | no such feature |
| Ctrl+Alt+G | grid colour | no |
| Ctrl+Alt+Home | resize from the left | no: clips resize from both ends already |
| Ctrl+Del, Ctrl+Ins | delete / insert space | no |
| Ctrl+Left / Right, Ctrl+Enter | select time before / after / around | no |
| Ctrl+F8 | project picker | no such feature |
| Ctrl+P | performance mode | no such feature (the key is the count-in) |
| Ctrl+right-click | zoom on selection | no (Shift+5) |
| Shift+wheel on the track area | reorder tracks | no |
| Shift+Alt+wheel | nudge a clip | no (Shift+Left / Right) |
| Shift+Alt+T | set a time signature | no: one time signature for the project |
| Shift+M | switch stretch mode | no: the clip's Stretch and pitch menu |
| right Shift / right Alt with clicks | slice, mute, quantize | no: left and right modifier keys are not told apart |

## Piano Roll

| Key | FL Studio | PULP |
|---|---|---|
| P, B, D, T, C, E, Z, Y | draw, paint, delete, mute, slice, select, zoom, play tools | same |
| Del | delete | same |
| PgUp / PgDn | zoom | same |
| Ctrl+A, Ctrl+D, Ctrl+B, Ctrl+C / X / V | select all, deselect, duplicate, copy / cut / paste | same |
| Shift+Up / Down, Ctrl+Up / Down | transpose by a semitone / an octave | same |
| Shift+Left / Right | move the selection | same |
| Ctrl+Q, Alt+Q | quick quantize, quantize | same |
| Ctrl+L | quick legato | same |
| Ctrl+G | glue | same |
| Alt+A, Alt+S, Alt+F, Alt+U, Alt+R, Alt+L, Alt+O, Alt+X, Alt+Y, Alt+E | arpeggiator, strum, flam, chop, randomize, articulate, LFO, scale levels, flip, riff machine | same (PULP's own versions of each tool) |
| Alt+W | claw machine | the key opens PULP's Trimmer, a simpler tool of the same kind |
| Ctrl+M | import a MIDI file | same (Ctrl+Shift+M exports) |
| Alt+M, Alt+Shift+M | mute / unmute the selection | added |
| Shift+I | invert the selection | added |
| S, O | slide note, portamento, for new notes | added |
| Alt+V | ghost channels on / off | added |
| G, K | the channel above / below | added |
| Shift+4 | show all | added (Ctrl+0 does the same) |
| H, J | channel above / below that has notes | no (G and K step through all) |
| F, Shift+F | next note property in the lane | no: click the lane's name |
| M | keyboard view mode | no: one keyboard view |
| Alt+K | limit tool | no such tool |
| Alt+C | colour of selected notes | no: the colour selector applies to new notes |
| Alt+B, Alt+N | note helpers, waveform helper | no (BACKDROP shows the waveform) |
| Alt+arrows | nudge | no (Shift+arrows move by the snap) |
| Alt+wheel | change the selected notes' property | no |
| Alt+G, Shift+G | ungroup / group notes | no such feature |
| Ctrl+U | quick chop | no (Alt+U opens the chop tool) |
| Ctrl+I | insert current controller value | no (the key toggles wait for input) |
| Ctrl+Del, Ctrl+Ins | delete / insert space | no |
| Ctrl+Left / Right, Ctrl+Enter | select time before / after / around | no |
| Ctrl+right-click | zoom on selection | no |
| Shift+1..3, Shift+5, Shift+0 | zoom levels, zoom to selection, centre | no |
| Shift+C, Shift+R, Shift+M, Shift+H, Shift+K, Shift+N | select by colour, at random, groups, next channel, chord off | no |
| Shift+D | discard note lengths | no |
| Shift+Ctrl+V | paste from the MIDI clipboard | no |
| double-click a note | note properties | added |
| double right-click a ghost note | swap to its channel | same (double-click a ghost note) |
| Left-Shift+click | add and resize | same (drag while placing) |
| Backspace | toggle snap | no |
| right Shift / right Alt with clicks | slice, quantize | no |

## Browser

| Key | FL Studio | PULP |
|---|---|---|
| Up / Down | move through the list | same (the list's own keys) |
| Enter | replace the channel with the item | no: double-click opens an item in a new channel; right-click > Replace the selected channel's sample |
| Shift+Up / Down | step through items with preview | no: click an item to hear it |
| Ctrl+Alt+click, Shift+click | replace channel, select several | no |

## Global and miscellaneous

| Key | FL Studio | PULP |
|---|---|---|
| Ctrl+Z | undo / redo the last edit | same |
| Ctrl+Alt+Z | undo step by step | same (Ctrl+Shift+Z steps forward) |
| F3 | tool selector for the active window | no |
| Alt+numpad 2..6 | octave of the typing keyboard | no: Z and Q rows span two octaves |
