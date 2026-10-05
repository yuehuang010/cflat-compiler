# P4: `<=>` three-way comparison operator in CFlat

Raised by the maintainer 2026-10-02 (TODO) after the std_full tier-2 suite: R1/R2 review confirmed CFlat has no
`<=>` spelling, so std_full legs for `pair`/`string`/`vector` `<=>`, `compare_three_way`, `strong_order` and
`lexicographical_compare_three_way` results could not be written as user code. Today CFlat only CONSUMES
`<=>`: an imported C++ class's `operator<=>` is extracted, `std::*_ordering` lowered as i8, and CFlat's
`<` / `<=` / `>` / `>=` on that class are rewritten through it (2026-09-09 operator-compatibility ruling,
landed 0d501a77).

Earlier rulings this item would revise:
- 2026-09-07: `<=>` excluded from the CFlat struct-member overload set.
- 2026-09-09: "no `<=>` spelling in CFlat itself".

## Proposed surface

1. Expression `a <=> b` at relational precedence (C++ puts it between shift and relational).
   - Builtin scalars: integers and pointers -> strong ordering, floating -> partial ordering (NaN unordered).
   - Imported C++ class: calls its `operator<=>` (member, free or rewritten/reversed), C++20 rules.
2. Result type: the C++ std ordering types when `<compare>` is imported (`std.strong_ordering` etc.), so the
   value crosses into C++ APIs unchanged; tests via `== 0`, `< 0`, or `std.is_lt(r)`.
3. CFlat struct member `ordering operator<=>(T other)`; a declared `<=>` synthesizes `<`, `<=`, `>`, `>=`
   (and `==` only if declared or defaulted, as C++). Whether `= default` member-wise `<=>` is wanted: ruling.

## Alternatives considered

1. **Consume-only (status quo).** No spelling; users call `std.compare_three_way{}(a, b)` - which itself is
   blocked today (p2/cpp-transparent-functor-template-call-operator-not-known.md, functor-temporary section).
2. **Library function only** - a core `compare(a, b)` returning a CFlat `ordering` enum. No grammar change, but
   the result does not interoperate with C++ ordering types and C++ classes' `<=>` cannot bind onto it.
3. **Full surface (above).**

## Open questions for the ruling

- Result type with no `<compare>` import: a core `ordering` type, or require the import?
- `= default` member-wise `<=>` (and `==`) on CFlat structs: in or out?
- Lexer: `<=>` becomes one token. Check generic argument lists ending in `<=` / `>` adjacency (e.g. a
  hypothetical `T<U<=>`) - expected harmless, verify against CFlat.g4.

## Acceptance

- `a <=> b` on int, double (NaN -> unordered), pointers, `std.string`, `std.pair`, `std.vector`; result
  compared with 0 and passed to `std.is_lt` / `std.is_eq`.
- CFlat struct with a declared `operator<=>` gets the four relationals; misuse diagnosed with `LogError`.
- std_full: add `<=>` legs to std_full_20_bit_numbers_compare_span and std_full_11_utility_tuple (tuple `<=>`)
  and their twins; coverage extends `Test/test_operators.cb`, no new test file.

## Ruling

RULED 2026-10-02 (maintainer): add `<=>` to the CFlat language - expression form, CFlat struct member
`operator<=>`, imported C++ classes - revising the 2026-09-07 / 09-09 exclusions. Buildable. Still to settle
in the brief (default: follow C++): result type with no `<compare>` import.

RULED 2026-10-02 (maintainer, "CFlat should also have == operator too"): defaulted member-wise `==` is in scope
with `<=>`. Probe on master: a user-declared `bool operator==(T o)` works and `!=` derives from it; a struct
with no declared `==` is refused ("no operator '==' for type 'Q'"). Surface (follow C++20):
`bool operator==(T other) = default;` compares fields in declaration order; `operator<=>(T other) = default;`
compares member-wise AND implies a defaulted `==`. A field type with no `==` / `<=>` makes the default
deleted -> LogError at the use site. No implicit `==` on undeclared structs (C++ parity). Acceptance adds:
defaulted `==` / `!=` on a struct with int, string and nested-struct fields; defaulted `<=>` gives all six
relationals; a field without `==` refused.
