# expect_error bare form still misses a deferred generic STRUCT method body

Bucket: batch mode (test infrastructure gap; no miscompile). Filed 2026-09-04 by the q14 and
q17 agents, master a09da972.

## Repro

```
struct S<T> { int bad() { return undefinedName; } };
extern int main()
{
    expect_error("Undefined variable undefinedName") { S<int> s; s.bad(); }
    return 0;
}
```

The diagnostic escapes the armed expectation and fails the compile (exit 1, no PASS) on master in
the statement-scope and bare-semicolon forms, and through the queued `unique<S<int>>` path. A
generic FREE function body under the same expectation matches correctly since q14/q17.

T354 status (2026-10-20): scoped and fresh `unique` method cases now match under the expectation.
The bare case remains unresolved; its worktree probe `scratch/t354_tmp/probes/d_method_bare.cb`
fails with the raw method diagnostic before the body arms the expectation. A same-specialization
retry after a swallowed scoped method error also remains unresolved:
`scratch/t354_tmp/probes/d_method_reuse.cb` catches the first method error, then gets `Unknown
identifier 'bad'` on the later `unique` use. The passing test legs use distinct struct templates so
they test the scoped and queued `unique` paths independently.

## Root cause

Scoped CFlat generic type uses are deferred by the function pre-scan, then drained while the
scoped expectation is active. The core `unique` declaration keeps that expectation active through
its immediate instantiation drain. Bare generic calls are deferred, but a bare generic type use is
still pre-scanned before codegen, so its method diagnostic escapes. After a scoped method failure
is swallowed, the struct specialization's method lookup is incomplete on a later request; the
instantiation marker alone does not describe the partial aggregate state.

## Fix direction

Continue by isolating the bare type-use pre-scan and the partial specialization left by a swallowed
method-body error. Preserve the passing scoped and fresh queued `unique` cases. Do not add another
requeue-only guard without accounting for the aggregate's already-created type and method table.

## Neighbouring shape, same area (from the q14 fix, unchanged by q14/q17)

Bare-semicolon `expect_error("...");` placed BEFORE a statement that instantiates a failing
generic: the expectation matches (prints PASS) but the compile still exits 1 with
`Module verification failed: Terminator found in the middle of a basic block! label %entry`.
The body is emitted after `AbortFunctionBlocks` (cflat/LLVMBackend_ControlFlowAndFunctions.cpp
~143, which sweeps the whole module) already sealed `main`'s entry block, so `unreachable` is the
FIRST instruction of `entry` followed by the call. Not one-site; fix together with this issue.

Design note: the queued-instantiation catch (MainListener_Declarations.cpp ~3863) deliberately
clears `expectedError` on abandon so a queued body's error is not swallowed, while the
generic-function path (q14/q17) lets the expectation match and re-emits on the next request.
Whatever window rule is chosen must resolve that asymmetry explicitly.
