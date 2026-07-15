#!/usr/bin/env bash
# test.sh - POSIX (Linux/WSL + macOS arm64) counterpart of test.bat.
#
# Compiles and runs the platform-portable subset of Test/*.cb against the
# native ELF cflat built by `cmake --preset linux-x64-release` (deployed to
# x64/<Config>/cflat), plus the Test/errors/*.cb negative tests. Runs in
# parallel with a per-test timeout and prints a PASS/FAIL/SKIP summary.
#
# Usage:
#   ./test.sh            # Release (default)
#   ./test.sh Debug      # Debug
#   ./test.sh -j 8       # cap parallelism (default: nproc)
#
# Like test.bat, after the cold pass this runs `cflat --init` and re-runs the
# negative tests against the warm bitcode cache. --init reconstructs compiler
# state from a hand-written serializer (a second code path), so a field the
# analysis reads but that is missing from the serializer is silently dropped on a
# warm cache - the expect_error then stops firing. The warm pass catches that here
# instead of only on Windows. See
# internal/issue/init-cache-state-drop-invisible-on-posix.md.
#
# SKIP list: tests that cannot pass on Linux because they exercise Windows-only
# functionality. These are TEST-CONTENT or unrelated-subsystem limitations, not
# core-library gaps (every portable core library compiles + runs on Linux).
#
# Keep this list HONEST: a test whose only Windows tie is an incidental symbol does
# not belong here. test_basic, test_stream, test_process and test_core were each
# skipped for one such reason (a Win32 extern, os.windows.* stdio, a hardcoded `cmd`
# shell -- and test_core for no reason at all) and were hiding thousands of lines of
# portable coverage. Before adding a test here, prove the whole file is Windows-bound.
#
# C interop is NOT on this list: test_c_interop binds the mathlib fixture, which
# cinterop/build_mathlib.sh rebuilds from source above (the archive keeps its .lib name
# on every platform), and test_crt binds the system CRT headers straight from the SDK.
#
# Remaining, genuinely Windows-only:
#   - Windows-only features (WinMD, the Win32/console test suites)
set -u

CONFIG=Release
JOBS=$(nproc 2>/dev/null || echo 4)
while [ $# -gt 0 ]; do
  case "$1" in
    Debug|debug)     CONFIG=Debug ;;
    Release|release) CONFIG=Release ;;
    -j) shift; JOBS="$1" ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac
  shift
done

ROOT="$(cd "$(dirname "$0")" && pwd)"
CFLAT="$ROOT/x64/$CONFIG/cflat"
SRC="$ROOT/Test"
LIB="$ROOT/Test/library"
OUT="$ROOT/out-linux"
RES="$OUT/results"
TIMEOUT_SECS=120

# GNU coreutils timeout: `timeout` on Linux, `gtimeout` on macOS (brew coreutils).
# Fall back to no wrapper if neither exists so tests still run (just unbounded).
if command -v timeout >/dev/null 2>&1; then
  TIMEOUT="timeout $TIMEOUT_SECS"
elif command -v gtimeout >/dev/null 2>&1; then
  TIMEOUT="gtimeout $TIMEOUT_SECS"
else
  echo "warning: no timeout/gtimeout found (brew install coreutils on macOS); running without per-test timeout"
  TIMEOUT=""
fi

if [ ! -x "$CFLAT" ]; then
  echo "cflat not found at $CFLAT - build it first:"
  echo "  cmake --preset linux-x64-${CONFIG,,} && cmake --build --preset linux-x64-${CONFIG,,}"
  exit 1
fi

rm -rf "$RES"; mkdir -p "$RES"

# Pre-build the C-interop fixture archive so test_c_interop.cb can bind it (the
# counterpart to test.bat's build_mathlib.bat call).
if [ -x "$SRC/cinterop/build_mathlib.sh" ]; then
  "$SRC/cinterop/build_mathlib.sh" >/dev/null 2>&1 \
    || echo "WARNING: failed to build cinterop fixture lib - test_c_interop may fail"
fi

# Windows-only .cb tests - see header comment for the category of each.
SKIP="test_helper \
  test_windows test_windows_cache test_winmd"

# OS-specific skip: the per-thread FP environment is implemented on Windows (_controlfp_s
# -> MXCSR) and on macOS/arm64 (FPCR.FZ via fegetenv/fesetenv), but core/thread.cb's
# __fp_apply is still a no-op on Linux (x86 MXCSR not ported), so flush-to-zero cannot
# take effect there. See internal/issue/fpenv-not-ported-to-linux.md.
case "$(uname -s)" in
  Linux) SKIP="$SKIP test_fpenv" ;;
esac

# Architecture-specific skips: test_intrinsic asserts __X86__==1 and exercises the
# x86 RDTSCP/LFENCE/PAUSE intrinsics, so it cannot pass on arm64 (Apple Silicon).
case "$(uname -m)" in
  arm64|aarch64) SKIP="$SKIP test_intrinsic" ;;
esac

# Windows-only negative tests: they assert messages tied to Windows headers
# (windows.h / tlhelp32.h C-interop), the Windows core externs (os.windows.fwrite), or
# WinMD/WinRT metadata, none of which exist on Linux/macOS. A WinMD import is rejected
# outright off-Windows, so the expected error never gets a chance to fire.
ERR_SKIP="err_orphan_header err_c_struct_incomplete_by_value err_extern_collides_with_core \
  err_winmd_alias_collides_with_interface"

is_skipped() {
  for s in $SKIP; do [ "$1" = "$s" ] && return 0; done
  return 1
}
is_err_skipped() {
  for s in $ERR_SKIP; do [ "$1" = "$s" ] && return 0; done
  return 1
}

# Worker: compile (and for .cb run) one test, writing a one-line .result file.
run_cb() {
  local f="$1" n; n="$(basename "$f" .cb)"
  local log="$RES/$n.log"
  if ! $TIMEOUT "$CFLAT" "$f" -i "$LIB" -o "$RES/$n.bin" >"$log" 2>&1; then
    echo "FAIL compile" >"$RES/$n.result"; return
  fi
  if $TIMEOUT "$RES/$n.bin" </dev/null >>"$log" 2>&1; then
    echo "PASS" >"$RES/$n.result"
  else
    echo "FAIL run(rc=$?)" >"$RES/$n.result"
  fi
}

# Worker: an err_*.cb passes when the compiler exits 0 (expect_error matched).
run_err() {
  local f="$1" n; n="$(basename "$f" .cb)"
  if $TIMEOUT "$CFLAT" "$f" -i "$LIB" --check >"$RES/$n.log" 2>&1; then
    echo "PASS" >"$RES/$n.result"
  else
    echo "FAIL" >"$RES/$n.result"
  fi
}

# Warm-cache worker: same negative test, but run after `cflat --init`, so the
# compiler state comes from the bitcode-cache serializer rather than a fresh parse.
# A ".warm" suffix keeps its result/log distinct from the cold run's.
run_err_warm() {
  local f="$1" n; n="$(basename "$f" .cb).warm"
  if $TIMEOUT "$CFLAT" "$f" -i "$LIB" --check >"$RES/$n.log" 2>&1; then
    echo "PASS" >"$RES/$n.result"
  else
    echo "FAIL" >"$RES/$n.result"
  fi
}

export -f run_cb run_err run_err_warm is_skipped
export CFLAT LIB RES TIMEOUT

# Build the work list, then fan out across $JOBS workers via xargs -P.
cb_list=""
for f in "$SRC"/test_*.cb; do
  n="$(basename "$f" .cb)"
  is_skipped "$n" && continue
  cb_list="$cb_list$f"$'\n'
done
err_list=""
if [ -d "$SRC/errors" ]; then
  for f in "$SRC"/errors/err_*.cb; do
    n="$(basename "$f" .cb)"
    is_err_skipped "$n" && continue
    err_list="$err_list$f"$'\n'
  done
fi

printf '%s' "$cb_list"  | grep -v '^$' | xargs -P "$JOBS" -I{} bash -c 'run_cb "$@"' _ {}
printf '%s' "$err_list" | grep -v '^$' | xargs -P "$JOBS" -I{} bash -c 'run_err "$@"' _ {}

# Warm-cache pass: populate the --init bitcode cache, then re-run the negative
# tests against it. This exercises the serializer round-trip that test.bat covers
# on Windows but test.sh previously never did. See
# internal/issue/init-cache-state-drop-invisible-on-posix.md.
if "$CFLAT" --init >"$RES/_init.log" 2>&1; then
  printf '%s' "$err_list" | grep -v '^$' | xargs -P "$JOBS" -I{} bash -c 'run_err_warm "$@"' _ {}
else
  echo "FAIL: cflat --init crashed (warm-cache pass could not run)"
  echo "FAIL" >"$RES/_init.result"
  tail -n 8 "$RES/_init.log" 2>/dev/null
fi

# Collect.
pass=0; fail=0; failed_names=""
for r in "$RES"/*.result; do
  n="$(basename "$r" .result)"
  read -r status <"$r"
  if [ "$status" = "PASS" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); failed_names="$failed_names $n"
    echo "=== $n ==="; tail -n 8 "$RES/$n.log" 2>/dev/null
    echo "$status"
  fi
done

skip=0
for s in $SKIP;     do [ "$s" = "test_helper" ] || skip=$((skip+1)); done
for s in $ERR_SKIP; do skip=$((skip+1)); done

echo
echo "$(uname -s) ($CONFIG): $pass passed, $fail failed, $skip skipped (platform-specific)."
if [ "$fail" -ne 0 ]; then
  echo "FAILED:$failed_names"
  exit 1
fi
echo "All runnable tests passed."
