#!/bin/sh
set -eu

export GIT_CONFIG_NOSYSTEM=1

if test "$#" -ne 4; then
    echo "usage: $0 TOOL_SOURCE_OR_BINARY WORK OUTPUT SITE_URL" >&2
    exit 2
fi

tool_arg=$1
work=$2
output=$3
site_url=$4

test ! -e "$work"
test ! -e "$output"
mkdir -p "$work/tmp"
export TMPDIR="$work/tmp"

started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
if test -d "$tool_arg"; then
    tool_source=$(realpath "$tool_arg")
    mkdir -p "$work/build"
    make -j2 -C "$tool_source" BUILD_ROOT="$work" BUILD_DIR="$work/build" \
        "$work/build/bin/ckdocs"
    ckdocs_bin="$work/build/bin/ckdocs"
elif test -f "$tool_arg" && test -x "$tool_arg"; then
    ckdocs_bin=$(realpath "$tool_arg")
else
    echo "expected ck-git-hosting source directory or executable ckdocs binary: $tool_arg" >&2
    exit 2
fi

"$ckdocs_bin" build --strict --root "$(pwd -P)" --out "$output" \
    --site-url "$site_url"
test -s "$output/index.html"
test -s "$output/apps.html"
test -s "$output/dynamic-icons.html"
test -s "$output/architecture.html"
test -s "$output/tools/ck-calc.html"
test -s "$output/companions/ck-browser.html"
test -s "$output/games/ck-mines.html"
test -s "$output/llms.txt"
test -s "$output/sitemap.xml"
grep -q 'CK Core Apps' "$output/index.html"
grep -q 'High-resolution dynamic icons' "$output/dynamic-icons.html"
ended=$(date -u +%Y-%m-%dT%H:%M:%SZ)
{
    printf 'started_utc=%s\n' "$started"
    printf 'ended_utc=%s\n' "$ended"
    printf 'source_commit=%s\n' "$(git rev-parse HEAD 2>/dev/null || printf '%s' "${CKGIT_COMMIT:-unknown}")"
    printf 'tool_version=%s\n' "$("$ckdocs_bin" --version)"
    printf 'site_url=%s\n' "$site_url"
} > "$work/receipt.txt"
