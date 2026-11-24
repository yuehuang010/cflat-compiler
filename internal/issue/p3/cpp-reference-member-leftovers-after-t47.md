# C++ reference-member leftovers after REFMEM

T47 items: designated init of a C++ aggregate with reference members, literal/rvalue/C++-class-prvalue binding with clang's
lifetime extension, `{ r = move x }`, and nested aggregates landed in REFMEM (1cc0ed5d2). The const array-reference write
(T104) and const C& result ranking already matched clang on master. All below are pre-existing (master behaves the same);
clang++ -std=c++20 is the oracle. Repros: scratch/repro_keep/refmem/ and scratch/repro_keep/t47/.

1. Positional base/member-aggregate reference members landed REFBASE (ef83ffa02). The array alias residue landed REFRES
   (5344802ed): 1-D and 2-D arrays, fixed-array fields, and globals now preserve aliases in brace-elided and fully braced forms.
   A C++ class prvalue element (`T(3)`) into an aggregate member inside an elided range is built in place since
   BRACEPRV (aggregates without reference members). Remaining pre-existing residue (master behaves the same): a
   converting element into a class-type member inside an elided range runs the ctor after later elements
   (12345769, clang 12374569; not reproduced in the REFMEM2 shapes, original repro lost); the same prvalue element into an aggregate WITH a reference member still makes the
   extra copy + destroy (BRACEPRV keeps the temp path there, a placement-new wrapper would end a bound temporary early).
2. Function-reference members landed REFFN (ef83ffa02): a named function or an existing C++ function reference binds, a
   function-pointer value is refused. RULED 2026-10-29 (main session, from precedent; maintainer may veto): `{ &f, 5 }` and `{ (function<int(int)>)f, 5 }` stay ACCEPTED - working
   as intended (T478: `&f` on a named function is the function value; no runtime risk; clang's pointer-prvalue refusal is a
   spelling rule). No err leg. Pre-existing (master refuses too, clang
   accepts): a function template specialization `q.tf<int>` and a ternary `c ? f : g` element. Repros
   scratch/repro_keep/reffn/.
3. Refused contexts landed REFCTX (483f3b83d): global, `return { r = x }`, call argument, CFlat struct field, array
   element names. Residue (conservative refusals, clang accepts; repros scratch/repro_keep/refctx/): return binding to
   `*p` or `hp.g` through a raw pointer parameter (pointee lifetime untracked), a global binding `{ p = *gptr }`, a
   global whose referent is a non-scalar temporary (clang 73), a field default binding another field of the frame
   (clang 9). Return-gate escapes (named local, nested brace, copies, ternary joins, branch/loop writes, call results)
   landed REFESC. Remaining unsound spots, accepted and dangling on master too (repros in the REFESC reviews):
   - a pointer argument loaded from a frame local (`int* p = &x; auto a = rvh.mkp(p); return a;`) is not traced;
   - a CFlat callee is trusted: `mkc(int* p) { rvh.R a = rvh.mkp(p); return a; }` called as `return mkc(&x);`;
   - a write through a lambda capture is not tracked (a Frame write inside the lambda is missed; the mirror Stable
     write is a conservative refusal).
   A lambda returning the aggregate lacks return-type inference (r13).
4. Full-expression temp destruction order landed DTORORD (483f3b83d). Pre-existing residue found by its reviews (master
   behaves the same; repros scratch/repro_keep/dtorord/):
   - Alias of a temporary landed ALIASTMP (483f3b83d): the temp is extended to scope end; an alias of a reference that may
     come from a temporary is refused. RULED 2026-10-29 (main session, from precedent; maintainer may veto): STAYS REFUSED (same policy as the 2026-09-24 &ref() gate; escape =
     name the temporary first, `C c(2); alias r = pickref(c, 0);`). Optional refinement: honour clang `[[clang::lifetimebound]]`
     when the callee carries it; never inspect inline bodies. `alias r = pickref(C(2), 0)` (clang 7) stays refused. Pre-existing: `alias r = *C(6).ptr();` (raw-pointer provenance) dangles.
     Repros scratch/repro_keep/aliastmp/.
   - Brace form residue (DIRORD r2): `T t{s, C(2)}` with `s` a C++ class lvalue still differs from clang. The aggregate
     brace route (TryEmitForeignCxxAggregateBraceInitFields -> RequestCxxDirectBraceConstructor ->
     CreateOverloadedFunctionCall) copies `s` after `C(2)` is built, and the wrapper takes class prvalues by move.
     Probe (fixture dirord, Test/library/cpp_interop_lastuse.h): `T t{s, C(2)}` prints `c1 c2 k1 m2 T d2 d1 d2 ~T d1`,
     clang `c1 k1 c2 T d2 d1 ~T d1`. An all-prvalue brace `T t{C(1), C(2)}` also differs (`c1 c2 m1 m2 T d2 d1 d2 d1 ~T`,
     clang `c1 c2 T d2 d1 ~T`), so the fix needs prvalue elision into the brace wrapper's by-value parameters as well as
     the copy-at-position. The paren direct-constructor cells (local, field, param, global, new; with and without
     throw) now match clang (DIRORD r2, Test/test_cpp_interop.cb DIRORD legs 57001-57014).
   - The reviewed `tone<T>(T)` free-function template path now uses the direct-construction guard. The guard stays
     off member-template wrappers: broadening it changed `PvHost::byVal(PvCount<T,F>)` from the existing no-move
     prvalue route to an extra move (`Test/test_cpp_interop_template.cb`, leg 2856). That member path matches both
     master and clang when kept on its prior route.
   - By-value param whose ctor takes a MUTABLE reference (`struct R { int v; R(int& x) { x += 100; ... } }`) is
     still constructed eagerly, so it dies inside the call (BVPARAM2 kept the eager branch on purpose,
     directConstructionHasMutableRefParameter, MainListener_PostfixExpression.cpp ~14124): `one(R(x)) + cref2(C(2), C(3))`
     with R printing 300 + v on ctor / 330 on dtor and C c<v>/d<v> as above gives `300 52 330 2 3 60 33 32`, clang
     `300 52 2 3 60 33 32 330`. Values and reference identity are right (x 7 -> 107).
   - A conditional mixing a prvalue and an lvalue arm (`c ? D(1) : y`) is refused; clang accepts.

