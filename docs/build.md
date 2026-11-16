---
title: Build CK Core Apps
sidebar_label: Building
description: Build and test the CK Core Apps suite against CK Core Motif ABI 9 and CK Core CDE.
---

# Build CK Core Apps

CK Core Apps requires a matched native desktop stack:

1. build the latest [`ck-core-motif`](https://github.com/cklukas/ck-core-motif) `master` branch into its own prefix (using a C23-capable compiler such as GCC 14 or Clang 19);
2. build [`ck-core-cde`](https://github.com/cklukas/ck-core-cde) `master` against that exact Motif prefix;
3. build `ck-core-apps` with both prefixes supplied explicitly.

```sh
make -j6 \
  MOTIF_PREFIX=/opt/ck-core-motif \
  CDE_PREFIX=/opt/ck-core-cde \
  CDE_SRC_INC=/path/to/ck-core-cde/cde/include
```

## System prerequisites

In addition to a C compiler (`gcc-14` / `clang`), C++20 compiler (`g++-14` / `clang++` for `ck-browser`), and the staged Motif/CDE prefixes, install the X11 and library development headers used by individual tools:

| Dependency | Used by |
| --- | --- |
| `libx11-dev`, `libxt-dev`, `libxpm-dev`, `libxinerama-dev` | All Motif/CDE applications |
| `libcairo2-dev` | [`ck-clock`](companions/ck-clock.md) |
| `libasound2-dev` (`-lasound`) | [`ck-mixer`](tools/ck-mixer.md) |
| `libxfixes-dev`, `libpng-dev` | [`ck-grab`](tools/ck-grab.md) |
| `libcurl4-openssl-dev`, `libxft-dev`, `libfontconfig1-dev`, `libfreetype-dev` | [`ck-coins`](companions/ck-coins.md) |
| CEF binary archive (`./download_cef.sh`) | [`ck-browser`](companions/ck-browser.md) |

## ABI preflight and unit checks

Every application target depends on `make check-motif`, which compiles and runs `tests/motif_abi_check.c` (`build/motif-abi-check`). It enforces `XmABI_VERSION == 9` at compile time, calls `XmToolkitABI()` from the runtime library, and fails immediately on any header/library mismatch. Linked binaries carry `-Wl,-rpath,$(MOTIF_LIB)` so an older system `libXm.so.4` is never selected implicitly.

Run the standalone unit tests (`tests/ck_load_icon_label_state_test.c`) with:

```sh
make check
```

## Building individual tools and `ck-browser`

Any application can be built individually by target name (`ck-about`, `ck-load`, `ck-tasks`, `ck-mixer`, `ck-clock`, `ck-calc`, `ck-character-map`, `ck-grab`, `ck-eyes`, `ck-coins`, `ck-nibbles`, `ck-mines`, `ck-plasma-1`, `ck-browser`).

The Chromium Embedded Framework binary archive required by [`ck-browser`](companions/ck-browser.md) is intentionally kept out of Git. Fetch the latest stable `linux64` or `linuxarm64` distribution into `third_party/cef/` before building `all` or `ck-browser`:

```sh
./download_cef.sh
make -j6 ck-browser \
  MOTIF_PREFIX=/opt/ck-core-motif \
  CDE_PREFIX=/opt/ck-core-cde \
  CDE_SRC_INC=/path/to/ck-core-cde/cde/include
```

## Make variables

| Variable | Meaning | Default |
| --- | --- | --- |
| `MOTIF_PREFIX` | Installed CK Core Motif ABI 9 prefix | `/usr/local/ck-core-motif` |
| `CDE_PREFIX` | Installed CK Core CDE prefix | `/usr/local/CDE` |
| `CDE_SRC_INC` | CDE source `include` directory (for headers such as `Dt/WmSettings.h`) | `/home/klukas/git/cde/cde/include` |
| `BUILD_DIR` | Out-of-tree build output root (`$(BUILD_DIR)/bin/...`) | `build` |
| `CC` / `CXX` | C and C++20 compilers | `cc` / `c++` |

Use at most six parallel jobs (`-j6`) on shared development hosts.
