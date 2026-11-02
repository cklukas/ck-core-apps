#!/bin/sh
set -eu

if test "$#" -ne 4; then
    echo "usage: $0 TOOL_SOURCE WORK OUTPUT SITE_URL" >&2
    exit 2
fi

tool_source=$(realpath "$1")
work=$2
output=$3
site_url=$4

test ! -e "$work"
test ! -e "$output"
mkdir -p "$work/tmp" "$work/build"
export TMPDIR="$work/tmp"

started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
make -j2 -C "$tool_source" BUILD_ROOT="$work" BUILD_DIR="$work/build" \
    "$work/build/bin/ckdocs"
"$work/build/bin/ckdocs" build --strict --root "$(pwd -P)" --out "$output" \
    --site-url "$site_url"
test -s "$output/index.html"
grep -q 'CK Core Apps' "$output/index.html"
grep -q 'High-resolution dynamic icons' "$output/dynamic-icons.html"
ended=$(date -u +%Y-%m-%dT%H:%M:%SZ)
{
    printf 'started_utc=%s\n' "$started"
    printf 'ended_utc=%s\n' "$ended"
    printf 'source_commit=%s\n' "$(git rev-parse HEAD 2>/dev/null || printf '%s' "${CKGIT_COMMIT:-unknown}")"
    printf 'tool_version=%s\n' "$("$work/build/bin/ckdocs" --version)"
    printf 'site_url=%s\n' "$site_url"
} > "$work/receipt.txt"
