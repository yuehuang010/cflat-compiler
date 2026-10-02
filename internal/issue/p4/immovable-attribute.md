# P4: `[immovable]` attribute - opt a type out of moves

Raised by the maintainer 2026-09-30 while discussing W8 (p2/cpp-struct-list-field-relocated-bitwise).
Separate from that bug: W8 is about HOW a movable type is moved (bitwise vs C++ move constructor);
this item is about declaring a type that may not be moved at all.

## Proposed surface
- `[immovable] struct Node { ... };` - every type is movable unless it carries the attribute.
- Any operation that would move a value of the type is a compile error at the operation: `move x`,
  by-value return of a local, by-value pass to a `move` parameter, container insertion by value,
  relocation inside a growing container, assignment that transfers ownership.
- Construction in place, borrows (`T*`, `alias T`) and use through a pointer stay legal.
- A struct with an `[immovable]` field is itself immovable (propagates).
- C++ interop: a C++ class with deleted move AND copy constructors maps to `[immovable]` semantics.

## Alternatives
- Rust `Pin<P>` (pinning a place, not a type) - more flexible, much heavier.
- C++ deleted move/copy constructors - CFlat has no user-declared special members to delete.

## Acceptance
- Error tests for each move operation above in a new `Test/errors/err_immovable_*.cb` set.
- `is_unique` / `is_copyable` style trait (`is_movable`) answers false for the type.

Needs a maintainer ruling on the surface (spelling, propagation, the C++ mapping) before build.

RULED 2026-10-01 (maintainer): surface APPROVED as proposed: spelling `[immovable]` (lowercase, like the language-semantics attributes `[unique]`, `[cpp]`, `[winrt]`; PascalCase is for library metadata such as `[JsonName]`), every move operation an error at the operation, propagates to containing structs, a C++ class with deleted move AND copy maps to it, `is_movable` trait. Buildable; move to a plan or p2 run when scheduled.
