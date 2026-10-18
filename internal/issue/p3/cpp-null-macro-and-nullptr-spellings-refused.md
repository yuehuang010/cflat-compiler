# NULL / nullptr macros (and nullptr into std::function) are refused at C++ call sites

Summary: clang++ -std=c++20 accepts each of these as a null pointer; cflat reports "no overload".
Pre-existing on master; found by the T52 reviews (2026-10-06).

- `#define Z nullptr` into `int*`.
- A C header's `#define CN NULL` (`((void*)0)`) into a C++ `int*` param.
- `nullptr` into a by-value `std::function<int()>` param (clang: empty function).

Repros: scratch/repro_keep/t52_rev/rev2_t52_p/ (oracle.sh runs the clang++ twin) and rev_t52_p/.

Fix direction: T52 marks integer-literal-zero macros (CMacroEntry.ilz) as null pointer constants;
extend the macro classifier to `__null` / `nullptr` / `((void*)0)` bodies with the same identity
(never spelling) rule. std::function: the converting-ctor path must offer `nullptr_t` sources.

## T103 status (2026-10-10)

The T103 macro spelling legs now pass in `Test/test_cpp_interop.cb`, including the C header's
`((void*)0)` macro. `nullptr` into a by-value `std::function<int()>` remains unsupported; its
fixture leg was removed after three ranking-path attempts, so this issue remains active.
T103 round 2 (2026-10-10): the `cppcr.NpMutRef(nullptr)` regression is fixed (the variadic wrapper
spelled a nullptr argument as `std.nullptr_t`, which let clang bind it to `nullptr_t&`; that spelling
was removed from `RequestCxxVariadicConstructor`). Literal-0 pointer admission was dropped (see the
p2 `cpp-null-constant-ambiguous-ctor-accepted` file).

Also (T52 round 6, 2026-10-06): a C++ CONSTRUCTOR taking `int*&&` or `int* const&` refuses a zero
macro (`C(ZERO)`, `D(Z0)`); free functions accept it after T52, clang accepts both.

Also (T52 review 7, 2026-10-06; literal 0 behaves the same, so these are not macro-only): function-like
`ID(0)` / `ID(ZERO)` and zero-arg `MZ()` at a C++ int* param, `if const (Z0 == 0)`, a C++ ctor taking
`int*` called with `0`, `operator+(int*)` with `0`, `tdp<int>(0)` - compare each with clang++
-std=c++20. Probes: scratch/repro_keep/t52_rev/rev7_t52_p/.

## After T68 review 1 (2026-10-07, pre-existing, master same)

(T296 2026-10-16: the system NULL at int* and the cast macro ((int*)0) are fixed; the NULL
leg in Test/test_cpp_interop.cb is a guard, it already passed on master.)
- Const-pointee cast macros (`((const int*)0)`) stay refused at `int*`: the typed pointer does not carry the
  const pointee (clang accepts them at `const int*`).

- Class with only `D(int*)`: `D(0)` refused (clang 30), while implicit `takeD(0)` works.
- `o + 0` with `operator+(Op, int*)` and `operator+(Op, long)`: clang ambiguous, CFlat picks int* (40).
- Probes: scratch/repro_keep/t68_rev/.

## Note 2026-10-16 (Windows close gate)
On LLP64 a `((int*)0)` macro at a C++ `long*` param is accepted (CFlat `long` = target width, so the int pointee matches);
clang-cl refuses. T296 leg retargeted to `long long*` (distinct on both ABIs). Whether CFlat should keep C++ `long` distinct from
`int` on Windows at C++ pointer params is open (L = target long ruling covers CFlat-side spelling only).
