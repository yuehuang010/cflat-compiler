# expect_error cannot capture an error raised in a deferred generic STRUCT method body

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

The diagnostic escapes the armed expectation and fails the compile (exit 1, no PASS), in both the
statement-scope and bare-semicolon forms, and through the queued path (`unique<Box<int>>`,
MainListener_Declarations.cpp ~3863 try/catch, which disarms `expectedError` on abandon). A
generic FREE function body under the same expectation matches correctly since q14/q17.

## Root cause

Struct method instantiation runs while the enclosing function is scanned (or from the deferred
queue), before or outside the statement-scope expectation window; the queued-instantiation catch
deliberately clears `expectedError` so a queued body's error is not swallowed.

## Fix direction

Decide where the expectation should apply for deferred bodies: either arm it for the duration of
the drain triggered by a statement inside the block, or document that struct-method body errors
need a file-scope `expect_error` block around the struct definition (Test/errors/
err_generic_array_view_arg.cb header already notes the limitation). Stale `instantiatedGenerics`
entries on the queued path become reachable only once the error can be swallowed - handle them
in the same change (mirror q17's discard).

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
