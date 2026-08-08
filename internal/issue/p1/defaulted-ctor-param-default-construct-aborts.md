# Default-constructing a class/struct whose only ctor has all-defaulted params aborts

## Summary

A `class` (or `struct`) whose only user-written constructor has EVERY parameter defaulted
crashes the COMPILER at compile time when the type is default-constructed (`CDef d;`, no
initializer): the `cflat ... -o` invocation itself exits 134/139/1 and no executable is ever
produced. The crash is NONDETERMINISTIC across runs of the same compiler binary on the same
input: observed exit codes are 134 (SIGABRT), 139 (SIGSEGV), and 1 (a compile-time internal
diagnostic). Explicitly supplying the argument (`CDef d = CDef(7);`) also fails, so the defect
is not specific to the zero-argument call form. Pre-existing on `master` (33b3ac4); identical on `fix/class-undef`
(63107e2). NOT a regression from the class-undef fix (which only changed the seed value of the
class default-ctor aggregate from `undef` to `zeroinitializer`; both are equally affected).

## Repro

```cflat
class CDef { int a; int b; CDef(int x = 3) { a = x; b = 99; } };
extern int main() { CDef d; printf("%d %d\n", d.a, d.b); return 0; }
```

Also reproduces with:
- The struct twin: `struct SDef { int a; int b; SDef(int x = 3) { a = x; b = 99; } };` with the
  same `main`.
- The explicit-argument spelling: `CDef d = CDef(7);` in place of `CDef d;`.

## Measurements

All rc values below are the exit code of the COMPILE step (`cflat <file>.cb -o <out>`); no
variant ever produces an executable, so the program's `printf` is never in play.

On `master` (33b3ac4, pre-fix compiler): compile `rc=134`, zero output, 8/8 runs of the class
repro.

On `fix/class-undef` (63107e2, this branch's compiler, re-verified on `x64/Release/cflat` in the
worktree): nondeterministic across repeated compiles of the identical class repro -
```
run1 rc=1   internal: local variable 'd' declared with no enclosing scope
run2 rc=134 (no output)
run3 rc=1   internal: local variable 'd' declared with no enclosing scope
run4 rc=134 (no output)
run5 rc=1   internal: local variable 'd' declared with no enclosing scope
run6 rc=1   internal: local variable 'd' declared with no enclosing scope
run7 rc=1   internal: local variable 'd' declared with no enclosing scope
run8 rc=134 (no output)
```
`rc=139` (SIGSEGV) was also observed on this compiler binary: once in the round-1 review's runs,
and again in a later 3-run spot check (`134, 139, 134`). The exact split across 134/139/1 varies
run to run - live UB in the compiler.

The struct twin on the post compiler: 3 of 4 compiles `rc=134` (no output), 1 of 4 `rc=1` with
the same "declared with no enclosing scope" internal error. The explicit-argument spelling
(`CDef d = CDef(7);`) on the post compiler: 4/4 compiles `rc=1` with the same internal error, at
the column of the `CDef(7)` call site rather than the bare declaration.

## Root cause

Not traced. The internal error text ("local variable 'd' declared with no enclosing scope") and
the mix of SIGABRT/SIGSEGV/internal-error exit codes across otherwise-identical runs of the same
binary point at memory corruption or an uninitialized-state read somewhere in the synthesized
default-ctor call path when the only available constructor candidate has zero required
arguments (all parameters defaulted) - i.e. the declaration-site "should I call the user ctor
with defaults, or the synthetic no-arg ctor" resolution appears to leave some compiler-internal
state (a scope stack entry, going by the error text) inconsistently populated. Needs a run under
a debugger / ASan build to pin down; not attempted here.

## Fix direction

Not investigated. Likely candidates to start from: whichever code path decides between calling
the user-written constructor (with its defaults filled in) versus the synthetic no-arg ctor at a
plain declaration `CDef d;`, and whatever pushes/pops the "enclosing scope" the internal error
complains about for the synthesized call.
