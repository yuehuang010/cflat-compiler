#!/usr/bin/env bash
# test.sh - Linux/WSL counterpart of test.bat.
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
# Unlike test.bat this does NOT run `cflat --init` (a bitcode-cache warmup, a
# pure optimization) - each worker parses the core closure fresh.
#
# SKIP list: tests that cannot pass on Linux because they exercise Windows-only
# functionality. These are TEST-CONTENT or unrelated-subsystem limitations, not
# core-library gaps (every portable core library compiles + runs on Linux):
#   - Win32 APIs called directly in the test body (os.windows.*, GetCurrentProcessId)
#   - C interop test CONTENT that is Windows-bound: a prebuilt Windows .lib
#     (test_c_interop/mathlib.lib), a .c that #includes <windows.h>
#     (test_reflect/test_collection_leaks via diagnostic/heap_audit.c), or CRT
#     header binding that redeclares a libc symbol cflat's POSIX runtime owns
#     (test_crt/stdlib.h posix_memalign). The C-interop MECHANISM itself works on
#     Linux now (.c compiled to ELF via clang, system headers bound from /usr/include
#     - see test_import_group, which runs).
#   - Windows-only features (WinMD, the Win32/console test suites)
#   - the FP-environment control (ftz/daz), POSIX-stubbed in thread.cb
#   - tests that assert Windows path separators or spawn Windows-only commands
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

if [ ! -x "$CFLAT" ]; then
  echo "cflat not found at $CFLAT - build it first:"
  echo "  cmake --preset linux-x64-${CONFIG,,} && cmake --build --preset linux-x64-${CONFIG,,}"
  exit 1
fi

rm -rf "$RES"; mkdir -p "$RES"

# Windows-only .cb tests - see header comment for the category of each.
SKIP="test_helper \
  test_basic test_socket test_threadpool test_stream \
  test_c_interop test_reflect test_collection_leaks test_crt \
  test_windows test_windows_cache test_winmd \
  test_fpenv \
  test_filesystem test_core test_process"

# Windows-only negative tests: they assert messages tied to Windows headers
# (windows.h / tlhelp32.h C-interop) or the Windows core externs (os.windows.fwrite),
# which don't exist on Linux.
ERR_SKIP="err_orphan_header err_c_struct_incomplete_by_value err_extern_collides_with_core"

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
  if ! timeout "$TIMEOUT_SECS" "$CFLAT" "$f" -i "$LIB" -o "$RES/$n.bin" >"$log" 2>&1; then
    echo "FAIL compile" >"$RES/$n.result"; return
  fi
  if timeout "$TIMEOUT_SECS" "$RES/$n.bin" </dev/null >>"$log" 2>&1; then
    echo "PASS" >"$RES/$n.result"
  else
    echo "FAIL run(rc=$?)" >"$RES/$n.result"
  fi
}

# Worker: an err_*.cb passes when the compiler exits 0 (expect_error matched).
run_err() {
  local f="$1" n; n="$(basename "$f" .cb)"
  if timeout "$TIMEOUT_SECS" "$CFLAT" "$f" -i "$LIB" --check >"$RES/$n.log" 2>&1; then
    echo "PASS" >"$RES/$n.result"
  else
    echo "FAIL" >"$RES/$n.result"
  fi
}

export -f run_cb run_err is_skipped
export CFLAT LIB RES TIMEOUT_SECS

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
echo "Linux ($CONFIG): $pass passed, $fail failed, $skip skipped (Windows-only)."
if [ "$fail" -ne 0 ]; then
  echo "FAILED:$failed_names"
  exit 1
fi
echo "All runnable tests passed."
