# Two destructor bodies on ONE line crash the compiler (no duplicate-body guard)

## Summary

`ParseDestructorDefinition` (and its `program` twin `ParseProgramDestructorDefinition`) emit a
body unconditionally after `CreateFunctionDefinition`. Every other body-emitting path first
checks the "this function already has a terminated entry block" early return. When two
destructors for the same type are written on the SAME source line, `DiagnoseDuplicateFunctionBody`
takes its `firstLine == line` early return and reports nothing, so `CreateFunctionDefinition`
hands back the already-defined function with no function scope pushed - and the destructor path
walks on, emitting a second body into the live function and then calling
`CreateBlockBreak(nullptr, true)`, which pops a `stackNamedVariable` frame that was never pushed.
That underflow takes the caller's frame with it: the compiler dies nondeterministically with no
diagnostic.

Written on two lines the same program is a clean hard error, so the crash is purely a
same-line spelling artefact.

## Repro

```cflat
struct S { int a; ~S() { } ~S() { } };
extern int main()
{
    S d1;
    S d2;
    printf("%d %d\n", d1.a, d2.a);
    return 0;
}
```

`cflat fx3_dtor_dup.cb -o out.exe`, 20 consecutive runs of the same binary:

- master (7037b95, Release): `139 139 139 139 139 139 139 134 134 139 139 139 139 139 139 139 134 139 139 139`
  - 17x SIGSEGV, 3x SIGABRT, zero output, no executable produced.
- fix/defctor (the all-defaulted-ctor fix + field-seeding fix, Release):
  `133 134 134 133 133 134 133 133 133 133 133 133 133 133 133 134 133 133 133 133`
  - 16x SIGTRAP, 4x SIGABRT, zero output, no executable produced.

The exit-code MIX is live UB and is not a signature: a later 10-run sample of the same
fix/defctor binary gave `1 1 139 134 134 133 134 134 133 139` - rc 1 (an internal diagnostic)
and rc 139 both occur too. What is stable across every sample and both binaries is the
CATEGORY: no diagnostic naming the duplicate, zero output, no executable. Re-measure the
category, not the numbers.

Pre-existing and unfixed by the all-defaulted-ctor work; that commit closed the constructor
default-parameter-wrapper spelling of the same underflow, not this one.

## Contrast measurements (same host, same two binaries)

Two destructors on SEPARATE lines - clean diagnostic, rc 1, 5/5 runs on BOTH binaries:

```
fx3_dtor_dup_multiline.cb(5,4): redefinition of '~S' - the same overload is already defined
at fx3_dtor_dup_multiline.cb(4). ...
```

The free-function twin on ONE line - compiles and runs, rc 0, 5/5 runs on BOTH binaries
(`int f() { return 1; } int f() { return 2; }` plus a `main` returns 6, i.e. the FIRST body
survives and the second is silently dropped). That is the `MainListener_Declarations.cpp:1922`
guard doing its job. The destructor path has no equivalent.

## Root cause

`MainListener_Aggregates.cpp:3242` (`ParseDestructorDefinition`) and `:3273`
(`ParseProgramDestructorDefinition`) call `CreateFunctionDefinition` and then unconditionally
`InitializeBlock(&fn->front(), false)` / emit the body / `CreateBlockBreak(nullptr, true)`.
The guard that every sibling has is missing:

```cpp
if (!fn->empty() && fn->getEntryBlock().getTerminator() != nullptr)
    return;
```

Present at `MainListener_Declarations.cpp:1922` (free/member functions) and
`MainListener_Aggregates.cpp:3135` (constructors). `DiagnoseDuplicateFunctionBody`
(`LLVMBackend_ControlFlowAndFunctions.cpp`, guard at :1078) cannot cover this: it deliberately
yields when the recorded origin line equals the new line, which is exactly the same-line case.

## Fix direction

Add the same early-return guard to both destructor emitters, immediately after
`CreateFunctionDefinition` and before `RegisterDestructor` / `InitializeBlock`. That makes the
same-line duplicate behave like the same-line free-function duplicate (first body wins,
compilation continues) rather than crashing.

Better than silence: prefer a located diagnostic. `OverloadSlotIsDefined()`
(`LLVMBackend.h`, added by the all-defaulted-ctor fix) already answers "is this slot already
defined, and at which origin line" BEFORE `CreateFunctionDefinition` is called; the constructor
path uses it to distinguish a compiler-synthesized origin (line 0, yield silently) from a real
user clash (report). A destructor can never be compiler-synthesized with a body, so a
same-line second `~S()` is always a genuine user error and can be reported outright - which
would also be an improvement over the free-function path's silent drop, though changing that
one is a separate decision.

Regression legs: the crash needs no `expect_error` support beyond the bare file-scope form, and
the current behaviour (crash, zero output) means any leg is non-vacuous against master by
construction. Probes live at `scratch/fx3_dtor_dup.cb`, `scratch/fx3_dtor_dup_multiline.cb`,
`scratch/fx3_freefn_dup.cb`, `scratch/fx3_freefn_dup_sameline.cb`.
