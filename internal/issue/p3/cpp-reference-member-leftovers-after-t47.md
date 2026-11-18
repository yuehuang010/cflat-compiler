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
   - A by-value copy of an lvalue is never destroyed when a later constructor in the same call throws (thrlv: 3210,
     clang 13210). BVPARAM did not resolve the direct C++ constructor-argument path after three targeted attempts:
     matrix row 10 (a / constructor / direct / throw) still copies the lvalue after the later `D` constructor; the
     worktree records `1/0/0/1`, while clang records `2/1/0/2` (`corder=12`, `dorder=21`). Repro shape:
     `T t = T(s, C(2))` with `s` a C++ class lvalue and `C(2)`'s constructor throwing; the fix prints `c1 d1`, clang
     `c1 k1 d1 d1` (the copy of `s` is ordered after the throwing constructor and never made).
   - The same direct constructor path also has a no-throw order mismatch: `T t = T(s, C(2))` (BVPARAM p.cb f13)
     records `c1 c2 k1 d1 d2 d1`; clang++ -std=c++20 records `c1 k1 c2 d2 d1 d1`. This remained after three
     targeted BVPARAM attempts and is documented residue for this round.
   - The reviewed `tone<T>(T)` free-function template path now uses the direct-construction guard. The guard stays
     off member-template wrappers: broadening it changed `PvHost::byVal(PvCount<T,F>)` from the existing no-move
     prvalue route to an extra move (`Test/test_cpp_interop_template.cb`, leg 2856). That member path matches both
     master and clang when kept on its prior route.
   - By-value param through the C++ wrapper is still destroyed inside the call when a constructor argument has side
     effects or is not a plain scalar (master the same, values right, order differs): `C(g())`, `C(i++)` / `--`,
     `C(GA[i])`, `C(*p)`, `C(a + b)` with an overloaded C++ `operator+`, and member-template callees. Repro:
     `one(C(g())) + cref2(C(G2), C(3))` (one takes C by value, cref2 two const C&, C prints c<v>/d<v>) prints
     `c2 d2 c2 c3 d3 d2`; clang++ -std=c++20 prints `c2 c2 c3 d3 d2 d2`. BVPARAM's deferral gate admits only
     side-effect-free scalar arguments (IsCxxSideEffectFreeScalarExpression, MainListener_Conditional.cpp).
   - A conditional mixing a prvalue and an lvalue arm (`c ? D(1) : y`) is refused; clang accepts.

