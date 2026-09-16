# Nested brace initializer for a 2D fixed array crashes or hangs past 16 elements

`T[N][M] g = { {..}, {..} }` compiles fine while `N*M <= 16`. Past that the compiler either
dies with no diagnostic at all (exit 139 / 138, i.e. SIGSEGV / SIGBUS) or spins past a
25 s timeout. A plain 1D `int[40]` brace list is unaffected, so the blow-up is specific to
the nested (row-of-rows) form.

## Repro

`scratch/dogfood/lib/repro_9.cb`:

```cflat
extern int main()
{
    int[9][2] g = { {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0} };
    return g[0][0];
}
```

```
x64/Release/cflat scratch/dogfood/lib/repro_9.cb --check
# no output; exit 139
```

## Observed

Measured on macOS arm64 Release (element count = N*M):

| shape | elements | result |
|-------|----------|--------|
| 2x8, 4x4, 3x5 | <= 16 | PASS |
| 2x9, 9x2, 3x6, 17x2, 5x6 | 18-34 | crash, exit 138/139, NO message |
| 4x5, 4x6 | 20, 24 | still running at 25 s (hang) |
| 1D `int[40]` | 40 | PASS |

`--check` crashes the same way as a full compile, so this is front end / codegen, not the
linker. Nothing is printed - not even the `CompilerManager` crash dump - so a user sees a
silent non-zero exit.

## Expected

A 5x6 grid literal is an ordinary thing to write. Either compile it, or, if nested brace
initializers are deliberately unsupported past some size, say so with a `LogError`.

## Root cause

Not investigated. The 16-element cliff plus the crash-vs-hang split suggests the
initializer lowering switches strategy at a size threshold (element-wise stores vs a
constant aggregate) and the nested path recurses per element on the wrong dimension -
consistent with stack exhaustion (SIGBUS) at some sizes and exponential work at others.

## Fix direction

Find the fixed-array brace-init lowering in `MainListener.h` (array initializer handling
under `ParseDeclarationSpecifiers` / the initializer-list walk) and check how a row
sub-list is folded when the element type is itself `T[M]`. Add a regression case to an
existing `Test/test_*.cb` array test covering a 5x6 nested literal, and a `Test/errors/`
case if the decision is to reject instead.

FIXED 2026-09-16 in worktree /Users/felixhuang/source/cflat-brace (branch wip/brace, uncommitted, host-verified): EmitPositionalFixedArrayIntoSlot collected element pointers through EmitFixedArrayElementWalk, which switches to a runtime loop above 16 elements and calls back once, so the collector indexed out of bounds. Now builds one static GEP per element. Rows in Test/test_initializer_list.cb. Delete this file when the branch lands.
