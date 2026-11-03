# Conditional default passed to by-value C++ constructor adds copy and leaks

Summary: A by-value C++ constructor argument `c ? default : v` performs an extra copy and leaks an allocation.
Repro: `T391SinkV(T391Val v)` with `c ? default : v`, `c=true`; T459 repro path `scratch/repro_keep/box1022/t459/`.
Master vs expected: CFlat 21021; clang 20020.
Found: T459 r2 haiku review, 2026-10-24 box, PRE-EXISTING

MIX10: not reproduced. Master matches clang (value and counters) for `c ? default : v` into a by-value C++ parameter or constructor in the shapes: paren ctor `rev.Val(c ? default : v)`, free function `t391_take(...)`, member `h.take(...)`, `T391Sink(...)`, `auto` declaration `auto s = rev.Val(...)`, `new rev.Val(...)`, and default-argument ctor `rev.Def(c ? default : v, 4)`. Clang: 711020/20020, 1111020/420020 for `c` false/true.
Brace form `rev.Val s{c ? default : v}` is refused on master by the mixed-ownership rule ('?:' arms differ in ownership), not a copy count; see mixed-ownership-ternary-argument-refused.md.
