#!/usr/bin/env bash
# package_release.sh - macOS counterpart of package_release.ps1.
#
# Stages Release artifacts into out/publish and archives them as a .tar.gz
# (tar preserves executable permissions natively, unlike zip on this path).
#
# Usage:
#   ./package_release.sh          # version parsed from cflat/Version.h
#   ./package_release.sh 1.2.0    # explicit version override
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

VERSION="${1:-}"

if [ -z "$VERSION" ]; then
    VERSION_HEADER="cflat/Version.h"
    if [ ! -f "$VERSION_HEADER" ]; then
        echo "ERROR: $VERSION_HEADER not found." >&2
        exit 1
    fi
    MAJOR="$(grep -E '#define[[:space:]]+CFLAT_VERSION_MAJOR[[:space:]]+[0-9]+' "$VERSION_HEADER" | grep -oE '[0-9]+' | head -1)"
    MINOR="$(grep -E '#define[[:space:]]+CFLAT_VERSION_MINOR[[:space:]]+[0-9]+' "$VERSION_HEADER" | grep -oE '[0-9]+' | head -1)"
    if [ -z "$MAJOR" ] || [ -z "$MINOR" ]; then
        echo "ERROR: Could not parse version from $VERSION_HEADER" >&2
        exit 1
    fi
    VERSION="$MAJOR.$MINOR.0"
fi

RELEASE_DIR="$SCRIPT_DIR/x64/Release"
PUBLISH_DIR="$SCRIPT_DIR/out/publish"
OUT_DIR="$SCRIPT_DIR/out"

if [ ! -f "$RELEASE_DIR/cflat" ]; then
    echo "ERROR: cflat not found at $RELEASE_DIR - run a Release build first." >&2
    exit 1
fi

rm -rf "$PUBLISH_DIR"
mkdir -p "$PUBLISH_DIR"

# Core deployed binaries. macOS has no LTO.dll/Remarks.dll/lld-link.exe/clang-cl.exe
# counterparts - the bundled linker is ld64.lld. Copy any deployed executable
# tool binary that sits directly next to cflat (skips the core/ directory).
cp "$RELEASE_DIR/cflat" "$PUBLISH_DIR/"

if [ ! -f "$RELEASE_DIR/ld64.lld" ]; then
    echo "ERROR: ld64.lld not found at $RELEASE_DIR - run a Release build first." >&2
    exit 1
fi
cp "$RELEASE_DIR/ld64.lld" "$PUBLISH_DIR/"

# Pick up any other deployed tool binaries next to cflat/ld64.lld (e.g. a
# bundled clang, if one is ever deployed there). cflat and ld64.lld already
# copied above are skipped via -o to avoid duplicate work; harmless either way.
while IFS= read -r -d '' tool; do
    name="$(basename "$tool")"
    if [ "$name" != "cflat" ] && [ "$name" != "ld64.lld" ]; then
        cp "$tool" "$PUBLISH_DIR/"
    fi
done < <(find "$RELEASE_DIR" -maxdepth 1 -type f -perm -u+x -print0)

if [ ! -d "$RELEASE_DIR/core" ]; then
    echo "ERROR: core/ not found at $RELEASE_DIR - run a Release build first." >&2
    exit 1
fi
cp -R "$RELEASE_DIR/core" "$PUBLISH_DIR/core"

cp -R "$SCRIPT_DIR/doc" "$PUBLISH_DIR/doc"
cp "$SCRIPT_DIR/README.md" "$PUBLISH_DIR/"
cp "$SCRIPT_DIR/LICENSE" "$PUBLISH_DIR/"

# Copy .cb sources and vcpkg.json manifests from example/, preserving directory structure.
EXAMPLE_ROOT="$SCRIPT_DIR/example"
if [ -d "$EXAMPLE_ROOT" ]; then
    while IFS= read -r -d '' f; do
        rel="${f#"$EXAMPLE_ROOT"/}"
        dest="$PUBLISH_DIR/example/$rel"
        mkdir -p "$(dirname "$dest")"
        cp "$f" "$dest"
    done < <(find "$EXAMPLE_ROOT" -type f \( -name "*.cb" -o -name "vcpkg.json" \) -print0)
fi

VSIX="$(find "$SCRIPT_DIR/vscode-extension" -maxdepth 1 -name "*.vsix" -print0 | xargs -0 ls -t 2>/dev/null | head -1)"
if [ -z "$VSIX" ]; then
    echo "ERROR: No .vsix found in vscode-extension/ - run vscode-extension/build.sh first." >&2
    exit 1
fi
cp "$VSIX" "$PUBLISH_DIR/"

ARCHIVE="$OUT_DIR/cflat-macos-arm64-v$VERSION.tar.gz"
rm -f "$ARCHIVE"
mkdir -p "$OUT_DIR"
tar -czf "$ARCHIVE" -C "$PUBLISH_DIR" .

SUM_FILE="$ARCHIVE.sha256"
if command -v shasum >/dev/null 2>&1; then
    HASH="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
elif command -v sha256sum >/dev/null 2>&1; then
    HASH="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
else
    echo "ERROR: neither shasum nor sha256sum found." >&2
    exit 1
fi
LEAF="$(basename "$ARCHIVE")"
printf '%s  %s\n' "$HASH" "$LEAF" > "$SUM_FILE"

echo "Published to: $PUBLISH_DIR"
echo "Archive:      $ARCHIVE"
echo "Checksum:     $SUM_FILE"
