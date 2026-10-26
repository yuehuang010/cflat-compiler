# Generic constructor deduction leftovers

The generic literal expression cases, inferred instance generic methods, and nested `list<T>` parameter deduction from T467 are fixed in the T467 worktree. The related BY DESIGN overload rule and literal widening ruling remain unchanged.

## Active item: native generic constructor deduction

C++20 accepts both `Box b = Box(3)` and `Box b{3}` when `Box<T>` has a constructor taking `T`. CFlat still rejects both forms:

- `scratch/t467/ctor_deduce.cb`: `Undefined variable Box.`
- `scratch/t467/brace_deduce.cb`: generic `Box` requires explicit type arguments.
- The master and T467 pre-fix binaries gave the same results; the C++20 twins print `4 3`.

The expression path only dispatches a native generic constructor after seeing explicit `genericTypeParameters()` at `cflat/MainListener_PostfixExpression.cpp:3945-3958`. The declaration path rejects the missing arguments at `cflat/MainListener_Declarations.cpp:1744-1748` through `RejectMissingNativeTemplateTypeArgs` (`cflat/MainListener_Declarations.cpp:3220`). There is no constructor argument deduction path to request the concrete generic type instance, and the ForwardRefScanner's generic type shell request currently requires explicit type arguments (`cflat/ForwardRefScanner.cpp:2282-2310`).

Fix direction: infer the generic type arguments from the selected constructor parameters before requesting the generic type instance, and issue the matching predeclaration request in ForwardRefScanner. Keep this item active until both constructor and brace forms have regression coverage.

## Preserved rulings

- (BY DESIGN, ruled 2026-10-13) For `i8 pick(i8)` beside `T pick<T>(T)`, `pick(7)` selects the non-generic `i8` overload. Keep the current result.
- Two unsuffixed literals that need different types continue to widen to the wider default type, e.g. `first(5000000000, 2)` deduces `long`.

## T467 review 1 leftovers (2026-10-24)
- `list<int*>` deduction: `szl<T>(list<T>)` called with a `list<int*>` reports "cannot find the type 'int*'" (master: "function 'szl'
  is not known"; explicit `szl<int*>(l)` works). MainListener_Generics.cpp ~727: the MangleType result for a pointer element must
  match the explicit <int*> spelling. Repro scratch/repro_keep/box1022/c02d_listptr_ded.cb.
- A namespace's own `list<T>` parameter (`ns.lf(list<int>)`) deduces T but resolves the pattern in the caller's scope ("no overload of
  'ns.lf<int>'", still refused). ResolveGenericTemplateBase at ~721 should resolve in the template's namespace. Repro c07d_ns_core.cb.
- A generic method beside a non-generic overload of the same name: the non-generic one keeps priority even when it does not match
  (`get(int)` + `get<T>(T)` called with a string is refused; clang picks the generic). Same as master.
- Same on master: an i8 variable in a ternary arm deduces i8 (clang int); a ternary of two string literals finds no overload; named
  arguments with generic deduction refused; generic method through implicit this (`get(3)` in a method) and a static generic method
  through an object (`h.sget(3)`) refused.
