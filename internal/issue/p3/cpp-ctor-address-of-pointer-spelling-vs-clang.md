# C++ constructor: CFlat `&p` at a pointer-reference parameter vs clang's `T**` reading

**RULED 2026-10-24 (main session, C meaning of `&p` wins; maintainer may veto):** when any viable candidate accepts `&p` read as a `T**` VALUE (exact `T**`, a deducing template, `void*`, `bool`), the clang pick wins - CFlat follows C++ overload resolution there. The CFlat slot-binding reading (`&p` at a `T*&` parameter) is a FALLBACK applied only when no candidate accepts the value reading (T131 behaviour, now the general rule for ctor, postfix-ctor and free-function paths alike). Fix the misleading "arg int** = param int**" detail. Divergences 1-3 above all resolve to the clang pick.

PRE-EXISTING / design (T131 review 1, 2026-10-12). CFlat spells "bind p by reference" as `&p`
at a reference parameter; C++ reads `&p` as a `T**` prvalue. T131 binds the slot when a constructor
takes `T*&` and no `T**` / constructor-template candidate exists. Remaining divergences:

1. `O5(int*&)` + `template<class U> O5(U&&)`, `rv.O5(&p)`: refused with "no overload of 'rv.O5'"
   (mismatch detail shows arg int** = param int**, a misleading message); clang picks the template
   (U = int**). Repro scratch/repro_keep/t131/rev_t131_o5a.cb + .hpp.
2. `O6(int*&)` + `O6(void*)` and `O7(int*&)` + `O7(bool)` with `&p`: CFlat binds the slot (1),
   clang picks `void*` (3) / `bool` (4). Same on master.
3. `auto x = T(&slot)` and `T(&slot).get()` (postfix constructor path) and a free function
   `f(&tp)` taking `int*&` are refused (master same).

Needs a maintainer ruling on which reading wins when both a slot-binding and a `T**`-accepting
candidate exist (CFlat spelling vs clang pick).
