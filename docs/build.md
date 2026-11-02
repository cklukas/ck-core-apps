---
title: Build CK Core Apps
description: Build the application suite with CK Core Motif ABI 9 and CK Core CDE.
---

# Build CK Core Apps

CK Core Apps requires a matched native stack:

1. build the latest `ck-core-motif` default branch into its own prefix;
2. build CK Core CDE against that prefix;
3. build CK Core Apps with both prefixes supplied explicitly.

```sh
make -j6 \
  MOTIF_PREFIX=/opt/ck-core-motif \
  CDE_PREFIX=/opt/ck-core-cde \
  CDE_SRC_INC=/path/to/ck-core-cde/cde/include
```

The build first compiles and runs `motif-abi-check`. It requires
`XmABI_VERSION == 9`, calls `XmToolkitABI()` from the selected runtime library,
and rejects header/library mismatches. The generated applications carry an
rpath to `MOTIF_PREFIX/lib` so a system `libXm.so.4` cannot silently replace
the selected toolkit.

The CEF archive used by `ck-browser` is not stored in Git. Run
`./download_cef.sh` before building the browser. Native applications other than
the browser can be built as individual Make targets.

## Useful variables

| Variable | Meaning | Default |
| --- | --- | --- |
| `MOTIF_PREFIX` | Installed CK Core Motif ABI 9 | `/usr/local/ck-core-motif` |
| `CDE_PREFIX` | Installed CK Core CDE | `/usr/local/CDE` |
| `CDE_SRC_INC` | CDE source include directory for newer private integration headers | `/home/klukas/git/cde/cde/include` |
| `BUILD_DIR` | Out-of-tree application output | `build` |

Use at most six parallel jobs on shared development hosts.
