# JXMPOSE

JXMPOSE is a free digital audio workstation for Linux, built on the
[Ardour](https://ardour.org/) engine with its own interface: a flat, dark
"studio" look with a track Inspector on the left, a right zone with plugin
browser and master loudness meter, compact track headers and a console-style
mixer.

JXMPOSE is a fork of Ardour. It is **not** affiliated with or endorsed by the
Ardour project, and it is not affiliated with any other DAW vendor. All
product names mentioned anywhere in this repository belong to their
respective owners.

> **Status:** work in progress. The interface redesign lives on the
> `jxmp-ui-phase1` branch; `master` tracks upstream Ardour.

## Features (on top of Ardour)

- **Studio style** — flat widgets, Barlow font, JXMP colour theme
  (Preferences → Appearance; the original look is still available).
- **Inspector** — collapsible Routing / Inserts / Sends / Channel / Notes
  sections for the selected track.
- **Right zone** — tabs for Tracks, a plugin & instrument browser
  (search, filter, double-click to insert, drag onto an insert slot) and a
  master meter with sample-peak level and EBU R128 loudness (momentary,
  short-term, integrated, loudness range, true peak) against a selectable
  target. Fold it away with the arrow on the right edge.
- **Editor** — compact track headers, editable region info line, regions in
  track colour, new tracks start neutral grey (double-click the colour strip
  to recolour), optional automatic type folders.
- **Mixer** — console-style strips with collapsible racks, sidebar tabs,
  mixer scenes.

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

"Ardour" is a trademark of Paul Davis. JXMPOSE uses the name only to state
its origin.
