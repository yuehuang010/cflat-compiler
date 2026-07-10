# Aligned new: alignment tag lost when buffer escapes into a struct field

Created: 2026-07-09 (during G1, internal/plan/hpc-gaps.md)

## Summary

`new T[n] alignas(N)` (N > 16) allocates via the aligned operator new and
must be freed via `__delete_aligned`. The per-site alignment is carried on
the owning LOCAL (`NamedVariable.AllocAlignment`), consumed by scope-exit
auto-free and explicit `delete`. If the pointer is assigned into a struct
FIELD, the tag does not travel with it - a later `delete` through the field
(or any field-driven auto-destruct) takes the unaligned free path.

## Repro

```cflat
struct Holder { double[] buf = default; }
Holder h;
h.buf = new double[64] alignas(64);   // tag lost here
delete h.buf;                          // unaligned free of _aligned_malloc block
```

Consequence: heap corruption on Windows (_aligned_malloc block freed with
plain free) or mis-free of the allocator stash header.

## Current mitigation (documented in doc/HPC.md)

Keep over-aligned buffers owned by a local, or delete them explicitly while
the owning local is in scope.

## Fix direction

Carry AllocAlignment through field assignment (TypeAndValue already threads
ownership; alignment could ride the same move path), or make the aligned
allocator's stash-header scheme self-describing so a single free entry point
handles both (bigger change: Windows path uses raw _aligned_malloc today).
