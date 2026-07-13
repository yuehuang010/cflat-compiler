# list<IFace> leaks its buffer (64 B) under HeapAudit

Created: 2026-07-13 (found while spiking interface fields)

## Summary

A `list<T>` whose element type T is an INTERFACE leaks its backing buffer
(observed: 64 bytes) even when the program does nothing but construct the list
and `add` one boxed element. Reported by the interface-fields spike, which
isolated it on the UNTOUCHED (stock) feature set - it is not caused by interface
fields, and it reproduces with no interface field anywhere in the program.

Status: DOES NOT REPRODUCE - disproven 2026-07-13. This is a MEASUREMENT ARTIFACT
in the spike's harness, not a compiler/stdlib bug. See "Verification" below before
spending any more time on it; this file is kept only so the claim is not
re-investigated from scratch.

## Repro (sketch - from the spike's isolation program)

```cflat
// HeapAudit.enable(); ... compile with -o, then reportLeaks()
interface IFace { int kind(); };
class Impl : IFace { int kind() { return 1; } };

list<IFace> xs;
xs.add(new Impl());   // boxed -> fat ptr stored by value
// ... hand-free the elements as usual; the LIST BUFFER still leaks 64 B
```

Removing the list (keeping the boxing) => zero leaks.

## Root cause (hypothesis)

`list<T>`'s destructor frees elements only when `is_pointer(T)`, and otherwise
calls `_data[i].~()` and frees the buffer (`core/list.cb:244-260`). An interface
T is not a pointer, and `.~()` on an interface value resolves through
`GetOrCreateFullDestructor(typeName)`, which returns nullptr for an interface
name (interfaces are not in `dataStructures`), so the element destruct is a
no-op. That much is BY DESIGN (interface elements are owned by convention and
freed by hand - this is how the UI tree works). What is NOT by design is the
BUFFER itself surviving; that suggests the `list<IFace>` destructor is not
running at all, rather than running and skipping the elements.

Suspect: an interface element type makes the list's own dtor fail to bind or fail
to be registered, so scope exit never destructs the list. Compare against
`list<int>` and `list<SomeClass*>` under the same harness.

## Verification (2026-07-13)

Re-ran the reported shape under the HeapAudit oracle (compiled with -o). Every
variant is leak-clean - `list<IFace>` destructs and frees its buffer normally:

  - list<IFace> + add(new Impl) + hand-free elements, with and without clear(): 0
  - list<IFace> grown past its initial capacity (40 elements):                   0
  - list<IFace> as a class FIELD, freed via a destroyAll() helper:               0
  - list<int> and list<Impl*> baselines under the same harness:                  0

The oracle is not blind to the buffer: a deliberately leaked `new int[16]` (64 B,
the same array-new shape list<T> uses for _data) IS reported, so a real buffer leak
would have shown up.

Root cause of the false report: HeapAudit is a point-in-time scan of LIVE
allocations, so it cannot distinguish "leaked" from "still in scope" - exactly what
heap_audit.cb's own comment on reportLeaks() warns about ("call at a quiescent
point, e.g. after a scope has exited"). The spike called reportLeaks() while the
`list<IFace>` was STILL LIVE in the enclosing scope, so the list's own live buffer
was reported as a 64-byte leak. Reproduced directly: with a live `list<IFace>` in
scope, reportLeaks() reports the 64 B buffer; adding a live `list<int>` alongside it
reports that list's 16 B buffer too. That also explains the spike's control -
"removing the list => zero leaks" is simply "no live buffer left to report".

Nothing to fix. The element-ownership-by-convention design (dtor frees elements only
when is_pointer(T)) is intact and is what the UI tree relies on.

## Why it mattered (original motivation, now moot)

The UI framework stores its entire element tree in `list<IElement>`
(`core/ui_native.cb:799`), and the pointers -> interfaces refactor
(`internal/plan/ui-interface-refactor.md`) leans on `list<IParent>` much harder.
The UI examples are leak-clean today, so either the leak is smaller than
HeapAudit's reporting threshold there, or the tree's manual `destroyTree()` path
happens to avoid it. Worth understanding before Phase 3.
