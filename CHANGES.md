# Changes

## 2026-11-17

- Add a native Wayland `zwlr_screencopy_manager_v1` capture backend and direct
  `--capture <file>` CLI options (`--window`, `--full-screen`, `--no-frame`,
  `--no-cursor`, `--delay`, `--format png|xpm`) to `ck-grab` so full-screen and
  active-window screenshots capture both native Wayland windows (including
  `dtwm-wayland`'s server-side CDE bevel frames via `_DT_ACTIVE_WINDOW_BOX`) and
  XWayland windows, while preserving the existing X11 `XGetImage` + `XFixes`
  path when `WAYLAND_DISPLAY` is unset.
- Update GitHub Actions workflows (`build.yml`, `pages.yml`) to Node 24
  `actions/checkout@v7` and `actions/upload-artifact@v7`.
- Document `rpi4` `ck-ci-runnerd` sandbox invariants and local `ckgit ci`
  lint/monitoring commands in `docs/ci.md`.

## 2026-11-16

- Add dedicated reference documentation pages for all 14 applications
  (`ck-about`, `ck-calc`, `ck-character-map`, `ck-grab`, `ck-mixer`,
  `ck-tasks`, `ck-clock`, `ck-load`, `ck-eyes`, `ck-coins`, `ck-browser`,
  `ck-mines`, `ck-nibbles` and `ck-plasma-1`) covering command-line options,
  keyboard shortcuts, UI modes, CDE session persistence, XDG configuration
  files, and current limitations.
- Add a shared architecture and persistence guide (`docs/architecture.md`)
  documenting `src/shared/` modules, `dtsession` integration, and the suite's
  `~/.config/ck-core/` and `~/.cache/ck-coins/` file layout.
- Upgrade documentation builds and publication to `ckdocs v0.11.0`: install the
  SHA-256-verified `ckdocs_0.11.0_amd64.deb` release package on GitHub Pages
  and pin the `ck-git-hosting` `v0.11.0` sister revision on `rpi4`.
- Support `v*` release tag runs and target the `primary` server runner in
  `.ckgit/ci.yml`, and link GitHub Releases and `CHANGES.md` from the
  documentation site header.
- Avoid checkout-timestamp races on the committed `src/ck-grab/ck-grab-camera.pm`
  XPM asset during read-only or CI builds (`make regen-grab-icon` regenerates it
  explicitly when `camera.png` changes).
- Align `scripts/build-native.sh` and GitHub/rpi4 CI with the current C23
  CK Core Motif and CK Core CDE stack (`gcc-14` / `g++-14`, isolated
  `en_US.UTF-8` `LOCPATH`, `rpcbind` / `rpcinfo` for ToolTalk `ttsession`
  checks, `dtksh` preflight, `--disable-docs` for the staged CDE dependency
  build, `make check` unit test execution, and host kernel provenance in
  `receipt.txt`).

## 2026-11-02

- Interlink the CK Core Apps, CDE, Motif and ck-core-fs-lib documentation sites
  through their site selectors and front-page project panels.
- Put related CK Core documentation links in the left-side site selector
  rather than the right-side header links.
- Make Clock query dtwm's advertised icon geometry and render responsive,
  readable layouts at both normal 48×48 and increased 96×96 sizes, including
  a compact pixel-digital time band and side-by-side small-date composition.
- Make System Load, Plasma and Browser follow the same dtwm icon-size contract
  instead of publishing fixed 96×96 pixmaps that are cropped at normal size.
- Build the Browser and CEF wrapper as C++20, as required by current CEF
  distributions.
- Build the Browser bookmarks menu once after widget realization and CEF
  initialization, avoiding stale child destruction during startup.
- Add uConsole comparison screenshots of dynamic application icons at both
  normal/increased pairs: 32×32 / 64×64 and 48×48 / 96×96.
- Use a two-row BTC/USD layout at 32×32 so currency and price never overlap.
- Keep the System Load icon label stable at narrow sizes: once the `CPU` prefix
  no longer fits in the actual Motif label font, use percentage-only labels
  until the icon geometry changes.
- Require CK Core Motif ABI 9 at build and runtime, with separate Motif and CDE
  prefixes and a preflight that rejects mismatched headers or libraries.
- Add GitHub and rpi4 CI that build the latest CK Core Motif, CK Core CDE and
  native application suite as one verified cohort.
- Add a ckdocs application catalogue, dynamic-icon reference, build guide and
  CI documentation, with GitHub Pages and rpi4 publication configuration.
- Document all implemented dynamic icons: Clock, System Load, Eyes, Browser,
  Coins and the Plasma demo.
- Cross-link CK Core Apps, CK Core CDE and CK Core Motif from the documentation
  header and front-page project panels.
- Build GitHub Pages and rpi4 documentation with ckdocs v0.10.1.
- Publish rpi4 documentation before starting the longer native stack build.
- Stage CDE under fakeroot so ownership-setting install hooks work in unprivileged
  GitHub and rpi4 CI jobs.
- Preflight the ALSA and LMDB development packages on rpi4 CI runners before
  starting the expensive native dependency build.
