#!/usr/bin/env bash
# Builds mathlib.lib (a STATIC archive) from mathlib.c on macOS/Linux - the counterpart
# to build_mathlib.bat. test_c_interop.cb binds it via
#   import package "cinterop/mathlib.h" lib "cinterop/mathlib.lib";
#
# The archive keeps the .lib name on every platform so that import line stays
# platform-neutral; the linker sniffs the archive magic, not the extension.
#
# A static archive needs no linking here - the libc symbols resolve later, when cflat
# links the final executable.
#
# Usage:  build_mathlib.sh            (config arg accepted and ignored, for parity
#                                      with the .bat, which picks a toolchain by config)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Same driver search order as the compiler's own POSIX C path (FindCDriver in
# LLVMBackend.h), so the fixture is built by whatever compiles the .c imports.
CC=""
for cand in clang clang-18 cc gcc; do
  if command -v "$cand" >/dev/null 2>&1; then CC="$cand"; break; fi
done
if [ -z "$CC" ]; then
  echo "ERROR: no C compiler driver (clang/cc/gcc) found - cannot build mathlib fixture." >&2
  exit 1
fi

OBJ="$HERE/mathlib.o"
LIB="$HERE/mathlib.lib"

# On macOS, match the deployment target cflat links against (EmitExecutableMachO uses
# macosx11.0.0); otherwise the host clang stamps a newer minos and ld64 warns at link.
CFLAGS=""
[ "$(uname -s)" = "Darwin" ] && CFLAGS="-mmacosx-version-min=11.0"

"$CC" -c $CFLAGS "$HERE/mathlib.c" -o "$OBJ"
rm -f "$LIB"
ar rcs "$LIB" "$OBJ"
rm -f "$OBJ"

echo "Built \"$LIB\""
