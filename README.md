# ck-core

Small CDE panel utilities:
- `ck-about` — CDE/version information notebook
- `ck-load` — CPU load meter
- `ck-mixer` — simple mixer using ALSA
- `ck-clock` — analog Cairo clock for the front panel

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
