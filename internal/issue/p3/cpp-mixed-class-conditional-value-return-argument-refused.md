# Mixed-class C++ conditional residue after MIXCOND3

MIXCOND (af5834ad3) landed decl / sret / single-argument sink; MIXCOND2 (483f3b83d) the last argument of a multi-argument
call, nested decl arm, F1 producer lifetime; MIXCOND3 (8844053c8, 2026-10-30) the converted B in caller-owned storage on the
full-expression ledger, which routes operator / unary / cast operands, overloaded callees, not-last and named arguments
through one path, hoists nested arm temps in place (either arm, unwind), and keeps callee-destroyed by-value sources and
sinks off the caller ledger on the MS ABI (both ABIs gated). clang++ -std=c++20 / clang-cl oracle.

## Still refused (clang accepts)
- S3, return of a converting by-value parameter lvalue arm `return k ? a : mkNB();` - root cause is the sret copy path.
- lv_bv, an lvalue arm converted through a by-value converting ctor.
- Nested mixed conditional inside a mixed conditional arm (pre-existing).

## Order / lifetime (values match clang, no leak)
- CFlat struct temps with a user dtor are not interleaved with C++ temps in reverse construction order:
  `take(mkA(useS(mkS(3))))` gives `ac[syx` vs clang `ac[yxs` (plain calls too; repro: the
  shape in this line with a CFlat `struct S { ~S(){...} }` temp and a C++ temp in one call). Direction: one shared ledger order for CFlat and C++ temps.
- A temp in the right operand of `&&` dies at the `&&`, not at the end of the full expression (`1a[xac[yx` vs clang `1a[ac[yxx`).
- Lambda arm: one extra move vs clang (pre-existing).
- Residual risk (no probe reached it): hoistArmStructTemps advances the flush mark past a temp HoistOwnedStructTempTo fails to
  hoist (non-entry alloca / dead insert block) - such a temp would leak.

MIXS2 (2026-10-30 box 7) re-admitted S2 (defaulted trailing params; IsDefaultParamOverload removed, cache 308). Opus review, PRE-EXISTING:
- `--platform win64` cross-compile on macOS refuses EVERY mixed A/B conditional call argument ("no common C++ type"), defaults or not
  (repro scratch/repro_keep/mixs2/pw2.cb + rv.hpp/rvw.hpp). Host-native Windows builds accept them (gate green), so the cross target loses
  the converting-ctor lookup.
- Class-typed defaults (CFlat default refused, C++ omitted default extra moves) are in flight as DEFCLS (pd.cb, pd2.cb, pc.cb).
