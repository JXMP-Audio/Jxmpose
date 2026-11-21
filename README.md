# JXMPOSE

JXMPOSE is a free digital audio workstation (DAW) for Linux: you record,
edit and mix music in it, with audio (vocals, guitars, drums …) and MIDI
(virtual instruments). It is built on the [Ardour](https://ardour.org/)
engine and has its own interface and tools: a flat dark "studio" look, a
track Inspector, a plugin browser, a loudness meter, a MIDI editor with piano
roll, drum editor, music notation and chord pads, per-note vocal editing and
ten bundled effect plugins.

JXMPOSE is a fork of Ardour. It is **not** affiliated with or endorsed by the
Ardour project.

> **Status:** work in progress. `master` holds the current JXMPOSE code;
> new work lands on feature branches first.

This README is the user manual. If you have never used a DAW, read
[Getting started](#1-getting-started) and [The screen](#2-the-screen) first;
everything else can be looked up when you need it.

## Contents

1. [Getting started](#1-getting-started) — first start, audio setup, your first project
2. [The screen](#2-the-screen) — what is where
3. [Words you will meet](#3-words-you-will-meet) — session, track, region, bus …
4. [Playing and moving around](#4-playing-and-moving-around)
5. [Recording](#5-recording) — audio, MIDI, metronome
6. [Editing in the timeline](#6-editing-in-the-timeline) — tools, cutting, moving, undo
7. [The MIDI editor](#7-the-midi-editor) — Key, Drum, Score and Chord Pads
8. [Edit Notes: correcting a vocal](#8-edit-notes-correcting-a-vocal)
9. [Mixing](#9-mixing) — mixer, channel strip, plugins, Quick Controls, loudness
10. [Bundled plugins](#10-bundled-plugins) — the JX plugins
11. [Exporting your song](#11-exporting-your-song)
12. [Streaming](#12-streaming)
13. [Customising](#13-customising) — look, preferences, your own shortcuts
14. [Keyboard shortcuts](#14-keyboard-shortcuts) — all of them, by task
15. [Problems and solutions](#15-problems-and-solutions)
16. [Known issues](#16-known-issues)
17. [Building from source](#17-building-from-source)
18. [Reporting bugs](#18-reporting-bugs) · [License](#19-license)

---

## 1. Getting started

### First start: choose how JXMPOSE talks to your audio interface

When JXMPOSE starts it shows the **Audio/MIDI Setup** window (later:
*Window › Audio/MIDI Setup*).

| Setting | What to choose |
|---|---|
| **Audio System** | **JACK/PipeWire** on a desktop with PipeWire (most current Linux distributions): JXMPOSE then plays together with your browser and other apps. **ALSA** talks to the interface directly and alone (lowest latency, other apps are silent meanwhile). |
| **Device** | your audio interface (e.g. *Scarlett 18i20*) |
| **Sample rate** | 48 kHz is a good default |
| **Buffer size** | 256 to start with; smaller = less delay when you play live, but more load on the computer. If you hear clicks and dropouts, make it bigger. |

Click **Start**. If the window says the engine is running you are ready.

### Your first project

1. *Session › New* (`Ctrl+N`): give it a name and a folder. A **session** is
   one song or project; everything (recordings, edits, mixer settings) is
   kept in that folder.
2. Add a track: *Track › Add Track, Bus or VCA…* (`Ctrl+Shift+N`), or
   double-click the empty area under the tracks.
   - **Audio track** for a microphone or a guitar,
   - **MIDI track** for a virtual instrument (choose the instrument in the
     same dialog, e.g. *ACE Fluid Synth* or *ACE Reasonable Synth*).
3. Choose the input of the track in the dialog (for audio: the input of your
   interface the microphone is plugged into).
4. Save often: `Ctrl+S`. *Session › Snapshot* (`Ctrl+Shift+S`) keeps a copy
   of the current state you can go back to.

To open a project later: *Session › Open* (`Ctrl+O`) or *Session › Recent*
(`Ctrl+Shift+O`).

---

## 2. The screen

```
┌ Menu: Session  Transport  Edit  Region  Track  View  Window  Help ──────────────┐
├ Toolbar: edit mode · tools (G R C T Y D E) · Snap · grid · zoom ─────────────────┤
├ Info line: name, start, end, length of the selected region (click to edit) ─────┤
│ ┌Inspector┐ ┌ Rulers: bars and beats, tempo, markers ────────────┐ ┌Right zone─┐ │
│ │ Routing │ │ Track headers │ Timeline with the regions           │ │ Plugins   │ │
│ │ Inserts │ │ M S R W name  │ ▭▭▭▭▭  ▭▭▭▭▭▭▭                    │ │ Meter     │ │
│ │ Sends   │ │               │                                     │ │ Regions   │ │
│ │ Channel │ │               │                                     │ │ ▾ more    │ │
│ └─────────┘ └───────────────┴─────────────────────────────────────┘ └───────────┘ │
├ Transport bar: punch · record mode · ◀◀ ▶ ■ ● · clocks · tempo · Rec Edit Cue Mix ┤
└───────────────────────────────────────────────────────────────────────────────────┘
```

- **Menu bar** — every command is somewhere here; the shortcut is printed
  next to it.
- **Toolbar** — the edit tools (see [Tools](#tools)), *Snap* on/off and the
  grid (Bar, 1/4 note …), zoom buttons.
- **Info line** — shows the selected region; click a value (start, length …)
  to type a new one.
- **Inspector** (left) — everything about the **selected track**: routing
  (inputs/outputs), inserts (plugins), the channel strip (Gate, EQ, Comp),
  sends, Quick Controls, fader and pan, notes. The *Visibility* tab next to
  it shows or hides tracks.
- **Track headers** — per track: colour strip (double-click to recolour),
  **M** mute, **S** solo, **R** read automation, **W** write automation,
  the name (double-click to rename) and the record button (red circle).
- **Timeline** — the recordings and MIDI parts (**regions**) on their tracks.
  The selected track's lane is a bit lighter.
- **Right zone** — tabs: **Plugins** (browser of all effects and
  instruments), **Meter** (master level and loudness), **Regions** (all
  recordings of the project) and **▾** for more (Tracks, Marks …). Fold it
  away with the arrow on the right edge.
- **Transport bar** (bottom) — play, stop, record, loop, the clocks
  (bars|beats and time), tempo and time signature, and the buttons
  **Rec · Edit · Cue · Mix** that switch between the recorder, the editor,
  the clip launcher and the mixer windows.

---

## 3. Words you will meet

| Word | Meaning |
|---|---|
| **Session** | one project (one song): a folder with all its files |
| **Track** | a lane that records and plays: audio track (sound) or MIDI track (notes for an instrument) |
| **Region** | one piece of a recording or MIDI on a track; you move, cut and copy regions |
| **Bus** | a track without recordings that collects other tracks (e.g. all drums, or a reverb shared by several tracks) |
| **Master** | the final output: everything you hear goes through it |
| **Plugin** | an effect (EQ, compressor, reverb …) or a virtual instrument |
| **Insert** | a plugin placed in a track: the track's sound goes through it |
| **Send** | a copy of a track's sound sent to a bus (e.g. to a reverb bus) |
| **Fader** | the volume slider of a track |
| **Automation** | changes of volume, pan or a plugin setting over time, drawn as a line |
| **Folder** | tracks grouped together; a folder can be collapsed to save space |
| **Playhead** | the vertical line that shows where playback is |
| **Marker / range** | a named position / a stretch of time on the ruler |
| **Latency** | the delay between playing something and hearing it |

---

## 4. Playing and moving around

| Do this | How |
|---|---|
| Play / stop | `Space` (or the ▶ / ■ buttons) |
| Go to the start / end | `Home` / `End` (`Return` also goes to the start) |
| Move the playhead | click in the ruler, or `P` puts it at the mouse; `←` / `→` step by the grid |
| Rewind / fast forward | `Shift+←` / `Shift+→` |
| Loop | `L` loops the loop range (set it with `Alt+,` and `Alt+.` or by dragging in the *Loop* ruler) |
| Zoom in / out | `=` / `-`; `Z` zooms to the selection, `Ctrl+Shift+_` to the whole session |
| Fit the selected tracks into the window | `F` |
| Follow the playhead while playing | `Ctrl+F` on/off |
| Add a marker at the playhead | `Tab`; jump between markers with `Q` / `W` |
| Solo the selected tracks | `A` |

---

## 5. Recording

### Audio (voice, guitar …)

1. Select the track's input in the Inspector (*Routing*) or when adding the
   track.
2. **Arm** the track: its red record button in the track header.
3. Play and check the level in the track's meter: loud, but never touching
   the top.
4. Start recording: `Shift+R` (arm the transport) and `Space`, or
   `Shift+Space` (record and play at once). **Numpad `*`** arms the
   selected tracks and records in one press; press it again to stop.
5. Stop with `Space`. The new recording appears as a region.

Recording over an existing recording keeps both (*Layered* in the transport
bar): the newest is on top, older takes stay underneath.

### MIDI (keyboard, virtual instrument)

Plug in a MIDI keyboard, create a MIDI track with an instrument and arm it;
what you play is heard right away and recorded like audio. Without a
keyboard, *Window › Virtual Keyboard* (`Alt+K`) plays notes from the
computer, or draw them in the [MIDI editor](#7-the-midi-editor).

### Metronome (click)

- `Shift+C` turns the click on and off (also the metronome button in the
  transport bar).
- Right-click the metronome button for its settings (sound, level, count-in).
- `Shift+>` records with a count-in, `Shift+<` with pre-roll.
- **Where does the click play?** It follows the Master output: whatever
  output Master plays to, the click plays there too, unless you route it
  yourself. To route it yourself: *Window › Audio Connections*
  (`Alt+Shift+A`), tab *JXMPOSE Misc*, row **Click**: click
  the squares of the outputs you want. Your choice is remembered.

### Punch in / out

Re-record only a part: set the punch range (`Ctrl+,` start, `Ctrl+.` end),
turn on **Punch** (`8` or *In*/*Out* in the transport bar) and record;
only the part inside the range is replaced.

---

## 6. Editing in the timeline

### Tools

The tool decides what the mouse does in the timeline. Each one has its own
single key (no Ctrl or Alt needed), and the toolbar shows which is active.

| Key | Tool | What the mouse does |
|---|---|---|
| `G` | **Grab** (hand) | select and move regions; drag a region's edge to shorten or lengthen it |
| `R` | **Range** | select a stretch of time over one or more tracks (then copy, cut, loop or export it) |
| `C` | **Cut** (scissors) | click a region to split it there |
| `T` | **Stretch** | drag a region's edge to make it longer or shorter in time (time-stretch) |
| `Y` | **Grid** | drag bar lines to adjust the tempo to a recording |
| `D` | **Draw** (pencil) | draw MIDI notes, automation lines and new MIDI regions |
| `E` | **Content** | edit inside regions: MIDI notes, automation points |
| `3` | **Smart** mode on/off | in Grab mode the upper half of a track selects ranges, the lower half grabs regions |

### Everyday editing

| Do this | How |
|---|---|
| Select a region | click it with Grab (`G`); `Ctrl`+click adds more; `Ctrl+A` selects all |
| Move it | drag it (it snaps to the grid while *Snap* is on) |
| Copy / cut / paste | `Ctrl+C` / `Ctrl+X` / `Ctrl+V` (paste goes to the playhead or edit point) |
| Duplicate right after itself | `Ctrl+D` (`Alt+D` several times) |
| Split at the mouse or playhead | `S` (or the Cut tool `C`) |
| Trim the start / end to the mouse | `J` / `K` |
| Delete | `Delete` (`Backspace` too) |
| Undo / redo | `Ctrl+Z` / `Ctrl+R` (also `Ctrl+Y`) |
| Hear just the selected region | `H` |
| Normalize (make it as loud as possible without clipping) | `Alt+3` |
| Reverse | `Alt+4` |
| Quantize MIDI to the grid | `Alt+5` |
| Snap on/off, grid smaller/bigger | `4` · `5` / `6` |
| Edit mode (Slide / Ripple / Lock) | `1` cycles through them |
| Where edits happen (mouse / playhead / marker) | `2` cycles through them |
| Remove the selected tracks | `Shift+Delete` |

### Tracks, colours and folders

- **Rename** — double-click the track name (also while it is armed for
  recording).
- **Colour** — new tracks start neutral grey; double-click the colour strip
  at the left of the header to choose a colour. Regions use the track colour.
- **Folders** — *Track › Sort into Folders by Type* puts tracks into
  *Audio / MIDI / Instruments / FX* folders; with *Preferences › Editor ›
  Put new tracks and busses into folders by type* new tracks go there by
  themselves. Click the folder tab to collapse it; a collapsed folder still
  shows its instrument bus. Folders can contain folders.
- **Hide** a track — *Visibility* tab in the left zone.

### Automation

Click **W** on a track, play, and move the fader or a knob: the movement is
recorded. **R** plays it back. With the Draw tool (`D`) you can draw the line
by hand in the track's automation lane (the *A* button of the track opens
the lanes).

---

## 7. The MIDI editor

**Open it:** double-click a MIDI region. It opens in its own window, or in
the editor's bottom pane (*View › Show Editor Properties Box*). At the top
you switch between four views of the same notes:

### Key (piano roll)

Notes are bars on a keyboard grid: higher = higher pitch, longer = longer
note. Draw notes with `D`, select and move them with `G`/`E`.

| Do this | How |
|---|---|
| Transpose the selected notes | `↑` / `↓` (semitone), `Alt+↑` / `Alt+↓` (octave) |
| Move earlier / later | `←` / `→` (`Alt`: finer) |
| Louder / softer (velocity) | `Ctrl+↑` / `Ctrl+↓` |
| Make longer / shorter | `.` / `Ctrl+.` (end), `,` / `Ctrl+,` (start) |
| Quantize to the grid | `Q` |
| Split / join notes | `S` / `J` |
| Duplicate | `Ctrl+D` |
| Select next / previous note | `Tab` / `Ctrl+Tab` |
| Edit velocities / channels in a dialog | `V` / `C` |
| Stop all stuck notes | `Ctrl+`` ` |

### Drum

One row per drum sound, named after the General MIDI drum map (Kick,
Snare, Hi-Hat …). It shows the sounds the region uses, or the whole drum map
with *All Drums*. `Alt`+mouse wheel changes the row height. Hits are
diamonds; draw them with the Draw tool (`D`).

### Score

Music notation, styled like MuseScore: A4 pages (or one continuous sheet),
the region name as the title, an input bar above and a status bar below.
Triplets, two voices, key signature, chord symbols, dynamics (pp–ff),
hairpins, accents and staccato are worked out from the MIDI.
Drum regions are shown as drum notation.

**Writing notes:** press `N` (note input; a cursor shows where notes go),
pick a value with `3`–`7` (16th … whole note, `.` dotted, `Ctrl+3`
triplet, `Ctrl+5` quintuplet), then type the letter `A`–`G`. `Shift+A`–`G`
adds a note to the chord, `0` writes a rest, `Alt+2`–`9` adds an interval
above, `T` ties. A click puts a note there; `Esc` leaves note input.

**Changing notes:** click to select (`Ctrl` adds, `Shift` selects a range,
drag lassoes), `↑`/`↓` transpose (`Ctrl`: octave, `Alt+Shift`: diatonic),
`+` / `-` / `=` sharp / flat / natural, a value key `3`–`7` changes the
length, `Q`/`W` halve/double, `R` repeats, `Delete` removes. See the
[shortcut list](#score-notation) for all keys.

### Chord Pads

Sixteen pads with chords, a circle of fifths and a chord lane: write chord
progressions without knowing music theory.

1. **Pick a key and scale** (e.g. *C* and *Major*, or *A* and *Minor*). The
   pads fill with the chords of that key: the bottom row with the basic
   triads (I, ii, iii …) and two colours of the tonic, the top row with
   seventh chords. Each pad shows its degree.
2. **Hold a pad** to hear it on the track's instrument.
3. **Write it:** drag the pad into the notes or onto the chord lane, or
   select it and click *Insert at Playhead*. The chord length is chosen
   above the pads (1/1 … 1/8).
4. **The circle of fifths** turns to the chord you heard last and colours
   the chords that fit next: **green** best, **yellow** good, **orange**
   possible. Click a chord on the circle to hear it, drag it onto a pad or
   into the notes.
5. **Edit a pad:** select it and click *Edit Chord*: root, type (Maj, Min,
   Dim, Aug, Sus2, Sus4, 5), tension (6, 7, maj7, 9, 11, 13, 6/9 …),
   bass note, inversion and octave.

In this view the piano roll becomes a **chord lane**: notes that start
together show as one block named by their chord. Drag blocks to move them,
their edges to change the length (`Shift`: finer), `Delete` removes,
`←`/`→` step through the chords and play them, `Ctrl+C`/`X`/`V` copy, cut,
paste (at the mouse), `Ctrl+D` duplicates, drag on empty space to select
several, `Ctrl`+wheel zooms. The key you set is remembered per region.

---

## 8. Edit Notes: correcting a vocal

Correct the pitch and timing of single notes of a vocal, bass or any other
single voice, without changing the original recording.

**Open it:** double-click an audio region (or *Region › Edit › Edit
Notes…*). The first time the recording is analysed (a ring shows the
progress); after that the result is remembered and the window opens at once.

Every sung note is a blob on a keyboard grid with the sung pitch line inside
it.

| Do this | How |
|---|---|
| Change a note's pitch | drag it up or down (whole semitones; `Alt` = free) |
| Put it exactly on the semitone | double-click it, or *Snap to Semitone* for the selection or all notes |
| Move it in time | drag it sideways |
| Make it longer / shorter | drag its left or right edge |
| Split / merge | `S` splits the note under the mouse, `M` merges the selected notes |
| Nudge | `←` / `→` by 10 ms |
| Straighten wobbly pitch | *Correct Pitch…*: how much to pull to the semitone and how much drift to remove (vibrato stays) |
| Louder / softer, voice character | *Level* and *Formant* for the selected notes |
| How fast it slides into the next note | drag the round handle between two notes (left = quicker); double-click = as sung |
| Undo everything on a note | *Reset Pitch*, *Reset Timing* |
| Undo the last change | `Ctrl+Z` |

Every change is rendered in the background (a small ring in the corner) and
is one undo step. The notes are saved with the session.

---

## 9. Mixing

### The mixer

Open it with **Mix** in the transport bar or `Alt+M` (again: back to the
editor). Every track is a vertical **strip**: input and plugins at the top,
then the channel strip, sends, pan, the **fader** with its meter, and the
name at the bottom in the track colour. Sections fold open and closed with
their headers (remembered). The mixer sidebar has tabs for *Visibility*,
*Groups*, *Plugins* and *Templates*, and *Mixer Scenes* (store and recall
whole mixer settings).

### Adding plugins (effects and instruments)

- **Plugins** tab in the right zone of the editor, or in the mixer sidebar:
  type to search (from the first letter), **double-click** a plugin to put
  it on the selected tracks, or **drag** it onto a track's insert slot.
- Double-click a plugin in a track to open its window. Right-click for
  bypass, presets and removal.
- JXMPOSE's own plugins and the bundled ACE plugins open as hardware-style
  panels: drag or scroll a knob (`Shift`: fine), double-click resets it,
  `Ctrl`+middle-click learns a MIDI controller. *Edit with generic
  controls* in the right-click menu shows plain sliders.

### Channel strip: Gate, EQ, Comp

Every track has **Gate**, **EQ** and **Comp** buttons in the Inspector (and in
the mixer strip). The first click adds the module, later clicks switch it on
and off; right-click to edit or remove it. The EQ shows its curve
(double-click it for all bands), the compressor how much it reduces.

**Compare EQs:** right-click the EQ curve and choose another track. Both
spectra are drawn behind the curve, and frequencies where both are loud
(e.g. kick drum and bass) are marked **red**: cut one of them there.

### Quick Controls

Eight knobs per track in the Inspector. They control the first settings of
the track's instrument; right-click a knob to assign any setting of any
plugin on the track. `Ctrl`+middle-click a knob and move a knob on your MIDI
controller to link them.

### Loudness: the Meter tab

The **Meter** tab in the right zone shows the master level (peak) and the
**loudness** (LUFS: momentary, short-term, integrated, loudness range, true
peak). Set the target loudness (*Preferences › Metering*, default
−14 LUFS for streaming). The difference to the target is shown in colour:
orange too quiet, green on target, red too loud.

---

## 10. Bundled plugins

Ten JX plugins come with JXMPOSE (in the plugin browser under *JX-…*):

| Plugin | What it is for |
|---|---|
| **JX-MBC** Multiband Compressor | compress lows, low-mids, high-mids and highs separately (mastering, taming a boomy bass) |
| **JX-VOC** Vocoder | the "robot voice": your voice shapes a synth played into the sidechain (or noise) |
| **JX-PIT** Pitch Shifter | two shifted copies of the sound (octaves, harmonies, doubling) mixed with the original |
| **JX-TUN** Tuner | shows the note and how far off it is; *Correct* mode pulls a voice to the notes of a key (auto-tune) |
| **JX-DNR** Noise Reduction | removes steady noise (hiss, hum): turn on *Learn Noise* during a part with only noise, turn it off, done |
| **JX-FRZ** Spectral Freeze | *Hold* freezes the current sound into an endless drone |
| **JX-HRM** Harmonizer | play MIDI notes and your voice sings them as extra harmony voices (up to 8) |
| **JX-SPA** Spectrum Analyzer | shows which frequencies are loud |
| **JX-LOOP** Looper | eight loop slots that record in sync and overdub |
| **JX-SMP** Sampler | plays one sound file from MIDI notes (drum hits, one-shots, simple instruments) |

**Keep Formants** (JX-PIT and JX-HRM): shifted voices normally sound like a
chipmunk (up) or a giant (down), because the shift also moves the
resonances that make a voice sound like itself (formants). With
**FORMANTS** on, only the pitch moves and the voice keeps its character.
It is made for a single voice (vocal, solo instrument) and adds 15 ms of
latency, which JXMPOSE compensates automatically.

The bundled ACE plugins (Compressor, Expander, EQ, Delay, Reverb, Fluid
Synth) also open as JXMP panels.

---

## 11. Exporting your song

*Session › Export › Export to Audio File(s)…* (`Alt+E`): choose the format
(WAV for archiving, MP3/FLAC/OGG for sharing), the range (whole session or a
range) and click *Export*. `Ctrl+E` is a quick export with the last
settings; *Stem Export* (`Alt+Shift+E`) writes every track as its own file.
Tip: check the loudness in the [Meter tab](#loudness-the-meter-tab) before
exporting.

---

## 12. Streaming

If you stream or share your screen with a PipeWire app (e.g. Fluxer),
JXMPOSE can send its sound to the stream by itself: *Preferences › Signal
Flow › Also send Master and the Click to the stream (PipeWire)* (also in the
Audio/MIDI Setup window). Master and the click are connected to the
stream's capture as soon as sharing starts; turning the option off removes
these connections.

---

## 13. Customising

- **Look** — *Window › Preferences › Appearance*: colour theme (*JXMPOSE
  Studio* is the default) and widget style: *Studio* (flat, the default),
  *Hardware* (rubber buttons, ridged knobs, fader caps, VFD clocks) or
  *Classic*. The track header layout changes after a restart.
- **Preferences** — *Window › Preferences*: editor behaviour, metering,
  signal flow, MIDI, plugins …
- **Your own shortcuts** — *Window › Keyboard Shortcuts*: find a command,
  click its shortcut field and press the new key. The window warns when a
  key is already used.
- **Window layout** — fold the right zone with the arrow on its edge, the
  Inspector sections with their headers; maximise the editor with
  `Ctrl+Alt+F`, the mixer with `Ctrl+Shift+F`.

---

## 14. Keyboard shortcuts

Modifier keys: `Ctrl`, `Alt`, `Shift`. Single letters work when the mouse
is over the editor (not while typing in a text field). Shortcuts marked **★**
are JXMPOSE's own.

### Project and windows

| Key | Action |
|---|---|
| `Ctrl+N` / `Ctrl+O` / `Ctrl+Shift+O` | new session / open / recent sessions |
| `Ctrl+S` / `Ctrl+Shift+S` | save / save a snapshot |
| `Ctrl+Shift+N` | add tracks, busses or VCAs |
| `Ctrl+I` | import audio files |
| `Alt+E` / `Ctrl+E` / `Alt+Shift+E` | export / quick export / stem export |
| `Alt+M` | switch between editor and mixer |
| `Alt+R` | recorder window |
| `Alt+C` | cue grid (clip launcher) |
| `Alt+B` | meterbridge |
| `Alt+K` | virtual MIDI keyboard |
| `Alt+Shift+A` / `Alt+Shift+M` | audio / MIDI connections |
| `Ctrl+Alt+F` / `Ctrl+Shift+F` | maximise editor / mixer |
| `Ctrl+Q` | quit |

### Transport

| Key | Action |
|---|---|
| `Space` | play / stop |
| `Shift+Space` | record and play |
| `Shift+R` | arm recording |
| **★** Numpad `*` | arm the selected tracks and record (again: stop) |
| **★** `Shift+C` | metronome on / off |
| `Shift+>` / `Shift+<` | record with count-in / with pre-roll |
| `Home` / `End` / `Return` | go to start / end / start |
| `Shift+←` / `Shift+→` | rewind / fast forward |
| `←` / `→` | playhead to the previous / next grid line |
| `P` | playhead to the mouse |
| `L` | loop on / off |
| `7` | auto return on / off |
| `8` | punch on / off |
| `A` | solo the selected tracks |
| `Ctrl+F` | follow the playhead |
| `Ctrl+`` ` | MIDI panic (stop all notes) |

### Tools

| Key | Tool |
|---|---|
| `G` | Grab |
| `R` | Range |
| `C` | Cut |
| `T` | Stretch |
| `Y` | Grid |
| `D` | Draw |
| `E` | Content (edit inside regions) |
| `3` | Smart mode on / off |
| `1` / `2` | cycle edit mode / edit point |
| `4` / `5` / `6` | snap on/off, finer / coarser grid |

### Editing

| Key | Action |
|---|---|
| `Ctrl+Z` / `Ctrl+R` (`Ctrl+Y`) | undo / redo |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | copy / cut / paste |
| `Delete`, `Backspace` | delete |
| `Ctrl+D` / `Alt+D` | duplicate / duplicate several times |
| `S` | split |
| `J` / `K` | trim start / end to the edit point |
| `Ctrl+J` / `Ctrl+K` | trim to the previous / next region |
| `H` | play the selected region |
| `Ctrl+A` / `Ctrl+Shift+I` | select all / invert the selection |
| `U` / `Ctrl+U` | select within / between the cursors |
| `Alt+0` / `Alt+3` / `Alt+4` / `Alt+5` / `Alt+6` | opaque, normalize, reverse, quantize, boost gain |
| `M` | add a cue marker to the region |
| **★** `Shift+Delete` | remove the selected tracks |

### View and markers

| Key | Action |
|---|---|
| `=` / `-` | zoom in / out |
| `Z` / `Shift+Z` | zoom to the selection / back |
| `Ctrl+Shift+_` | zoom to the whole session |
| `F` | fit the selected tracks |
| `Tab` / `Ctrl+Tab` | add / remove a marker at the playhead |
| `Q` / `W` | jump to the previous / next marker |
| `,` / `.` | start / end a range |
| `Alt+,` / `Alt+.` | start / end the loop range |
| `Ctrl+,` / `Ctrl+.` | start / end the punch range |

### MIDI notes (Key and Drum views)

| Key | Action |
|---|---|
| `↑` / `↓` | transpose a semitone |
| `Alt+↑` / `Alt+↓` | transpose an octave |
| `←` / `→` (`Alt`: fine) | move earlier / later |
| `Ctrl+↑` / `Ctrl+↓` (`Ctrl+Alt`: fine) | velocity up / down |
| `.` / `Ctrl+.` | end later / earlier |
| `,` / `Ctrl+,` | start earlier / later |
| `Q` | quantize |
| `S` / `Shift+S` / `J` | split (more / fewer parts) / join |
| `Ctrl+D` / `Shift+D` | duplicate the notes / the selection |
| `Tab` / `Ctrl+Tab` | select the next / previous note (`Shift`: add) |
| `Ctrl+A` / `Shift+I` / `Esc` | select all / invert / deselect |
| `V` | edit velocities |
| `C` | edit note channels |

### Score (notation)

| Key | Action |
|---|---|
| `N` / `Esc` | note input on / off (`Esc` again: deselect) |
| `3`–`7`, `.` | note value 16th … whole, dotted |
| `Ctrl+3` / `Ctrl+5` | triplet / quintuplet |
| `A`–`G` | write a note |
| `Shift+A`–`G` | add a note to the chord |
| `0` | rest |
| `Alt+2`–`9` | add an interval above |
| `T` | tie |
| `←` / `→` | previous / next chord (note input: move the cursor) |
| `Shift+←` / `Shift+→` | extend the selection |
| `Ctrl+←` / `Ctrl+→` | previous / next bar |
| `Alt+↑` / `Alt+↓` | note within the chord |
| `↑` / `↓`, `Ctrl+↑` / `Ctrl+↓`, `Alt+Shift+↑` / `Alt+Shift+↓` | transpose a semitone, an octave, a scale step |
| `+` / `-` / `=` | sharp / flat / natural |
| `R` | repeat |
| `Q` / `W` | halve / double the length |
| `Delete` | delete |
| `Ctrl+A` / `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | select all, copy, cut, paste |
| `Ctrl+Z` / `Ctrl+Y` | undo / redo |

### Chord lane (Chord Pads view)

| Key | Action |
|---|---|
| `←` / `→` | previous / next chord (and play it) |
| `Ctrl+A` | select all |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | copy / cut / paste at the mouse |
| `Ctrl+D` | duplicate |
| `Delete` | delete |
| `Esc` | deselect |
| `Ctrl+Z` | undo |
| `Ctrl`+wheel | zoom |

### Edit Notes

| Key | Action |
|---|---|
| `S` | split the note under the mouse |
| `M` | merge the selected notes |
| `←` / `→` | nudge 10 ms |
| `Ctrl+Z` | undo |
| `Alt` while dragging | free pitch (not whole semitones) |

---

## 15. Problems and solutions

| Problem | Try this |
|---|---|
| **No sound at all** | *Window › Audio/MIDI Setup*: is the engine running and the right device chosen? Is Master connected to your outputs (*Window › Audio Connections*)? Is a track muted (**M**) or another soloed (**S**)? |
| **Crackles, dropouts** | make the buffer size bigger in *Audio/MIDI Setup*; close heavy plugins; the DSP value at the top right shows the load |
| **I hear myself late while recording** | make the buffer smaller; avoid plugins with latency on the track you record |
| **No metronome** | `Shift+C`; check the click's output in *Window › Audio Connections* (row *Click*) |
| **A recording is too quiet / distorted** | set the input gain on your audio interface, not in JXMPOSE; the meter should stay below the top |
| **The MIDI keyboard does nothing** | arm the MIDI track; check *Window › MIDI Connections*; `Alt+I` toggles MIDI input of the selected track |
| **Notes keep sounding** | `Ctrl+`` ` (MIDI panic) |
| **I deleted something** | `Ctrl+Z` |
| **JXMPOSE crashed** | start it again and open the session: it offers to recover the unsaved work (*Crash Recovery*) |
| **Edit Notes finds no notes** | it works on one voice at a time (vocal, solo instrument), not on chords or full mixes; check the region is not silent |

---

## 16. Known issues

- The chord lane is made for chords: on a drum region every hit becomes
  its own narrow block.
- *Keep Formants* (JX-PIT, JX-HRM) follows one voice; on chords or full
  mixes use it off. JX-TUN corrects one voice at a time.
- Without *Keep Formants*, JX-PIT and JX-HRM shift granularly: large shifts
  sound grainy on sustained sounds.

---

## 17. Building from source

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

**Tests:** `gtk2_ardour/jxmp-tests` runs JXMPOSE's own tests (chord theory
of the Chord Pads; Edit Notes: glides, rendering, the analysis cache). They
are built with every build and need no test framework. Ardour's library
tests need `./waf configure --test` and CppUnit (`gtk2_ardour/artest`).

---

## 18. Reporting bugs

Please use [GitHub Issues](https://github.com/JXMP-Audio/Jxmpose/issues) for
bugs and [Discussions](https://github.com/JXMP-Audio/Jxmpose/discussions) for
questions and ideas. Do not report JXMPOSE problems to the Ardour project.

## 19. License

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
