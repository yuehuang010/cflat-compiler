#!/usr/bin/env bash
# example_mac.sh - macOS example gate (the mac counterpart of example.bat).
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
#   ./example_mac.sh            # builds Release binaries into out/, runs the gate
#
# Note: --heap-audit is a Windows-only diagnostic (its C shim includes <windows.h>),
# so it is not used here; the leak-clean teardown (nativeTeardownForTest) is the same
# code exercised by --heap-audit on the Windows box (see example.bat).
set -u

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

PASS=0
FAIL=0

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

# selftest_case <name> <output-binary> <compile-args...>
# Tier 1: compile, then run "<bin> --selftest" and check for a PASS line + exit 0.
selftest_case()
{
    name="$1"; shift
    bin="$1"; shift
    printf "  %-22s " "$name"
    if ! compile "$name" "$bin" "$@"; then
        echo "FAIL (compile)"; sed 's/^/      /' "$OUT/$name.build.log"; FAIL=$((FAIL + 1)); return
    fi
    out="$($TIMEOUT "$bin" --selftest 2>&1)"; rc=$?
    if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q "PASS"; then
        echo "PASS"; PASS=$((PASS + 1))
    else
        echo "FAIL (rc=$rc)"; printf '%s\n' "$out" | sed 's/^/      /'; FAIL=$((FAIL + 1))
    fi
}

# run_case <name> <output-binary> <compile-args...>
# Tier 2: compile, then run headless (stdin from /dev/null) and expect exit 0.
run_case()
{
    name="$1"; shift
    bin="$1"; shift
    printf "  %-22s " "$name"
    if ! compile "$name" "$bin" "$@"; then
        echo "FAIL (compile)"; sed 's/^/      /' "$OUT/$name.build.log"; FAIL=$((FAIL + 1)); return
    fi
    out="$($TIMEOUT "$bin" </dev/null 2>&1)"; rc=$?
    if [ $rc -eq 0 ]; then
        echo "PASS"; PASS=$((PASS + 1))
    else
        echo "FAIL (rc=$rc)"; printf '%s\n' "$out" | tail -5 | sed 's/^/      /'; FAIL=$((FAIL + 1))
    fi
}

# build_case <name> <output-binary> <compile-args...>
# Tier 3: compile only; PASS when a binary is produced (needs argv/tty to run).
build_case()
{
    name="$1"; shift
    bin="$1"; shift
    printf "  %-22s " "$name"
    rm -f "$bin"
    if compile "$name" "$bin" "$@" && [ -x "$bin" ]; then
        echo "PASS (compiled)"; PASS=$((PASS + 1))
    else
        echo "FAIL (compile)"; sed 's/^/      /' "$OUT/$name.build.log"; FAIL=$((FAIL + 1))
    fi
}

echo "== macOS example gate =="

echo "-- GUI/editor self-tests --"
selftest_case "cocoa_probe"     "$OUT/cocoa_probe"    "$IMAC/cocoa_probe.cb"           -i "$IMAC"
selftest_case "cocoa_window"    "$OUT/cocoa_window"   "$IMAC/cocoa_window.cb"          -i "$IMAC"
selftest_case "cocoa_settings"  "$OUT/nsettings_mac"  "$IUI/04-native-controls/cocoa_native_settings.cb"  -i "$IUI" -i "$IMAC"
selftest_case "fedit"           "$OUT/fedit_mac"      "$IUI/08-fedit/fedit.cb"            -i "$IUI" -i "$IMAC"
selftest_case "fedit_jsx"       "$OUT/fedit_jsx_mac"  "$IUI/08-fedit/fedit_jsx.cb"        -i "$IUI" -i "$IMAC"
selftest_case "gallery"         "$OUT/gallery_mac"    "$IUI/05-gallery/gallery.cb"        -i "$IUI" -i "$IMAC"
selftest_case "map"             "$OUT/map_mac"        "$IUI/09-map/map.cb"                -i "$IUI" -i "$IMAC"

echo "-- compile-and-run (headless, exit 0) --"
run_case "raytracer"     "$OUT/raytracer"     "example/graphics/raytracer.cb"
run_case "fp_trap_demo"  "$OUT/fp_trap_demo"  "example/hpc/fp_trap_demo.cb"
run_case "lu_bench"      "$OUT/lu_bench"      "example/hpc/lu_bench.cb"
run_case "mc_pi"         "$OUT/mc_pi"         "example/hpc/mc_pi.cb"
run_case "nbody"         "$OUT/nbody"         "example/hpc/nbody.cb"
run_case "poisson_cg"    "$OUT/poisson_cg"    "example/hpc/poisson_cg.cb"
run_case "interp"        "$OUT/interp"        "example/tools/interp.cb"
run_case "json_config"   "$OUT/json_config"   "example/tools/json_config.cb"
run_case "test_http"     "$OUT/test_http"     "example/restAPI/test_http.cb" $IREST
run_case "framework_link" "$OUT/framework_link" "$IMAC/framework_link.cb"       -i "$IMAC"
run_case "hello_objc"    "$OUT/hello_objc"    "$IMAC/hello_objc.cb"           -i "$IMAC"
run_case "sysinfo_mac"   "$OUT/sysinfo_mac"   "$IMAC/sysinfo_mac.cb"          -i "$IMAC"
run_case "ui_app"        "$OUT/ui_app"        "$IUI/01-elements/app.cb"                   -i "$IUI"
run_case "ui_counter"    "$OUT/ui_counter"    "$IUI/01-elements/counter.cb"               -i "$IUI"
run_case "ui_counter_jsx" "$OUT/ui_counter_jsx" "$IUI/01-elements/counter_jsx.cb"         -i "$IUI"
run_case "shell_echo"    "$OUT/sh_echo"       "example/shell/echo.cb"
run_case "shell_pwd"     "$OUT/sh_pwd"        "example/shell/pwd.cb"

echo "-- compile-only gate (needs argv/tty/stdin) --"
build_case "huffman"     "$OUT/huffman"       "example/tools/huffman.cb"
build_case "bitmap"      "$OUT/sh_bitmap"     "example/shell/bitmap.cb"
build_case "shell_cls"   "$OUT/sh_cls"        "example/shell/cls.cb"
build_case "shell_copy"  "$OUT/sh_copy"       "example/shell/copy.cb"
build_case "shell_del"   "$OUT/sh_del"        "example/shell/del.cb"
build_case "shell_mkdir" "$OUT/sh_mkdir"      "example/shell/mkdir.cb"
build_case "shell_move"  "$OUT/sh_move"       "example/shell/move.cb"
build_case "shell_ren"   "$OUT/sh_ren"        "example/shell/ren.cb"
build_case "shell_rmdir" "$OUT/sh_rmdir"      "example/shell/rmdir.cb"
build_case "shell_type"  "$OUT/sh_type"       "example/shell/type.cb"
build_case "shell_wc"    "$OUT/sh_wc"         "example/shell/wc.cb"

# Remove artifacts the run_case demos write into the tree (raytracer -> render.bmp
# in cwd; json_config -> config.out.json next to its input) so the gate is clean.
rm -f render.bmp example/tools/config.out.json

echo "== summary: $PASS passed, $FAIL failed =="
if [ $FAIL -ne 0 ]; then
    exit 1
fi
exit 0
