#!/usr/bin/env bash
# test.sh - POSIX (Linux/WSL + macOS arm64) counterpart of test.bat.
#
# Compiles and runs the platform-portable subset of Test/*.cb against the
# native ELF cflat built by `cmake --preset linux-x64-release` (deployed to
# x64/<Config>/cflat), plus the Test/errors/*.cb and explicit policy negatives.
# Runs in
# parallel with a per-test timeout and prints a PASS/FAIL/SKIP summary.
#
# Usage:
#   ./test.sh            # Release (default)
#   ./test.sh Debug      # Debug
#   ./test.sh -j 8       # cap parallelism (default: nproc)
#   ./test.sh --run      # opt-in in-process JIT smoke set
#
# Like test.bat, after the cold pass this runs `cflat --init-local` and re-runs the
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
RUN_MODE=0
while [ $# -gt 0 ]; do
  case "$1" in
    Debug|debug)     CONFIG=Debug ;;
    Release|release) CONFIG=Release ;;
    --run)           RUN_MODE=1 ;;
    -j) shift; JOBS="$1" ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac
  shift
done

SCRIPT_START=$(date +%s)
ROOT="$(cd "$(dirname "$0")" && pwd)"
CFLAT="$ROOT/x64/$CONFIG/cflat"
SRC="$ROOT/Test"
LIB="$ROOT/Test/library"
LOCALE_DIR="$ROOT/cflat/locales"
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

# Millisecond wall clock: bash's $SECONDS is too coarse and BSD date has no %N,
# so use perl Time::HiRes (present on macOS and Linux), falling back to seconds.
now_ms() {
  if command -v perl >/dev/null 2>&1; then
    perl -MTime::HiRes=time -e 'printf "%d", time()*1000'
  else
    echo $(( $(date +%s) * 1000 ))
  fi
}

# write_result <name> <status> <start_ms> - one-line .result plus a .time sidecar.
write_result() {
  local ms; ms=$(( $(now_ms) - $3 ))
  echo "$2" >"$RES/$1.result"
  echo "$ms" >"$RES/$1.time"
}

# Worker: compile (and for .cb run) one test, writing a one-line .result file.
run_cb() {
  local f="$1" n; n="$(basename "$f" .cb)"
  local log="$RES/$n.log" status t0; t0=$(now_ms)
  if [ "$RUN_MODE" -eq 1 ]; then
    if $TIMEOUT "$CFLAT" "$f" -i "$LIB" --locale-dir "$LOCALE_DIR" --run --nologo >"$log" 2>&1; then
      status="PASS"
    else
      status="FAIL run"
    fi
  elif ! $TIMEOUT "$CFLAT" "$f" -i "$LIB" --locale-dir "$LOCALE_DIR" -o "$RES/$n.bin" >"$log" 2>&1; then
    status="FAIL compile"
  elif $TIMEOUT "$RES/$n.bin" </dev/null >>"$log" 2>&1; then
    status="PASS"
  else
    status="FAIL run(rc=$?)"
  fi
  write_result "$n" "$status" "$t0"
}

load_err_flags() {
  local sidecar="$1" line value
  ERR_FLAGS=()
  ERR_EXPECT_EXIT=""
  ERR_EXPECT_OUTPUT=""
  if [ -f "$sidecar" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
      case "$line" in
        flags=*)
          value="${line#flags=}"
          local -a sidecar_flags=()
          read -r -a sidecar_flags <<< "$value"
          ERR_FLAGS+=("${sidecar_flags[@]}")
          ;;
        expect_exit=*) ERR_EXPECT_EXIT="${line#expect_exit=}" ;;
        expect_output=*) ERR_EXPECT_OUTPUT="${line#expect_output=}" ;;
      esac
    done <"$sidecar"
  fi
}

check_err_result() {
  local rc="$1" log="$2"
  if [ "$ERR_EXPECT_EXIT" = "nonzero" ]; then
    [ "$rc" -ne 0 ] && [ -n "$ERR_EXPECT_OUTPUT" ] \
      && grep -Fq -- "$ERR_EXPECT_OUTPUT" "$log"
    return
  fi
  [ "$rc" -eq 0 ]
}

# Worker: an err_*.cb passes when expect_error matches, or when its sidecar expects
# a nonzero exit and an output substring.
run_err() {
  local f="$1" n; n="$(basename "$f" .cb)"
  local log="$RES/$n.log" rc=0 t0; t0=$(now_ms)
  load_err_flags "$f.flags"
  $TIMEOUT "$CFLAT" "$f" -i "$LIB" --locale-dir "$LOCALE_DIR" --check \
    "${ERR_FLAGS[@]}" >"$log" 2>&1 || rc=$?
  if check_err_result "$rc" "$log"; then
    write_result "$n" "PASS" "$t0"
  else
    write_result "$n" "FAIL" "$t0"
  fi
}

# Warm-cache worker: same negative test, but run after `cflat --init`, so the
# compiler state comes from the bitcode-cache serializer rather than a fresh parse.
# A ".warm" suffix keeps its result/log distinct from the cold run's.
run_err_warm() {
  local f="$1" n; n="$(basename "$f" .cb).warm"
  local log="$RES/$n.log" rc=0 t0; t0=$(now_ms)
  load_err_flags "$f.flags"
  $TIMEOUT "$CFLAT" "$f" -i "$LIB" --locale-dir "$LOCALE_DIR" --check \
    "${ERR_FLAGS[@]}" >"$log" 2>&1 || rc=$?
  if check_err_result "$rc" "$log"; then
    write_result "$n" "PASS" "$t0"
  else
    write_result "$n" "FAIL" "$t0"
  fi
}

export -f run_cb load_err_flags check_err_result run_err run_err_warm is_skipped \
  now_ms write_result
export CFLAT LIB LOCALE_DIR RES TIMEOUT RUN_MODE

# The JIT path is deliberately opt-in. This list excludes fixtures that require a prebuilt C
# library or the C-backed HeapAudit oracle; those remain AOT-only by design. test_program is
# included because it exercises both CFlat and real-C imported program adapters.
RUN_TESTS="test_allocators test_basic test_bitmap test_c test_com test_core test_crt \
  test_filesystem test_fpenv test_function_ptr test_generics test_hpc test_hpc_kernels \
  test_import_group test_initializer_list test_interface test_linear_mat test_math test_module \
  test_move test_operators test_parallel test_process test_program test_random test_regex \
  test_socket test_stdio test_stream test_sync test_terminal test_threadpool test_time test_vectorize"

# Build the work list, then fan out across $JOBS workers via xargs -P.
cb_list=""
if [ "$RUN_MODE" -eq 1 ]; then
  for n in $RUN_TESTS; do
    f="$SRC/$n.cb"
    [ -f "$f" ] || { echo "missing opt-in --run fixture: $f"; exit 1; }
    is_skipped "$n" && continue
    cb_list="$cb_list$f"$'\n'
  done
else
  for f in "$SRC"/test_*.cb; do
    n="$(basename "$f" .cb)"
    is_skipped "$n" && continue
    cb_list="$cb_list$f"$'\n'
  done
fi
err_list=""
err_files=()
err_discovery_files=()
if [ -d "$SRC/errors" ]; then
  for f in "$SRC"/errors/err_*.cb; do
    n="$(basename "$f" .cb)"
    is_err_skipped "$n" && continue
    err_list="$err_list$f"$'\n'
    err_files+=("$f")
    err_discovery_files+=("$f")
  done
  if [ -d "$SRC/errors/policy" ]; then
    for f in "$SRC"/errors/policy/err_*.cb; do
      [ -f "$f" ] || continue
      n="$(basename "$f" .cb)"
      is_err_skipped "$n" && continue
      err_list="$err_list$f"$'\n'
      err_files+=("$f")
    done
  fi
fi

# Discovery pass: the error suite is the authoritative diagnostic inventory. Run it
# serially with pseudo output so expect_error continues to match source English while
# --update-locale records every template encountered by the batch.
if [ "$RUN_MODE" -eq 0 ] && [ "${#err_discovery_files[@]}" -gt 0 ]; then
  if ! "$CFLAT" --locale pseudo --update-locale en-pseudo --locale-dir "$LOCALE_DIR" \
      --check -i "$LIB" "${err_discovery_files[@]}" >"$RES/_locale.log" 2>&1; then
    echo "FAIL: pseudo-locale diagnostic discovery failed"
    tail -n 20 "$RES/_locale.log"
    exit 1
  fi
fi

# Policy fixtures carry their own --isolated sidecars, so discover their diagnostic
# templates one file at a time; expected failures are data, not discovery errors.
if [ "$RUN_MODE" -eq 0 ] && [ -d "$SRC/errors/policy" ]; then
  for f in "$SRC"/errors/policy/err_*.cb; do
    [ -f "$f" ] || continue
    load_err_flags "$f.flags"
    "$CFLAT" --locale pseudo --update-locale en-pseudo --locale-dir "$LOCALE_DIR" \
      --check -i "$LIB" "${ERR_FLAGS[@]}" "$f" >"$RES/$(basename "$f").locale.log" 2>&1 || true
  done
fi

printf '%s' "$cb_list"  | grep -v '^$' | xargs -P "$JOBS" -I{} bash -c 'run_cb "$@"' _ {}
if [ "$RUN_MODE" -eq 0 ]; then
  printf '%s' "$err_list" | grep -v '^$' | xargs -P "$JOBS" -I{} bash -c 'run_err "$@"' _ {}
fi

# The opt-in JIT smoke set also checks the deliberate prebuilt-library rejection. The fixture
# mixes an imported C source with a prebuilt package library; the latter must be diagnosed before
# JIT materialization rather than producing an unresolved-symbol list.
if [ "$RUN_MODE" -eq 1 ]; then
  rejected_name="run_prebuilt_c_rejected"
  rejected_log="$RES/$rejected_name.log"
  rejected_t0=$(now_ms)
  if $TIMEOUT "$CFLAT" "$SRC/test_c_interop.cb" -i "$LIB" --locale-dir "$LOCALE_DIR" \
      --run --nologo >"$rejected_log" 2>&1; then
    write_result "$rejected_name" "FAIL: prebuilt C library was accepted by --run" "$rejected_t0"
  elif grep -Fq "does not support prebuilt C libraries" "$rejected_log"; then
    write_result "$rejected_name" "PASS" "$rejected_t0"
  else
    write_result "$rejected_name" "FAIL: --run prebuilt-library diagnostic missing" "$rejected_t0"
  fi
fi

# Warm-cache pass: populate the --init-local bitcode cache, then re-run the negative
# tests against it. This exercises the serializer round-trip that test.bat covers
# on Windows but test.sh previously never did. See
# internal/issue/init-cache-state-drop-invisible-on-posix.md.
# --init-local (not --init) so the cache lands in <exe dir>/.cflat: two worktrees or a
# Debug/Release pair testing concurrently then cannot collide on one ~/.cflat, and the
# suite never overwrites the developer's own per-user cache.
if [ "$RUN_MODE" -eq 0 ]; then
  if "$CFLAT" --init-local >"$RES/_init.log" 2>&1; then
    printf '%s' "$err_list" | grep -v '^$' | xargs -P "$JOBS" -I{} bash -c 'run_err_warm "$@"' _ {}
  else
    echo "FAIL: cflat --init-local crashed (warm-cache pass could not run)"
    echo "FAIL" >"$RES/_init.result"
    tail -n 8 "$RES/_init.log" 2>/dev/null
  fi
fi

# Tooling regression: compile the existing function-pointer fixture with the ownership
# sanitizer, then verify a static-local move keeps both its runtime origin and DI record.
if [ "$RUN_MODE" -eq 0 ]; then
tooling_name="static_local_tooling"
tooling_log="$RES/$tooling_name.log"
tooling_bin="$RES/$tooling_name.bin"
tooling_ll="$RES/$tooling_name.ll"
run_tooling_probe() {
  $TIMEOUT sh -c 'trap "exit 134" ABRT; "$1" static-origin-check' sh "$tooling_bin" >>"$tooling_log" 2>&1
  return $?
}
tooling_t0=$(now_ms)
if ! $TIMEOUT "$CFLAT" "$SRC/test_function_ptr.cb" -i "$LIB" --locale-dir "$LOCALE_DIR" \
    --sanitize=ownership -o "$tooling_bin" --out-lli "$tooling_ll" >"$tooling_log" 2>&1; then
  write_result "$tooling_name" "FAIL compile" "$tooling_t0"
elif run_tooling_probe; then
  write_result "$tooling_name" "FAIL: sanitizer probe did not trap" "$tooling_t0"
elif ! grep -Fq "ownership violation: value moved at" "$tooling_log"; then
  write_result "$tooling_name" "FAIL: sanitizer probe lost the move origin" "$tooling_t0"
elif ! grep -Fq ".static.node.own_origin" "$tooling_ll" \
    || ! grep -Eq 'name: "node".*isLocal: true' "$tooling_ll"; then
  write_result "$tooling_name" "FAIL: static-local origin or debug metadata is missing" "$tooling_t0"
else
  write_result "$tooling_name" "PASS" "$tooling_t0"
fi
fi

# CLI regression: -D global defines. Covers the attached and separated spellings, an int
# and a string value, later-wins ordering, defines used as ordinary values and as
# compile-time constants in generic code, and the reserved-name rejection.
if [ "$RUN_MODE" -eq 0 ]; then
defines_name="cli_defines"
defines_log="$RES/$defines_name.log"
defines_off_log="$RES/$defines_name.off.log"
defines_ir="$RES/$defines_name.ll"
defines_t0=$(now_ms)
if ! $TIMEOUT "$CFLAT" "$SRC/cli_defines_fixture.cb" -i "$LIB" --locale-dir "$LOCALE_DIR" \
    -DCLI_DEF_ON=0 -DCLI_DEF_ON -D CLI_DEF_LEVEL=6 -DCLI_DEF_LEVEL=7 -DCLI_DEF_TAG=nightly -DCLI_SIMD_LANES=8 -DCAP=4 \
    --run --nologo >"$defines_log" 2>&1; then
  write_result "$defines_name" "FAIL: -D fixture did not compile or run" "$defines_t0"
elif ! grep -Fq "mode=on level=7 tag=nightly" "$defines_log"; then
  write_result "$defines_name" "FAIL: -D value or later-wins ordering is wrong" "$defines_t0"
elif ! grep -Fq "value sum=21 scaled=14" "$defines_log"; then
  write_result "$defines_name" "FAIL: -D constant did not fold as a value" "$defines_t0"
elif ! grep -Fq "generic cap=8 alias=8 count=2 pick=10" "$defines_log"; then
  write_result "$defines_name" "FAIL: -D constant did not reach generic code" "$defines_t0"
elif ! grep -Fq "value member=80 function=8 negative=-1 nested=8" "$defines_log"; then
  write_result "$defines_name" "FAIL: generic value parameter specialization is wrong" "$defines_t0"
elif ! grep -Fq "value bare=4 constarg=16" "$defines_log"; then
  write_result "$defines_name" "FAIL: a bare -D define or const global did not fold as a value argument" "$defines_t0"
elif ! grep -Fq "simd define=3 const=4" "$defines_log"; then
  write_result "$defines_name" "FAIL: -D or const global did not reach simd lane count" "$defines_t0"
elif ! $TIMEOUT "$CFLAT" "$SRC/cli_defines_fixture.cb" -i "$LIB" --locale-dir "$LOCALE_DIR" \
    -DCLI_DEF_ON=0 -DCLI_DEF_LEVEL=7 -DCLI_DEF_TAG=nightly -DCLI_SIMD_LANES=8 -DCAP=4 --run --nologo \
    >"$defines_off_log" 2>&1; then
  write_result "$defines_name" "FAIL: -D fixture did not run with the off define set" "$defines_t0"
elif ! grep -Fq "mode=off level=7 tag=nightly" "$defines_off_log" \
  || ! grep -Fq "generic cap=8 alias=8 count=2 pick=20" "$defines_off_log"; then
  write_result "$defines_name" "FAIL: flipping a -D did not reselect the if const arm" "$defines_t0"
elif ! $TIMEOUT "$CFLAT" "$SRC/cli_defines_fixture.cb" -i "$LIB" --locale-dir "$LOCALE_DIR" \
    -DCLI_DEF_ON=1 -DCLI_DEF_LEVEL=7 -DCLI_DEF_TAG=nightly -DCLI_SIMD_LANES=8 -DCAP=4 \
    --out-lli "$defines_ir" --nologo >>"$defines_log" 2>&1; then
  write_result "$defines_name" "FAIL: value-parameter IR probe did not compile" "$defines_t0"
elif [ "$(grep -c 'Buf\$int\$.8" = type' "$defines_ir")" -ne 1 ]; then
  write_result "$defines_name" "FAIL: equivalent folded values did not share one generic instantiation" "$defines_t0"
elif ! grep -Fq "simd define=3 const=4" "$defines_off_log"; then
  write_result "$defines_name" "FAIL: -D or const global did not reach simd lane count" "$defines_t0"
elif $TIMEOUT "$CFLAT" "$SRC/cli_defines_fixture.cb" -i "$LIB" --locale-dir "$LOCALE_DIR" \
    -D__MACOS__=0 --check >>"$defines_log" 2>&1; then
  write_result "$defines_name" "FAIL: -D redefined a builtin macro" "$defines_t0"
else
  write_result "$defines_name" "PASS" "$defines_t0"
fi
fi

# Collect. Matches test.bat's per-test output: "PASSED: <name>  [<elapsed>]".
pass=0; fail=0; failed_names=""
for r in "$RES"/*.result; do
  n="$(basename "$r" .result)"
  read -r status <"$r"
  elapsed=""
  if [ -f "$RES/$n.time" ]; then
    read -r ms <"$RES/$n.time"
    elapsed="$(awk -v ms="$ms" 'BEGIN{printf "%.2fs", ms/1000}')"
  fi
  if [ "$status" = "PASS" ]; then
    pass=$((pass+1))
    echo "PASSED: $n  [$elapsed]"
  else
    fail=$((fail+1)); failed_names="$failed_names $n"
    echo; echo "=== $n ==="; tail -n 8 "$RES/$n.log" 2>/dev/null
    echo "$status"
  fi
done

skip=0
for s in $SKIP;     do [ "$s" = "test_helper" ] || skip=$((skip+1)); done
for s in $ERR_SKIP; do skip=$((skip+1)); done

echo
echo "Elapsed: $(( $(date +%s) - SCRIPT_START ))s"
echo "$(uname -s) ($CONFIG): $pass passed, $fail failed, $skip skipped (platform-specific)."
if [ "$fail" -ne 0 ]; then
  echo "FAILED:$failed_names"
  exit 1
fi
echo "All runnable tests passed."
