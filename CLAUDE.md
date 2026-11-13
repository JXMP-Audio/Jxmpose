# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Ardour is a C++ digital audio workstation. Upstream docs: https://ardour.org/development.html

## Build (waf)

The build uses the bundled `./waf` (Python). The version is derived from `git describe`, so a full git clone with tags is required (GitHub release tarballs don't build — see `README-GITHUB.txt`).

```sh
./waf configure [options]     # e.g. --with-backends=jack,alsa,dummy  --debug-symbols  --test  --cxx17
./waf -j$(nproc)              # build into ./build
./waf --targets=<target>      # build a single target (e.g. libardour, libardour-tests)
./waf configure --help        # all options (see options() in ./wscript)
```

Useful configure options: `--test` (build unit tests, needs cppunit), `--run-tests`, `--single-tests` (one executable per test), `--compile-database` (compile_commands.json), `--address-sanitizer`, `--thread-sanitizer`, `--rt-alloc-debug`, `--no-vst3`, `--no-lxvst`.

Run the freshly built GUI without installing: `gtk2_ardour/ardev` (debug variants: `ardbg`, `ardbg-core`). These source `build/gtk2_ardour/ardev_common_waf.sh`, which sets library/data paths into the build tree — any in-tree executable needs that environment.

## Tests

Unit tests use CppUnit and are only built with `./waf configure --test`. Test sources live in `libs/<lib>/test/` (ardour, pbd, temporal, evoral, midi++2, canvas, audiographer).

```sh
gtk2_ardour/artest                        # run all library test suites
gtk2_ardour/artest <lib> [<test-binary>]  # one library, or one test binary within it
libs/ardour/run-tests.sh                  # libardour suite (also: --debug for gdb, --valgrind)
libs/ardour/run-tests.sh --single <name>  # run test_*<name>* binaries (requires --single-tests)
```

New libardour test programs are registered in `libs/ardour/wscript` via `create_ardour_test_program(...)`. See `doc/unit_tests.txt`.

## Architecture

Layered set of internal shared libraries under `libs/`, with the GUI in `gtk2_ardour/` on top. Each directory has its own `wscript`, recursed from the top-level `wscript`.

- **`libs/pbd`**: foundation. Signals (`PBD::Signal`, cross-thread delivery via event loops), `Stateful`/XML state, `Controllable`, string composition, RCU (`SerializedRCUManager`), lock-free ringbuffers, debug tracing (`DEBUG_TRACE`, enabled at runtime with `-D` flags).
- **`libs/temporal`**: time model — `timepos_t`/`timecnt_t` that can be in audio time (superclock) or musical time (beats), `TempoMap` (RCU-managed, thread-local copies via `TempoMap::use()`/`fetch()`; edits go through `TempoMap::write_copy()` + `update()`).
- **`libs/evoral`**: MIDI event/sequence/control-list data structures (automation curves live here).
- **`libs/ardour`**: the engine and session model, no GUI dependency. `Session` owns routes, playlists, sources, regions, transport; `Route` is a chain of `Processor`s (plugin inserts, sends, meters, amp/panner, disk reader/writer). `AudioEngine` drives the process callback. Plugin hosting for LV2, LADSPA, LXVST/Windows VST, VST3 (`vst3_*.cc`, SDK in `libs/vst3`) and AU (macOS). Lua bindings in `luabindings.cc`.
- **`libs/backends`**: audio/MIDI I/O backends (jack, alsa, pulseaudio, coreaudio, portaudio, dummy) loaded as modules by `AudioEngine`.
- **`libs/surfaces`**: control-surface modules (mackie, osc, generic_midi, push2, faderport8, websockets, …) built on `libs/ctrl-interface`, loaded at runtime.
- **`libs/audiographer`**: export processing graph. **`libs/panners`**: panner plugins. **`libs/plugins`**: bundled LV2 plugins (a-comp, a-eq, …).
- **GUI support libs**: `libs/tk` (vendored/forked GTK2 stack "ytk": ydk/ytk/ytkmm), `libs/gtkmm2ext`, `libs/widgets`, `libs/canvas` (custom Cairo canvas used by the editor/mixer), `libs/waveview`.
- **`gtk2_ardour`**: the GTK GUI application (editor, mixer, dialogs, `ARDOUR_UI` singleton). It observes model objects in libardour through PBD signals and must not be called from the realtime thread.
- **`headless/`, `luasession/`, `session_utils/`**: GUI-less frontends (headless server, Lua session REPL `luasession`, and CLI session tools).
- **`share/scripts`**: bundled Lua scripts (DSP, editor actions, session hooks).

### Threading rules that matter
- The process callback runs in a realtime thread: no allocation, no blocking locks, no I/O there. Disk I/O runs in butler threads; GUI work runs in the GUI thread.
- Cross-thread communication goes through PBD signals connected with an event-loop target (`invalidator(...)`, `gui_context()`), `SessionEvent`s queued to the process thread, and RCU-managed containers (e.g. the route list).
- `--rt-alloc-debug` / `--thread-sanitizer` configure options help catch violations.

## Code conventions

- C++11 baseline by default (C++17 forced on macOS or with `--cxx17`). Indentation uses tabs in C/C++ (`.dir-locals.el`). `tools/clang-format` holds the format config; `tools/cstyle.py` is a style checker.
- New `.h`/`.cc` files carry a GPL copyright header; `tools/pre-commit` (optional git hook) checks copyright years on staged C/C++ files.
- Commit subjects are short and often prefixed with the area (`VST3: …`, `mackie: …`, `MSVC: …`, `editor: …`, `no-op: …` for non-functional changes).
- Windows/MSVC project files live in `MSVCardour3/`, `MSVCMixbus3/`, `MSVCvst_scan/`; keep them in mind when adding source files.
