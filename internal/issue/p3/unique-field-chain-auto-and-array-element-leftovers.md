# p3: `auto` from a unique-interface field assignment: ternary and generic-body leftovers after T476

T476 (2026-10-24) refuses `auto o = w.i = new BI();` (UI has `unique IB i`, BI implements IB) like the explicit
`unique IB o = w.i = new BI();`, also through `->`, array elements, nested fields and implicit this. Legs:
Test/errors/err_auto_unique_field_chain.cb, Test/test_move.cb (T476 block).

Still open (PRE-EXISTING, master same; T476 Opus review 1):
1. Ternary initializer: `auto o = c ? (w.i = new BI()) : (w.i = new BI());` compiles and crashes (139, `o` null); the
   explicit `unique IB` and the struct-field ternary are refused. Fix: in MainListener_Declarations.cpp ~9305, also look
   through SoleTernaryExpression and refuse when either arm is a unique interface field assignment. Keep
   `auto o = c ? w.i : w2.i` accepted (2 dtors, no leak). Repro scratch/repro_keep/box1022/t476/rev_t476_opus_ternary_auto.cb.
2. Inside a generic function body the guard (and the explicit-type check) is skipped (`!InGenericInstantiation()`):
   `auto o = w.i = new BI()` compiles and crashes (139); the explicit spelling does the same.
