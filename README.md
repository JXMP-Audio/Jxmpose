# JXMPOSE

JXMPOSE is a free digital audio workstation for Linux, built on the
[Ardour](https://ardour.org/) engine with its own interface: a flat, dark
"studio" look with a track Inspector on the left, a right zone with plugin
browser and master loudness meter, compact track headers and a console-style
mixer.

JXMPOSE is a fork of Ardour. It is **not** affiliated with or endorsed by the
Ardour project.

> **Status:** work in progress. The interface redesign lives on the
> `jxmp-ui-phase1` branch; `master` tracks upstream Ardour.

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
  to recolour), optional automatic type folders.
- **Mixer** — console-style strips with collapsible racks, sidebar tabs,
  mixer scenes.
- **Track folders** — route groups shown as collapsible folders with
  subfolders; a collapsed folder keeps its instrument bus visible.
- **MIDI editor** — switch between *Key*, *Drum* (one row per drum sound)
  and *Score* (music notation, also as drum notation) for the same region.
  In the score, pick a note value (keys 4–8, `.` dotted, `;` triplet) and
  click notes into the staff; click or drag notes to select and move them,
  pick a value to change their length, Delete removes, Up/Down transposes,
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
  (Region › Edit › Edit Notes…). The recording is analysed (pYIN) and every
  note becomes a blob on a keyboard grid, shaped by its level, with the
  sung pitch curve inside. Drag a note up or down for its pitch (whole
  semitones, Alt = free), sideways to move it in time, or drag its left /
  right edge to change its length; neighbouring notes played legato stretch
  along. Double-click puts a note exactly on the nearest semitone, S splits
  the note under the mouse, M merges the selected ones, Left / Right nudge
  by 10 ms; Snap to Semitone, Reset Pitch and Reset Timing work on the
  selection or on all notes (also in the right-click menu). Only the audio
  around moved notes is time-stretched, each re-pitched note is shifted on
  its own with formants preserved (Rubber Band R3) and crossfaded into the
  untouched audio; the original recording is never changed, every edit is
  one undo step (Ctrl+Z works in the window) and the notes are saved with
  the session.

## Building

JXMPOSE builds exactly like Ardour, with `waf`. A **full git clone with tags**
is required, because the version is derived from `git describe` (GitHub
"Download ZIP" archives will not build).

```sh
git clone https://github.com/JXMP-Audio/Jxmpose.git
cd Jxmpose
git checkout jxmp-ui-phase1
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
