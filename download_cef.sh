#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THIRD_PARTY_DIR="$REPO_ROOT/third_party/cef"
INCLUDE_DIR="$THIRD_PARTY_DIR/include"
LIB_DIR="$THIRD_PARTY_DIR/lib"
RESOURCES_DIR="$THIRD_PARTY_DIR/resources"
LOCALES_DIR="$THIRD_PARTY_DIR/locales"
INFO_FILE="$THIRD_PARTY_DIR/.cef_package_info"

FORCE_DOWNLOAD=0
if [[ "${1:-}" == "--force" ]]; then
  FORCE_DOWNLOAD=1
fi

LIBCEF_DLL_DIR="$THIRD_PARTY_DIR/libcef_dll"
if [[ $FORCE_DOWNLOAD -eq 0 && -f "$INFO_FILE" && -d "$INCLUDE_DIR/include" && -f "$LIB_DIR/libcef.so" && -d "$LIBCEF_DLL_DIR" && -d "$RESOURCES_DIR" && -d "$LOCALES_DIR" ]]; then
  echo "CEF package already staged (use --force to refresh)."
  exit 0
fi

case "$(uname -m)" in
  aarch64) PLATFORM="linuxarm64" ;;
  x86_64) PLATFORM="linux64" ;;
  *)
    echo "Unsupported architecture: $(uname -m)"
    exit 1
    ;;
esac

INDEX_URL="https://cef-builds.spotifycdn.com/index.json"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

curl -L "$INDEX_URL" -o "$TMP_DIR/index.json"

PACKAGE_NAME="$(python3 - "$PLATFORM" "$TMP_DIR/index.json" <<'PY'
import json
import sys

platform = sys.argv[1]
index_path = sys.argv[2]

with open(index_path, "r", encoding="utf-8") as fh:
    data = json.load(fh)

platform_entry = data.get(platform)
if not platform_entry:
    raise SystemExit(f"No builds published for platform {platform}")

for build in platform_entry.get("versions", []):
    if any("_beta" in entry.get("name", "") for entry in build.get("files", [])):
        continue
    for entry in build.get("files", []):
        if entry.get("type") == "standard":
            print(entry.get("name"))
            sys.exit(0)

raise SystemExit("No stable standard distribution found")
PY
)"

if [[ -z "$PACKAGE_NAME" ]]; then
  echo "Unable to determine CEF package name for $PLATFORM"
  exit 1
fi

if [[ -f "$INFO_FILE" && $FORCE_DOWNLOAD -eq 0 ]]; then
  PREV_NAME="$(grep -E '^CEF_PACKAGE_NAME=' "$INFO_FILE" | cut -d= -f2-)"
  if [[ "$PREV_NAME" == "$PACKAGE_NAME" && -d "$INCLUDE_DIR/include" && -f "$LIB_DIR/libcef.so" && -d "$LIBCEF_DLL_DIR" && -d "$RESOURCES_DIR" && -d "$LOCALES_DIR" ]]; then
    echo "CEF package $PACKAGE_NAME already staged, skipping download."
    exit 0
  fi
fi

PACKAGE_URL="https://cef-builds.spotifycdn.com/$PACKAGE_NAME"
PACKAGE_PATH="$TMP_DIR/$PACKAGE_NAME"

curl -L "$PACKAGE_URL" -o "$PACKAGE_PATH"

tar -xjf "$PACKAGE_PATH" -C "$TMP_DIR"

EXTRACTED_DIR="$(find "$TMP_DIR" -maxdepth 1 -type d -name 'cef_binary_*' | head -n 1)"
if [[ -z "$EXTRACTED_DIR" || ! -d "$EXTRACTED_DIR" ]]; then
  echo "CEF archive did not expand to the expected directory"
  exit 1
fi

rm -rf "$INCLUDE_DIR" "$LIB_DIR" "$RESOURCES_DIR" "$LOCALES_DIR"
mkdir -p "$INCLUDE_DIR" "$LIB_DIR" "$RESOURCES_DIR" "$LOCALES_DIR"

cp -r "$EXTRACTED_DIR/include" "$INCLUDE_DIR"
find "$EXTRACTED_DIR/Release" -maxdepth 1 -type f -name 'libcef.so*' -exec cp {} "$LIB_DIR" \;
find "$EXTRACTED_DIR/Release" -maxdepth 1 -type f -name 'libEGL.so*' -exec cp {} "$LIB_DIR" \;
find "$EXTRACTED_DIR/Release" -maxdepth 1 -type f -name 'libGLESv2.so*' -exec cp {} "$LIB_DIR" \;

if [[ -d "$EXTRACTED_DIR/Resources" ]]; then
  cp -a "$EXTRACTED_DIR/Resources/." "$RESOURCES_DIR"
elif [[ -d "$EXTRACTED_DIR/resources" ]]; then
  cp -a "$EXTRACTED_DIR/resources/." "$RESOURCES_DIR"
elif [[ -d "$EXTRACTED_DIR/Release/resources" ]]; then
  cp -a "$EXTRACTED_DIR/Release/resources/." "$RESOURCES_DIR"
elif [[ -d "$EXTRACTED_DIR/Release/Resources" ]]; then
  cp -a "$EXTRACTED_DIR/Release/Resources/." "$RESOURCES_DIR"
else
  for resource_file in icudtl.dat chrome_*.pak resources.pak *.pak v8_context_snapshot.bin snapshot_blob.bin; do
    if [[ -f "$EXTRACTED_DIR/Release/$resource_file" ]]; then
      cp -a "$EXTRACTED_DIR/Release/$resource_file" "$RESOURCES_DIR" 2>/dev/null || true
    fi
  done
fi

if [[ -d "$EXTRACTED_DIR/Release/locales" ]]; then
  cp -a "$EXTRACTED_DIR/Release/locales/." "$LOCALES_DIR"
elif [[ -d "$EXTRACTED_DIR/locales" ]]; then
  cp -a "$EXTRACTED_DIR/locales/." "$LOCALES_DIR"
fi

if [[ ! -f "$RESOURCES_DIR/v8_context_snapshot.bin" && -f "$EXTRACTED_DIR/Release/v8_context_snapshot.bin" ]]; then
  cp -a "$EXTRACTED_DIR/Release/v8_context_snapshot.bin" "$RESOURCES_DIR"/
fi

if [[ -f "$RESOURCES_DIR/icudtl.dat" ]]; then
  cp -a "$RESOURCES_DIR/icudtl.dat" "$LIB_DIR"/
fi
for resource_file in chrome_100_percent.pak chrome_200_percent.pak resources.pak v8_context_snapshot.bin snapshot_blob.bin; do
  if [[ -f "$RESOURCES_DIR/$resource_file" ]]; then
    cp -a "$RESOURCES_DIR/$resource_file" "$LIB_DIR"/
  fi
done

rm -rf "$LIBCEF_DLL_DIR"
if [[ -d "$EXTRACTED_DIR/libcef_dll" ]]; then
  cp -a "$EXTRACTED_DIR/libcef_dll" "$LIBCEF_DLL_DIR"
else
  echo "CEF archive does not include libcef_dll, skipping wrapper sources."
fi

printf 'CEF_PACKAGE_NAME=%s\nCEF_PACKAGE_URL=%s\n' "$PACKAGE_NAME" "$PACKAGE_URL" > "$INFO_FILE"

echo "CEF sources staged under $INCLUDE_DIR, $LIB_DIR, $RESOURCES_DIR, $LOCALES_DIR, and $LIBCEF_DLL_DIR"
