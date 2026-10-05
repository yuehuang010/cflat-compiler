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
# Per-test compiler flags: a first line `// cflat-args: <flags>` in a Test/*.cb is read by
# cb_extra_args() below and appended to that test's compile command. Error fixtures use a
# `<name>.cb.flags` sidecar instead (flags=..., see run_err).
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
# 240: test_cpp_interop cold-compiles in ~117 s standalone (every C++ type request re-runs
# clang; see internal/issue/p2/cpp-interop-fixture-near-timeout.md). test.bat uses 600.
TIMEOUT_SECS=240
# The three cpp-interop files: cold compile measured 197-237 s on 2026-09-16/17 under suite load
# for the single file they were split from, so each gets its own budget instead of raising the
# global one (same issue file).
HEAVY_TIMEOUT_SECS=480
HEAVY_TESTS=" test_cpp_interop test_cpp_interop_template test_cpp_interop_bridge "

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

cpp_budget_enabled() {
  case "${CFLAT_CPP_INCREMENTAL:-}" in
    0|off|false) return 1 ;;
  esac
  return 0
}

is_cpp_interop_test() {
  case "$1" in
    test_cpp_interop*) return 0 ;;
  esac
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

# Per-test compiler flags: a Test/*.cb whose FIRST line is `// cflat-args: <flags>` is compiled
# with those extra flags appended. One line in the test itself, so the flag travels with the test
# instead of becoming a name-matched special case in this script.
cb_extra_args() {
  local first; first="$(head -n 1 "$1" 2>/dev/null)"
  case "$first" in
    '// cflat-args:'*) printf '%s' "${first#'// cflat-args:'}" ;;
  esac
}

# A FIRST line `// cflat-twin-args: <flags>` keeps the default compile and runs the test a second
# time with those flags (e.g. -O2), so one file is covered at both optimization levels.
cb_twin_args() {
  local first; first="$(head -n 1 "$1" 2>/dev/null)"
  case "$first" in
    '// cflat-twin-args:'*) printf '%s' "${first#'// cflat-twin-args:'}" ;;
  esac
}

# Worker: compile (and for .cb run) one test, writing a one-line .result file.
run_cb() {
  local f="$1" n; n="$(basename "$f" .cb)"
  local log="$RES/$n.log" status t0; t0=$(now_ms)
  local -a xargs_cb=()
  read -r -a xargs_cb <<< "$(cb_extra_args "$f")"
  if cpp_budget_enabled; then
    xargs_cb+=(--error-on-cpp-reparse)
  fi
  local TIMEOUT="$TIMEOUT"
  case "$HEAVY_TESTS" in *" $n "*) TIMEOUT="${TIMEOUT/$TIMEOUT_SECS/$HEAVY_TIMEOUT_SECS}" ;; esac
  if [ "$RUN_MODE" -eq 1 ]; then
    if $TIMEOUT "$CFLAT" "$f" -i "$LIB" --locale-dir "$LOCALE_DIR" \
        ${xargs_cb[@]+"${xargs_cb[@]}"} --run --nologo >"$log" 2>&1; then
      status="PASS"
    else
      status="FAIL run"
    fi
  elif ! $TIMEOUT "$CFLAT" "$f" -i "$LIB" --locale-dir "$LOCALE_DIR" \
        ${xargs_cb[@]+"${xargs_cb[@]}"} -o "$RES/$n.bin" >"$log" 2>&1; then
    status="FAIL compile"
  elif $TIMEOUT "$RES/$n.bin" </dev/null >>"$log" 2>&1; then
    status="PASS"
  else
    status="FAIL run(rc=$?)"
  fi
  local -a twin_cb=()
  read -r -a twin_cb <<< "$(cb_twin_args "$f")"
  if [ "$status" = "PASS" ] && [ "$RUN_MODE" -eq 0 ] && [ "${#twin_cb[@]}" -gt 0 ]; then
    if ! $TIMEOUT "$CFLAT" "$f" -i "$LIB" --locale-dir "$LOCALE_DIR" \
          ${xargs_cb[@]+"${xargs_cb[@]}"} "${twin_cb[@]}" -o "$RES/$n.twin.bin" >>"$log" 2>&1; then
      status="FAIL compile(${twin_cb[*]})"
    else
      $TIMEOUT "$RES/$n.twin.bin" </dev/null >>"$log" 2>&1
      local twin_rc=$?
      [ "$twin_rc" -eq 0 ] || status="FAIL run(${twin_cb[*]}, rc=$twin_rc)"
    fi
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
  local -a compiler_env=()
  local -a tu_check=()
  if [ "$n" = "err_cpp_header_parse_budget" ]; then
    compiler_env=(env CFLAT_CPP_MAX_HEADER_PARSES=0)
  elif cpp_budget_enabled; then
    tu_check=(--error-on-cpp-reparse)
  fi
  load_err_flags "$f.flags"
  $TIMEOUT "${compiler_env[@]}" "$CFLAT" "$f" -i "$LIB" --locale pseudo --locale-dir "$LOCALE_DIR" --check \
    ${tu_check[@]+"${tu_check[@]}"} "${ERR_FLAGS[@]}" >"$log" 2>&1 || rc=$?
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
  local -a compiler_env=()
  if [ "${n%.warm}" = "err_cpp_header_parse_budget" ]; then
    compiler_env=(env CFLAT_CPP_MAX_HEADER_PARSES=0)
  fi
  load_err_flags "$f.flags"
  $TIMEOUT "${compiler_env[@]}" "$CFLAT" "$f" -i "$LIB" --locale pseudo --locale-dir "$LOCALE_DIR" --check \
    "${ERR_FLAGS[@]}" >"$log" 2>&1 || rc=$?
  if check_err_result "$rc" "$log"; then
    write_result "$n" "PASS" "$t0"
  else
    write_result "$n" "FAIL" "$t0"
  fi
}

export -f run_cb cb_extra_args cb_twin_args load_err_flags check_err_result run_err run_err_warm \
  is_cpp_interop_test cpp_budget_enabled is_skipped \
  now_ms write_result
export CFLAT LIB LOCALE_DIR RES TIMEOUT RUN_MODE TIMEOUT_SECS HEAVY_TIMEOUT_SECS HEAVY_TESTS

# The JIT path is deliberately opt-in. This list excludes fixtures that require a prebuilt C
# library or the C-backed HeapAudit oracle; those remain AOT-only by design. The macOS JIT
# keeps its image mapped until host exit, so test_program is included for its program and
# real-C imported program adapters.
RUN_TESTS="test_allocators test_basic test_bitmap test_c test_com test_core test_crt \
  test_cpp_interop test_filesystem test_fpenv test_function_ptr test_generics test_hpc test_hpc_kernels \
  test_import_group test_initializer_list test_interface test_linear_mat test_math test_module \
  test_operators test_parallel test_process test_program test_random test_regex \
  test_socket test_stdio test_stream test_sync test_terminal test_threadpool test_time test_vectorize"

# Build the work list, then fan out across $JOBS workers via xargs -P.
cb_list=""
cpp_list=""
if [ "$RUN_MODE" -eq 1 ]; then
  for n in $RUN_TESTS; do
    f="$SRC/$n.cb"
    [ -f "$f" ] || { echo "missing opt-in --run fixture: $f"; exit 1; }
    is_skipped "$n" && continue
    if is_cpp_interop_test "$n"; then cpp_list="$cpp_list$f"$'\n';
    else cb_list="$cb_list$f"$'\n'; fi
  done
else
  for f in "$SRC"/test_*.cb; do
    n="$(basename "$f" .cb)"
    is_skipped "$n" && continue
    if is_cpp_interop_test "$n"; then cpp_list="$cpp_list$f"$'\n';
    else cb_list="$cb_list$f"$'\n'; fi
  done
fi
err_list=""
err_files=()
err_discovery_files=()
err_sidecar_files=()
if [ -d "$SRC/errors" ]; then
  for f in "$SRC"/errors/err_*.cb; do
    n="$(basename "$f" .cb)"
    is_err_skipped "$n" && continue
    err_list="$err_list$f"$'\n'
    err_files+=("$f")
    # A fixture with a `.cb.flags` sidecar is discovered one at a time below, with its flags.
    if [ -f "$f.flags" ]; then
      err_sidecar_files+=("$f")
    elif [ "$n" != "err_cpp_header_parse_budget" ]; then
      err_discovery_files+=("$f")
    fi
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

if [ "$RUN_MODE" -eq 0 ] && [ -f "$SRC/errors/err_cpp_header_parse_budget.cb" ]; then
  if ! CFLAT_CPP_MAX_HEADER_PARSES=0 "$CFLAT" --locale pseudo --update-locale en-pseudo \
      --locale-dir "$LOCALE_DIR" --check -i "$LIB" \
      "$SRC/errors/err_cpp_header_parse_budget.cb" >"$RES/_header_budget_locale.log" 2>&1; then
    echo "FAIL: header-parse budget diagnostic discovery failed"
    tail -n 20 "$RES/_header_budget_locale.log"
    exit 1
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

# Sidecar fixtures (every policy fixture, plus any top-level err_*.cb with a `.cb.flags`) carry
# their own flags, so discover their diagnostic templates one file at a time; expected failures
# are data, not discovery errors.
if [ "$RUN_MODE" -eq 0 ]; then
  for f in "${err_sidecar_files[@]}" "$SRC"/errors/policy/err_*.cb; do
    [ -f "$f" ] || continue
    load_err_flags "$f.flags"
    "$CFLAT" --locale pseudo --update-locale en-pseudo --locale-dir "$LOCALE_DIR" \
      --check -i "$LIB" "${ERR_FLAGS[@]}" "$f" >"$RES/$(basename "$f").locale.log" 2>&1 || true
  done
fi

# C++ interop fixtures go first so the longest tests start first. One run each under the shared
# header cache: --error-on-cpp-reparse still fails any second parse of a header.
printf '%s%s' "$cpp_list" "$cb_list" | grep -v '^$' | xargs -P "$JOBS" -I{} bash -c 'run_cb "$@"' _ {}
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

# Warm/cold parity of global default-construction folding. A core constructor arrives LAZY from
# the bitcode cache, so a fold that reads its body must materialize it; when it does not, a global
# silently drops to zero-init and warns only under a warm cache. Compare the warning count of the
# same file compiled cold (an empty CFLAT_CACHE_DIR) and warm (the --init-local cache above).
if [ "$RUN_MODE" -eq 0 ]; then
  fold_name="global_default_fold_warm_parity"
  fold_log="$RES/$fold_name.log"
  fold_cold_cache="$RES/$fold_name.coldcache"
  fold_t0=$(now_ms)
  rm -rf "$fold_cold_cache"
  mkdir -p "$fold_cold_cache"
  CFLAT_CACHE_DIR="$fold_cold_cache" $TIMEOUT "$CFLAT" "$SRC/test_move.cb" -i "$LIB" \
    --check -v --locale en --locale-dir "$LOCALE_DIR" >"$fold_log" 2>&1
  fold_cold=$(grep -c "zero-initialized" "$fold_log")
  fold_warm_log="$RES/$fold_name.warm.log"
  CFLAT_CACHE_DIR="$(dirname "$CFLAT")/.cflat" $TIMEOUT "$CFLAT" "$SRC/test_move.cb" -i "$LIB" \
    --check -v --locale en --locale-dir "$LOCALE_DIR" >"$fold_warm_log" 2>&1
  fold_warm=$(grep -c "zero-initialized" "$fold_warm_log")
  rm -rf "$fold_cold_cache"
  if ! grep -q "core bitcode cache: miss" "$fold_log"; then
    write_result "$fold_name" "FAIL: cold leg reused a bitcode cache" "$fold_t0"
  elif ! grep -q "core bitcode cache: hit" "$fold_warm_log"; then
    write_result "$fold_name" "FAIL: warm leg did not hit the bitcode cache" "$fold_t0"
  elif [ "$fold_cold" = "$fold_warm" ]; then
    write_result "$fold_name" "PASS" "$fold_t0"
  else
    write_result "$fold_name" \
      "FAIL: zero-init warnings cold=$fold_cold warm=$fold_warm" "$fold_t0"
  fi
fi

# C++ request cache consistency across compiles sharing one cache. (1) A store for program b
# must not prune program a's still-valid entries once their markers are past the prune grace
# (the prune used to delete by age alone, under concurrent readers). (2) An entry stamped by
# another compiler build must be a miss ("compiler build"), and the compile still succeeds.
# (3)/(4) demand replay across compiles; (5) a store by ANOTHER build must not prune either;
# (6) with CFLAT_CACHE_BUILD_STAMP unset (default) entries survive a rebuild. Only (2) and (5)
# set the switch. (7) an edit to a header reached only through another import's #include misses
# that import's entries and demand companion, repeated edits do not grow the request entries,
# and a touch-only change still hits. (8) a same-stamp primary-header edit invalidates the
# demand companion even when no type request independently hashes the header. (9) a poisoned
# body verdict from one program does not leak into another through a shared request entry.
if [ "$RUN_MODE" -eq 0 ]; then
  cc_name="cxx_request_cache_consistency"
  cc_dir="$RES/$cc_name.d"
  cc_log="$RES/$cc_name.log"
  cc_t0=$(now_ms)
  rm -rf "$cc_dir"; mkdir -p "$cc_dir/cache"
  [ -d "$(dirname "$CFLAT")/.cflat/runtime" ] && cp -R "$(dirname "$CFLAT")/.cflat/runtime" "$cc_dir/cache/"
  printf '%s\n' 'import cpp "vector";' 'int main()' '{' '    std.vector<int> v = default;' \
    '    v.push_back(40);' '    v.push_back(2);' '    return v[0] + v[1] == 42 ? 0 : 1;' '}' \
    >"$cc_dir/a.cb"
  printf '%s\n' 'import cpp "vector";' 'int main()' '{' '    std.vector<double> v = default;' \
    '    v.push_back(40.0);' '    v.push_back(2.0);' '    return (int)(v[0] + v[1]) == 42 ? 0 : 1;' '}' \
    >"$cc_dir/b.cb"
  cc_compile() {
    CFLAT_CACHE_DIR="$cc_dir/cache" $TIMEOUT "$CFLAT" "$cc_dir/$1.cb" -B -o "$cc_dir/$1.bin" "${@:3}" \
      >"$cc_dir/$2.log" 2>&1 && "$cc_dir/$1.bin"
  }
  cc_fail=""
  if ! cc_compile a a_cold || ! cc_compile a a_ctl -v; then cc_fail="compile of a failed"
  else
    # Probes that are never stored miss on every run; the control counts them.
    cc_ctl="$(grep -c "request cache MISS" "$cc_dir/a_ctl.log")/$(grep -c "request cache HIT" "$cc_dir/a_ctl.log")"
    perl -e 'my $t = time - 1200; utime $t, $t, @ARGV' "$cc_dir"/cache/cheaders/v*/*.rq
    if ! cc_compile b b_store; then cc_fail="compile of b failed"
    elif ! cc_compile a a_warm -v; then cc_fail="warm compile of a failed"
    else
      cc_now="$(grep -c "request cache MISS" "$cc_dir/a_warm.log")/$(grep -c "request cache HIT" "$cc_dir/a_warm.log")"
      if [ "$cc_now" != "$cc_ctl" ] || [ "${cc_ctl#*/}" = 0 ]; then
        cc_fail="b's store pruned a's live entries (a warm misses/hits: $cc_now, control $cc_ctl)"
      else
        # (2) runs with the build-stamp switch on (default off): restamp every entry, then forge one.
        if ! CFLAT_CACHE_BUILD_STAMP=1 cc_compile a a_stamped; then cc_fail="compile of a with the build stamp on failed"
        else
          cc_entry=$(grep -l '"cxxRequestKey":"|RQstd::vector<int>' "$cc_dir"/cache/cheaders/v*/*.json | head -n 1)
          perl -pi -e 's/"cstamp":"[^"]*"/"cstamp":"another-build"/' "$cc_entry"
        fi
        if [ -n "$cc_fail" ]; then :
        elif ! CFLAT_CACHE_BUILD_STAMP=1 cc_compile a a_mixed -v; then cc_fail="compile of a over a foreign-build entry failed"
        elif [ "$(grep -c 'request cache MISS for std::vector<int>.*(compiler build)' "$cc_dir/a_mixed.log")" != 1 ]; then
          cc_fail="an entry stamped by another build was not exactly one 'compiler build' miss"
        else
          # (3) Entries stored by two cold compiles (separate caches, merged) replay into one
          # interpreter for a program needing both: their chunk names must not collide.
          mkdir -p "$cc_dir/m1" "$cc_dir/m2"
          [ -d "$cc_dir/cache/runtime" ] && cp -R "$cc_dir/cache/runtime" "$cc_dir/m1/" \
            && cp -R "$cc_dir/cache/runtime" "$cc_dir/m2/"
          printf '%s\n' 'import cpp "vector";' 'int main()' '{' '    std.vector<int> v = default;' \
            '    v.push_back(40);' '    v.push_back(2);' '    std.vector<double> w = default;' \
            '    w.push_back(40.0);' '    w.push_back(2.0);' \
            '    return v[0] + v[1] == 42 && (int)(w[0] + w[1]) == 42 ? 0 : 1;' '}' >"$cc_dir/ab.cb"
          if ! (CFLAT_CACHE_DIR="$cc_dir/m1" $TIMEOUT "$CFLAT" "$cc_dir/a.cb" -B -o "$cc_dir/a.bin" \
                  >"$cc_dir/m1.log" 2>&1 \
                && CFLAT_CACHE_DIR="$cc_dir/m2" $TIMEOUT "$CFLAT" "$cc_dir/b.cb" -B -o "$cc_dir/b.bin" \
                  >"$cc_dir/m2.log" 2>&1); then
            cc_fail="cold compile into a separate cache failed"
          else
            for cc_f in "$cc_dir"/m2/cheaders/*/*.json "$cc_dir"/m2/cheaders/*/*.rq; do
              cc_t="$cc_dir/m1/cheaders/${cc_f#"$cc_dir"/m2/cheaders/}"
              [ -e "$cc_t" ] || cp "$cc_f" "$cc_t"
            done
            if ! CFLAT_CACHE_DIR="$cc_dir/m1" $TIMEOUT "$CFLAT" "$cc_dir/ab.cb" -B -o "$cc_dir/ab.bin" -v \
                >"$cc_dir/ab.log" 2>&1 || ! "$cc_dir/ab.bin"; then
              cc_fail="program over merged entries failed"
            elif grep -q "demand replay failed" "$cc_dir/ab.log"; then
              cc_fail="replaying two compiles' entries failed: $(grep -o 'demand replay failed ([^)]*' "$cc_dir/ab.log" | head -n 1)"
            elif ! grep -q "replayed [0-9]* cached chunk" "$cc_dir/ab.log"; then
              cc_fail="program over merged entries did not replay (leg is vacuous)"
            else
              # (4) p1 stores two requests sharing a [cpp] struct prefix (the second chunk parsed
              # with that prefix already seen); p2 needs only the second. Its replay must still
              # declare the struct ('undeclared identifier __cflat_user' before the fix).
              mkdir -p "$cc_dir/m3"
              [ -d "$cc_dir/cache/runtime" ] && cp -R "$cc_dir/cache/runtime" "$cc_dir/m3/"
              printf '%s\n' 'import cpp "vector";' '[cpp] struct Leaf' '{' '    int v = 0;' '};' \
                'int main()' '{' '    std.vector<Leaf> a = default;' '    Leaf one = default;' \
                '    one.v = 40;' '    Leaf two = default;' '    two.v = 40;' '    a.push_back(move two);' \
                '    std.vector<Leaf*> b = default;' '    b.push_back(&one);' \
                '    return a[0].v + 2 == 42 && b[0].v == 40 && b.size() == 1 ? 0 : 1;' '}' >"$cc_dir/p1.cb"
              printf '%s\n' 'import cpp "vector";' '[cpp] struct Leaf' '{' '    int v = 0;' '};' \
                'int main()' '{' '    Leaf one = default;' '    one.v = 40;' \
                '    std.vector<Leaf*> b = default;' '    b.push_back(&one);' \
                '    return b[0].v + 2 == 42 && b.size() == 1 ? 0 : 1;' '}' >"$cc_dir/p2.cb"
              if ! CFLAT_CACHE_DIR="$cc_dir/m3" $TIMEOUT "$CFLAT" "$cc_dir/p1.cb" -B -o "$cc_dir/p1.bin" \
                  >"$cc_dir/p1.log" 2>&1 || ! "$cc_dir/p1.bin"; then
                cc_fail="cold compile of p1 failed"
              elif ! CFLAT_CACHE_DIR="$cc_dir/m3" $TIMEOUT "$CFLAT" "$cc_dir/p2.cb" -B -o "$cc_dir/p2.bin" -v \
                  >"$cc_dir/p2.log" 2>&1 || ! "$cc_dir/p2.bin"; then
                cc_fail="p2 over p1's entries failed"
              elif grep -q "demand replay failed" "$cc_dir/p2.log"; then
                cc_fail="replaying a subset of p1's chunks failed: $(grep -o 'demand replay failed ([^)]*' "$cc_dir/p2.log" | head -n 1)"
              elif ! grep -q "replayed [0-9]* cached chunk" "$cc_dir/p2.log"; then
                cc_fail="p2 did not replay p1's chunks (leg is vacuous)"
              else
                # (5) A second cflat build (a copy of the exe: new mtime, so a new build stamp)
                # stores into the group with the stamp switch on; a's aged, still-valid entries
                # must survive its prune. (6) With the switch off (default), a's entries must
                # survive a rebuild: the second build compiles a warm.
                mkdir -p "$cc_dir/alt" "$cc_dir/m4" "$cc_dir/m5"
                for cc_f in "$(dirname "$CFLAT")"/*; do
                  [ "$cc_f" = "$CFLAT" ] || ln -s "$cc_f" "$cc_dir/alt/"
                done
                cp -c "$CFLAT" "$cc_dir/alt/" 2>/dev/null || cp "$CFLAT" "$cc_dir/alt/"
                touch "$cc_dir/alt/$(basename "$CFLAT")"
                cc_alt="$cc_dir/alt/$(basename "$CFLAT")"
                # A private copy for build A: a rebuild or touch of $CFLAT mid-check would restamp A.
                mkdir -p "$cc_dir/prim"
                for cc_f in "$(dirname "$CFLAT")"/*; do
                  [ "$cc_f" = "$CFLAT" ] || ln -s "$cc_f" "$cc_dir/prim/"
                done
                cp -p "$CFLAT" "$cc_dir/prim/"; cc_prim="$cc_dir/prim/$(basename "$CFLAT")"
                [ -d "$cc_dir/cache/runtime" ] && cp -R "$cc_dir/cache/runtime" "$cc_dir/m4/" \
                  && cp -R "$cc_dir/cache/runtime" "$cc_dir/m5/"
                cc_run() { # <cache> <exe> <program> <log> [args]
                  CFLAT_CACHE_DIR="$cc_dir/$1" $TIMEOUT "$2" "$cc_dir/$3.cb" -B -o "$cc_dir/$3.bin" "${@:5}" \
                    >"$cc_dir/$4.log" 2>&1 && "$cc_dir/$3.bin"
                }
                cc_counts() { echo "$(grep -c "request cache MISS" "$cc_dir/$1.log")/$(grep -c "request cache HIT" "$cc_dir/$1.log")"; }
                export CFLAT_CACHE_BUILD_STAMP=1
                if ! cc_run m4 "$cc_prim" a x_cold || ! cc_run m4 "$cc_prim" a x_ctl -v; then
                  cc_fail="compile of a into the cross-build cache failed"
                else
                  cc_ctl=$(cc_counts x_ctl)
                  perl -e 'my $t = time - 1200; utime $t, $t, @ARGV' "$cc_dir"/m4/cheaders/v*/*.rq
                  if ! cc_run m4 "$cc_alt" b x_store; then
                    cc_fail="compile of b by a second build failed"
                  elif ! cc_run m4 "$cc_prim" a x_warm -v; then
                    cc_fail="warm compile of a after the second build's store failed"
                  else
                    cc_now=$(cc_counts x_warm)
                    if [ "$cc_now" != "$cc_ctl" ] || [ "${cc_ctl#*/}" = 0 ]; then
                      cc_fail="another build's store pruned a's live entries (a warm misses/hits: $cc_now, control $cc_ctl)"
                    fi
                  fi
                fi
                unset CFLAT_CACHE_BUILD_STAMP
                if [ -n "$cc_fail" ]; then :
                elif ! cc_run m5 "$CFLAT" a y_cold || ! cc_run m5 "$CFLAT" a y_ctl -v; then
                  cc_fail="compile of a with the build stamp off failed"
                elif ! cc_run m5 "$cc_alt" a y_rebuilt -v; then
                  cc_fail="compile of a by a rebuilt cflat failed"
                else
                  cc_ctl=$(cc_counts y_ctl); cc_now=$(cc_counts y_rebuilt)
                  if [ "$cc_now" != "$cc_ctl" ] || [ "${cc_ctl#*/}" = 0 ] \
                      || grep -q "compiler build" "$cc_dir/y_rebuilt.log"; then
                    cc_fail="with the build stamp off a rebuild went cold (misses/hits: $cc_now, control $cc_ctl)"
                  fi
                fi
                # (7) An edit to a header the import only INCLUDES (a.h, under h2.h / h3.h) must
                # miss the requests and demand companions keyed on the importing groups. Keyed on
                # the top-level headers only, a warm compile replayed the old bodies: (7a) a stale
                # value, (7b) a definition strong before the edit colliding with the one now
                # emitted inline ("symbol multiply defined").
                cc_hdr() { # <dir> <inline|""> <bump>
                  printf '%s\n' '#pragma once' 'namespace ccinc' '{' \
                    '    struct R { int v = 0; R() = default; R(int x) : v(x) {} };' \
                    "    $2 R& operator*=(R& a, int k) { a.v = a.v * k + $3; return a; }" '}' >"$cc_dir/$1/a.h"
                }
                if [ -n "$cc_fail" ]; then :
                else
                  for cc_s in m6 m7; do
                    mkdir -p "$cc_dir/$cc_s"
                    [ -d "$cc_dir/cache/runtime" ] && cp -R "$cc_dir/cache/runtime" "$cc_dir/$cc_s/"
                    printf '%s\n' '#pragma once' '#include "a.h"' \
                      'namespace ccinc2 { template <class T> struct P2 { T v; P2(T x) : v(x) {} T get() const { return v * 2; } }; }' \
                      >"$cc_dir/$cc_s/h2.h"
                    printf '%s\n' '#pragma once' '#include "a.h"' \
                      'namespace ccinc3 { template <class T> struct P3 { T v; P3(T x) : v(x) {} T get() const { return v * 3; } }; }' \
                      >"$cc_dir/$cc_s/h3.h"
                  done
                  cc_inc() { # <dir> <expected r.v> <log> [-v]
                    printf '%s\n' 'import cpp "h2.h";' 'import cpp "h3.h";' 'int main()' '{' \
                      '    ccinc2.P2<int> p2 = ccinc2.P2<int>(1);' '    ccinc3.P3<int> p3 = ccinc3.P3<int>(1);' \
                      '    ccinc.R r = ccinc.R(10);' '    r *= 4;' \
                      "    return p2.get() + p3.get() == 5 && r.v == $2 ? 0 : 1;" '}' >"$cc_dir/$1/inc.cb"
                    CFLAT_CACHE_DIR="$cc_dir/$1" $TIMEOUT "$CFLAT" "$cc_dir/$1/inc.cb" -B -o "$cc_dir/$1/inc.bin" \
                      ${4:-} >"$cc_dir/$3.log" 2>&1 && "$cc_dir/$1/inc.bin"
                  }
                  cc_rq() { ls "$cc_dir"/m6/cheaders/v*/*.rq 2>/dev/null | wc -l | tr -d ' '; }
                  cc_age() { perl -e 'my $t = time - 1200; utime $t, $t, @ARGV' "$cc_dir"/m6/cheaders/v*/*.rq "$cc_dir"/m6/cheaders/v*/*.json; }
                  cc_req() { grep -c "request cache $1" "$cc_dir/$2.log"; }
                  cc_hdr m6 inline 1
                  if ! cc_inc m6 41 z_seed; then cc_fail="(7a) cold compile over an included header failed"
                  else
                    sleep 1; cc_hdr m6 inline 2
                    if ! cc_inc m6 42 z_edit; then
                      cc_fail="(7a) a warm compile ignored an edit to an included header (stale value or compile failure)"
                    # Past the prune grace, further included-header edits rewrite the same request
                    # entries (the closure is keyed on content) or prune the dead ones: no growth.
                    elif cc_n=$(cc_rq); cc_age; sleep 1; cc_hdr m6 inline 3; ! cc_inc m6 43 z_edit2; then
                      cc_fail="(7a) second included-header edit gave a stale value or failed"
                    elif cc_age; sleep 1; cc_hdr m6 inline 4; ! cc_inc m6 44 z_edit3; then
                      cc_fail="(7a) third included-header edit gave a stale value or failed"
                    elif [ "$(cc_rq)" -gt "$cc_n" ]; then
                      cc_fail="(7a) request entries grew across included-header edits ($cc_n -> $(cc_rq) .rq)"
                    # A touch that leaves the content alone must hit exactly like an unchanged run.
                    elif ! cc_inc m6 44 z_same -v || { sleep 1; touch "$cc_dir/m6/a.h"; ! cc_inc m6 44 z_touch -v; }; then
                      cc_fail="(7a) warm or touch-only compile failed"
                    elif [ "$(cc_req HIT z_same)" -eq 0 ] || [ "$(cc_req HIT z_touch)" -ne "$(cc_req HIT z_same)" ] \
                         || [ "$(cc_req MISS z_touch)" -ne "$(cc_req MISS z_same)" ]; then
                      cc_fail="(7a) touching an included header missed the request cache (hit $(cc_req HIT z_touch) vs $(cc_req HIT z_same))"
                    else
                      # The strong seed does not link (one TU per import line); it only stores.
                      cc_hdr m7 "" 1; cc_inc m7 41 z_strong
                      sleep 1; cc_hdr m7 inline 1
                      if ! cc_inc m7 41 z_inline; then
                        cc_fail="(7b) strong -> inline edit of an included header failed warm: $(grep -o 'Linking globals[^!]*!' "$cc_dir/z_inline.log" | head -n 1)"
                      elif ! grep -q "multiply defined" "$cc_dir/z_strong.log"; then
                        cc_fail="(7b) the strong seed linked, so it stored no strong body (leg is vacuous)"
                      fi
                    fi
                  fi
                fi
              fi
            fi
          fi
        fi
      fi
    fi
  fi
  if [ -z "$cc_fail" ]; then
    mkdir -p "$cc_dir/free/cache"
    [ -d "$cc_dir/cache/runtime" ] && cp -R "$cc_dir/cache/runtime" "$cc_dir/free/cache/"
    printf '%s\n' '#pragma once' 'namespace ccfree { inline int value() { return 1; } }' \
      >"$cc_dir/free/free.h"
    printf '%s\n' 'import cpp "free.h";' 'int main() { return ccfree.value(); }' \
      >"$cc_dir/free/free.cb"
    cc_free() {
      CFLAT_CACHE_DIR="$cc_dir/free/cache" $TIMEOUT "$CFLAT" "$cc_dir/free/free.cb" -B -v \
        -o "$cc_dir/free/free.bin" >"$cc_dir/free_$1.log" 2>&1 || return 1
      "$cc_dir/free/free.bin"
      local cc_rc="$?"
      [ "$cc_rc" -eq "$2" ]
    }
    if ! cc_free seed 1 || ! cc_free warm 1; then
      cc_fail="(8) free-function demand companion seed or warm compile failed"
    elif ! grep -q 'full header parse count: 0' "$cc_dir/free_warm.log"; then
      cc_fail="(8) the unchanged free-function compile reparsed (leg is vacuous)"
    elif ! python3 -c 'import os,sys; p=sys.argv[1]; s=os.stat(p); b=open(p,"rb").read(); b=b.replace(b"return 1;",b"return 2;"); assert len(b)==s.st_size; open(p,"wb").write(b); os.utime(p,ns=(s.st_atime_ns,s.st_mtime_ns))' \
        "$cc_dir/free/free.h"; then
      cc_fail="(8) could not preserve the edited header stamp"
    elif ! cc_free edited 2; then
      cc_fail="(8) a same-size, same-mtime primary-header edit replayed a stale free-function body"
    fi
  fi
  # (9) A body that failed for program a's [cpp] Key must not be replayed as poisoned into
  # program b (a different Key) through an unrelated request entry both programs share.
  if [ -z "$cc_fail" ]; then
    mkdir -p "$cc_dir/pz/cache"
    [ -d "$cc_dir/cache/runtime" ] && cp -R "$cc_dir/cache/runtime" "$cc_dir/pz/cache/"
    printf '%s\n' '#pragma once' 'namespace pz {' 'struct HasV { int v_ = 4; };' \
      'struct NoV { int w_ = 4; };' \
      'template<class T> struct Obs { int n = 0; int observe(const T& k) { return k.v_ + 1; } };' \
      'struct Foo { int x = 3; int get() { return x; } };' '}' >"$cc_dir/pz/pz.h"
    printf '%s\n' 'import cpp "pz.h";' '[cpp] struct Key : pz.NoV { };' 'extern int main()' '{' \
      '    pz.Obs<Key> o = default;' '    Key k = default;' \
      '    expect_error("cannot be instantiated") {' '        int r = o.observe(k);' '    }' \
      '    pz.Foo f = default;' '    return f.get();' '}' >"$cc_dir/pz/a.cb"
    printf '%s\n' 'import cpp "pz.h";' '[cpp] struct Key : pz.HasV { };' 'extern int main()' '{' \
      '    pz.Obs<Key> o = default;' '    Key k = default;' '    pz.Foo f = default;' \
      '    int r = o.observe(k);' '    return f.get() + r;' '}' >"$cc_dir/pz/b.cb"
    cc_pz() {
      CFLAT_CACHE_DIR="$cc_dir/pz/cache" $TIMEOUT "$CFLAT" "$cc_dir/pz/$1.cb" -B \
        -o "$cc_dir/pz/$1.bin" >"$cc_dir/pz_$1.log" 2>&1 || return 1
      "$cc_dir/pz/$1.bin"
      local cc_rc="$?"
      [ "$cc_rc" -eq "$2" ]
    }
    if ! cc_pz a 3; then cc_fail="(9) program a (failing body under expect_error) failed"
    elif ! cc_pz b 8; then
      cc_fail="(9) program b replayed program a's poisoned body verdict from a shared entry"
    fi
  fi
  # (10) Two programs with different function demand sets share one cache. Their values must
  # match clang++ on cold and warm compiles.
  if [ -z "$cc_fail" ]; then
    mkdir -p "$cc_dir/dup/cache"
    [ -d "$cc_dir/cache/runtime" ] && cp -R "$cc_dir/cache/runtime" "$cc_dir/dup/cache/"
    printf '%s\n' '#pragma once' 'namespace cctwoprog {' \
      'int f() { return 40; }' 'int g() { return 2; }' '}' >"$cc_dir/dup/shared.h"
    printf '%s\n' 'import cpp "shared.h";' 'extern int main() {' \
      '    printf("%d\n", cctwoprog.f() + 1);' '    return 0;' '}' >"$cc_dir/dup/a.cb"
    printf '%s\n' 'import cpp "shared.h";' 'extern int main() {' \
      '    printf("%d\n", cctwoprog.f() + cctwoprog.g());' '    return 0;' '}' >"$cc_dir/dup/b.cb"
    printf '%s\n' '#include <cstdio>' 'namespace cctwoprog {' \
      'int f() { return 40; }' 'int g() { return 2; }' '}' \
      'int a_value() { return cctwoprog::f() + 1; }' \
      'int b_value() { return cctwoprog::f() + cctwoprog::g(); }' \
      'int main() { std::printf("%d\n%d\n", a_value(), b_value()); }' >"$cc_dir/dup/twin.cpp"
    cc_dup_run() {
      CFLAT_CACHE_DIR="$cc_dir/dup/cache" $TIMEOUT "$CFLAT" "$cc_dir/dup/$1.cb" -i "$cc_dir/dup" -B -v \
        -o "$cc_dir/dup/$1.bin" >"$cc_dir/dup/$1.compile.log" 2>&1 || return 1
      "$cc_dir/dup/$1.bin" >"$cc_dir/dup/$1.out" || return 1
    }
    if ! clang++ -std=c++20 "$cc_dir/dup/twin.cpp" -o "$cc_dir/dup/twin" \
        >"$cc_dir/dup/twin.compile.log" 2>&1 || ! "$cc_dir/dup/twin" >"$cc_dir/dup/twin.out"; then
      cc_fail="(10) clang++ twin did not compile or run"
    elif ! head -n 1 "$cc_dir/dup/twin.out" >"$cc_dir/dup/a.expected" \
        || ! tail -n 1 "$cc_dir/dup/twin.out" >"$cc_dir/dup/b.expected"; then
      cc_fail="(10) could not read clang++ twin values"
    elif ! cc_dup_run a || ! cc_dup_run b || ! cc_dup_run a || ! cc_dup_run b; then
      cc_fail="(10) shared-cache A/B/A/B compile or execution failed"
    elif ! grep -q "C++ demand companion cache hit" "$cc_dir/dup/a.compile.log" \
        || ! grep -q "C++ demand companion cache hit" "$cc_dir/dup/b.compile.log"; then
      cc_fail="(10) repeated A/B compiles did not hit their cached companions"
    elif ! cmp -s "$cc_dir/dup/a.out" "$cc_dir/dup/a.expected" \
        || ! cmp -s "$cc_dir/dup/b.out" "$cc_dir/dup/b.expected"; then
      cc_fail="(10) CFlat results did not match the clang++ twin values"
    fi
  fi
  cat "$cc_dir"/*.log >"$cc_log" 2>/dev/null
  if [ -z "$cc_fail" ]; then
    write_result "$cc_name" "PASS" "$cc_t0"
    rm -rf "$cc_dir"
  else
    write_result "$cc_name" "FAIL: $cc_fail" "$cc_t0"
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

# Darwin debug-info tooling: -g must leave source lines reachable through either a dSYM
# or the retained object when dsymutil is unavailable.
if [ "$RUN_MODE" -eq 0 ] && [ "$(uname -s)" = "Darwin" ] \
    && command -v dwarfdump >/dev/null 2>&1; then
dsym_name="macos_g_dsym"
dsym_log="$RES/$dsym_name.log"
dsym_bin="$RES/$dsym_name.bin"
dsym_debug="$RES/$dsym_name.debug"
dsym_t0=$(now_ms)
if ! $TIMEOUT "$CFLAT" "$SRC/test_function_ptr.cb" -i "$LIB" -g \
    --locale-dir "$LOCALE_DIR" -o "$dsym_bin" >"$dsym_log" 2>&1; then
  write_result "$dsym_name" "FAIL compile" "$dsym_t0"
else
  dsym_artifact=""
  if [ -d "$dsym_bin.dSYM" ]; then
    dsym_artifact="$dsym_bin.dSYM"
  elif [ -f "$dsym_bin.o" ]; then
    dsym_artifact="$dsym_bin.o"
  fi
  if [ -z "$dsym_artifact" ] \
      || ! dwarfdump --debug-line "$dsym_artifact" >"$dsym_debug" 2>&1 \
      || ! grep -Fq "test_function_ptr.cb" "$dsym_debug"; then
    write_result "$dsym_name" "FAIL: source lines are not reachable" "$dsym_t0"
  else
    write_result "$dsym_name" "PASS" "$dsym_t0"
  fi
fi
fi

# Sanitizer pass: test_core.cb carries legs gated on `if const (__SANITIZE_OWNERSHIP__)`
# (init_capacity poison fill). The ordinary pass compiles without the sanitizer, so those
# arms fold away and never run. Compile and run the same fixture once more with
# --sanitize=ownership (AOT -o, not --run: the sanitizer implies -g) and report it as
# test_core.sanitize. The fixture also raises its own expected-leg total under the gate,
# so a sanitized total no higher than the plain one means the gate did not take - that
# is the non-vacuity check below.
if [ "$RUN_MODE" -eq 0 ] && ! is_skipped test_core; then
san_name="test_core.sanitize"
san_log="$RES/$san_name.log"
san_bin="$RES/$san_name.bin"
san_t0=$(now_ms)
read_leg_total() {
  sed -n 's/^[0-9]* \/ \([0-9]*\) tests passed\.$/\1/p' "$1" | tail -n 1
}
if ! $TIMEOUT "$CFLAT" "$SRC/test_core.cb" -i "$LIB" --locale-dir "$LOCALE_DIR" \
    --sanitize=ownership -o "$san_bin" >"$san_log" 2>&1; then
  write_result "$san_name" "FAIL compile" "$san_t0"
elif ! $TIMEOUT "$san_bin" </dev/null >>"$san_log" 2>&1; then
  write_result "$san_name" "FAIL run" "$san_t0"
else
  san_total="$(read_leg_total "$san_log")"
  plain_total="$(read_leg_total "$RES/test_core.log")"
  if [ -z "$san_total" ]; then
    write_result "$san_name" "FAIL: sanitized run printed no leg total" "$san_t0"
  elif [ -n "$plain_total" ] && [ "$san_total" -le "$plain_total" ]; then
    write_result "$san_name" "FAIL: sanitizer-gated legs did not run ($san_total <= $plain_total)" "$san_t0"
  else
    write_result "$san_name" "PASS" "$san_t0"
  fi
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

# Header cache: a dead cheaders/v<M> directory whose newest mtime is over 7 days old is removed on
# the first header import of a process; a fresh one, a non-numeric name, a symlink (and its target)
# and the live version stay. Private cache dir; fake dirs back-dated with touch -t.
if [ "$RUN_MODE" -eq 0 ]; then
  pv_name="cheader_version_prune"
  pv_dir="$RES/$pv_name.d"
  pv_log="$RES/$pv_name.log"
  pv_t0=$(now_ms)
  rm -rf "$pv_dir"; mkdir -p "$pv_dir/cache/cheaders/v1" "$pv_dir/cache/cheaders/v2" \
    "$pv_dir/cache/cheaders/vX" "$pv_dir/outside"
  [ -d "$(dirname "$CFLAT")/.cflat" ] && cp -R "$(dirname "$CFLAT")/.cflat/." "$pv_dir/cache/"
  touch "$pv_dir/cache/cheaders/v1/f" "$pv_dir/cache/cheaders/v2/f" "$pv_dir/cache/cheaders/vX/f" \
    "$pv_dir/outside/f"
  ln -s "$pv_dir/outside" "$pv_dir/cache/cheaders/v3"
  pv_old=$(date -v-30d +%Y%m%d%H%M 2>/dev/null || date -d '30 days ago' +%Y%m%d%H%M)
  touch -t "$pv_old" "$pv_dir/cache/cheaders/v1/f" "$pv_dir/cache/cheaders/v1" \
    "$pv_dir/cache/cheaders/vX/f" "$pv_dir/cache/cheaders/vX" "$pv_dir/outside/f" "$pv_dir/outside"
  printf '%s\n' 'import "c_macro_helpers.h";' 'int main()' '{' '    return 0;' '}' >"$pv_dir/p.cb"
  if ! CFLAT_CACHE_DIR="$pv_dir/cache" $TIMEOUT "$CFLAT" "$pv_dir/p.cb" -i "$LIB" -B -o "$pv_dir/p.bin" \
      >"$pv_log" 2>&1; then
    write_result "$pv_name" "FAIL: header import did not compile" "$pv_t0"
  elif [ -e "$pv_dir/cache/cheaders/v1" ]; then
    write_result "$pv_name" "FAIL: a dead version directory older than 7 days survived" "$pv_t0"
  elif [ ! -e "$pv_dir/cache/cheaders/v2/f" ]; then
    write_result "$pv_name" "FAIL: a fresh version directory was removed" "$pv_t0"
  elif [ ! -e "$pv_dir/cache/cheaders/vX/f" ]; then
    write_result "$pv_name" "FAIL: a non-numeric directory was removed" "$pv_t0"
  elif [ ! -L "$pv_dir/cache/cheaders/v3" ] || [ ! -e "$pv_dir/outside/f" ]; then
    write_result "$pv_name" "FAIL: a symlinked version directory or its target was removed" "$pv_t0"
  elif [ -z "$(find "$pv_dir/cache/cheaders" -mindepth 1 -maxdepth 1 -type d -name 'v[0-9]*' \
      ! -name v2 ! -name v1 | head -n 1)" ]; then
    write_result "$pv_name" "FAIL: the live version directory is missing (leg is vacuous)" "$pv_t0"
  else
    write_result "$pv_name" "PASS" "$pv_t0"
  fi
  rm -rf "$pv_dir"
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
