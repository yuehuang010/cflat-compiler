#!/usr/bin/env bash
# example_mac.sh - macOS example gate for the AppKit NativeHost work (the mac
# counterpart of example.bat's native/fedit workers). Builds and runs the headless
# self-tests for the Cocoa bridge probe, the cocoa_window demo, the native settings
# panel, and the fedit editor, then prints a PASS/FAIL summary.
#
# Run from the repo root on an Apple Silicon Mac (Homebrew tools on PATH):
#   ./example_mac.sh            # builds Release binaries into out/, runs --selftest
#
# Note: --heap-audit is a Windows-only diagnostic (its C shim includes <windows.h>),
# so it is not used here; the leak-clean teardown (nativeTeardownForTest) is the same
# code exercised by --heap-audit on the Windows box (see example.bat).
set -u

CFLAT="x64/Release/cflat"
OUT="out"
IUI="example/ui"
IMAC="example/macos"

# A short timeout guard when gtimeout (coreutils) is available; otherwise run direct.
TIMEOUT=""
if command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT="gtimeout 60"
fi

if [ ! -x "$CFLAT" ]; then
    echo "FAIL: compiler not found at $CFLAT (build Release first)"
    exit 1
fi
mkdir -p "$OUT"

PASS=0
FAIL=0

# run_case <name> <output-binary> <compile-args...>
# Compiles, then runs "<bin> --selftest" and checks for a PASS line + exit 0.
run_case()
{
    name="$1"; shift
    bin="$1"; shift
    printf "%-24s " "$name"

    if ! "$CFLAT" "$@" -o "$bin" >"$OUT/$name.build.log" 2>&1; then
        echo "FAIL (compile)"
        sed 's/^/    /' "$OUT/$name.build.log"
        FAIL=$((FAIL + 1))
        return
    fi

    out="$($TIMEOUT "$bin" --selftest 2>&1)"
    rc=$?
    if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q "PASS"; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL (rc=$rc)"
        printf '%s\n' "$out" | sed 's/^/    /'
        FAIL=$((FAIL + 1))
    fi
}

echo "== macOS example gate =="
run_case "cocoa_probe"     "$OUT/cocoa_probe"    "$IMAC/cocoa_probe.cb"                 -i "$IMAC"
run_case "cocoa_window"    "$OUT/cocoa_window"   "$IMAC/cocoa_window.cb"                -i "$IMAC"
run_case "cocoa_settings"  "$OUT/nsettings_mac"  "$IUI/cocoa_native_settings.cb"        -i "$IUI" -i "$IMAC"
run_case "fedit"           "$OUT/fedit_mac"      "$IUI/fedit/fedit.cb"                  -i "$IUI" -i "$IMAC"

echo "== summary: $PASS passed, $FAIL failed =="
if [ $FAIL -ne 0 ]; then
    exit 1
fi
exit 0
