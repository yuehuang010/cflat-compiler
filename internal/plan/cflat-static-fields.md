# CFlat `static` struct fields: shared storage

Status: PLANNED (ruled 2026-11-18, maintainer). Not started. Issue: `internal/issue/p3/member-type-path-remaining-gaps.md`
section 1.

## Ruling

`static` on a CFlat struct data member gets the C++ / C# meaning: one storage per struct (per instantiation of a
generic struct), not a per-instance field. Read and written as `Spec.value` and through an instance (`a.value`);
the initializer runs once. No interim refusal - the spelling already parses and both parents give it this meaning.
Storage follows the globals ruling (owning types legal, implicit consume an error, `move` re-inits, no exit-time
destruction).

## Today (measured 2026-11-18, Release, macOS arm64)

`struct Spec { static int value = 7; int x = 0; }`: `static` is accepted and ignored. The field is per instance
(`a.value = 9` leaves `b.value == 7`), `sizeof(Spec) == 8`, and `Spec.value` is refused with "'value' does not
name a value here". C++ static data members of imported classes already work (STATMEM 289499697 folds scalar
constexpr / const statics; runtime reads go through the C++ symbol).

## Phases (one branch each)

1. Storage and access. A `static` data member is left out of the struct layout and emitted as one module global
   per struct, mangled under the owner. `Spec.value`, `a.value`, `p->value` and a bare `value` inside a member
   function read and write it. Constant initializer only. Both `ParseDeclarationSpecifiers` copies; any new
   `StructData` field goes through the `--init` cache round-trip in the same change. Acceptance: the repro reads
   `b.value == 9`, `Spec.value == 9`, `sizeof(Spec) == 4`.
2. Generic structs: one storage per instantiation (`Box<int>.count` and `Box<long>.count` are distinct), merged
   across modules that instantiate the same type.
3. Non-constant initializers and owning types: initialization order and the globals ruling applied to a static
   member (`unique T*`, `string`, a struct with a destructor).
4. Surfaces: namespace-qualified and aliased owners, a static inside a nested struct, LSP hover / completion,
   `--symbol-dump`, and the Structs section of `doc/LANGUAGE.md`.

## Open questions (ask before the phase that needs them)

- Phase 1: is `static` allowed on a `[cpp]` struct field, and does it then bind as a C++ static data member?
- Phase 1: `static const` / compile-time use (`if const (Spec.value > 0)`, array bound, value template argument)
  - in scope, or a later phase?
- Phase 3: when does a non-constant initializer run - before `main` in declaration order (C++), or on first use
  (C#)? The parents disagree; the skill's rule says C++ wins, not confirmed for this case.
- Phase 3: thread-local static members (`thread_local static`) - wanted?
- Static member FUNCTIONS are not part of this ruling.
