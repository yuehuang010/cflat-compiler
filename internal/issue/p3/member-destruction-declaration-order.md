# Struct members are destroyed in declaration order; C++ destroys them in reverse

Found by the T368 round-2 review (2026-10-20). Pre-existing on master; needs a maintainer ruling before any build.

## Repro
```cflat
int seq = 0;
struct A { int id = 0; ~A() { seq = seq * 10 + id; } };
struct H { A a; A b; A c; };
int main() { { H h = default; h.a.id = 1; h.b.id = 2; h.c.id = 3; } printf("%d\n", seq); return 0; }
```
Master prints 123. The clang++ twin prints 321 (members destroyed in reverse declaration order, [class.dtor]).
Test/test_generics.cb (T368 legs, lifecycle control) freezes the current CFlat order 12364678 vs the clang oracle 12364687.

## Why it matters
A later member that depends on an earlier one (a guard over a mutex member, a view/iterator over a container member, an
arena-allocated member over an arena member) is destroyed AFTER the thing it depends on - a use-after-free in the later
member's destructor. Structs with C++ class fields are lowered "like C++" (2026-09-27 rule A), so the order there may also
differ from what the C++ side expects.

## Ruling question
Follow C++ (reverse declaration order for members, matching the reverse order already used for globals at exit and for
locals at scope end) or keep declaration order as a CFlat rule (then document it in doc/LANGUAGE.md)? If C++: one change in
the member-dtor emission plus updating the frozen T368 control leg; check native, generic, and C++-field-lowered structs.
