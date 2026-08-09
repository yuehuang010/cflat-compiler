# A user method named `run`/`WaitForExit`/`exitCode` in a `program` crashes the compiler

Filed 2026-08-09 by the review of `fix/dtordup` (the same-line duplicate-destructor fix),
which audited every `CreateFunctionDefinition` call site and found the "program synthesized
methods are unreachable by user syntax" assumption FALSE for exact-signature collisions.

**Severity: compiler crash (SIGSEGV/SIGABRT mix), zero output, no diagnostic.** Pre-existing:
measured identically on eb56af6 (master) and on the fix/dtordup branch, 3-5 runs each -
the categories match, exit codes are a live-UB mix (139/134), do not pin them.

## Repro

A `program` whose body declares a method whose name AND signature exactly match one of the
synthesized program methods:

```cflat
import "list.cb";
program P
{
    int exitCode() { return 7; }     // collides with the synthesized exitCode()
    int main(move list<string> args) { return 0; }
};
extern int main() { P p; list<string> a; p.run(a); p.WaitForExit(); return 0; }
```

Same crash for `void WaitForExit()` and `bool run(move list<string> args)`. Reviewer repro
files were `scratch/rdd_prog_exitCode.cb`, `rdd_prog_wfe.cb`, `rdd_prog_run2.cb` (scratch,
not preserved). SIGNATURE MISMATCHES ARE HARMLESS: `int WaitForExit()` and `int run()` both
compile and run today, which is why the suite never caught this.

## Root cause

The program synthesized-method emitters (`MainListener_Aggregates.cpp:1481` `run`, `:1562`
`WaitForExit`, `:1741` `exitCode`; the trampoline/`RequestStop`/`Kill` family sits in the
same :995-1741 range) call `CreateFunctionDefinition` unconditionally. When the user's
same-signature method was already emitted, `CreateFunctionDefinition` takes the
`!fn->empty()` early return, which pushes NO function scope; the synth walks on, emits into
the terminated function, then `CreateBlockBreak(nullptr, true)` pops a `stackNamedVariable`
frame that was never pushed - the same underflow fixed for destructors by `fix/dtordup` and
for ctor default-param wrappers by the all-defaulted-ctor fix.

Note `MainListener_Aggregates.cpp:2082` already rejects a program FIELD named `exitCode`;
nothing rejects a METHOD with a reserved name.

## Fix direction

Either (a) reject user-written program methods whose name matches a synthesized member
(`run`, `WaitForExit`, `RequestStop`, `Kill`, `exitCode`, the trampoline) with a located
LogError - the `:2082` field check is the pattern; or (b) add the `OverloadSlotIsDefined()`
pre-check the destructor fix used to each synth emitter and report the collision there.
(a) is simpler and reserves the namespace explicitly; it needs an accept-set check that
signature-MISMATCHED spellings (`int WaitForExit()`, `int run()`), which compile today,
either keep compiling or are rejected deliberately with the same message - decide one way
and test it, do not leave it half.
