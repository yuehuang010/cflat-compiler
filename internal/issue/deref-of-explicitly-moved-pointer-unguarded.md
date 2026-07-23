# Dereferencing an explicitly-moved `unique T*` (statically null) segfaults with no diagnostic

Opened 2026-07-22 while closing the four `unique-*` assignment soundness issues
(commit 0a04749 cluster). Pre-existing gap, unrelated to those fixes - filed so it
is not re-investigated from scratch.

## Summary

After an explicit `move a`, the source `a` is intentionally left readable-as-null
(by design - see the `explicit-move-nulls-source` direction; the implicit-vs-explicit
asymmetry is deliberate and load-bearing for existing tests). A plain READ of `a`
(e.g. `a == nullptr`) correctly yields null and is allowed. But a DEREFERENCE of the
moved source - `a->field` or `a->method()` - is not guarded: it dereferences the
now-null pointer and segfaults (SIGSEGV / exit 139) at runtime, with NO compile-time
diagnostic, even though the compiler statically knows `a` was just moved (it set
`IsMoved` and stored null into the slot itself).

Both field access (`a->v`) and method-receiver access (`a->get()`) crash identically;
this is NOT specific to method receivers. It also matches thin `unique R*` and
`unique <interface>` locals - any explicitly-moved owning pointer.

The type system is the root of the unsoundness: `a`'s static type stays a non-nullable
`unique R*` while its value becomes null, so `->` is permitted with no null check.

## Repro (both segfault; expected: compile error on the deref)

```cflat
struct R { int v = 0; int get() { return v; } };
extern int main()
{
    unique R* a = new R();
    a->v = 5;
    unique R* b = move a;       // a is now null but still typed unique R*
    printf("%d\n", (int)(a == nullptr));   // OK: allowed, prints 1 (readable-as-null)
    printf("%d\n", a->v);                  // UNGUARDED: null deref -> segfault (exit 139)
    printf("%d\n", a->get());              // UNGUARDED: null deref -> segfault (exit 139)
    return 0;
}
```

`cflat --check` passes (0 failed) for all three lines; `--run` exits 139 on either
deref. Contrast the IMPLICIT-move path (`w.f = a;` without `move`), where a subsequent
plain read of `a` IS rejected with "use of moved variable 'a'" - so the diagnostic
machinery exists and fires there, just not for an explicit-move deref.

## Root cause

The moved-state machinery is present and used elsewhere:
- `NamedVariable.IsMoved` (LLVMBackend.h:725), `MarkVariableMoved` (LLVMBackend.h:15451),
  `FindMovedVariable`-style lookups, and the "use of moved variable" diagnostic sites
  (MainListener.h:11454, 17565, 17614, 17820, 18068, 18230).
- Explicit `move a` DOES null the slot and (post the interface fix) marks the source
  moved, but explicit-move reads are intentionally NOT flagged as moved-use errors
  (the readable-as-null design). The `->` / `.method()` dereference lowering never
  consults `IsMoved` at all, so a deref of a known-moved (statically-null) pointer is
  emitted as an ordinary load+GEP with no guard.

So this is a null-safety hole, not a leak/double-free: the value is known-null at
compile time but its non-nullable type lets `->` through.

## Fix direction

Keep the intentional readable-as-null semantics for plain reads, but reject a
DEREFERENCE of a variable whose `IsMoved` is set. At the `->` / member-call lowering
(the postfix member-access path in MainListener.h), when the base resolves to a named
local that is currently `IsMoved`, emit a compile error like
"dereference of moved variable '{}' (it is null after the move)" - reusing the same
lookup the "use of moved variable" sites already use. This diagnoses both the field
and method-receiver forms in one place and stays consistent with the design that
plain reads of a moved value remain legal. Add coverage under `Test/errors/`
(e.g. `err_deref_moved_pointer.cb`) with `expect_error`.

Broader (out of scope here): this is one instance of "non-nullable type holding a
null value after move"; the principled fix is to re-type a moved pointer as nullable
at its use sites, which ties into the ownership-transparent-assignment plan.
