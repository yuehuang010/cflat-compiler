#!/usr/bin/env bash
# buildci.sh - macOS counterpart of buildci.bat.
#
# Stages:
#   1. BUILD Release        (cmake_build.sh release)
#   2. TESTS                (test.sh Release)
#   3. LIBS [Release]       (test_libs.sh - tier 1; tiers 1-3 with --nightly)
#   4. EXAMPLES             (test_example.sh - Release-only, takes JOBS not a config)
#   5. LSP TESTS            (test_lsp.sh Release)
#   6. BUILD vscode-extension (vscode-extension/build.sh)
#   7. PACKAGE              (package_release.sh -> out/cflat-macos-arm64-v<ver>.tar.gz)
#
# --nightly: LIBS runs tiers 1-3 (adds Eigen and libtorch, ~6 min at -j3 on a warm
# out/libs-cache) instead of tier 1 only. Every other stage is unchanged.
#
# A failed BUILD (stage 1) aborts; test/package failures are counted and CI
# continues, mirroring buildci.bat. Exits 0 only if every stage passed.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Homebrew tools (cmake, ninja, antlr, coreutils) and the keg-only openjdk
# must be on PATH even when invoked from a bare shell (launchd, cron, CI).
export PATH="/opt/homebrew/bin:/opt/homebrew/opt/openjdk/bin:$PATH"

LIBS_TIER=1
for arg in "$@"; do
    case "$arg" in
        --nightly) LIBS_TIER=3 ;;
        *) echo "buildci.sh: unknown argument '$arg' (only --nightly)"; exit 2 ;;
    esac
done

START_TIME=$SECONDS
OVERALL_ERRORS=0

# Per-stage wall time: each banner closes the previous stage. The table prints at the end and one
# CSV line per run goes to scratch/buildci_timings.csv so runtimes can be tracked across runs.
STAGE_NAMES=()
STAGE_SECS=()
STAGE_NAME=""
STAGE_START=$SECONDS
stage_close() {
    if [ -n "$STAGE_NAME" ]; then
        STAGE_NAMES+=("$STAGE_NAME")
        STAGE_SECS+=($((SECONDS - STAGE_START)))
    fi
    STAGE_NAME="$1"
    STAGE_START=$SECONDS
}
timing_report() {
    stage_close ""
    echo "Stage timings:"
    local i csv="$(date '+%Y-%m-%d %H:%M'),$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null),tier$LIBS_TIER"
    for i in "${!STAGE_NAMES[@]}"; do
        printf '  %-45s %5ss\n' "${STAGE_NAMES[$i]}" "${STAGE_SECS[$i]}"
        csv="$csv,${STAGE_NAMES[$i]}=${STAGE_SECS[$i]}"
    done
    printf '  %-45s %5ss\n' "TOTAL" "$((SECONDS - START_TIME))"
    mkdir -p "$SCRIPT_DIR/scratch" && echo "$csv,total=$((SECONDS - START_TIME))" >> "$SCRIPT_DIR/scratch/buildci_timings.csv"
}

banner() {
    [ -n "$1" ] && stage_close "$1"
    echo
    echo "========================================================================="
    echo "$1"
    echo "========================================================================="
}

banner "BUILD: Release"
# Go through cmake_build.sh (as buildci.bat goes through cmake_build.bat) rather
# than calling cmake directly: the presets read $env{VCPKG_ROOT}, and resolving it
# from the main checkout's ./vcpkg clone is that script's job.
if ! bash "$SCRIPT_DIR/cmake_build.sh" release; then
    echo "BUILD FAILED: Release"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
    banner ""
    timing_report
    echo "Elapsed: $((SECONDS - START_TIME))s"
    echo "CI FAILED: $OVERALL_ERRORS stages failed."
    exit 1
fi

banner "TESTS [Release]: test.sh"
if ! bash "$SCRIPT_DIR/test.sh" Release; then
    echo "TESTS FAILED: Release test.sh"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
fi

banner "LIBS [Release]: test_libs.sh (tiers 1-$LIBS_TIER)"
if ! bash "$SCRIPT_DIR/test_libs.sh" Release -t "$LIBS_TIER"; then
    echo "LIBS FAILED: Release test_libs.sh"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
fi

banner "EXAMPLES [Release]: test_example.sh"
if ! bash "$SCRIPT_DIR/test_example.sh"; then
    echo "EXAMPLES FAILED: Release test_example.sh"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
fi

banner "TESTS [Release]: test_lsp.sh"
if ! bash "$SCRIPT_DIR/test_lsp.sh" Release; then
    echo "TESTS FAILED: Release test_lsp.sh"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
fi

banner "BUILD: vscode-extension"
if ! bash "$SCRIPT_DIR/vscode-extension/build.sh" ci; then
    echo "BUILD FAILED: vscode-extension"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
fi

banner "PUBLISH: packaging release archive"
if ! bash "$SCRIPT_DIR/package_release.sh"; then
    echo "PUBLISH FAILED"
    OVERALL_ERRORS=$((OVERALL_ERRORS + 1))
fi

banner ""
timing_report
echo "Elapsed: $((SECONDS - START_TIME))s"
if [ "$OVERALL_ERRORS" -eq 0 ]; then
    echo "CI PASSED."
    exit 0
else
    echo "CI FAILED: $OVERALL_ERRORS stages failed."
    exit 1
fi
