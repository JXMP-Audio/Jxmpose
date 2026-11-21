# JXMPOSE

JXMPOSE is a free digital audio workstation for Linux, built on the
[Ardour](https://ardour.org/) engine with its own interface: a flat, dark
"studio" look with a track Inspector on the left, a right zone with plugin
browser and master loudness meter, compact track headers and a console-style
mixer.

JXMPOSE is a fork of Ardour. It is **not** affiliated with or endorsed by the
Ardour project.

> **Status:** work in progress. `master` holds the current JXMPOSE code;
> new work lands on feature branches first.

## Features (on top of Ardour)

- **Studio style** — flat widgets, Barlow font, JXMP colour theme
  (Preferences → Appearance; the original look is still available).
- **Left zone** — *Inspector* tab with collapsible Routing / Inserts /
  Sends / Channel / Notes sections for the selected track, and a
  *Visibility* tab to show or hide tracks.
- **Right zone** — tabs for a plugin & instrument browser
  (search, filter, double-click to insert, drag onto an insert slot) and a
  master meter with sample-peak level and EBU R128 loudness (momentary,
  short-term, integrated, loudness range, true peak) against a selectable
  target. Fold it away with the arrow on the right edge.
- **Editor** — compact track headers, editable region info line, regions in
  track colour, new tracks start neutral grey (double-click the colour strip
  to recolour), optional automatic type folders (Track › Sort into Folders
  by Type), the lane of the selected track is lightened. Double-click a
  track name to rename it, also while the track is armed for recording (it
  is disarmed for the rename and armed again; not while it records).
  Double-click a MIDI region for the MIDI editor, an audio region for
  *Edit Notes*.
- **Mixer** — console-style strips with collapsible racks (remembered),
  R/W automation buttons, the channel name at the bottom in track colour, a
  built-in channel strip in every strip, sidebar tabs, mixer scenes. The
  *Plugins* tab lists all plugins (or favorites, recent, top-10) with the
  search right under it, from the first letter; drag one onto a strip.
- **Track folders** — route groups shown as collapsible folders with
  subfolders; a collapsed folder keeps its instrument bus visible.
- **MIDI editor** — in its own window or in the editor's bottom pane,
  switch between *Key*, *Drum* (one row per drum sound: the sounds used, or
  the whole drum map with *All Drums*; Alt+wheel changes the row height),
  *Score* (music notation, also as drum notation) and *Chord Pads* for the
  same region. Chord Pads (laid out like Cubase's): sixteen pads in two
  rows, each a chord built from a root and a bass note, a type (Maj, Min,
  Dim, Aug, Sus2, Sus4, 5), a tension (6, 7, maj7, 9, 11, 13, b9, #9, #11,
  b13, 6/9 …), an inversion and an octave (*Edit Chord* or double-click a
  pad opens the editor). Pick a key and a scale (major, minor, the modes
  Dorian … Locrian, harmonic and melodic minor, harmonic major, Phrygian
  dominant, Lydian dominant, altered, Hungarian, Neapolitan, double
  harmonic, Persian, enigmatic) and the pads fill with its chords (the
  triads and seventh chords of every degree, the tonic as 6 and 6/9), named
  by degree. Hold a pad to hear it on the track's instrument, drag it into
  the notes to write it there (snapped to the grid) or onto another pad, or
  use *Insert at Playhead*. The **circle of fifths** next to the pads turns
  to the chord heard last (majors outside, relative minors inside, the
  leading-tone chord in the middle) and colours what follows it best:
  green a step around the circle or its relative (IV, V, vi …), yellow two
  steps, orange three; a dominant seventh prefers to resolve down a fifth.
  The pads get the same coloured strip. Click a chord of the circle to hear
  it, drag it onto a pad or into the notes.
  In this mode the piano roll gives way to a **chord lane**: the region's
  notes that start together show as one block named by their chord (any
  voicing, slash chords too). Drop pads or circle chords on it to write
  them (onto a block: replace it), drag a block to move it, its edges to
  change its length (snapped to beats, Shift: sixteenths; with several
  selected all of them move), Delete removes it, Left / Right step through
  the chords and play them, drag on empty space selects with a rubber band (Ctrl: adds),
  Ctrl/Shift-click selects more, Ctrl+A all, Ctrl+C / X / V copy, cut
  and paste (at the mouse), Ctrl+D duplicates,
  Ctrl+Z undoes, double-click on empty space writes the selected pad,
  a click on the ruler moves the playhead, Ctrl+wheel zooms.
  The score looks like MuseScore: A4 pages on a grey background (or one
  continuous sheet), the region name as the title, the track name left of the
  first system, a note input bar above (input mode, durations, dot, rest,
  accidentals, tie, tuplets as music glyphs) and a status bar below showing
  the selected note (pitch, duration, velocity, measure, beat) and the zoom.
  The score is edited like MuseScore: `N` starts note input (an input cursor
  shows where notes go), keys 3–7 pick the value (16th … whole, `.` dotted,
  Ctrl+3 triplet, Ctrl+5 quintuplet), A–G write a note at the cursor in the
  octave nearest the previous one (Shift: add to the chord), `0` a rest,
  Alt+2–9 an interval above, `T` a tied note; a click adds to the chord there,
  Shift+click replaces, right click writes a rest. Outside note input a click
  selects (Ctrl toggles, Shift selects a range, drag lassoes), ←/→ step
  through the chords (Ctrl: bars, Alt+↑/↓ within the chord), a value key
  changes the selected notes' length, ↑/↓ transposes (Ctrl: octave,
  Alt+Shift: diatonic), `+`/`-`/`=` sharp/flat/natural, `R` repeats, `T` ties
  to the next note, `Q`/`W` halve/double, Ctrl+A/C/X/V, Delete removes,
  Ctrl+Z undoes. Triplets, sixteenth triplets and quintuplets are recognised
  from the MIDI and written as such, overlapping lines become a second voice,
  the key signature comes from the track's key or is guessed from the
  notes (with key changes inside a region). Chord symbols (Dm9, G13, F/A …)
  are recognised from the sounding notes, dynamics (pp–ff) and crescendo /
  diminuendo hairpins from the velocities, accents from notes louder than
  their line, and staccato from notes played much shorter than written.

- **Quick Controls** — eight knobs per track in the Inspector. They follow
  the first parameters of the track's instrument until you right-click a
  knob and pick any parameter of any plugin on the track; Ctrl+middle-click
  learns a MIDI controller. Assignments are saved with the session.

- **Channel strip** — Gate, EQ and Comp buttons in the Inspector. The first
  click adds the module (bundled ACE expander, four band EQ with shelves,
  compressor) right before the fader, later clicks bypass it; right-click
  to edit or remove. The EQ shows its response curve (double-click it for
  all bands), the compressor a gain reduction meter. The modules are not
  listed with the inserts. Right-click the EQ curve to compare with another
  track: both spectra are drawn behind the curve and the frequencies where
  both tracks are loud (e.g. kick and bass) are marked red.

- **Edit Notes** — per-note editing of a vocal, bass or other single voice
  (double-click an audio region, or Region › Edit › Edit Notes…; the region
  window with its effects is in Region › Edit in separate window…). The recording is analysed (pYIN) and every
  note becomes a blob on a keyboard grid, shaped by its level, with the
  sung pitch curve inside; a thin line continues the curve between the notes
  (slides, scoops, tails). Drag a note up or down for its pitch (whole
  semitones, Alt = free), sideways to move it in time, or drag its left /
  right edge to change its length; neighbouring notes played legato stretch
  along. Double-click puts a note exactly on the nearest semitone, S splits
  the note under the mouse, M merges the selected ones, Left / Right nudge
  by 10 ms; Snap to Semitone, Reset Pitch and Reset Timing work on the
  selection or on all notes (also in the right-click menu). Correct Pitch…
  moves notes towards the nearest semitone and flattens their pitch drift
  by an amount (vibrato stays); Level and Formant set the loudness and the
  formants (voice character) of the selected notes. The slide between two
  neighbouring notes glides from one edit to the other, a scoop into a note
  and its tail move with it. A handle in the middle of each slide sets how
  long it takes (drag left = quicker, right = slower, 5–500 ms;
  double-click = as sung). Only the audio
  around moved notes is time-stretched, each re-pitched note is shifted on
  its own with formants preserved (Rubber Band R3) and crossfaded into the
  untouched audio; the original recording is never changed, every edit is
  one undo step (Ctrl+Z works in the window) and the notes are saved with
  the session.

- **JX plugins** — ten bundled LV2 plugins (`libs/plugins/jxmp.lv2`) for
  jobs the bundled ACE plugins don't cover. Each draws a JXMP-style display
  (VFD windows, CRT traces, LED bars, its own colour stripe), compact in the
  mixer strip. Its window is a JXMP hardware panel (`jx_plugin_ui.cc`): the
  display on top, then sections with ridged knobs (cap colour by role,
  printed scales in the parameter's units), rubber buttons and stepped
  selectors with a VFD readout. Drag or scroll a knob (Shift: fine),
  double-click resets it, Ctrl+middle-click learns a MIDI controller;
  automation, presets and bypass work as usual. *Edit with generic
  controls* still shows the plain sliders.

  | Plugin | What it does |
  |---|---|
  | JX-MBC Multiband Compressor | 4 bands, Linkwitz-Riley crossovers that sum flat, threshold / ratio / attack / release / makeup / active / solo per band, soft knee |
  | JX-VOC Vocoder | 4–32 bands; the voice on the input shapes the carrier on the sidechain (or internal noise); formant shift, sibilance, swap inputs |
  | JX-PIT Pitch Shifter | two shifters (semitones, cents, level, pan) mixed with the dry signal, no latency |
  | JX-TUN Tuner | note, cents and frequency; Correct mode pulls the pitch to the nearest note of a key and scale (13 scales) with speed and amount |
  | JX-DNR Noise Reduction | switch on *Learn Noise* during a passage of noise only, switch it off; the noise is then subtracted (profile saved with the session) |
  | JX-FRZ Spectral Freeze | *Hold* sustains the spectrum of what plays as a drone, *Recapture* takes a new snapshot |
  | JX-HRM Harmonizer | MIDI notes add pitch-shifted copies of the input (up to 8 voices); feed MIDI through a sidechain MIDI input in Pin Connections |
  | JX-SPA Spectrum Analyzer | FFT 1024–8192, window, integration, floor, slope, peak hold |
  | JX-LOOP Looper | eight synchronised loop slots: the first recording sets the length, later ones record in sync and overdub; reverse, level, clear per slot (up to 30 s) |
  | JX-SMP Sampler | plays one sample file from MIDI: key range, root note, velocity, ADSR, loop, start offset, voices, probability |

- **ACE plugins in the JXMP look** — the bundled ACE Compressor and
  Expander (mono and stereo), EQ, Delay, Reverb and Fluid Synth open as
  JXMP panels too, each in its own colour with *Ardour Community Effects*
  on the footer. The inline displays of the Compressor, Expander and EQ
  follow the same guidelines: in the mixer strip a VFD with input level,
  threshold and gain reduction (or the EQ curve on a small CRT); in the
  plugin window the transfer curve or frequency response on a CRT with the
  operating point or band centres, LED level bars and a gain reduction
  readout. Delay's *NOTE* selector steps through the note values (1/1 …
  1/32 T); Fluid Synth shows the loaded SoundFont next to *LOAD*.

## Keyboard shortcuts (added or changed)

| Key | Action |
| --- | --- |
| Numpad `*` | arm the selected tracks and record (again: stop recording) |
| `Shift+C` | metronome on / off |
| `C` | cut tool (MIDI editor: edit note channels) |
| `Shift+Delete` | remove the selected tracks |
| Chord lane: `Ctrl+A / C / X / V / D`, `Delete`, `Esc`, `←/→` | select all, copy, cut, paste at the mouse, duplicate, delete, deselect, step |
| Score: `N`, `Esc` | note input on / off (Esc again: deselect) |
| Score: `3`–`7`, `.`, `Ctrl+3`, `Ctrl+5` | note value 16th … whole, dotted, triplet, quintuplet |
| Score: `A`–`G`, `Shift+A`–`G`, `0`, `Alt+2`–`9` | enter note, add to chord, rest, interval above |
| Score: `←/→`, `Shift+←/→`, `Ctrl+←/→`, `Alt+↑/↓` | next / previous chord (input: move cursor), extend, bar, note in chord |
| Score: `↑/↓`, `Ctrl+↑/↓`, `Alt+Shift+↑/↓`, `+ - =` | transpose semitone, octave, diatonic step; sharp, flat, natural |
| Score: `T`, `R`, `Q / W`, `Delete` | tie, repeat, halve / double, delete |
| Score: `Ctrl+A / C / X / V`, `Ctrl+Z / Y` | select all, copy, cut, paste at the selection or cursor; undo / redo |
| Edit Notes: `S`, `M`, `←/→`, `Ctrl+Z` | split, merge, nudge 10 ms, undo |

## Known issues

- With PipeWire the metronome output may start unconnected (no physical
  outputs are reported): connect *Click* once in the routing, the
  connection is remembered from then on.
- The chord lane is made for chords: on a drum region every hit becomes
  its own narrow block.
- Edit Notes analyses the whole region when it opens, which takes a moment
  on long recordings.
- JX-PIT, JX-HRM and JX-TUN shift pitch granularly: no formant control,
  and large shifts sound grainy on sustained sounds. JX-TUN corrects one
  voice at a time.

## Building

JXMPOSE builds exactly like Ardour, with `waf`. A **full git clone with tags**
is required, because the version is derived from `git describe` (GitHub
"Download ZIP" archives will not build).

```sh
git clone https://github.com/JXMP-Audio/Jxmpose.git
cd Jxmpose
./waf configure --cxx17          # --cxx17 is needed with GCC 16+
./waf -j$(nproc)
gtk2_ardour/ardev                # run from the build tree
./waf install                    # optional: installs as "jxmpose9"
```

If `git describe` finds no tags, fetch the upstream release tags:

```sh
git fetch --no-tags https://github.com/Ardour/ardour.git 'refs/tags/9.*:refs/tags/9.*'
```

Build dependencies are the same as Ardour's; see
<https://ardour.org/development.html>. Developer notes are in
[`CLAUDE.md`](CLAUDE.md).

## Reporting bugs

Please use [GitHub Issues](https://github.com/JXMP-Audio/Jxmpose/issues) for
bugs and [Discussions](https://github.com/JXMP-Audio/Jxmpose/discussions) for
questions and ideas. Do not report JXMPOSE problems to the Ardour project.

## License

JXMPOSE is free software under the **GNU General Public License, version 2
or (at your option) any later version** — see [`COPYING`](COPYING). It
contains code from Ardour (© Paul Davis and the Ardour contributors) and
other projects under their own GPL-compatible licenses. Builds with VST3
support include the VST3 SDK under its GPLv3 option, so such binaries are
distributed under GPLv3.

The Barlow typeface is licensed under the SIL Open Font License
([`gtk2_ardour/fonts/OFL-Barlow.txt`](gtk2_ardour/fonts/OFL-Barlow.txt)).
The Leland music font (used for notation) is © MuseScore BVBA, also under
the SIL Open Font License
([`gtk2_ardour/fonts/OFL-Leland.txt`](gtk2_ardour/fonts/OFL-Leland.txt)).

"Ardour" is a trademark of Paul Davis. JXMPOSE uses the name only to state
its origin.
