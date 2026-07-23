# `_ = move <global>` does not release a GLOBAL owning string - buffer leaks

Filed 2026-07-23 (found incidentally during STEP R1 of
internal/plan/ownership-transparent-assignment.md; unrelated to sinks - no function
params involved).

## Summary

The discard-release form `_ = move x;` releases LOCAL owners (bare local, slot
`_data[i]`) via DropValue, but a GLOBAL owning string destination is silently not
released: the moved-out buffer leaks. No error, no free.

## Repro (scratch/global_string_discard.cb, verified at R1 working tree)

```cflat
import "diagnostic/heap_audit.cb";

string g_s = default;

extern int main()
{
    HeapAudit.enable();
    {
        string a = "hello there this is a heap buffer for sure";
        string b = "world and more text to force a heap buffer";
        g_s = a + b;      // global owns the concat buffer
        _ = move g_s;     // expected: buffer freed; actual: nothing emitted
    }
    u64 live = HeapAudit.reportLeaks();
    return live == 0u ? 0 : 1;   // exits 1: LEAK ptr size=85 from operator+_string
}
```

## Root cause (hypothesized, not fully traced)

The `_ =` discard branch of ParseAssignmentExpression (MainListener.h ~9364) resolves
the moved source as a live LOCAL via the stack scopes (stackNamedVariable) or as an
element slot via ApplyMovedSlotOwnership; a bare identifier naming a GLOBAL
(globalNamedVariable) does not match either recognizer, so no DropValue is emitted and
the move presumably degrades to a plain load/discard. Needs a trace to confirm which
guard the global falls out of.

## Fix direction

Extend the discard-release recognizer to resolve globals (globalNamedVariable lookup)
and route them through the same DropValue + null-the-source path locals use. Note
globals are also released by the exit-time global dtor pass - the fix must null the
global's slot so exit teardown sees a moved-out (null) value, not a double-free.
Add a regression leg to Test/test_collection_leaks.cb (global string + global owning
struct discard) when fixing.
