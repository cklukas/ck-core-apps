---
title: CI, Releases and Validation
sidebar_label: CI & Releases
description: Cohort validation, release-based ckdocs v0.11.0 publishing, and native ARM64 CI on GitHub Actions and rpi4.
---

# CI, releases and validation

`ck-core-apps` validates both its native Motif/CDE application cohort and its documentation site across two independent environments: **GitHub Actions** (`ubuntu-24.04` / `ubuntu-24.04-arm`) and the self-hosted **`rpi4`** (`ck-git-hosting` `primary` ARM64 sandbox).

## Native cohort CI (`scripts/build-native.sh`)

Both [`.github/workflows/build.yml`](https://github.com/cklukas/ck-core-apps/blob/main/.github/workflows/build.yml) and [`.ckgit/ci.yml`](https://github.com/cklukas/ck-core-apps/blob/main/.ckgit/ci.yml) check out the latest `master` branch of [CK Core Motif](https://github.com/cklukas/ck-core-motif) and [CK Core CDE](https://github.com/cklukas/ck-core-cde) alongside `ck-core-apps`:

1. Compiles CK Core Motif ABI 9 from a clean `git archive` snapshot with C23 (`gcc-14` / `g++-14`).
2. Generates an isolated `en_US.UTF-8` locale (`LOCPATH`), builds CK Core CDE against the staged Motif prefix (`--disable-docs` to skip redundant SGML guide generation already covered by `ck-core-cde` CI), runs `make check` (full ASan + UBSan on 48-bit VA `ubuntu-24.04-arm`, and runtime-probed UBSan + functional checks on `rpi4`'s 39-bit VA `rpt-rpi-v8` kernel), and stages CDE under `fakeroot`.
3. Builds all 13 native non-CEF `ck-core-apps` binaries and executes `make check` (`ck-load-icon-label-state-test`).
4. Verifies every native executable and `dtwm` carry `libXm.so.9` in `readelf -dW` and that `motif-abi-check` reports `CK Core Motif ABI 9` at runtime.
5. Emits `receipt.txt` recording start/end UTC timestamps, elapsed seconds, host kernel, compiler toolchain, exact `apps`, `motif`, and `cde` Git SHA-1 revisions, and SHA-256 digests of every built binary.

[`ck-browser`](companions/ck-browser.md) is omitted from the offline cohort job because its external CEF binary archive is fetched on demand via `./download_cef.sh` rather than stored in Git.

## Release-based `ckdocs v0.11.0` documentation publishing

Documentation is built from `ckdocs.yml` and `docs/` with **`ckdocs v0.11.0`** in strict mode (`ckdocs build --strict`, failing on any broken link, heading fragment, unreferenced page, or invalid front matter) via `scripts/build-docs.sh`:

| Target | How `ckdocs v0.11.0` is provisioned | Published URL |
| --- | --- | --- |
| **GitHub Pages** (`.github/workflows/pages.yml`) | Downloads the official `ckdocs_0.11.0_amd64.deb` release package from `cklukas/ck-git-hosting` (`v0.11.0`) and verifies its pinned SHA-256 digest before installing with `dpkg`. | <https://cklukas.github.io/ck-core-apps/> |
| **`rpi4` LAN Pages** (`.ckgit/ci.yml`) | Compiles `ckdocs` (`v0.11.0`) offline inside the network-isolated `primary` runner from the pinned `ck-git-hosting` sister repository (`fbe21b49d6c5d5afebffa3d3de745351e07c6b5e`). | `http://rpi4:8421/ck-core-apps/` |

Both builds generate HTML pages, tokenized client-side search (`search-index.json`), `sitemap.xml`, `llms.txt`, and companion Markdown exports, and archive `receipt.txt` recording the exact source commit and `ckdocs --version`.

## Tag and release runs

On `rpi4`, `.ckgit/ci.yml` triggers on both `branches: [main]` and `tags: [v*]`:

- **`main` branch pushes:** Publish updated documentation to `ck-git` Pages and archive ephemeral build/documentation receipts.
- **`v*` release tags:** Execute the full `documentation` and `build` verification jobs and store durable, non-expiring release evidence records on `rpi4` without overwriting default-branch Pages.

Validate documentation locally before committing with:

```sh
ckdocs check --config ckdocs.yml
```
