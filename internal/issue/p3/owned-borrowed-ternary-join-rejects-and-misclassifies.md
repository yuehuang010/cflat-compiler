# `?:` owned-vs-place joins: const leaf still refused (imported C++ class)

Status (OBJOIN, 2026-11-19): an imported-class prvalue arm (`default`, a call, `a + b`, a nested
join of such) beside an UNMOVED NON-CONST place arm (local, parameter, field, `this` field, `*p`,
C++ `T&` result) now copies the place into the prvalue join, as clang: binary operands, `y = ...`,
call arguments, brace members. Legs 99705-99742 in Test/test_cpp_interop.cb (header
Test/library/cpp_objoin.hpp). Native CFlat structs keep the mixed-ownership refusal (ruling
2026-09-03); strings were already correct.

Residue (clang++ -std=c++20 accepts):

- A CONST place arm: `t = c ? a + b : a.const_ref()` (cppas.Life) is still refused. clang's
  composite is a const prvalue, so `t =` COPY-assigns (c0/c1: ca1 ma0). Accepting it through the
  same copy-into-temp path move-assigned (ma1), so the gate `!lvalueJoinConst()` in
  MainListener_Conditional.cpp (the "imported class prvalue arm ... beside an unmoved place" branch)
  keeps it refused. A fix needs the const composite to reach assignment (copy-assign from the temp).
  Pinned by Test/errors/err_cpp_ternary_mixed_ownership.cb cppConstRefBorrow.

Repro (Obj with counting specials, ctor|copy|move|mvasg|cpasg|dtor):

```cflat
import cpp "library/cpp_interop_assign.h";
void f(bool c) {
    cppas.Life a = cppas.Life(3); cppas.Life b = cppas.Life(4); cppas.Life t = cppas.Life(0);
    t = c ? a + b : a.const_ref();   // refused; clang: ctor 1 or copy 1, copy-assign 1, dtors 4
}
```

Not this issue (measured with no `?:`): a brace member `Holder h{ mk(5) }` makes one extra move +
dtor vs clang (aggregate and constructor holders alike), so `Holder h{ c ? default : x }` carries
the same +1 move. Brace-element elision is tracked separately.
