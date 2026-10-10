# NULL / nullptr macros (and nullptr into std::function) are refused at C++ call sites

Summary: clang++ -std=c++20 accepts each of these as a null pointer; cflat reports "no overload".
Pre-existing on master; found by the T52 reviews (2026-10-06).

- `lp(NULL)` and `#define Z NULL` (C++ `__null`) into `int lp(int*)`.
- `#define Z nullptr` into `int*`.
- A C header's `#define CN NULL` (`((void*)0)`) into a C++ `int*` param.
- `nullptr` into a by-value `std::function<int()>` param (clang: empty function).

Repros: scratch/repro_keep/t52_rev/rev2_t52_p/ (oracle.sh runs the clang++ twin) and rev_t52_p/.

Fix direction: T52 marks integer-literal-zero macros (CMacroEntry.ilz) as null pointer constants;
extend the macro classifier to `__null` / `nullptr` / `((void*)0)` bodies with the same identity
(never spelling) rule. std::function: the converting-ctor path must offer `nullptr_t` sources.
