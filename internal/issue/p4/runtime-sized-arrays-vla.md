# P4: runtime-sized local arrays (`int[n] a`), as a C VLA

RULED 2026-11-18 (maintainer): a fixed-array declaration whose bound is not a compile-time constant declares a
RUNTIME-SIZED array, and the feature follows the C VLA feature (C99 6.7.6.2) - where C says yes CFlat says yes,
where C refuses CFlat refuses. It is an optional feature, so it lives in p4 (moved the same day from
p3/member-type-path-remaining-gaps section 3). Rejected: refusing the bound as C++ and C# do (the main session's
pick) - the spelling already parses, so it gets the parent meaning.

## Surface (no new syntax)

`T[n] a;` at block scope, `n` any non-constant integer expression (local, parameter, C++ mutable static, call
result). Derived from "as C", marked derived where the maintainer did not say it in words:

- `n` is evaluated once, at the declaration; `n * sizeof(T)` bytes are allocated in the frame there and live to
  the end of the enclosing block (a declaration in a loop body allocates and releases per iteration).
- `sizeof(a)` is `n * sizeof(T)`, evaluated at run time. Indexing, `&a[i]` and view decay work as for a fixed
  array.
- Refused, as C refuses (derived): a struct field with a runtime bound, a global or `static` with a runtime
  bound, an initializer on the declaration (`int[n] a = {...}`).
- Multi-dimensional (`int[n][m] a`): C allows it (derived: allowed; may ship after the one-dimensional form).

## Not ruled - ask before building

- A zero or negative bound: undefined in C. Candidates: leave undefined as C, or a runtime trap.
- An owning element type (`unique T[n]`, a struct with a destructor): C has no such case. Candidates: refuse,
  or run n destructors at scope exit.
- `goto` / `break` across the declaration (C refuses a jump into the scope of a VLA).

## Master today (the gap this replaces)

`int[rs.Mut.n] a;` where `n` is a mutable C++ static (`static int n;`, or `static const int rt = getn();`)
compiles, and so does the native `int n = argc + 2; int[n] a;`. Measured 2026-11-18 (Release, macOS arm64):
`sizeof(a) == 4` (one int) for every one of them, and `a[2] = 3` writes past the slot without a diagnostic.
STATMEM (289499697) folds only scalar constexpr / const statics; a non-constant bound falls through to this
path. C-style `int a[n]` is refused by cflat ("use '[n] a'") and stays refused.

RULED 2026-11-18 (maintainer): until this feature is built, a non-constant bound is a COMPILER ERROR (the bug
half, p3/non-constant-array-bound-silently-one-element). Building this feature removes that refusal for block-scope
locals and flips its `err_` legs to value legs.

## Acceptance

The repro prints `3 12` (master `3 4`, by overrun); value legs in the closest array test for a local bound, a
parameter bound, a C++ static bound, `sizeof`, a loop-body declaration and view decay; `err_` legs for the field
/ global / initializer refusals.

Repro (header `sm.hpp`, then `cflat a.cb -i . -o a.out`):
```cpp
namespace rs { struct Mut { static int n; }; inline int Mut::n = 3; }
```
```cflat
import cpp "sm.hpp";
int main() { int[rs.Mut.n] a; for (int i = 0; i < 3; i++) { a[i] = i + 1; } printf("%d %d\n", a[2], (int)sizeof(a)); return 0; }
```
