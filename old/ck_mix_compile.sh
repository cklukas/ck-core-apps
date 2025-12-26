#!/bin/sh
#!/bin/sh
# Build ck-mixer via the root Makefile so artifacts land in build/bin
set -e
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$ROOT_DIR"
make ck-mixer
