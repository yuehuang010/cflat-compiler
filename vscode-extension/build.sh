#!/bin/bash
set -e
cd "$(dirname "$0")"

echo "=== cflat VSCode Extension Build ==="
echo

# Check for Node.js
if ! command -v node >/dev/null 2>&1; then
    echo "ERROR: Node.js is not installed or not on PATH."
    echo "       Download from https://nodejs.org/"
    exit 1
fi

# Install deps if node_modules is missing OR out of sync with package.json.
# A bare "node_modules exists" check misses partial installs (e.g. esbuild
# absent), so also probe a known build-time dependency.
if [ ! -d node_modules ] || [ ! -d node_modules/esbuild ]; then
    echo "[1/3] Installing npm dependencies..."
    if ! npm install; then
        echo "ERROR: npm install failed."
        exit 1
    fi
else
    echo "[1/3] npm dependencies already installed. Skipping npm install."
    echo "       Run \"npm install\" manually if you need to refresh packages."
fi

# Compile TypeScript
echo "[2/3] Compiling TypeScript..."
if ! npm run compile; then
    echo "ERROR: TypeScript compilation failed."
    exit 1
fi

# Stamp the extension version from the compiler's Version.h (single source of truth)
if ! node sync-version.js; then
    echo "ERROR: Version sync failed."
    exit 1
fi

# Install vsce if not present
echo "[3/3] Packaging extension as .vsix..."
if ! npx vsce --version >/dev/null 2>&1; then
    if ! npm install --save-dev @vscode/vsce; then
        echo "ERROR: Failed to install vsce."
        exit 1
    fi
fi
if ! npx vsce package --allow-missing-repository --no-git-tag-version; then
    echo "ERROR: Packaging failed."
    exit 1
fi

echo
echo "=== Build successful! ==="
echo
echo "Next steps:"
echo "  ./launch.sh          - Open VSCode with the extension loaded (development mode)"
echo "  ./install.sh         - Install the .vsix into VSCode"
echo
