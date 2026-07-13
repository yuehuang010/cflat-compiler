# list<IFace> leaks its buffer (64 B) under HeapAudit

Created: 2026-07-13 (found while spiking interface fields)

## Summary

A `list<T>` whose element type T is an INTERFACE leaks its backing buffer
(observed: 64 bytes) even when the program does nothing but construct the list
and `add` one boxed element. Reported by the interface-fields spike, which
isolated it on the UNTOUCHED (stock) feature set - it is not caused by interface
fields, and it reproduces with no interface field anywhere in the program.

Status: REPORTED BY SPIKE, not yet independently re-verified. Confirm the
64-byte figure and the exact shape before fixing.

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

## Why it matters here

The UI framework stores its entire element tree in `list<IElement>`
(`core/ui_native.cb:799`), and the pointers -> interfaces refactor
(`internal/plan/ui-interface-refactor.md`) leans on `list<IParent>` much harder.
The UI examples are leak-clean today, so either the leak is smaller than
HeapAudit's reporting threshold there, or the tree's manual `destroyTree()` path
happens to avoid it. Worth understanding before Phase 3.
