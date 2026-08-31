#!/usr/bin/env bash
# example.sh - macOS example gate (the mac counterpart of example.bat).
#
# Covers the platform-portable subset of example/*.cb on an Apple Silicon Mac.
# Windows-only examples (COM/WinRT, Win32 GUI, WinUI3, os.windows.* content,
# winsock/windows.h imports) and vcpkg-package demos are SKIPped - compile those
# on their native platform instead. Three tiers, mirroring example.bat:
#
#   1. GUI/editor self-tests  - compile, run "<bin> --selftest", expect a PASS
#      line + exit 0 (Cocoa NativeHost bridge, cocoa_window, native settings, fedit).
#   2. Compile-and-run        - compile, run headless (</dev/null), expect exit 0
#      (compute/tools/data demos that self-terminate without args or a tty).
#   3. Compile-only gate      - compile to a binary only (shell utilities that need
#      argv/a tty/stdin to do anything - the build itself is the coverage).
#
# Run from the repo root on an Apple Silicon Mac (Homebrew tools on PATH):
#   ./example.sh [JOBS]     # builds Release binaries into out/, runs the gate
# JOBS may also be set in the environment; a positional value takes precedence.
#
# Note: --heap-audit's C shim (diagnostic/heap_audit.c) is portable (POSIX branch via
# pthread/backtrace/dladdr) and works here too, but this gate does not build with it -
# the leak-clean teardown (nativeTeardownForTest) is the same code exercised by
# --heap-audit on the Windows box (see example.bat).
set -u

DEFAULT_JOBS=8
if command -v sysctl >/dev/null 2>&1; then
    PHYSICAL_CPUS="$(sysctl -n hw.physicalcpu 2>/dev/null || true)"
    case "$PHYSICAL_CPUS" in
        ''|*[!0-9]*) ;;
        *)
            if [ "$PHYSICAL_CPUS" -gt 0 ]; then
                DEFAULT_JOBS="$PHYSICAL_CPUS"
            fi
            ;;
    esac
fi
if [ "$#" -ge 1 ]; then
    JOBS="$1"
elif [ -n "${JOBS:-}" ]; then
    JOBS="$JOBS"
else
    JOBS="$DEFAULT_JOBS"
fi
case "$JOBS" in
    ''|*[!0-9]*|0)
        echo "FAIL: JOBS must be a positive integer"
        exit 1
        ;;
esac

CFLAT="x64/Release/cflat"
OUT="out"
IUI="example/ui"
IMAC="example/macos"
IREST="-i example/restAPI -i example/restAPI/network"

# A short timeout guard when gtimeout (coreutils) is available; otherwise run direct.
TIMEOUT=""
if command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT="gtimeout 90"
fi

if [ ! -x "$CFLAT" ]; then
    echo "FAIL: compiler not found at $CFLAT (build Release first)"
    exit 1
fi
mkdir -p "$OUT"

# compile <name> <bin> <compile-args...> -> 0 on success (logs to $OUT/<name>.build.log)
compile()
{
    name="$1"; shift
    bin="$1"; shift
    if ! "$CFLAT" "$@" -o "$bin" >"$OUT/$name.build.log" 2>&1; then
        return 1
    fi
    return 0
}

# compile_case <name> <output-binary> <compile-args...>
# Compile a tier-1 or tier-2 case and leave a marker for its later run phase.
compile_case()
{
    name="$1"; shift
    bin="$1"; shift
    result="$OUT/$name.result"
    marker="$OUT/$name.compile.ok"
    rm -f "$result" "$marker"
    if compile "$name" "$bin" "$@"; then
        : > "$marker"
    else
        {
            printf "  %-22s FAIL (compile)\n" "$name"
            sed 's/^/      /' "$OUT/$name.build.log"
        } > "$result"
    fi
}

# run_selftest_case <name> <output-binary>
# Run a compiled tier-1 case serially and check for a PASS line + exit 0.
run_selftest_case()
{
    name="$1"
    bin="$2"
    result="$OUT/$name.result"
    out="$($TIMEOUT "$bin" --selftest 2>&1)"; rc=$?
    if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q "PASS"; then
        printf "  %-22s PASS\n" "$name" > "$result"
    else
        {
            printf "  %-22s FAIL (rc=%s)\n" "$name" "$rc"
            printf '%s\n' "$out" | sed 's/^/      /'
        } > "$result"
    fi
}

# run_headless_case <name> <output-binary>
# Run a compiled tier-2 case headless (stdin from /dev/null) and expect exit 0.
run_headless_case()
{
    name="$1"
    bin="$2"
    result="$OUT/$name.result"
    out="$($TIMEOUT "$bin" </dev/null 2>&1)"; rc=$?
    if [ $rc -eq 0 ]; then
        printf "  %-22s PASS\n" "$name" > "$result"
    else
        {
            printf "  %-22s FAIL (rc=%s)\n" "$name" "$rc"
            printf '%s\n' "$out" | sed 's/^/      /'
        } > "$result"
    fi
}

# compile_build_case <name> <output-binary> <compile-args...>
# Compile a tier-3 case; PASS when a binary is produced.
compile_build_case()
{
    name="$1"; shift
    bin="$1"; shift
    result="$OUT/$name.result"
    rm -f "$result"
    rm -f "$bin"
    if compile "$name" "$bin" "$@" && [ -x "$bin" ]; then
        printf "  %-22s PASS (compiled)\n" "$name" > "$result"
    else
        {
            printf "  %-22s FAIL (compile)\n" "$name"
            sed 's/^/      /' "$OUT/$name.build.log"
        } > "$result"
    fi
}

# Run a command in a bounded pool. A completion marker lets this stay compatible
# with the Bash shipped by macOS, which has no wait -n.
POOL_PIDS=()
POOL_DONE=()
POOL_ACTIVE=0
POOL_SEQUENCE=0
start_pool_job()
{
    done="$OUT/.example_job_$$.$POOL_SEQUENCE.done"
    rm -f "$done"
    (
        "$@"
        rc=$?
        : > "$done"
        exit $rc
    ) &
    POOL_PIDS[$POOL_SEQUENCE]=$!
    POOL_DONE[$POOL_SEQUENCE]="$done"
    POOL_SEQUENCE=$((POOL_SEQUENCE + 1))
    POOL_ACTIVE=$((POOL_ACTIVE + 1))
    if [ "$POOL_ACTIVE" -ge "$JOBS" ]; then
        wait_for_pool_slot
    fi
}

wait_for_pool_slot()
{
    while true; do
        for index in "${!POOL_PIDS[@]}"; do
            done="${POOL_DONE[$index]}"
            if [ -f "$done" ]; then
                wait "${POOL_PIDS[$index]}" || true
                rm -f "$done"
                unset 'POOL_PIDS[index]'
                unset 'POOL_DONE[index]'
                POOL_ACTIVE=$((POOL_ACTIVE - 1))
                return
            fi
        done
        sleep 0.05
    done
}

wait_for_pool()
{
    while [ "$POOL_ACTIVE" -gt 0 ]; do
        wait_for_pool_slot
    done
}

queue_compile_cases()
{
    start_pool_job compile_case "cocoa_probe"     "$OUT/cocoa_probe"    "$IMAC/cocoa_probe.cb"           -i "$IMAC"
    start_pool_job compile_case "cocoa_window"    "$OUT/cocoa_window"   "$IMAC/cocoa_window.cb"          -i "$IMAC"
    start_pool_job compile_case "cocoa_settings"  "$OUT/nsettings_mac"  "$IUI/04-native-controls/cocoa_native_settings.cb"  -i "$IUI" -i "$IMAC"
    start_pool_job compile_case "fedit"           "$OUT/fedit_mac"      "$IUI/08-fedit/fedit.cb"            -i "$IUI" -i "$IMAC"
    start_pool_job compile_case "fedit_jsx"       "$OUT/fedit_jsx_mac"  "$IUI/08-fedit/fedit_jsx.cb"        -i "$IUI" -i "$IMAC"
    start_pool_job compile_case "gallery"         "$OUT/gallery_mac"    "$IUI/05-gallery/gallery.cb"        -i "$IUI" -i "$IMAC"
    start_pool_job compile_case "map"             "$OUT/map_mac"        "$IUI/09-map/map.cb"                -i "$IUI" -i "$IMAC"
    start_pool_job compile_case "mempress"        "$OUT/mempress_mac"   "$IUI/11-mempress/mempress.cb"      -i "$IUI/11-mempress" -i "$IUI/11-mempress/memcore"
    start_pool_job compile_case "trade_charts"    "$OUT/trade_charts"   "$IUI/10-trade/charts.cb"
    start_pool_job compile_case "trade_pead"      "$OUT/trade_pead"     "$IUI/10-trade/pead.cb"
    start_pool_job compile_case "trade_chart_export" "$OUT/trade_chart_export" "$IUI/10-trade/chart_export.cb"

    start_pool_job compile_case "raytracer"     "$OUT/raytracer"     "example/graphics/raytracer.cb"
    start_pool_job compile_case "fp_trap_demo"  "$OUT/fp_trap_demo"  "example/hpc/fp_trap_demo.cb"
    start_pool_job compile_case "lu_bench"      "$OUT/lu_bench"      "example/hpc/lu_bench.cb"
    start_pool_job compile_case "mc_pi"         "$OUT/mc_pi"         "example/hpc/mc_pi.cb"
    start_pool_job compile_case "nbody"         "$OUT/nbody"         "example/hpc/nbody.cb"
    start_pool_job compile_case "poisson_cg"    "$OUT/poisson_cg"    "example/hpc/poisson_cg.cb"
    start_pool_job compile_case "interp"        "$OUT/interp"        "example/tools/interp.cb"
    start_pool_job compile_case "json_config"   "$OUT/json_config"   "example/tools/json_config.cb"
    start_pool_job compile_case "test_http"     "$OUT/test_http"     "example/restAPI/test_http.cb" $IREST
    start_pool_job compile_case "https_get"     "$OUT/https_get"     "example/restAPI/https_get.cb" $IREST
    start_pool_job compile_case "framework_link" "$OUT/framework_link" "$IMAC/framework_link.cb" -i "$IMAC"
    start_pool_job compile_case "hello_objc"    "$OUT/hello_objc"    "$IMAC/hello_objc.cb" -i "$IMAC"
    start_pool_job compile_case "sysinfo_mac"   "$OUT/sysinfo_mac"   "$IMAC/sysinfo_mac.cb" -i "$IMAC"
    start_pool_job compile_case "ui_app"        "$OUT/ui_app"        "$IUI/01-elements/app.cb" -i "$IUI"
    start_pool_job compile_case "ui_counter"    "$OUT/ui_counter"    "$IUI/01-elements/counter.cb" -i "$IUI"
    start_pool_job compile_case "ui_counter_jsx" "$OUT/ui_counter_jsx" "$IUI/01-elements/counter_jsx.cb" -i "$IUI"
    start_pool_job compile_case "shell_echo"    "$OUT/sh_echo"       "example/shell/echo.cb"
    start_pool_job compile_case "shell_pwd"     "$OUT/sh_pwd"        "example/shell/pwd.cb"

    start_pool_job compile_build_case "huffman"     "$OUT/huffman"       "example/tools/huffman.cb"
    start_pool_job compile_build_case "bitmap"      "$OUT/sh_bitmap"     "example/shell/bitmap.cb"
    start_pool_job compile_build_case "shell_cls"   "$OUT/sh_cls"        "example/shell/cls.cb"
    start_pool_job compile_build_case "shell_copy"  "$OUT/sh_copy"       "example/shell/copy.cb"
    start_pool_job compile_build_case "shell_del"   "$OUT/sh_del"        "example/shell/del.cb"
    start_pool_job compile_build_case "shell_mkdir" "$OUT/sh_mkdir"      "example/shell/mkdir.cb"
    start_pool_job compile_build_case "shell_move"  "$OUT/sh_move"       "example/shell/move.cb"
    start_pool_job compile_build_case "shell_ren"   "$OUT/sh_ren"        "example/shell/ren.cb"
    start_pool_job compile_build_case "shell_rmdir" "$OUT/sh_rmdir"      "example/shell/rmdir.cb"
    start_pool_job compile_build_case "shell_type"  "$OUT/sh_type"       "example/shell/type.cb"
    start_pool_job compile_build_case "shell_wc"    "$OUT/sh_wc"         "example/shell/wc.cb"
    start_pool_job compile_build_case "trade_fetch"      "$OUT/trade_fetch"      "$IUI/10-trade/fetch.cb"
    start_pool_job compile_build_case "trade_backtest"   "$OUT/trade_backtest"   "$IUI/10-trade/backtest.cb"
    start_pool_job compile_build_case "trade_analysis"   "$OUT/trade_analysis"   "$IUI/10-trade/analysis.cb"
    start_pool_job compile_build_case "trade_edgar_fetch" "$OUT/trade_edgar_fetch" "$IUI/10-trade/edgar_fetch.cb"
    wait_for_pool
}

run_serial_selftests()
{
    run_one_selftest "cocoa_probe" "$OUT/cocoa_probe"
    run_one_selftest "cocoa_window" "$OUT/cocoa_window"
    run_one_selftest "cocoa_settings" "$OUT/nsettings_mac"
    run_one_selftest "fedit" "$OUT/fedit_mac"
    run_one_selftest "fedit_jsx" "$OUT/fedit_jsx_mac"
    run_one_selftest "gallery" "$OUT/gallery_mac"
    run_one_selftest "map" "$OUT/map_mac"
    run_one_selftest "mempress" "$OUT/mempress_mac"
    run_one_selftest "trade_charts" "$OUT/trade_charts"
    run_one_selftest "trade_pead" "$OUT/trade_pead"
    run_one_selftest "trade_chart_export" "$OUT/trade_chart_export"
}

run_one_selftest()
{
    name="$1"
    bin="$2"
    if [ -f "$OUT/$name.compile.ok" ]; then
        start_pool_job run_selftest_case "$name" "$bin"
        wait_for_pool
    fi
}

queue_headless_runs()
{
    if [ -f "$OUT/raytracer.compile.ok" ]; then start_pool_job run_headless_case "raytracer" "$OUT/raytracer"; fi
    if [ -f "$OUT/fp_trap_demo.compile.ok" ]; then start_pool_job run_headless_case "fp_trap_demo" "$OUT/fp_trap_demo"; fi
    if [ -f "$OUT/lu_bench.compile.ok" ]; then start_pool_job run_headless_case "lu_bench" "$OUT/lu_bench"; fi
    if [ -f "$OUT/mc_pi.compile.ok" ]; then start_pool_job run_headless_case "mc_pi" "$OUT/mc_pi"; fi
    if [ -f "$OUT/nbody.compile.ok" ]; then start_pool_job run_headless_case "nbody" "$OUT/nbody"; fi
    if [ -f "$OUT/poisson_cg.compile.ok" ]; then start_pool_job run_headless_case "poisson_cg" "$OUT/poisson_cg"; fi
    if [ -f "$OUT/interp.compile.ok" ]; then start_pool_job run_headless_case "interp" "$OUT/interp"; fi
    if [ -f "$OUT/json_config.compile.ok" ]; then start_pool_job run_headless_case "json_config" "$OUT/json_config"; fi
    if [ -f "$OUT/test_http.compile.ok" ]; then start_pool_job run_headless_case "test_http" "$OUT/test_http"; fi
    if [ -f "$OUT/https_get.compile.ok" ]; then start_pool_job run_headless_case "https_get" "$OUT/https_get"; fi
    if [ -f "$OUT/framework_link.compile.ok" ]; then start_pool_job run_headless_case "framework_link" "$OUT/framework_link"; fi
    if [ -f "$OUT/hello_objc.compile.ok" ]; then start_pool_job run_headless_case "hello_objc" "$OUT/hello_objc"; fi
    if [ -f "$OUT/sysinfo_mac.compile.ok" ]; then start_pool_job run_headless_case "sysinfo_mac" "$OUT/sysinfo_mac"; fi
    if [ -f "$OUT/ui_app.compile.ok" ]; then start_pool_job run_headless_case "ui_app" "$OUT/ui_app"; fi
    if [ -f "$OUT/ui_counter.compile.ok" ]; then start_pool_job run_headless_case "ui_counter" "$OUT/ui_counter"; fi
    if [ -f "$OUT/ui_counter_jsx.compile.ok" ]; then start_pool_job run_headless_case "ui_counter_jsx" "$OUT/ui_counter_jsx"; fi
    if [ -f "$OUT/shell_echo.compile.ok" ]; then start_pool_job run_headless_case "shell_echo" "$OUT/sh_echo"; fi
    if [ -f "$OUT/shell_pwd.compile.ok" ]; then start_pool_job run_headless_case "shell_pwd" "$OUT/sh_pwd"; fi
    wait_for_pool
}

print_result()
{
    name="$1"
    result="$OUT/$name.result"
    if [ -f "$result" ]; then
        cat "$result"
        first_line="$(sed -n '1p' "$result")"
        case "$first_line" in
            *PASS*) PASS=$((PASS + 1)) ;;
            *FAIL*) FAIL=$((FAIL + 1)) ;;
            *)
                printf "  %-22s FAIL (invalid result)\n" "$name"
                FAIL=$((FAIL + 1))
                ;;
        esac
    else
        printf "  %-22s FAIL (missing result)\n" "$name"
        FAIL=$((FAIL + 1))
    fi
}

PASS=0
FAIL=0
echo "== macOS example gate =="
queue_compile_cases
run_serial_selftests
queue_headless_runs

echo "-- GUI/editor self-tests --"
print_result "cocoa_probe"
print_result "cocoa_window"
print_result "cocoa_settings"
print_result "fedit"
print_result "fedit_jsx"
print_result "gallery"
print_result "map"
print_result "mempress"
print_result "trade_charts"
print_result "trade_pead"
print_result "trade_chart_export"

echo "-- compile-and-run (headless, exit 0) --"
print_result "raytracer"
print_result "fp_trap_demo"
print_result "lu_bench"
print_result "mc_pi"
print_result "nbody"
print_result "poisson_cg"
print_result "interp"
print_result "json_config"
print_result "test_http"
print_result "https_get"
print_result "framework_link"
print_result "hello_objc"
print_result "sysinfo_mac"
print_result "ui_app"
print_result "ui_counter"
print_result "ui_counter_jsx"
print_result "shell_echo"
print_result "shell_pwd"

echo "-- compile-only gate (needs argv/tty/stdin) --"
print_result "huffman"
print_result "bitmap"
print_result "shell_cls"
print_result "shell_copy"
print_result "shell_del"
print_result "shell_mkdir"
print_result "shell_move"
print_result "shell_ren"
print_result "shell_rmdir"
print_result "shell_type"
print_result "shell_wc"
print_result "trade_fetch"
print_result "trade_backtest"
print_result "trade_analysis"
print_result "trade_edgar_fetch"

echo "-- incremental O2 view gate --"
if [ "${SKIP_INCREMENTAL:-0}" = "1" ]; then
    echo "  incremental-o2 SKIP (SKIP_INCREMENTAL=1)"
elif ! command -v python3 >/dev/null 2>&1; then
    echo "  incremental-o2 SKIP (python3 unavailable)"
elif python3 Test/tools/incremental_o2_gate.py --exe "$CFLAT"; then
    echo "  incremental-o2 PASS"
    PASS=$((PASS + 1))
else
    echo "  incremental-o2 FAIL"
    FAIL=$((FAIL + 1))
fi

# Remove artifacts the run_case demos write into the tree (raytracer -> render.bmp
# in cwd; json_config -> config.out.json next to its input) so the gate is clean.
rm -f render.bmp example/tools/config.out.json
rm -f results/pead_selftest_ar_by_quintile.bmp results/pead_selftest_cum_ar.bmp
rm -f results/pead_selftest_equity.bmp results/pead_selftest_events.csv
rm -f results/pead_selftest_report.md
rm -f data/earnings_test/prices/SPY.csv
rm -f data/earnings_test/prices/TS01.csv data/earnings_test/prices/TS02.csv
rm -f data/earnings_test/prices/TS03.csv data/earnings_test/prices/TS04.csv
rm -f data/earnings_test/prices/TS05.csv data/earnings_test/prices/TS06.csv
rm -f data/earnings_test/prices/TS07.csv data/earnings_test/prices/TS08.csv
rm -f data/earnings_test/prices/TS09.csv data/earnings_test/prices/TS10.csv
rm -f data/earnings_test/prices/TS11.csv data/earnings_test/prices/TS12.csv
rm -f data/earnings_test/prices/TS13.csv data/earnings_test/prices/TS14.csv
rm -f data/earnings_test/prices/TS15.csv
rm -f data/earnings_test/TS01.csv data/earnings_test/TS02.csv
rm -f data/earnings_test/TS03.csv data/earnings_test/TS04.csv
rm -f data/earnings_test/TS05.csv data/earnings_test/TS06.csv
rm -f data/earnings_test/TS07.csv data/earnings_test/TS08.csv
rm -f data/earnings_test/TS09.csv data/earnings_test/TS10.csv
rm -f data/earnings_test/TS11.csv data/earnings_test/TS12.csv
rm -f data/earnings_test/TS13.csv data/earnings_test/TS14.csv
rm -f data/earnings_test/TS15.csv
rmdir data/earnings_test/prices data/earnings_test data 2>/dev/null || true
rmdir results 2>/dev/null || true

echo "== summary: $PASS passed, $FAIL failed =="
if [ $FAIL -ne 0 ]; then
    exit 1
fi
exit 0
