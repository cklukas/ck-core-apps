# Changes

## 2026-11-02

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
