# PULP factory sample sources

Where the single-note recordings behind the PULP factory instruments come from, under what licence, and where each
licence was read. The machine-readable version (every URL, cache path, pitch, SHA-256 and size) is
`factory/sources.json`. The audio itself is not in the source tree: `python -I tools\fetch-samples.py` downloads it
into `factory-cache\` and verifies every checksum (`--check` verifies without downloading).

282 files, 416.5 MB, 12 sources. Every licence below was read on the source's own page or in its own repository on
2026-10-16; GitHub URLs are pinned to the commit that was read and downloaded.

## Sources

| id | Source | Author | Licence | Where the licence is stated | What it says |
|---|---|---|---|---|---|
| `vsco2ce` | VSCO 2: Community Edition | Versilian Studios (Sam Gossner) and Ivy Audio (Simon Dalzell); sample cutting by Elan Hickler / Soundemote | CC0-1.0 | [LICENSE](https://github.com/sgossner/VSCO-2-CE/blob/440300901dfe9275fd84e0b7763af1f8443ae62e/LICENSE) (and [Readme.txt](https://github.com/sgossner/VSCO-2-CE/blob/440300901dfe9275fd84e0b7763af1f8443ae62e/Readme.txt), section 2) | "CC0 1.0 Universal" |
| `vcsl` | Versilian Community Sample Library (VCSL) | Versilian Studios LLC (Sam Gossner) and contributors | CC0-1.0 | [README.md](https://github.com/sgossner/VCSL/blob/c1ea7bcc3c7309650ab0da9d15c9cd1fbc4a4c7e/README.md) (and [LICENSE](https://github.com/sgossner/VCSL/blob/c1ea7bcc3c7309650ab0da9d15c9cd1fbc4a4c7e/LICENSE)) | "This collection is under a Creative Commons 0 license." |
| `shinyguitar` | Karoryfer Samples Shinyguitar | Karoryfer Lecolds (played by D. Smolken) | CC0-1.0 | [LICENSE](https://github.com/sfzinstruments/karoryfer.shinyguitar/blob/57243cca85277dbcc120ce17c6178032f93c80f3/LICENSE) | "CC0 1.0 Universal" |
| `blackgreen` | Karoryfer Samples Black And Green Guitars | Karoryfer Lecolds (recorded by Brian Wood) | CC0-1.0 | [LICENSE](https://github.com/sfzinstruments/karoryfer.black-and-green-guitars/blob/b3b3249d37dc977a1a297bd2dc053e6d9b6b805c/LICENSE) | "CC0 1.0 Universal" |
| `growlybass` | Karoryfer Samples Growlybass | Karoryfer Lecolds | CC0-1.0 | [LICENSE](https://github.com/sfzinstruments/karoryfer.growlybass/blob/4f483268fc66b5a6d5781d421c0d11b8d08d3fc6/LICENSE) | "CC0 1.0 Universal" |
| `meatbass` | Karoryfer Samples Meatbass | Karoryfer Lecolds (played by Drogomir Smolken, recorded by Ludwik Zamenhof) | CC0-1.0 | [LICENSE](https://github.com/sfzinstruments/karoryfer.meatbass/blob/ac9e859564bda286ab5ec672d00ff1aa2fef2895/LICENSE) | "CC0 1.0 Universal" |
| `bigcatcello` | Karoryfer x bigcat cello | Karoryfer Lecolds and bigcat instruments (performed by Kamila Borowiak) | CC0-1.0 | [LICENSE](https://github.com/sfzinstruments/karoryfer-bigcat.cello/blob/6fd75fbfc1dbb3109bf26220ba1adea46188a18b/LICENSE) | "CC0 1.0 Universal" |
| `legatovocal` | Legato vocal tutorial (the "a" vowel from Karoryfer Samples Hadzi-Fia) | Karoryfer Samples | CC0-1.0 | [LICENSE](https://github.com/sfzinstruments/legato_vocal_tutorial/blob/fac6461ee4c7f498b23246eced644616fa58d2ec/LICENSE) | "CC0 1.0 Universal" |
| `ganjo` | Ganjo (SX six-string guitar banjo) | itsclipping | CC0-1.0 | [LICENSE.md](https://github.com/sfzinstruments/ganjo/blob/ccff5cd5cd3b513873a48994c07724d9d3c39e1c/LICENSE.md) | "CC0 1.0 Universal" |
| `virtuosity` | Virtuosity Drums | Versilian Studios and Karoryfer Samples (drummer Austin McMahon) | CC0-1.0 | [LICENSE](https://github.com/sfzinstruments/virtuosity_drums/blob/9f04cf9a734527edfbb0a4eee1f674e45bbf71bc/LICENSE) | "CC0 1.0 Universal" |
| `sullivan` | Greg Sullivan's E-Pianos (Wurlitzer EP200, Hohner Pianet T) | Greg Sullivan; sfz mapping and FLAC conversion by kinwie | **CC-BY-3.0** | [README.md](https://github.com/sfzinstruments/GregSullivan.E-Pianos/blob/8c3e581acda3594b553948ff0222d4f84a698376/README.md) (and [LICENSE](https://github.com/sfzinstruments/GregSullivan.E-Pianos/blob/8c3e581acda3594b553948ff0222d4f84a698376/LICENSE)) | "This work is licensed under a Creative Commons Attribution 3.0 Unported License." |
| `uiowa` | University of Iowa Musical Instrument Samples (MIS) | Lawrence Fritts, University of Iowa Electronic Music Studios | Unrestricted use (the university's own statement; not a formal licence) | [MIS.html](https://theremin.music.uiowa.edu/MIS.html) | "may be downloaded and used for any projects, without restrictions" |

Notes on the licences:

- `sullivan` is the only source that **requires** attribution. Its licence was read in the sfzinstruments repository
  (README and LICENSE file), which says the instruments are published there with the author's permission; the
  author's own site (http://www.sullivang.net/) could not be read by the tools used and was not checked.
- `uiowa` is not CC0 or a named licence: the grant is the sentence quoted above on the project's own page. It is
  used for one instrument only (Viola). If that is not good enough, the CC0 fallback is the VSCO 2 CE viola *section*
  (`Strings/Viola Section/susvib`).
- `vsco2ce`: the LICENSE file is CC0; its Readme.txt additionally *asks* (does not require) for credit to Versilian
  Studios / Sam Gossner and/or Ivy Audio / Simon Dalzell and a link to the VSCO: CE home page, and asks that the
  samples are not sold directly.
- The readme files of the Karoryfer guitar, bass and cello libraries also state that they are royalty-free for
  commercial and non-commercial use.

## Credit lines for the program's CREDITS file

Required (CC BY 3.0), in full:

> Wurlitzer EP200 and Hohner Pianet T electric piano samples by Greg Sullivan (http://www.sullivang.net/), FLAC
> conversion and sfz mapping by kinwie, from "Greg Sullivan's E-Pianos"
> (https://github.com/sfzinstruments/GregSullivan.E-Pianos), licensed under the Creative Commons Attribution 3.0
> Unported licence (https://creativecommons.org/licenses/by/3.0/). The samples have been trimmed, looped,
> level-matched and format-converted for PULP.

(CC BY 3.0 requires saying whether the material was changed: keep the last sentence if PULP processes the files in
any way, and replace it with "The samples are used unmodified." if it does not.)

Requested or customary (not legally required, please keep):

- VSCO 2: Community Edition by Versilian Studios (Sam Gossner) and Ivy Audio (Simon Dalzell), sample cutting by Elan
  Hickler / Soundemote. CC0. https://github.com/sgossner/VSCO-2-CE (home page: http://vis.versilstudios.net/vsco-community.html)
- Versilian Community Sample Library (VCSL) by Versilian Studios LLC and contributors. CC0. https://github.com/sgossner/VCSL
- Virtuosity Drums by Versilian Studios and Karoryfer Samples, performed by Austin McMahon, recorded at Virtuosity
  Musical Instruments, Boston. CC0. https://github.com/sfzinstruments/virtuosity_drums
- Shinyguitar, Black And Green Guitars, Growlybass, Meatbass and the Hadzi-Fia "a" vowel (legato vocal tutorial) by
  Karoryfer Samples (Karoryfer Lecolds; D. Smolken, Brian Wood, Ludwik Zamenhof). CC0. https://github.com/sfzinstruments
- Karoryfer x bigcat cello by Karoryfer Samples and bigcat instruments, performed by Kamila Borowiak. CC0.
  https://github.com/sfzinstruments/karoryfer-bigcat.cello
- Ganjo guitar banjo by itsclipping. CC0. https://github.com/sfzinstruments/ganjo
- University of Iowa Musical Instrument Samples, created by Lawrence Fritts, Electronic Music Studios, University of
  Iowa. https://theremin.music.uiowa.edu/MIS.html

## Instruments

Notes use middle C = C4 = MIDI 60 and are sounding pitches; where a source names its files in another octave the
manifest converts them and keeps the original token in `source_note` (see `note_convention` in the manifest).

| Instrument | Category | Source | Files | MB | Notes | Format | Remarks |
|---|---|---|---|---|---|---|---|
| Grand Piano | Keys | `vcsl` | 15 | 64.2 | A#0 E1 A#1 E2 A#2 E3 A#3 D4 G#4 D5 G#5 D6 G#6 D7 G#7 | WAV 44.1 kHz 24-bit stereo | Steinway B, close mics, no pedal, layer 3 of 2-4 |
| Pianet Electric Piano | Keys | `sullivan` (CC BY) | 16 | 3.7 | F1 A1 C#2 F2 A2 C#3 F3 A3 C#4 F4 A4 C#5 F5 A5 C#6 E6 | FLAC 44.1 kHz 16-bit mono | Hohner Pianet T, FF layer, full decays |
| Wurlitzer Electric Piano | Keys | `sullivan` (CC BY) | 12 | 0.8 | A1 C2 F2 B2 E3 G#3 C#4 G#4 C#5 G5 C#6 G#6 | FLAC 44.1 kHz 16-bit mono | Short samples that stop before the note has died away; fade or loop |
| Pipe Organ | Keys | `vcsl` | 11 | 23.8 | C1 F#1 C2 F#2 C3 F#3 C4 F#4 C5 F#5 C6 | WAV 44.1 kHz 16-bit stereo | Loud manual; quiet files, normalise |
| Acoustic Guitar | Guitars | `shinyguitar` | 9 | 7.4 | C#2 F#2 C3 F#3 C4 F#4 C5 F#5 C6 | WAV 44.1 kHz 24-bit mono | Archtop recorded with a microphone, not a flat-top |
| Electric Guitar | Guitars | `blackgreen` | 9 | 6.6 | E2 A#2 D#3 A3 D#4 A4 D5 G#5 D6 | WAV (extensible) 44.1 kHz 24-bit mono | Green Gretsch Anniversary, direct, forte |
| Electric Bass | Bass | `growlybass` | 8 | 6.4 | C#1 F#1 C2 F#2 C3 D#3 A3 D#4 | WAV 44.1 kHz 24-bit mono | Squier Jazz Bass, direct, forte |
| Upright Bass | Bass | `meatbass` | 7 | 3.6 | C1 F#1 C2 F#2 C3 F#3 C4 | WAV 44.1 kHz 24-bit mono | Double bass, pizzicato |
| Violin | Strings | `vsco2ce` | 8 | 18.9 | G3 C4 G4 C5 G5 C6 G6 C7 | WAV 44.1 kHz 16-bit stereo | Solo, arco vibrato, forte |
| Viola | Strings | `uiowa` | 9 | 8.6 | C3 F3 A#3 D#4 G#4 C#5 F#5 B5 E6 | AIFF 44.1 kHz 24-bit stereo | Solo, arco, ff, anechoic, no vibrato; leading silence |
| Cello | Strings | `bigcatcello` | 9 | 5.8 | C2 F#2 C3 F#3 C4 F#4 C5 F#5 C6 | WAV 44.1 kHz 24-bit mono | Solo, sustained mf; cut for looping, no release |
| String Ensemble | Strings | `vsco2ce` | 9 | 25.1 | C2 G2 D3 A3 D4 A4 E5 B5 D6 | WAV 44.1 kHz 16/24-bit stereo | Cello, viola and violin sections, sustained vibrato |
| Trumpet | Brass | `vsco2ce` | 7 | 9.3 | F3 A3 D#4 G4 D5 F5 C6 | WAV 44.1 kHz 16-bit stereo | Sustained, loudest layer |
| Trombone | Brass | `vsco2ce` | 7 | 7.5 | A#1 D#2 A#2 D3 F3 C4 F4 | WAV 44.1 kHz 16-bit stereo | Tenor trombone, sustained, loudest layer; quiet files |
| French Horn | Brass | `vsco2ce` | 8 | 11.7 | A1 D#2 A#2 F3 A3 C4 D5 F5 | WAV 44.1 kHz 16-bit stereo | Sustained; uneven levels between notes |
| Tenor Saxophone | Woodwinds | `vcsl` | 8 | 19.1 | G#2 D3 G#3 D4 G#4 C5 E5 A#5 | WAV 48 kHz 24-bit stereo | Non-vibrato; A#5 is altissimo |
| Flute | Woodwinds | `vsco2ce` | 7 | 13.8 | C4 E4 C5 E5 C6 E6 C7 | WAV 44.1 kHz 24-bit stereo | Sustained with vibrato |
| Clarinet | Woodwinds | `vsco2ce` | 8 | 15.0 | D3 A#3 D4 F4 D5 F5 D6 F6 | WAV 44.1 kHz 16-bit stereo | Long sustains, loudest layer; top note relabelled (source says F#) |
| Solo Voice Aah | Voices | `legatovocal` | 6 | 9.8 | D2 F2 G#2 C3 D3 G#3 | WAV 44.1 kHz 24-bit stereo | One deep male voice, not a choir; several notes 20 to 27 cents flat; no release |
| Harp | Plucked | `vcsl` | 11 | 19.5 | G1 D2 A2 E3 B3 F4 C5 G5 D6 A6 D7 | WAV 44.1 kHz 16-bit stereo | Concert harp, forte; about 10 cents flat overall; quiet bass |
| Harpsichord | Plucked | `vcsl` | 8 | 24.8 | D1 A#1 F#2 D3 C4 G#4 E5 C6 | WAV 48 kHz 24-bit stereo | French harpsichord |
| Pizzicato Strings | Plucked | `vsco2ce` | 10 | 4.9 | C2 G2 D3 A3 C4 F#4 C5 E5 B5 D6 | WAV 44.1 kHz 16/24-bit stereo | Cello section and violin section pizzicato |
| Banjo | Plucked | `ganjo` | 6 | 2.5 | F#3 A3 B3 D#4 G#4 B4 | WAV 44.1 kHz 32-bit float stereo | Guitar banjo; source is tuned sharp, only in-tune takes used; short |
| Psaltery | Plucked | `vcsl` | 6 | 7.0 | A#4 D5 F#5 A#5 D6 F#6 | WAV 44.1 kHz 24-bit stereo | Bowed psaltery, plucked |
| Dan Tranh | Plucked | `vcsl` | 7 | 5.0 | B2 F#3 B3 F#4 B4 F#5 B5 | WAV 44.1 kHz 24-bit mono | Vietnamese zither (koto-like) |
| Kalimba | Plucked | `vcsl` | 7 | 10.1 | B3 C#4 D#4 F#4 B4 C#5 D#5 | WAV 48 kHz 24-bit stereo | Kenyan kalimba, traditional tuning; only near-equal-tempered tines |
| Marimba | Mallets | `vcsl` | 8 | 9.5 | F2 C3 G3 F4 C5 G5 F6 C7 | WAV 44.1 kHz 24-bit stereo | Loud layer; quiet files |
| Vibraphone | Mallets | `vcsl` | 6 | 13.4 | F3 C4 G4 D5 A5 E6 | WAV 44.1 kHz 16-bit stereo | Soft mallets |
| Glockenspiel | Mallets | `vcsl` | 6 | 6.7 | G5 C6 G6 C7 G#7 C8 | WAV 44.1 kHz 16-bit stereo | Loud layer; 8 to 24 cents sharp; quiet files |
| Tubular Bells | Bells | `vcsl` | 5 | 19.9 | C4 E4 G#4 C5 E5 | WAV 44.1 kHz 24-bit stereo | Orchestral chimes; note = strike tone |
| Acoustic Kit | Drums | `virtuosity`, `vcsl` | 18 | 8.0 | kick, snare, rimshot, sidestick, closed / open / pedal hi-hat, high / mid / low tom, crash, ride, ride bell, tambourine, shaker, clap, cowbell, woodblock | FLAC 48 kHz 24-bit (kit), WAV 44.1 / 48 kHz 16/24-bit (percussion) | Kit pieces from Virtuosity Drums; low tom and the five percussion hits from VCSL |
| Orchestral Effects | Effects | `vcsl` | 11 | 24.1 | gong, bass drum, clash cymbals, anvil, slapstick (impacts); cymbal crescendos 2 s / 4 s / 7.5 s and a bass-drum roll crescendo (risers); wind chimes up / down (sweeps) | WAV 44.1 kHz 16/24-bit stereo | Acoustic stand-ins only |

Each instrument's `comment` in the manifest carries the full caveats.

## Looked for and not used

| Wanted | Why not |
|---|---|
| Choir | No multi-voice choir under CC0 / CC BY on the listed sources. The solo male "ah" above is the nearest thing; synthesise a choir. |
| Rhodes electric piano | jRhodes3d samples are CC BY-NC (non-commercial clause). |
| Tonewheel / drawbar organ | Only pipe organs found under a clean licence; synthesise. |
| Flat-top acoustic guitar | Only an archtop found as single notes; the University of Iowa guitar page offers multi-note scale files (its zip archives were not inspected); GitHub reports no licence file for OvationGuitar (sfzinstruments). |
| Five-string banjo | FlameStudios Kay 5-String Banjo is GPL-3.0. |
| Mandolin, ukulele, koto, music box | GitHub reports no licence file for Terkelsen.Mandolin (sfzinstruments); the others were not found on the listed sources. |
| Salamander Grand Piano | CC BY 3.0 and usable, but a CC0 grand (VCSL Steinway B) was available, so it was not needed. |
| FluidR3, Freesound | Not checked at all: the brief marked both as probable skips and the other sources covered the list. |
| Vinyl noise; synthetic risers, sweeps and impacts | Not present in the checked sources; synthesise. |
| Third kit tom from the same kit | Virtuosity Drums has two toms only. |

## What was checked on the downloaded files

Every file was decoded with libsndfile and with ffmpeg (282 of 282 decode in both), and its duration, channel count,
sample rate, peak level and pitch were measured. Pitch was estimated by autocorrelation (YIN) on the steadiest part of
each note and refined on the spectrum of the fundamental. No file in the manifest measures more than 30 cents from
its stated note; the measured deviation is recorded per file as `measured_cents`. Files that failed this check in
the first selection were replaced or dropped (see the instrument comments). For tubular bells, three of the
upright-bass notes (F#2, C3, F#3), and a handful of notes with weak fundamentals the automatic estimate was not trustworthy and the
pitch was confirmed by reading the partials instead.
