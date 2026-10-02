#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
CONFIG="${CFLAT_CONFIG:-Release}"
MAX_TIER=1
JOBS=4
WARM=0
STRICT=0
INCLUDE_DISABLED=0
LIST=0
LIB_FILTER=()
while [ $# -gt 0 ]; do
  case "$1" in
    Release|Debug) CONFIG="$1" ;;
    release) CONFIG=Release ;;
    debug) CONFIG=Debug ;;
    -t) shift; MAX_TIER="$1" ;;
    -j) shift; JOBS="$1" ;;
    --warm) WARM=1 ;;
    --strict) STRICT=1 ;;
    --include-disabled) INCLUDE_DISABLED=1 ;;
    --list) LIST=1 ;;
    -*) echo "unknown option: $1"; exit 2 ;;
    *) LIB_FILTER+=("$1") ;;
  esac
  shift
done
if [ "$MAX_TIER" -lt 1 ] || [ "$JOBS" -lt 1 ]; then echo "-t and -j must be positive"; exit 2; fi
CFLAT="$ROOT/x64/$CONFIG/cflat"
OUT="$ROOT/out/libs"
RESULTS="$OUT/.results"
mkdir -p "$OUT" "$RESULTS"
rm -f "$RESULTS"/*
if [ ! -x "$CFLAT" ]; then echo "cflat not found: $CFLAT"; exit 1; fi
if ! command -v timeout >/dev/null 2>&1 && ! command -v gtimeout >/dev/null 2>&1; then
  echo "warning: no timeout/gtimeout found; running cases without per-case timeout"
fi
export CFLAT_CACHE_DIR="$ROOT/out/libs-cache"

declare -a LIBS CASES
for dir in "$ROOT"/test_libs/*; do
  [ -f "$dir/lib.cfg" ] || continue
  lib="${dir##*/}"
  if [ ${#LIB_FILTER[@]} -gt 0 ]; then
    matched=0; for wanted in "${LIB_FILTER[@]}"; do [ "$wanted" = "$lib" ] && matched=1; done
    [ "$matched" -eq 1 ] || continue
  fi
  LIBS+=("$lib")
done

failures=0; stale_count=0; pass_count=0; skip_count=0; disabled_count=0; xfail_count=0; xpass_count=0
start_time=$(date +%s)
if [ "$LIST" -eq 0 ]; then
  "$CFLAT" --init-local >"$OUT/init.log" 2>&1 || { tail -n 5 "$OUT/init.log"; exit 1; }
fi

read_cfg() {
  CFG_TIER=1; CFG_ROOT_MAC=deps; CFG_ROOT_WIN=deps; CFG_PROBE=; CFG_INCLUDE=; CFG_LIB_MAC=; CFG_LIB_WIN=; CFG_RUNPATH_MAC=; CFG_RUNPATH_WIN=; CFG_VERSION_MAC=; CFG_VERSION_WIN=; CFG_HINT_MAC=; CFG_HINT_WIN=; CFG_ARGS=; CFG_TIMEOUT=300
  while IFS='=' read -r key value || [ -n "$key" ]; do
    case "$key" in ''|\#*) continue ;; esac
    case "$key" in
      tier) CFG_TIER="$value" ;; root_mac) CFG_ROOT_MAC="$value" ;; root_win) CFG_ROOT_WIN="$value" ;;
      probe) CFG_PROBE="$value" ;; include) CFG_INCLUDE="$value" ;; lib_mac) CFG_LIB_MAC="$value" ;; lib_win) CFG_LIB_WIN="$value" ;;
      runpath_mac) CFG_RUNPATH_MAC="$value" ;; runpath_win) CFG_RUNPATH_WIN="$value" ;; version_mac) CFG_VERSION_MAC="$value" ;; version_win) CFG_VERSION_WIN="$value" ;;
      hint_mac) CFG_HINT_MAC="$value" ;; hint_win) CFG_HINT_WIN="$value" ;; args) CFG_ARGS="$value" ;; timeout) CFG_TIMEOUT="$value" ;;
    esac
  done < "$1"
}

resolve_root() {
  spec="$1"
  case "$spec" in
    none) printf '' ;;
    system) printf '%s/test_libs' "$ROOT" ;;
    deps) printf '%s/arm64-osx' "${CFLAT_VCPKG_INSTALLED:-$HOME/.cflat-compiler-deps/vcpkg_installed}" ;;
    brew:*) brew --prefix "${spec#brew:}" 2>/dev/null ;;
    env:*) printenv "${spec#env:}" 2>/dev/null || true ;;
    *) printf '%s' "$spec" ;;
  esac
}

run_case() {
  local lib="$1" casefile="$2" root="$3" mode="$4" disabled="$5" case_dir="$6" statusfile="$7" timeout_s="$8"
  local name start end elapsed rc; name="${casefile##*/}"; name="${name%.cb}"
  mkdir -p "$case_dir"
  local args=(); for d in $CFG_INCLUDE; do args+=(--c-include "$root/$d"); done
  for f in $CFG_LIB_MAC; do args+=(--c-lib "$root/$f"); done
  start=$(date +%s)
  local cmd_timeout=(); if command -v timeout >/dev/null 2>&1; then cmd_timeout=(timeout "$timeout_s"); elif command -v gtimeout >/dev/null 2>&1; then cmd_timeout=(gtimeout "$timeout_s"); fi
  if [ "$mode" = check ]; then
    "${cmd_timeout[@]}" "$CFLAT" "$casefile" "${args[@]}" --check >"$case_dir/compile.log" 2>&1; rc=$?
  else
    "${cmd_timeout[@]}" "$CFLAT" "$casefile" "${args[@]}" -o "$case_dir/$name" >"$case_dir/compile.log" 2>&1; rc=$?
    if [ "$rc" -eq 0 ]; then
      (cd "$case_dir" && env DYLD_LIBRARY_PATH="$root/${CFG_RUNPATH_MAC:-lib}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" "./$name" ${CFG_ARGS//@REPO@/$ROOT}) >"$case_dir/run.log" 2>&1; rc=$?
    fi
  fi
  end=$(date +%s); elapsed=$((end - start))
  if [ "$disabled" = 1 ]; then
    if [ "$rc" -eq 0 ]; then printf 'XPASS|%s\n' "$elapsed" > "$statusfile"; else printf 'XFAIL|%s\n' "$elapsed" > "$statusfile"; fi
  elif [ "$rc" -eq 0 ]; then printf 'PASS|%s\n' "$elapsed" > "$statusfile"
  else printf 'FAIL|%s\n' "$elapsed" > "$statusfile"; fi
}

for lib in "${LIBS[@]}"; do
  dir="$ROOT/test_libs/$lib"; read_cfg "$dir/lib.cfg"
  # Out-of-tier libraries still get the stale-marker check below, but no root/version lookup.
  root=; probe_ok=1; hint="$CFG_HINT_MAC"
  if [ "$LIST" -eq 0 ] && [ "$CFG_TIER" -le "$MAX_TIER" ]; then
    root="$(resolve_root "$CFG_ROOT_MAC")"
    if [ -n "$CFG_VERSION_MAC" ]; then echo "[$lib] $($CFG_VERSION_MAC 2>&1)"; fi
    [ -n "$root" ] && [ -e "$root/$CFG_PROBE" ] || probe_ok=0
  fi
  for casefile in "$dir"/*.cb; do
    [ -f "$casefile" ] || continue
    name="${casefile##*/}"; name="${name%.cb}"
    mode=run; marker=; from_marker=; disabled=0
    while IFS= read -r line; do
      case "$line" in '// MODE: check'*) mode=check ;; '// DISABLED: '*) marker="${line#// DISABLED: }"; disabled=1 ;; esac
    done < "$casefile"
    if [ "$disabled" -eq 1 ]; then for issue in $marker; do
      if [ ! -f "$ROOT/$issue" ]; then echo "FAIL $lib/$name: stale DISABLED marker $issue"; stale_count=$((stale_count + 1)); failures=$((failures + 1)); fi
    done; fi
    if [ "$LIST" -eq 1 ]; then
      printf '%-12s tier=%s %-32s mode=%s%s\n' "$lib" "$CFG_TIER" "$name" "$mode" "$([ "$disabled" -eq 1 ] && printf ' DISABLED: %s' "$marker")"
      continue
    fi
    [ "$CFG_TIER" -le "$MAX_TIER" ] || continue
    case_dir="$OUT/$lib/$name"; statusfile="$RESULTS/$lib-$name.status"
    if [ "$disabled" -eq 1 ] && [ "$INCLUDE_DISABLED" -eq 0 ]; then echo "DISABLED $lib/$name"; disabled_count=$((disabled_count + 1)); continue; fi
    if [ "$probe_ok" -eq 0 ]; then
      reason="missing probe $root/$CFG_PROBE"; [ -n "$root" ] || reason="no root for this platform"
      if [ "$STRICT" -eq 1 ]; then echo "FAIL $lib/$name: $reason${hint:+; $hint}"; failures=$((failures + 1)); else echo "SKIP $lib/$name: $reason${hint:+; $hint}"; skip_count=$((skip_count + 1)); fi
      continue
    fi
    rm -rf "$case_dir"; mkdir -p "$case_dir"
    run_case "$lib" "$casefile" "$root" "$mode" "$disabled" "$case_dir" "$statusfile" "$CFG_TIMEOUT" &
    while [ "$(jobs -rp | wc -l | tr -d ' ')" -ge "$JOBS" ]; do wait -n 2>/dev/null || wait || true; done
  done
done
wait || true

if [ "$LIST" -eq 0 ]; then
  for lib in "${LIBS[@]}"; do for casefile in "$ROOT/test_libs/$lib"/*.cb; do
    [ -f "$casefile" ] || continue; name="${casefile##*/}"; name="${name%.cb}"; sf="$RESULTS/$lib-$name.status"
    [ -f "$sf" ] || continue; IFS='|' read -r status seconds < "$sf"
    case "$status" in
      PASS) echo "PASS $lib/$name (${seconds}s)"; pass_count=$((pass_count + 1)) ;;
      FAIL) echo "FAIL $lib/$name (${seconds}s)"; failures=$((failures + 1)); for log in "$OUT/$lib/$name/compile.log" "$OUT/$lib/$name/run.log"; do [ -f "$log" ] && tail -n 5 "$log"; done ;;
      XFAIL) echo "XFAIL $lib/$name (${seconds}s)"; xfail_count=$((xfail_count + 1)) ;;
      XPASS) echo "XPASS $lib/$name (${seconds}s)"; xpass_count=$((xpass_count + 1)) ;;
    esac
  done; done
fi
if [ "$LIST" -eq 0 ] && [ "$WARM" -eq 1 ]; then
  echo "Warm pass:"
  for lib in "${LIBS[@]}"; do
    dir="$ROOT/test_libs/$lib"; read_cfg "$dir/lib.cfg"; root="$(resolve_root "$CFG_ROOT_MAC")"
    for casefile in "$dir"/*.cb; do
      [ -f "$casefile" ] || continue
      name="${casefile##*/}"; name="${name%.cb}"; cold_file="$RESULTS/$lib-$name.status"
      [ -f "$cold_file" ] || continue
      IFS='|' read -r cold_status cold_seconds < "$cold_file"
      [ "$cold_status" = PASS ] || continue
      mode=run; while IFS= read -r line; do [ "$line" = '// MODE: check' ] && mode=check; done < "$casefile"
      warm_file="$RESULTS/$lib-$name.warm"
      run_case "$lib" "$casefile" "$root" "$mode" 0 "$OUT/$lib/$name" "$warm_file" "$CFG_TIMEOUT"
      IFS='|' read -r warm_status warm_seconds < "$warm_file"
      echo "$lib/$name cold=${cold_seconds}s warm=${warm_seconds}s status=$warm_status"
      if [ "$warm_status" != PASS ]; then
        echo "FAIL warm $lib/$name"; failures=$((failures + 1))
        for log in "$OUT/$lib/$name/compile.log" "$OUT/$lib/$name/run.log"; do [ -f "$log" ] && tail -n 5 "$log"; done
      fi
    done
  done
fi
elapsed=$(($(date +%s) - start_time))
echo "Summary: PASS=$pass_count FAIL=$failures SKIP=$skip_count DISABLED=$disabled_count XPASS=$xpass_count XFAIL=$xfail_count"
echo "Elapsed: ${elapsed}s"
uptime 2>/dev/null | sed 's/^/Load: /'
if [ "$failures" -gt 0 ]; then exit 1; fi
