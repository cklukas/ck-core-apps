# ck-core

Motif/CDE utilities and games.

## Apps

### ck-about

CDE About dialog with notebook pages that report CDE version and platform details.

### ck-calc

Basic calculator with a classic desktop layout, session handling, and multiple-window support.

### ck-character-map

Character map and glyph browser with font selection and copy/paste support.

### ck-clock

Analog front-panel clock rendered with cairo and Motif color sets.

### ck-load

System load monitor showing CPU, RAM, swap usage, and 1/5/15-minute load averages.

### ck-mixer

ALSA mixer frontend with per-channel sliders, mute toggles, and device selection.

### ck-mines

Minesweeper clone built for Motif/CDE with configurable grid and classic flags/reveals.

### ck-nibbles

QBasic-style Nibbles (snake) clone with 1- or 2-player support and keyboard controls.

## Build

Prerequisites: a C compiler plus the development headers for Motif/X11, the CDE libraries, ALSA (`-lasound`), and cairo (for `ck-clock`).

Build everything into `build/bin`:

```sh
make
```

The Makefile assumes CDE is under `/usr/local/CDE`; override if needed:

```sh
make CDE_PREFIX=/opt/cde
```

Other knobs:
- `BUILD_DIR=out` changes the build output root.
- `CC=clang` or `CFLAGS='-g -O0'` to tweak compilation.

Cleaning:

```sh
make clean
```

The legacy `compile.sh` scripts in each `src/ck-*` directory now delegate to the Makefile so the binaries are always placed under `build/bin`.
