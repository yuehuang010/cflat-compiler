#!/usr/bin/env bash
# test_lsp.sh - POSIX (Linux/WSL + macOS) counterpart of test_lsp.bat.
#
# Drives the three LSP Python suites (smoke, fixtures, bulk source sweep)
# against the native cflat built into x64/<Config>/cflat. These are kept
# separate from test.sh, exactly like test_lsp.bat vs test.bat on Windows.
#
# Usage:
#   ./test_lsp.sh            # Release (default)
#   ./test_lsp.sh Debug      # Debug
#   ./test_lsp.sh Release 8  # explicit config + LSP worker-pool size
#
# The suites' Python sources use PEP 604 unions ("str | None"), so a Python
# >= 3.10 interpreter is required; macOS ships /usr/bin/python3 == 3.9. This
# script auto-selects the first python3.1x it can find (Homebrew keg included).
set -u

CFG="${CFLAT_CONFIG:-Release}"
POOL=4
case "${1:-}" in
    Debug|Release) CFG="$1"; [ $# -ge 2 ] && POOL="$2" ;;
    "" ) ;;
    * ) POOL="$1" ;;
esac

COMPILER="x64/$CFG/cflat"
if [ ! -x "$COMPILER" ]; then
    echo "FAIL: compiler not found at $COMPILER (build $CFG first)"
    exit 1
fi

# Pick a Python >= 3.10 (PEP 604 unions in the suite sources need it).
PY=""
for cand in python3.13 python3.12 python3.11 python3.10 \
            /opt/homebrew/bin/python3.13 /opt/homebrew/bin/python3.12 \
            /opt/homebrew/bin/python3.11 /opt/homebrew/bin/python3.10 python3; do
    if command -v "$cand" >/dev/null 2>&1; then
        if "$cand" -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>/dev/null; then
            PY="$cand"; break
        fi
    fi
done
if [ -z "$PY" ]; then
    echo "FAIL: no Python >= 3.10 found (the LSP suites use 'str | None' union syntax)."
    echo "      On macOS: brew install python@3.12"
    exit 1
fi

POOL_ARG="--lsp-pool-size $POOL"
T="vscode-extension/test"
rc=0

echo "=== LSP smoke tests ==="
"$PY" "$T/lsp_test.py" "$COMPILER" $POOL_ARG || { echo "FAILED: LSP smoke tests"; rc=1; }

echo
echo "=== LSP fixture tests ==="
"$PY" "$T/lsp_fixture_test.py" "$COMPILER" $POOL_ARG || { echo "FAILED: LSP fixture tests"; rc=1; }

echo
echo "=== LSP bulk source sweep ==="
"$PY" "$T/lsp_bulk_test.py" "$COMPILER" $POOL_ARG || { echo "FAILED: LSP bulk source sweep"; rc=1; }

echo
if [ $rc -eq 0 ]; then
    echo "All LSP tests passed."
else
    echo "Some LSP tests failed."
fi
exit $rc
