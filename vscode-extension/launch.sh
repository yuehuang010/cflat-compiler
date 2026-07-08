#!/bin/bash
set -e
cd "$(dirname "$0")"

echo "=== cflat VSCode Extension - Development Launch ==="
echo

# Check for VSCode
if ! command -v code >/dev/null 2>&1; then
    echo "ERROR: 'code' not found on PATH."
    echo "       In VSCode: Cmd+Shift+P -> \"Shell Command: Install 'code' command in PATH\""
    exit 1
fi

# Check for Node.js
if ! command -v node >/dev/null 2>&1; then
    echo "ERROR: Node.js not found. Download from https://nodejs.org/"
    exit 1
fi

# Build first if out/ doesn't exist
NEEDS_BUILD=0
if [ ! -f out/extension.js ]; then NEEDS_BUILD=1; fi
if [ ! -f out/server.js ]; then NEEDS_BUILD=1; fi

if [ "$NEEDS_BUILD" = "1" ]; then
    echo "Extension not built yet. Running build..."
    if ! ./build.sh; then
        echo "Build failed."
        exit 1
    fi
    echo
fi

EXT_PATH="$(pwd)"

# Determine workspace folder to open (default to cflat source root)
WORKSPACE="$(dirname "$EXT_PATH")"
if [ -n "$1" ]; then WORKSPACE="$1"; fi

echo "Launching VSCode with extension in development (experimental) mode..."
echo "  Extension path : $EXT_PATH"
echo "  Workspace      : $WORKSPACE"
echo

# --extensionDevelopmentPath loads the extension without installing it.
# VSCode opens a NEW "[Extension Development Host]" window with the extension active.
code --extensionDevelopmentPath="$EXT_PATH" --new-window "$WORKSPACE"

echo "VSCode launched. A new window should appear momentarily."
echo
echo "Tips:"
echo "  - Open a .cb file to activate the language server."
echo "  - View -> Output -> \"cflat Language Server\" to see server logs."
echo "  - Press F5 inside the extension source to attach the debugger (port 6009)."
echo "  - Run ./build.sh and restart VSCode to pick up code changes."
echo
