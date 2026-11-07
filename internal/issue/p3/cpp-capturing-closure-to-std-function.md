# Capturing closures passed to C++: remaining refusals after FNWRAP

N50 added the generated `__cflat_closure<R (*)(A...)>` wrapper. FNWRAP fixed the concrete
capture-triviality false refusals for a copied typed `auto` closure and a typed closure in a field;
both preserve their signature and match clang's `apply_n(F&&)` pick and value. FNWRAP r3 also
lands scalar, pointer, struct, string, alias and move-parameter closure signatures, and by-reference
captures passed to a callee that does not keep a reference to the closure. Typed locals, call results,
`std.function`, and deduced callable arguments remain covered by their existing legs.

Remaining limitations (each one is a kept refusal, with the clang result it differs from):

- A value with no static closure signature cannot cross to a deduced C++ callable parameter. The
  target signature can still guide conversion into `std.function`.
- A typed closure that may own a non-trivial capture (for example a string) is refused when it is
  copied into C++ implicitly, from a field, an array element, an `auto` copy or a call result, and
  needs an explicit `move s`. Clang accepts these with `apply_n(F&&)` on an lvalue (value 7); CFlat
  refuses at the implicit copy site because it has no proof that the C++ callee copies nothing
  owning. The proof walks the slot's root local: every write that may reach the closure bytes must
  store a capture-free or trivially-capturing env. A slot reached through a loaded pointer
  (`p->fn`), a parameter, or a variable index (`arr[i]`) has no such proof and is always refused,
  whatever it captures. A trivially-capturing closure in a field or constant element is accepted.
  Tests: err_cpp_closure_fnwrap_refusals.cb.
- A by-reference capture (a struct captured by a lambda) is refused when the C++ callee keeps a
  reference to the closure (`closureKeptParams`, for example `t28.holdfwd`), because the captured
  local may leave scope while C++ still holds the closure. A non-retaining `apply_n(F&&)` and a
  by-value owner such as `holdowned(f)` (clang 20) are accepted in scope. A closure result
  (`copy_return(f)`, `cref(f)`) keeps the argument's capture names as bond sources, so returning it
  from the capturing frame is refused as for the native `return f`. A record result such as
  `OwnedBox<F>` carries no bond; no escape route was found for it (its type cannot be named as a
  return type and its copy assignment is deleted), so it is not refused.
- A unique capture cannot be copied into `std::function`; clang rejects that shape because the
  lambda copy constructor is deleted. A by-reference capture of a move-only value is also refused
  for a non-copying callee such as `apply_n(F&&)`, which clang accepts; the existing legs pin that refusal.
  `std::move_only_function` remains outside this issue's scope.
- Pre-existing global `std.function` initialization from a non-constant value remains unsupported.
  Inline value-returning lambdas still need a `Lambda<...>` or `function<...>` target to infer the
  return type.

Found by: N50 and FNWRAP (2026-11-05); FNWRAP r3 and r4 (2026-11-06).
