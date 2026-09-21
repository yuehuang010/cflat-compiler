# Copying a nontrivial C++ class value out of a struct FIELD skips the copy constructor

## Summary

HELD 2026-09-16 by maintainer ruling: batched into a later CFlat-interop plan. Do not start.

HOLD LIFTED 2026-09-20 by ruling R1 (`internal/plan/cpp-bridge-transparency.md`): map directly
onto the C++ special members, CFlat picks the safer/faster default (borrow over copy, error over
an implicit copy of a move-only type). The whole family is scheduled as bridge plan phase 1.

`std.shared_ptr<Leaf> copy = net->l1;` (or `= m.l1;` on a value struct) where `l1` is a
`std.shared_ptr<Leaf>` field produces a bitwise copy of the handle: no copy constructor runs,
the use_count does not change. The copy is then treated as an OWNING handle: `copy =
std.make_shared<Leaf>(9);` runs `shared_ptr::operator=` on it, which releases one reference
of the control block the FIELD still shares. The count drops below the true number of owners
(field + registered child + copy): use-after-free / double release when the real owners go
away. Local-to-local copies (`std.shared_ptr<Leaf> b = a;`) are correct (copy ctor runs, count
bumps, reassignment releases only b's reference). Found 2026-09-14 during M10 timebox 2;
repro scratch/m10/tb2/r1.cb (field source, wrong: `u0=2 u1=2 u2=1`) vs r2.cb (local source,
right: `u1=2 u2=1`). Applies to any nontrivial C++ class held in a field, not only shared_ptr
and not only `[cpp] struct` owners.

## Root cause (hypothesis)

The declaration path for a foreign nontrivial class checks `lastCxxRetTemp_` (call result)
and the local-variable source for the copy/move-construct route
(cflat/MainListener_Declarations.cpp ~:4084-4143); a member-access source
(`obj->field`, `obj.field`) arrives as a loaded aggregate with `Storage` set to the field GEP
and falls to the raw `CreateStore`, i.e. the CFlat "borrow by default" struct rule, while the
later assignment treats the local as an owned C++ object (runs operator=) and its scope exit
runs the destructor (second release).

## Fix direction

Two consistent choices; needs the maintainer's ruling on which spelling means what:
1. C++ semantics for imported C++ classes: a declaration from any lvalue of a nontrivial
   C++ class copy-constructs (field, element, deref sources join the local-source path);
   `move` is the only way to transfer. This matches "C++ std types are just types".
2. Borrow semantics: a declaration from a field is an alias of the field (no own storage);
   then assignment through the alias writes the FIELD (operator= on the field, count stays
   consistent) and the alias runs no destructor; explicit `std.shared_ptr<Leaf> c = copy
   net->l1;` spelling would not exist, so this needs a copy spelling.
Either way: tests in Test/test_cpp_interop.cb with `cppi.Tracked` counters (copy_count,
dtor_count) for field, element (`vec[i]`), and deref sources, declaration and reassignment,
plus a shared_ptr use_count row; and the M78 rows that currently avoid reassigning a
field-sourced handle (rows 1543-1553) get the reassignment leg back.
