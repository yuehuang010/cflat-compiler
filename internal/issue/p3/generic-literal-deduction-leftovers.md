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

## Preserved behavior

- `szl<T>(list<int*>)`, namespaced `list<T>` patterns, `i8` conditional promotion, and static generic calls through an instance remain covered and supported.
- For `i8 pick(i8)` beside `T pick<T>(T)`, `pick(7)` selects the GENERIC (T = int), as clang: an exact match beats a narrowing one. Measured on master 2026-11-18 (prints 2). The earlier line here said the `i8` overload is selected; that was a wording error, never the behaviour (found by the GENLIT review). CONFIRMED 2026-11-18 (maintainer: "land the rest", on the main session's recommendation): keep the current result.
- Two unsuffixed literals that need different types continue to widen to the wider default type, e.g. `first(5000000000, 2)` deduces `long`.
