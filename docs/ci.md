---
title: CI and validation
description: GitHub and rpi4 build the same Motif, CDE and application cohort.
---

# CI and validation

Both CI implementations check out the latest `master` branch of CK Core Motif
and CK Core CDE alongside the Apps `main` branch. They build Motif ABI 9 first,
build and test CDE against it, stage CDE, and finally build the native CK Core
Apps targets.

Acceptance includes:

- `make check` for the CDE cohort;
- `libXm.so.9` in every native app's ELF dependencies;
- a runtime call to `XmToolkitABI()` returning 9;
- a build receipt containing the exact app, Motif and CDE revisions and output
  hashes;
- strict ckdocs validation and a rendered documentation site.

The browser is excluded from the default native CI build because its external
CEF binary archive is intentionally not stored in Git. Its Motif integration
uses the same build flags when the archive is supplied.
