#!/bin/bash
set -e
cd "$(dirname "$0")"

echo "=== cflat VSCode Extension - Package and Install ==="
echo

# Check prerequisites
if ! command -v node >/dev/null 2>&1; then
    echo "ERROR: Node.js not found. Download from https://nodejs.org/"
    exit 1
fi
if ! command -v code >/dev/null 2>&1; then
    echo "ERROR: 'code' (VSCode CLI) not found on PATH."
    echo "       Ensure VSCode is installed and \"code\" is added to PATH."
    echo "       In VSCode: Cmd+Shift+P -> \"Shell Command: Install 'code' command in PATH\""
    exit 1
fi

# Install dependencies
if [ ! -d node_modules ]; then
    echo "[1/4] Installing npm dependencies..."
    if ! npm install; then
        echo "ERROR: npm install failed."
        exit 1
    fi
else
    echo "[1/4] npm dependencies already installed."
fi

# Compile
echo "[2/4] Compiling TypeScript..."
if ! npm run compile; then
    echo "ERROR: TypeScript compilation failed."
    exit 1
fi

# Install vsce if not present
echo "[3/4] Checking for vsce (VSCode Extension CLI)..."
if ! npx vsce --version >/dev/null 2>&1; then
    echo "       Installing @vscode/vsce..."
    if ! npm install --save-dev @vscode/vsce; then
        echo "ERROR: Failed to install vsce."
        exit 1
    fi
fi

# Stamp the extension version from the compiler's Version.h (single source of truth)
if ! node sync-version.js; then
    echo "ERROR: Version sync failed."
    exit 1
fi

# Package
echo "[4/4] Packaging extension..."
if ! npx vsce package --allow-missing-repository --no-git-tag-version; then
    echo "ERROR: Packaging failed."
    exit 1
fi

# Find the generated .vsix file (newest first)
VSIX_FILE=$(ls -t ./*.vsix 2>/dev/null | head -n 1)
if [ -z "$VSIX_FILE" ]; then
    echo "ERROR: No .vsix file found after packaging."
    exit 1
fi

echo
echo "Installing $VSIX_FILE into VSCode..."
if ! code --install-extension "$VSIX_FILE"; then
    echo "ERROR: Extension install failed."
    exit 1
fi

echo
echo "=== Extension installed successfully! ==="
echo "  Name   : $VSIX_FILE"
echo "  Reload VSCode (Cmd+Shift+P -> \"Developer: Reload Window\") to activate."
echo
echo "TIP: After reloading, open a .cb file to activate the language server."
echo "     Set cflat.executablePath in Settings if auto-detect doesn't find your build."
echo
