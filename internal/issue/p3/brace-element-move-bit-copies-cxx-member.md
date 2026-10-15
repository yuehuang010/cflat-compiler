# A `move` brace element bit-copies a C++ member instead of move-constructing it

Summary: a CFlat struct holding a C++ class member, written as a `move` element of a positional
array brace list, is bit-copied into the slot. A plain scalar `move` runs the C++ move constructor.
`struct HV { cppexp.ConvCnt m; int k = 1; }; struct OV { HV[2] h; };` with
`HV v1 = { m = 3 }; HV s = move v1;` gives `s.m.v == 1003` (move ctor: 3 + 1000), but
`OV o = { h = { move v2, s } };` gives `o.h[0].m.v == 4` (v2 was 1004; no move ctor ran).
Same for user-ctor elements (`HU[2]`, `T129UserCtor`): `{ move x1, x2 }` gives 1003, scalar 2003.

Found: T218 (2026-10-14), pre-existing on master. Not caused by the element-overwrite fix; the same
bit copy happens on the fresh (no user-ctor) path. Counts balance at scope end only because the
moved-from local's destructor still runs on the zeroed bits.

Root cause (measured, not yet traced): ConsumeOwningBraceElementSource (MainListener_Expressions.cpp,
Move branch) takes the source's struct value as bits, zeroes the source, and the element store copies
the bits. Rule A (cflat-struct-cxx-field-lowering-ruling) says C++ members of a lowered struct are
move-constructed member-wise (EmitLoweredMemberwiseMove, reached through EmitCxxCopyOrMoveConstruct).

Fix direction: for a struct type that NeedsCxxLoweringType, a `move` source into an array element
goes through EmitCxxCopyOrMoveConstruct(elem, elemPtr, source, useMove) and the source keeps its
destructible moved-from members, as the scalar path does. Shared by fresh and update paths, so it
changes values on master: needs a leg per path (`HV` fresh, `HU` update) and a suite run.
