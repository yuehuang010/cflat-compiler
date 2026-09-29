# Cross-target `-p` compiles are broken beyond `--check`

Deferred: focus is macOS native compile. Do not work this until the maintainer lifts the
deferral. Found during the 2026-09-23 night box (cross-target C++ import crash fix, 11a58ada).

## Symptoms (host: macOS arm64)

1. `-o` under any cross `-p` (`linux`, `win64`, `win32`) fails at link, even for a plain
   program with no imports beyond `runtime.cb`.
2. Debug build: `-p win64 -o out.exe` on a plain program hits an LLVM assert (assertions-on
   LLVM only; Release does not assert but still fails per item 1).
3. `-p win32 --check` of a trivial program fails in `core/os.cb` (~line 475): no overload of
   `os.windows.VirtualAllocExNuma` matches on the 32-bit target.

## Repro

```cflat
extern int main() { return 0; }
```

```bash
x64/Release/cflat scratch/plain.cb -p linux -o scratch/plain       # link failure
x64/Debug/cflat scratch/plain.cb -p win64 -o scratch/plain.exe     # LLVM assert
x64/Release/cflat scratch/plain.cb -p win32 --check                # os.cb overload error
```

## Root cause

Not investigated. Likely separate causes: (1) cross linker/sysroot selection in the link
step, (2) a Debug-only IR invariant violated when the target triple differs from the host,
(3) `core/os.cb` Windows prototypes assume 64-bit integer widths.

## Fix direction

Root-cause each independently when the deferral lifts. Item 2 needs a proper compiler error
once root-caused (LLVM assert rule). Consider an `-p` suite leg so cross targets stay covered.

Cache note (A8 round-2 review, 2026-09-28): the request-cache prune scope (the `.rq` line 3) records only Windows vs
non-Windows, not the full target triple. Two non-Windows targets sharing one cache would treat each other's live
entries as dead and delete them. It is a one-line fix (fold the triple into line 3); do it when `-p` cross-target returns.
