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

Also (T52 round 6, 2026-10-06): a C++ CONSTRUCTOR taking `int*&&` or `int* const&` refuses a zero
macro (`C(ZERO)`, `D(Z0)`); free functions accept it after T52, clang accepts both.

Also (T52 review 7, 2026-10-06; literal 0 behaves the same, so these are not macro-only): function-like
`ID(0)` / `ID(ZERO)` and zero-arg `MZ()` at a C++ int* param, `if const (Z0 == 0)`, a C++ ctor taking
`int*` called with `0`, `operator+(int*)` with `0`, `tdp<int>(0)` - compare each with clang++
-std=c++20. Probes: scratch/repro_keep/t52_rev/rev7_t52_p/.

## After T68 review 1 (2026-10-07, pre-existing, master same)

- Real system `NULL` (`__null`) at a C++ `int*` param: "no overload" (clang accepts); T68's leg uses a
  fixture NULL defined as 0, so the system spelling is untested.
- Cast macro `#define RCAST ((int*)0)` imports as int and is refused at `int*` (clang accepts).
- Class with only `D(int*)`: `D(0)` refused (clang 30), while implicit `takeD(0)` works.
- `o + 0` with `operator+(Op, int*)` and `operator+(Op, long)`: clang ambiguous, CFlat picks int* (40).
- Probes: scratch/repro_keep/t68_rev/.
