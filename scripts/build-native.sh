#!/bin/sh
set -eu

if test "$#" -ne 4; then
    echo "usage: $0 MOTIF_SOURCE CDE_SOURCE WORK OUTPUT" >&2
    exit 2
fi

motif_input=$(realpath "$1")
cde_input=$(realpath "$2")
work=$3
output=$4
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
apps_source=$(dirname "$script_dir")

test ! -e "$work"
test ! -e "$output"
mkdir -p "$work/tmp" "$work/motif-source" "$work/motif-build" \
    "$work/motif-prefix" "$work/cde-source" "$work/cde-stage"
export TMPDIR="$work/tmp"

started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
started_epoch=$(date +%s)
apps_revision=$(git -C "$apps_source" rev-parse HEAD)
motif_revision=$(git -C "$motif_input" rev-parse HEAD)
cde_revision=$(git -C "$cde_input" rev-parse HEAD)

# Build disposable, revision-exact source snapshots. CI must not generate files
# or in-tree build output in its sister checkouts.
git -C "$motif_input" archive "$motif_revision" | tar -x -C "$work/motif-source"
git -C "$cde_input" archive "$cde_revision" | tar -x -C "$work/cde-source"
motif_source="$work/motif-source"
cde_source="$work/cde-source"

(cd "$motif_source" && autoreconf -fi)
(cd "$work/motif-build" && "$motif_source/configure" \
    --prefix="$work/motif-prefix" --disable-demos --disable-static \
    --enable-xft --enable-unicode-text --enable-atspi --enable-svg \
    CFLAGS='-O1 -g0' CXXFLAGS='-O1 -g0')
for component in bindings bitmaps config localized lib include tools/wml clients/uil; do
    make -j4 -C "$work/motif-build/$component"
    make -j1 -C "$work/motif-build/$component" install
done
test -e "$work/motif-prefix/lib/libXm.so.9"

export PATH="$work/motif-prefix/bin:$PATH"
export CPPFLAGS="-I$work/motif-prefix/include $(pkg-config --cflags xft)"
export LDFLAGS="-L$work/motif-prefix/lib -Wl,-rpath,$work/motif-prefix/lib"
export LD_LIBRARY_PATH="$work/motif-prefix/lib"

(cd "$cde_source/cde" && ./autogen.sh && \
    ./configure CFLAGS='-O1 -g0' CXXFLAGS='-O1 -g0' && \
    make -j4 && make -j4 check && \
    make -j1 install DESTDIR="$work/cde-stage")
readelf -dW "$cde_source/cde/programs/dtwm/.libs/dtwm" | grep -F 'libXm.so.9'

cde_prefix="$work/cde-stage/usr/dt"
native_targets="
$output/bin/ck-about
$output/bin/ck-load
$output/bin/ck-tasks
$output/bin/ck-mixer
$output/bin/ck-clock
$output/bin/ck-calc
$output/bin/ck-character-map
$output/bin/ck-grab
$output/bin/ck-eyes
$output/bin/ck-coins
$output/bin/ck-nibbles
$output/bin/ck-mines
$output/bin/ck-plasma-1
"
make -j4 -C "$apps_source" BUILD_DIR="$output" \
    MOTIF_PREFIX="$work/motif-prefix" CDE_PREFIX="$cde_prefix" \
    CDE_SRC_INC="$cde_source/cde/include" $native_targets

for executable in $native_targets; do
    test -x "$executable"
    readelf -dW "$executable" | grep -F 'libXm.so.9'
done

LD_LIBRARY_PATH="$work/motif-prefix/lib:$cde_prefix/lib" \
    "$output/motif-abi-check" | grep -F 'CK Core Motif ABI 9'

ended=$(date -u +%Y-%m-%dT%H:%M:%SZ)
ended_epoch=$(date +%s)
{
    printf 'started_utc=%s\n' "$started"
    printf 'ended_utc=%s\n' "$ended"
    printf 'elapsed_seconds=%s\n' "$((ended_epoch - started_epoch))"
    printf 'target=linux-arm64-native\n'
    printf 'configuration=optimized-no-debug;motif-abi-9;cde-matched\n'
    printf 'output=%s\n' "$(realpath "$output")"
    printf 'apps_revision=%s\n' "$apps_revision"
    printf 'motif_revision=%s\n' "$motif_revision"
    printf 'cde_revision=%s\n' "$cde_revision"
    printf 'motif_abi=9\n'
    sha256sum $native_targets
} > "$work/receipt.txt"
