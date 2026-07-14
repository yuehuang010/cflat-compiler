#!/usr/bin/env bash
# Cross-platform CMake build helper for macOS / Linux (counterpart to cmake_build.bat).
#
# Usage:
#   ./cmake_build.sh             builds release
#   ./cmake_build.sh debug       builds debug
#   ./cmake_build.sh release
#
# Output: x64/<Config>/cflat (+ core/, ld64.lld) - the layout test.sh expects.
#
# Worktree-friendly. The vcpkg dependency tree lives OUTSIDE the source tree at
# $CFLAT_VCPKG_INSTALLED (default ~/.cflat-compiler-deps/vcpkg_installed), so every
# worktree shares one 12 GB copy and `git worktree add` needs no post-processing.
# VCPKG_ROOT is resolved from the MAIN checkout, since ./vcpkg is gitignored and
# therefore absent in a linked worktree.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "${1:-release}" in
  debug)   cfg=debug   ;;
  release) cfg=release ;;
  *) echo "Unknown config \"$1\" - use debug or release" >&2; exit 2 ;;
esac

case "$(uname -s)" in
  Darwin) preset="macos-arm64-$cfg" ;;
  Linux)  preset="linux-x64-$cfg"   ;;
  *) echo "Unsupported host $(uname -s) - use cmake_build.bat on Windows" >&2; exit 2 ;;
esac

if [ "$(uname -s)" = "Darwin" ]; then
  # ./vcpkg is gitignored, so a linked worktree has none. Resolve the main
  # checkout (the one that owns the real .git) and use its clone.
  if [ -z "${VCPKG_ROOT:-}" ]; then
    common_git="$(git -C "$script_dir" rev-parse --path-format=absolute --git-common-dir)"
    main_root="$(dirname "$common_git")"
    VCPKG_ROOT="$main_root/vcpkg"
  fi
  if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
    echo "ERROR: no bootstrapped vcpkg at \"$VCPKG_ROOT\"." >&2
    echo "Set VCPKG_ROOT, or bootstrap the main checkout's clone:" >&2
    echo "  \"$VCPKG_ROOT/bootstrap-vcpkg.sh\"" >&2
    exit 1
  fi
  export VCPKG_ROOT

  # openjdk is keg-only; the ANTLR generator step needs `java` on PATH.
  [ -d /opt/homebrew/opt/openjdk/bin ] && export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"

  installed="${CFLAT_VCPKG_INSTALLED:-$HOME/.cflat-compiler-deps/vcpkg_installed}"
  if [ ! -d "$installed/arm64-osx" ]; then
    echo "ERROR: shared dependency tree not found at \"$installed\"." >&2
    echo "Populate it once (restores LLVM from the binary cache, no source build):" >&2
    echo "  \"$VCPKG_ROOT/vcpkg\" install --triplet=arm64-osx \\" >&2
    echo "      --x-manifest-root=\"$script_dir\" --x-install-root=\"$installed\"" >&2
    exit 1
  fi
fi

# A build dir configured against a different dependency tree has stale absolute
# paths cached (LLVM_DIR etc). Reconfiguring in place would silently keep them.
cache="$script_dir/build/$preset/CMakeCache.txt"
if [ "$(uname -s)" = "Darwin" ] && [ -f "$cache" ]; then
  cached="$(sed -n 's/^VCPKG_INSTALLED_DIR:[^=]*=//p' "$cache" || true)"
  if [ -n "$cached" ] && [ "$cached" != "$installed" ]; then
    echo "== dependency tree moved ($cached -> $installed); wiping stale build/$preset"
    rm -rf "$script_dir/build/$preset"
  fi
fi

# `cmake --preset` reads CMakePresets.json from the CWD, so anchor to this
# script's own checkout. Without this, running <worktree>/cmake_build.sh from
# another directory silently configures THAT directory's tree instead.
cd "$script_dir"

echo "=== Configuring $preset ==="
cmake --preset "$preset"

echo "=== Building $preset ==="
cmake --build --preset "$preset"

out_cfg=$([ "$cfg" = "debug" ] && echo Debug || echo Release)
echo "=== Done: $script_dir/x64/$out_cfg/cflat ==="
