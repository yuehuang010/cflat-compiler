# nullptr into a lone C++ `const C&` parameter skips the C(std::nullptr_t) converting constructor (refused; was a crash)

`inline int m(const C& c)` with `struct C { int t; C(std::nullptr_t): t(220) {} };`: CFlat `m(nullptr)` passes
`ptr null` as the reference (SIGSEGV, rc 139); clang returns 220. Same for a lone free
`operator==(const C&, const C&)`: `cc == nullptr` crashes, clang builds a temporary C (242).

## Status after T12 (MSVC nullptr fix, 2026-10-02)

No longer a crash: a nullptr argument for a lone C++ class reference is now REFUSED ("no overload matches")
because CanImplicitlyConstructCxxClass does not count nullptr as convertible - e.g. `boxRef(nullptr)` with
`Box(std::nullptr_t)`, `spRef(nullptr)` with `const std::shared_ptr<int>&`; clang accepts (42 and 4). Probes:
scratch/repro_keep/t12/. Remaining work: materialize the converted temporary (and teach
CanImplicitlyConstructCxxClass about nullptr_t ctors) so these compile with clang's values.

## Repro

scratch/repro_keep/t8_conv/rv.hpp + conv.cb / conv2.cb (master 72dcd9d7 and the T8 branch alike).

## Root cause (GUESS)

With a single candidate no ranking runs; RankCxxConversionSequences scores the user-defined sequence (rank 3)
but the call / free-operator argument builders never materialize the converted temporary - the lone-candidate
fallback binds the raw null as an address.

## Fix direction

When a nullptr (IsCxxNullTypedArgument) binds a C++ class reference/value parameter, construct a temporary
through the converting ctor (same path as `CtorPick(nullptr)` construction), or refuse with a LogError; never
pass a null reference.

Found by: T8 review, fix timebox 2026-10-02.
