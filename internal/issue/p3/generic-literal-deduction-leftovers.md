# Generic deduction and CTAD leftovers

## Landed (GENCTAD): parenthesized native constructor deduction

`Box(args)` deduces from the template's constructors (C++20 implicit guides, clang oracle) at the point where
`Undefined variable` would be reported; locals, parameters, implicit-this fields, globals and functions keep
precedence. Covers `auto b = Box(3)`, `Box b = Box(3)`, `f(Box(3))`, `Box(3).value`, `new Box(3)`, `ns.Box(3)`,
enclosing-namespace lookup, named arguments, and nested `wrap(Box(5000000000))`. Inside Box's own members
(methods, generic methods, static methods, lambdas) a bare `Box` is the current instance, the C++ injected
class name; constructors inside `if const` member arms are guides only when the deduced arguments take the arm. Legs: `gct_*` in
Test/test_generics.cb; refusals in Test/errors/err_generic_ctor_deduction.cb.

## Active item: brace constructor deduction

- `Box b{3}` (constructor) and `Agg a{3}` (no constructor) still report `'Box' is a generic type; you must
  provide type arguments (expects 1 type parameter: T), for example Box<int>`, same as master. CFlat braces
  are designated field init: explicit `Box<int> b{3}` is refused (`positional initializers are not supported
  for struct type 'Box<int>'`), so brace deduction needs a ruling on aggregate deduction first
  (`Agg a{value = 3}` -> `Agg<int>`, C++20 aggregate guides).

## Measured limits that follow the explicit form (not deduction bugs)

- Value-parameter templates (`struct Arr<T, int N>`): `Arr(3)` still reports `Undefined variable Arr.`
  (deduction is gated to type-parameter-only templates).
- File-scope `Box<int> g = Box(3)`: `global variable initializer must be a compile-time constant`; the
  explicit `= Box<int>(3)` is refused at file scope too.
- Copy deduction `Box(b)` with `b` a `Box<int>` deduces `Box<int>` like C++, then fails like the explicit
  `Box<int>(b)`: `no overload of 'Box<int>' matches the given arguments`.
- `Two(1, 2.5)` with one `T`: literals widen to `double`, then `no overload of 'Two<double>'`, the same as the
  explicit `Two<double>(1, 2.5)` and the generic-function twin `sumT(1, 2.5)`.

## Preserved behavior

- `szl<T>(list<int*>)`, namespaced `list<T>` patterns, `i8` conditional promotion, and static generic calls through an instance remain covered and supported.
- For `i8 pick(i8)` beside `T pick<T>(T)`, `pick(7)` selects the GENERIC (T = int), as clang: an exact match beats a narrowing one. Measured on master 2026-11-18 (prints 2). The earlier line here said the `i8` overload is selected; that was a wording error, never the behaviour (found by the GENLIT review). CONFIRMED 2026-11-18 (maintainer: "land the rest", on the main session's recommendation): keep the current result.
- Two unsuffixed literals that need different types continue to widen to the wider default type, e.g. `first(5000000000, 2)` deduces `long`.
- Remeasured after GENCTAD 2026-11-19 on master and fix/genctad: `pick(7)` = 2, `sizeof(first(5000000000, 2))` = 8,
  `i8 ? : 300` = 4 bytes, `szl(list<int*>)` = 18 (identical).
