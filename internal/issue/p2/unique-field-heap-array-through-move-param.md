# `new T[n]` reaches a `unique T*` FIELD through a `move T*` parameter and is freed as one object

**RULED 2026-10-21:** Static rule: `new T[n]` cannot bind an owning `move T*` sink; keep `move T[]` sinks; exempt trivially destructible T; spike the site count first. Buildable (left hold/). See fix-issue-lessons.md digest.

Found 2026-09-02 by the round-2 Opus review of the unique<T> branch. Memory-safety class:
n-1 element destructors never run and a `new[]` block is released with scalar `delete`.

## Repro

```cflat
int gd = 0;
struct Y { int v = 0; ~Y() { gd = gd + 1; } };
struct Holder { unique Y* arr = default; };
void stash(Holder* h, move Y* p) { h.arr = p; }
int main()
{
    { Holder h = default; stash(&h, new Y[3]); }
    return gd == 3 ? 0 : 1;   // observed: gd == 1
}
```

Probe: scratch/rev2/movearrayfield2.cb on the branch. The direct store `h.arr = new Y[3]`,
the `return move r` into `unique<T>`, and passing the array to a `unique<T>` parameter are
all rejected (Test/errors/err_unique_array_view.cb); this indirection is the one open leg.

## Root cause

Raw-array count is per-LOCAL metadata (`AllocatedByRawNewArray`, `RawArrayLength`,
`RawArrayLengthStorage`; `HasRawNewArrayProvenance` in LLVMBackend_OwnershipTemps.cpp).
A `move T*` parameter carries no provenance, so `RejectRawHeapArrayIntoUniqueField` cannot fire
inside `stash`, and the call site cannot tell that `stash` stores its parameter into a unique
field: `IsOwningSink` / `IsConsumeInferredSink` exclude pointer parameters and
`ParameterProvablyRetainsArgument` proves escape, not a unique-field destination.

## Fix direction

Needs a maintainer ruling; three options, cheapest first:
1. Record a per-parameter fact "stored into a unique field" during the callee's body walk
   (same family as the move-inference facts) and reject at the call site when an argument with
   array provenance binds such a parameter.
2. Reject `new T[n]` (array provenance) bound to ANY `move T*` parameter. Sound but breaks
   legitimate array sinks (`move Y* mkArr` style callers, Test/test_core.cb runRawCount*).
3. Put the count with the value (allocation header written by `new T[n]`) so the field's
   destructor can free correctly. Largest change; also fixes container elements.

## Measured 2026-09-02 (spike before ruling; probes scratch/rt/p1-p9.cb on the branch)

The "no per-parameter fact" premise above is WRONG. A `move T*` parameter already carries a
hidden `i64 <name>.raw_array_count` ABI argument (`ParameterCarriesRawArrayCount`,
LLVMBackend_ControlFlowAndFunctions.cpp ~1313; producer `RawArrayCountArgument`; consumer
LLVMBackend_CodegenHelpers.cpp ~186-210 which allocas a `.raw_array_count` slot and sets
`AllocatedByRawNewArray` on EVERY move pointer parameter, -1 = scalar/unknown). `move T*`
returns carry the same count. Measured with `new Y[3]` (Y has a destructor, gd counts dtors):

| shape | gd | verdict |
|-------|----|---------|
| `void sink(move Y* p) { }` scope-exit cleanup | 3 | count honoured (runtime branch on the slot) |
| `sink(move Y* p) { inner(move p); }` forwarding | 3 | count forwarded |
| `sink(move Y* p) { delete p; }` | 1 | explicit `delete` ignores the slot (scalar dtor + free) |
| `sink(move Y* p) { delete[_] p; }` | 0 | documented no-dtor form, as designed |
| `stash(Holder* h, move Y* p) { h.arr = p; }` (this issue) | 1 | `reset(move T* p)` receives the count, `_p = p` drops it |

So `move T*` is NOT a downgrade to a bare `T*`: it is a counted owning pointer at the ABI level,
and the suite pins that (Test/test_core.cb `runRawCount*`, forwarding shapes). Only two consumers
drop the count: explicit `delete p` on a parameter (filed separately,
internal/issue/p2/delete-of-move-param-ignores-raw-array-count.md) and adoption into `unique<T>`
(this issue).

Option 2 measured: the blanket form breaks core immediately (`toString` hands `new i8[32]` to
`_strOwned(move i8* buf, ...)`, string.cb:80 - every test fails). Narrowed to destructible
element types with precise provenance (count slot present), it finds ZERO true positives in the
suite but 5 false positives, all because the count slot marks every move parameter
(test_move.cb:503 `freeTree(node->left)`, test_core.cb:2490 `sinkPtr(makePtrOwned())`,
test_list_ownership.cb:1336 `items.add(move p)`, btree.cb:1207 `_freeSubtree(n->children[i])`
hit by 3 error tests). A static rule cannot separate "move param that IS an array" from "move
param of unknown count" - that is exactly what the runtime slot is for.

## Recommendation (for maintainer ruling)

Option 4, not in the list above: a RUNTIME check at the adoption site. Every path into a
`unique<T>` value goes through a call to core `unique<T>(move T* p)` / `reset(move T* p)`, and
the overload call emission (LLVMBackend_Overloads.cpp ~1605) already computes the count
argument for that parameter. When the callee is a core unique<T> ctor/reset, emit
`if (count >= 0) trap("unique<T> cannot own a heap array")` beside the static gate that handles
the provable case. No ABI change, no core edit, no new per-parameter fact; it is the same
sanitizer class as the existing move-origin probes. Option 1 duplicates a fact the ABI already
carries; option 2 has no sound static form; option 3 rebuilds what `.raw_array_count` is.

Under the alternative model ("`move T*` downgrades to bare `T*`, zeroing the source") the count
ABI would be deleted, and then `new T[n]` of a destructible T bound to any `move T*` (parameter
OR return) must be rejected statically, which also retires the "return 'move X*' instead"
advice in err_unique_array_view.cb:23 and the `runRawCount*` legs. Trivially destructible
arrays (the string case) would still pass as bare pointers since `free` is count-agnostic. That
is a language capability removal, not a fix, and needs its own ruling.

## Ruling 2026-09-03 (maintainer, second pass): block `new T[n]` binding to a bare `T*`

Direction chosen over the runtime trap (option 4): remove the hidden count by making a fresh
heap array bind only to a counted destination (`array<T>`, or `move T*` / `move T[]` at a
boundary) and rejecting the bare `T*` / `T[]` binding. Reverses the same-day "deferred, too
large" ruling on [[raw-array-count-desugar-direction]]. NOT yet started: the maintainer wants to
weigh the blast radius first. Two open questions block it:

1. Blast radius. Same-line grep of `new T[n]` sites on master d7af90d (heuristic, by destination):

   | tree | total | `T* x = new` | `T[] x = new` | `array/list<T> x = new` | reassign `x = new` | `return new` | call arg |
   |------|-------|--------------|---------------|-------------------------|--------------------|--------------|----------|
   | core | 178 | 93 | 53 | 0 | 18 | 0 | 1 |
   | Test | 608 | 94 | 377 | 4 | 27 | 15 | 12 |
   | example | 119 | 31 | 73 | 0 | 3 | 0 | 0 |

   `T[]` is documented as a thin `T*` (doc/LANGUAGE.md ~451), so a rule on `T*` covers the `T[]`
   column too: about 900 sites, 178 of them in core (string.cb 28, hpc/parallel.cb 16,
   filesystem.cb 13, graphic/bitmap.cb 8, regex.cb 6). Returns are already rejected today
   ("the element count is lost at the return").

2. Spelling ambiguity. `unique T[3]` could read as `unique<array<T>>` (one owned block of 3) or
   `array<unique<T>>` (3 owned scalars). Measured today: `unique Y[3] a;` is rejected ("unique
   requires a pointer or interface type"); `unique Y* a[3];` compiles as a fixed array of three
   unique pointers (the `array<unique<T>>` reading); `unique Y* p = new Y[3];` is rejected
   ("unique<T> does not own arrays - use 'array<T>'"); both `array<unique<Y>>` and
   `unique<array<Y>>` spell out and compile. The ruling must say which reading (if any) the
   short form gets before the desugar is designed.

Until ruled, this issue stays open and the hidden-count machinery stays; do not start option 4.

## Ruling 2026-09-04: raw `new T[n]` into `T*` stays legal

The 2026-09-03 "block `new T[n]` binding to bare `T*`" direction is withdrawn: `T* p = new T[n]`
is C-compatible and must be preserved. This issue is now check 4 of
internal/plan/delete-form-static-analysis.md: a static reject where array provenance is precise
(option 1 / narrowed option 2 above) plus the runtime trap at the core `unique<T>` ctor / reset
where it is not (option 4). Ruling still needed on whether the runtime trap is acceptable now
that the static-only route is closed; the measured false positives above stand.

## ON HOLD 2026-09-04 (maintainer, same day as the ruling above)

No work on the raw-heap-array / delete-form / unique-adoption area until the maintainer reopens
it. Do not re-raise the runtime-trap question and do not start check 4 of
internal/plan/delete-form-static-analysis.md. Related held item:
`p3/new-pointer-array-count-form-unparseable.md` (grammar only, but it borders this area).

## Discussion 2026-10-15 (NOT ruled; still ON HOLD)

Maintainer is not ready to rule; recorded so the next pass starts here. The 2026-09-04 hold stands.

- Context restated: CFlat rejected the C++ array cookie in favour of `array<T>` (core/array.cb, owning `{ T* _ptr; int _len; }`);
  `new T[n]` pairs with user-counted `delete[n]`. C++ `unique_ptr<T>(new T[3])` is silent UB, so a runtime trap (option 4)
  would be stricter than C++. Earlier attempt to make `new T[n]` return `array<T>` hit the blast-radius wall.
- `unique Y[]` today: refused for local and field ("array views are not supported"); see also
  p3/unique-local-view-diagnostic-doubles-brackets.
- Maintainer idea 1: add `unique T[]` (would be the `array<T>` layout - likely a spelling of it, not a second owned-array type)
  and block array -> unique pointer. Alone it does not close this issue: inside `stash` nothing says `p` is an array
  (the 2026-09-02 spike: 0 true / 5 false positives).
- Maintainer idea 2 (direction under consideration): `new T[n]` decays to a plain `T*` (legal, usable, passable), but it is NOT
  MOVABLE at a call site - it cannot bind to an owning `move T*` sink:
  ```cflat
  stash(&h, new Y[3]);                 // would be refused
  Y* q = new Y[3]; stash(&h, move q);  // would be refused (q has new[] provenance)
  ```
  Then `move T*` always means one object, this issue closes statically (no runtime trap), and the hidden
  `.raw_array_count` ABI on `move T*` becomes dead. Accepted residual: wrong `n` in `delete[n]` and provenance-lost pointers
  (raw field, arithmetic, cast) stay the user's job ("new T[] pure, size up to the user" - maintainer notes it can lead to
  mistakes; the held delete-form plan is the mitigation where provenance is precise).
- Open questions for the ruling:
  1. Does a new[] block still go to a counted `move T[]` sink (`move int[] _newStamp() { return new int[n]; }` regex.cb:859,
     `move VrS[] VrMake` test_cpp_interop:1243)? Recommendation: keep `move T[]`, close only `move T*`.
  2. Exempt trivially destructible T? core string.cb passes `new i8[32]` to `_strOwned(move i8* buf)`. Recommendation: exempt.
  3. Spike before ruling: count `new T[n]` sites reaching a `move T*` today, split by destructible element type.
