# Generic deduction and CTAD leftovers

## Active item: native generic constructor deduction (parenthesized and brace)

Parenthesized constructor deduction is removed from this run. It was literal-only, changed existing lookup meaning, and did not agree with C++ deduction or target conversion. Keep it open until a complete design and coverage are available.

Review direction:
- `Name(args)` may trigger constructor deduction only at the lookup point where `Undefined variable` would otherwise be reported; locals, parameters, implicit-this fields, globals, and functions must keep precedence.
- Type each constructor argument through the normal expression path, not by walking primary expressions. Include non-literals, casts, unary/binary expressions, calls, fields, indexes, and overload selection.
- Deduce from constructor parameters, then require the deduced type to equal the destination type. Do not use the destination to replace argument deduction.
- Resolve bare and qualified names through namespaces, including enclosing namespace lookup.
- Cover the remaining forms noted by review: `auto b = Box(3)`, `f(Box(3))`, `Box(3).value`, `new Box(3)`.
- Brace spelling `Box b{3}` reports `'Box' is a generic type; you must provide type arguments (expects 1 type parameter: T), for example Box<int>` (remeasured after removing the leftover bare-generic rejection branch; same behavior as master). Brace deduction remains open.
- Nested argument deduction example: `Box b = wrap(Box(5000000000))` remains unsupported.

## Other open generic-literal inference gaps

- A ternary of two string literals still has no matching `sz<i8>` overload:
  ```cflat
  struct Box<T> { T value; Box(T x) { value = x; } };
  int sz<T>(T v) { return (int)sizeof(T); }
  int main() { bool k=true; printf("%d\n",sz(k?"a":"b")); return 0; }
  ```
- A nested generic pattern declared in an enclosing namespace is still refused: `A.C.f<T>(W<T>)` with `W` in `A`:
  ```cflat
  namespace A { struct W<T> { T v = 6; }; namespace C { int f<T>(W<T> x) { return x.v; } } }
  int main() { A.W<int> a = default; printf("%d\n",A.C.f(a)); return 0; }
  ```

## Open pointer-element spelling gap

- `.p$` type spelling still fails when the deduced element is a pointer to a generic or otherwise compound base. `szl(list<G<int>*>)` fails with `cannot find the type 'G<int>'`, while explicit `szl<G<int>*>(...)` gives 8. The same gap covers `G<int>**`, `G<int>***`, `G<int*>*`, `ns.G<int>*`, `list<int>*`, `function<int(int)>*`, `(const int)*`, and `(const int)**`. These forms are also refused on master; only the diagnostic differs. Repro: `import "list.cb"; struct G<T> { T v = default; }; int szl<T>(list<T> l) { return (int)sizeof(T); } int main() { list<G<int>*> l = default; printf("%d\n",szl(l)); return 0; }`. `(const int)*` fails with "cannot find the type '.p$int'" instead.

## Preserved behavior

- `szl<T>(list<int*>)`, namespaced `list<T>` patterns, `i8` conditional promotion, and static generic calls through an instance remain covered and supported.
- (BY DESIGN, ruled 2026-10-13) For `i8 pick(i8)` beside `T pick<T>(T)`, `pick(7)` selects the non-generic `i8` overload. Keep the current result.
- Two unsuffixed literals that need different types continue to widen to the wider default type, e.g. `first(5000000000, 2)` deduces `long`.
