# Imported macro constant `auto&&` binding

Repro (`rv.hpp` from `scratch/rev_macroleft_opus_repro/`):

```cflat
import cpp "rv.hpp";
extern int main() { auto&& r = EM; return r; }
```

Master and the r1 worktree both reject at `auto&&` with `cannot understand the code at 'auto&&'`;
the matching `clang++ -std=c++20` program compiles and returns 0. The failure happens before the
initializer is visited, so this is not specific to imported macro binding.

Root cause: `cflat/CFlat.g4`'s `declarator` only wraps `directDeclarator`; it has no reference
declarator form. `&&` is available to expression rules, but not to object declarations.

Fix direction: if local reference declarations are supported, add lvalue/rvalue reference
declarators and implement `auto` reference deduction, category, and lifetime handling together.

# Anonymous-enum lvalues into C++ overloads, constructors and templates

Fixtures: `cpp_t98_macro_constant.hpp` (`T98_ANON_VALUE`, `using T98AnonEnum = decltype(T98_ANON_VALUE)`),
review repros in `scratch/rev_macroleft_sol_tmp/`, `scratch/rev_macroleft_r4_sol_tmp/` and
`scratch/rev_macroleft_r5_sol_tmp/` (each `.cb` has a `clang++ -std=c++20` `.cpp` twin).

State after macroleft r7:
- The ENUMERATOR rvalue follows clang (`IsUnnamedEnumValue`, keyed on the enumerator global in
  `cxxUnnamedEnumExpressions_`): brace construction picks the enum constructor (71, a namespaced one 31),
  paren construction of a namespaced class too (31), `T` / `T&&` deduce the enum type (3, 22). Legs 99313,
  99317, 99321, 99322 in `Test/test_cpp_interop.cb`.
- LVALUES behave exactly as on master (value or refusal), every shape: typed / auto / const local, field,
  array element, dereference, pointer copy, `?:` join, any consumer. The alias `T98AnonEnum` is only
  remembered by name (`RememberCxxAnonymousEnumAlias`) and stays the plain integer master maps it to.
  Proof: `scratch/macroleft_r7_tmp/master_parity.sh` (empty output = parity).

Open (clang value vs master/branch behaviour, all lvalues):
- `?:` join of two lvalues (`auto a = A; AT b = B; f(true ? a : b)`): clang `T&&` = `AT&` (21); master
  refuses (namespaced alias) or deduces int. Constructor / `T` / `T&` / variadic twins: clang 61/31/41/51.
- Int deduction through a dereference or pointer copy (`auto p = &x; auto q = p; f(*q)`, `h->a`, `*p`):
  clang deduces the enum (21, ctor 61); master deduces int (29, ctor 69) or fails at link.
- Reference category and const: `T&&` on a typed local is `AT&` (21), on a const one `const AT&` (23); `T&`
  deduces `AT` (11), const `const AT` (12); `C(AT&)` vs `C(const AT&)` constructors pick 51 / 52. Master
  refuses a namespaced alias (`cannot find the type 'ns::(unnamed enum ...)'`) and deduces int for a
  file-scope one (`T98AnonEnum v; macroAnonFwd(v)` is 29, clang 21).
- Comma arguments (`f((0, x))`, `f((0, h.a))`, `f((0, AN))`, `f((0, AN_MACRO))` with `#define AN_MACRO AN`):
  clang `T&&` deduces `AT&` (21) for a variable or field and `AT` (22) for the enumerator or macro. Master
  refuses them all; the branch's C++ comma lowering (ParseTupleExpression) refuses them too ("a comma
  expression ending in a variable or an anonymous-enum value"), the macro by its entry's clang type
  (`CMacroEntry::isUnnamedEnum`). Repros `scratch/rev_macroleft_r6_sol_tmp/comma_*.cb`; legs in
  `Test/errors/err_macro_constant_iref.cb`.
- Free overloads with an anonymous-enum and an `int` parameter (`macroTypePick`): clang picks the enum one
  (81) for the enumerator, the macro and any lvalue; master and the branch refuse ("cannot choose which",
  both spelled `int`).
- An anonymous-enum MACRO (`#define AM rw::AN`) in a C++ constructor call: `C2(AM)` now returns clang's
  value (31) and `rw::C4(RWM)` returns 41, both matching clang. The old link failure
  (`undefined rw::C2::C2(rw::$_0)`) is no longer reproduced. Function templates (`tw(AM)`, macro prvalue) are
  fixed in macleft (identity via the macro global, legs 99318/99319). `std.min`, `retr`, `retf` and
  `make_pair` over the macro fall back to master's int path (legs 99324-99326).

Fix direction: an unnamed-enum lvalue needs its enum identity, cv and reference category spelled from its
storage in every wrapper (the int it is lowered to cannot carry them). r2-r6 tried to track auto slots and
respell wrappers; each round left a shape silently wrong, so that work was backed out in r7. A function with
an unnamed-enum parameter and C++ linkage also needs a generated C++ thunk (clang mangles the type `$_0`).
