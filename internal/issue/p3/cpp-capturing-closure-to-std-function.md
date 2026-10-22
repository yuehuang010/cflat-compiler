# Capturing closure into C++: remaining gaps after the N50 wrapper

Summary: N50 landed the generated `__cflat_closure<R (*)(A...)>` C++ wrapper (LLVMBackend_CInterop.cpp
`CxxClosureArgumentRefusal` / `CxxClosureWrapperPreamble`): capturing closures cross into std.function
(decl init, ctor arg, assignment init, field init, return, call argument) and into deduced C++ callable
parameters. Trivial captures cross by borrow (named) or by ownership (temps); non-trivial captures need
`move`; unique<T> and every by-reference (struct-valued, bonded) capture are refused. What is left:

- Closure with no static signature at the crossing: an `auto` local, a call result or a field passed to a
  deduced C++ parameter is refused ("signature is not known ... bind it to a 'Lambda<...>' local first").
  Into std.function the target signature is used as a hint, so `std.function g = f` with `auto f` works.
- A `Lambda<...>` function parameter (or any slot whose stores are not visible) cannot prove trivial
  captures, so it needs `move` even when the caller's closure was trivial.
- Only scalar/pointer parameter and return types cross (no struct, string, alias, move parameters).
- By-reference captures are always refused; existing borrow rules give no lifetime proof for a C++
  callable, so no case was allowed.
- std::move_only_function out of scope (ruling); unique<T> capture refused.
- Pre-existing, not N50: `sf = <callable>` assignment to an existing std.function (operator= not bound);
  a global std.function with a non-constant initializer; an inline value-returning lambda to a deduced
  parameter (or `auto h = (int x) => {...}`) cannot infer its return type; calling an `auto` closure
  local `v()` reports "function 'v' is not known".

Fix direction: carry the closure signature on `auto`/call-result/field NamedVariables (TypeAndValue would
need the --init serializer round-trip), and a per-closure "captures trivial" flag recorded at lambda
creation instead of the slot store scan.
