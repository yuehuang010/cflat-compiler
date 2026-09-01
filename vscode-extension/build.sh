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

# Sync deps unconditionally - npm decides whether anything needs doing. A
# hand-rolled "is node_modules current" probe only catches deps someone
# remembered to add a line for, and silently skips newly added ones.
# "build.sh ci" installs from the lockfile only: reproducible, fails on
# lockfile drift, never rewrites package-lock.json. Costs a full reinstall,
# so the dev loop gets the incremental npm install instead.
if [ "$1" = "ci" ]; then
    echo "[1/3] Installing npm dependencies from lockfile (npm ci)..."
    NPM_INSTALL_CMD="ci"
else
    echo "[1/3] Syncing npm dependencies..."
    NPM_INSTALL_CMD="install"
fi
if ! npm "$NPM_INSTALL_CMD" --no-audit --no-fund; then
    echo "ERROR: npm dependency install failed."
    exit 1
fi

# Compile TypeScript
echo "[2/3] Regenerating l10n bundles and compiling TypeScript..."
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
