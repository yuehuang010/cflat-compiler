#!/usr/bin/env bash
# buildci.sh - macOS counterpart of buildci.bat.
#
# Stages:
#   1. BUILD Release        (cmake macos-arm64-release preset)
#   2. TESTS                (test.sh Release)
#   3. LSP TESTS            (test_lsp.sh Release)
#   4. BUILD vscode-extension (vscode-extension/build.sh)
#   5. PACKAGE              (package_release.sh -> out/cflat-macos-arm64-v<ver>.tar.gz)
#
# A failed BUILD (stage 1) aborts; test/package failures are counted and CI
# continues, mirroring buildci.bat. Exits 0 only if every stage passed.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Homebrew tools (cmake, ninja, antlr, coreutils) and the keg-only openjdk
# must be on PATH even when invoked from a bare shell (launchd, cron, CI).
export PATH="/opt/homebrew/bin:/opt/homebrew/opt/openjdk/bin:$PATH"

START_TIME=$SECONDS
OVERALL_ERRORS=0

banner() {
    echo
    echo "========================================================================="
    echo "$1"
    echo "========================================================================="
}

banner "BUILD: Release"
if ! (cmake --preset macos-arm64-release && cmake --build --preset macos-arm64-release); then
    echo "BUILD FAILED: Release"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
    banner ""
    echo "Elapsed: $((SECONDS - START_TIME))s"
    echo "CI FAILED: $OVERALL_ERRORS stages failed."
    exit 1
fi

banner "TESTS [Release]: test.sh"
if ! bash "$SCRIPT_DIR/test.sh" Release; then
    echo "TESTS FAILED: Release test.sh"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
fi

banner "TESTS [Release]: test_lsp.sh"
if ! bash "$SCRIPT_DIR/test_lsp.sh" Release; then
    echo "TESTS FAILED: Release test_lsp.sh"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
fi

banner "BUILD: vscode-extension"
if ! bash "$SCRIPT_DIR/vscode-extension/build.sh"; then
    echo "BUILD FAILED: vscode-extension"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
fi

banner "PUBLISH: packaging release archive"
if ! bash "$SCRIPT_DIR/package_release.sh"; then
    echo "PUBLISH FAILED"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
fi

banner ""
echo "Elapsed: $((SECONDS - START_TIME))s"
if [ "$OVERALL_ERRORS" -eq 0 ]; then
    echo "CI PASSED."
    exit 0
else
    echo "CI FAILED: $OVERALL_ERRORS stages failed."
    exit 1
fi
