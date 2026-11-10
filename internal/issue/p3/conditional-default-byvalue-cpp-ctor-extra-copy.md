# Conditional default passed to by-value C++ constructor adds copy and leaks

Summary: A by-value C++ constructor argument `c ? default : v` performs an extra copy and leaks an allocation.
Repro: `T391SinkV(T391Val v)` with `c ? default : v`, `c=true`; T459 repro path `scratch/repro_keep/box1022/t459/`.
Master vs expected: CFlat 21021; clang 20020.
Found: T459 r2 haiku review, 2026-10-24 box, PRE-EXISTING

MIX10's free-function, member, `const&` constructor, `auto`, `new`, and default-argument constructor cells match clang. Re-probing the exact by-value constructor shape with the fixture's equivalent `rev.Val(T391Val v)` does NOT match: master CFlat returns `21021` for `c=true`, clang++ returns `20020`; for `c=false` both return `711020`.

Root cause from the linked `-o` IR: the conditional selects a storage phi between `v` and a fresh `cxxfielddefault`, then CFlat unconditionally copy-constructs that phi into `cxx.argtemp` before calling the by-value constructor. On the default arm this adds a copy, and the original `cxxfielddefault` slot has no destructor registration, leaving one live allocation. Fix direction: build the conditional prvalue into the call's by-value argument slot, copying only the lvalue arm, and track the selected slot through normal full-expression cleanup.
Brace form `rev.Val s{c ? default : v}` is refused on master by the mixed-ownership rule ('?:' arms differ in ownership), not a copy count; see mixed-ownership-ternary-argument-refused.md.
