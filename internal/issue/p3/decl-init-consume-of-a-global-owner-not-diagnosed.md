# The declaration-init spelling `Box o = gBox;` consumes a global owner with no diagnostic

Filed 2026-08-11 while documenting the q11 ruling in `doc/LANGUAGE.md`. Found because a doc
example was written against the ruling and did NOT compile to an error - the doc now carries a
"Known gap" note pointing here.

Severity: **P3, silent value loss** (memory-safe). The second execution of the same statement
reads an empty value. This is the same defect q11 fixed for six other spellings.

## Repro

```cflat
class Res { int id = default; ~Res() { } };
struct Box { unique Res* item = nullptr; };
Box mk(int n) { Box b; b.item = new Res(); b.item->id = n; return b; }
Box gBox;
int f() { Box o = gBox; return o.item == nullptr ? -1 : o.item->id; }
extern int main() { gBox = mk(5); printf("%d %d\n", f(), f()); return 0; }
```

Measured (`scratch/uniqglobal/d_declinit.cb`): `5 -1 seedNull=1`, rc 0. The global is emptied by
the first call with no diagnostic.

The ASSIGNMENT spelling of the identical consume IS rejected:

```cflat
int f() { Box o; o = gBox; return ...; }   // error: cannot consume owning value 'gBox' out of
                                            // storage that outlives this function ...
```

## What is already covered

`RejectImplicitConsumeOfOutlivingOwner` (`MainListener_Expressions.cpp`) is wired into the six
assignment/element/deref store arms and the field-path return arm. Verified enforced: plain `=`,
element slot, deref destination, field default (`struct W { Box[2] arr = { gSeed }; };`), and
`return gWrap.b;`. Verified NOT a consume at all, so correctly silent: `return gBox;` (returning
the whole global by value copies a borrow and transfers nothing).

## Root cause - UNRESOLVED, two candidate sites ruled OUT

The decl-init path for a whole owning-value local from a global source does not reach any
`ClassifyOwningAssignSource` call site that carries the source's storage. Two patches were written,
measured, and REVERTED because neither fired:

1. `MainListener_Declarations.cpp` ~4415 - that `ClassifyOwningAssignSource` site is reached only
   for a FIELD PATH source (`srcFieldPathNV`), not a whole global.
2. `EmitImplicitUniqueFieldMove` (`MainListener_Declarations.cpp`) - the guard placed at its head
   did not fire, so either this is not the path or `rightNV.Storage` is not the `GlobalVariable`
   there.

The emitted IR for the repro is the useful clue - the consume is a single null store into the
global's first field, with no aggregate zero:

```llvm
define internal i32 @_f_i32__() {
entry:
  %.unpack = load ptr, ptr @gBox, align 8
  %o = alloca %Box, align 8
  store ptr null, ptr @gBox, align 8     ; <- the consume
  store ptr %.unpack, ptr %o, align 8
```

Next step: find the emitter of that null store (breakpoint on it, or bisect the decl-init path),
rather than guessing at another call site.

## Fix direction

Call the existing `RejectImplicitConsumeOfOutlivingOwner` from whatever site emits that store; the
helper, its diagnostic and its remedies are already built and covered. Both remedies are known to
compile: `Box o = move gBox;` and giving `Box` a `copy()` method.

## Adjacent

The landed q11 ruling and its five other spellings - see the digest entry in
[[fix-issue-lessons]]. Regression coverage for the enforced spellings is in
`Test/errors/err_move.cb` and `Test/test_move.cb`'s `testUniqueGlobalStorage`.
