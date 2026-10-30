# C++ reference-member leftovers after REFMEM

T47 items: designated init of a C++ aggregate with reference members, literal/rvalue/C++-class-prvalue binding with clang's
lifetime extension, `{ r = move x }`, and nested aggregates landed in REFMEM (791f1281e). The const array-reference write
(T104) and const C& result ranking already matched clang on master. All below are pre-existing (master behaves the same);
clang++ -std=c++20 is the oracle. Repros: scratch/repro_keep/refmem/ and scratch/repro_keep/t47/.

1. Positional base/member-aggregate reference members landed REFBASE (d4999d9da). Pre-existing residue found by its reviews
   (master behaves the same): a C++ class prvalue element (`T(3)`) into an aggregate member inside an elided range makes an
   extra copy + destroy (CFlat 127458969, clang 1274569); a converting element into a class-type member inside an elided range
   runs the ctor after later elements (12345769, clang 12374569); an array of aggregates with reference members loses the alias,
   elided and fully braced (exit 3, clang 0). Repros in the REFBASE review files (scratch/repro_keep/refbase/).
2. Function-reference members landed REFFN (ec5f57a0e): a named function or an existing C++ function reference binds, a
   function-pointer value is refused. NEEDS RULING: `{ &f, 5 }` and `{ (function<int(int)>)f, 5 }` are accepted (the same
   function, no runtime risk); clang refuses both because `&f` is a pointer prvalue. Pre-existing (master refuses too, clang
   accepts): a function template specialization `q.tf<int>` and a ternary `c ? f : g` element. Repros
   scratch/repro_keep/reffn/.
3. Refused contexts landed REFCTX (ce8ff8163): global, `return { r = x }`, call argument, CFlat struct field, array
   element names. Residue (conservative refusals, clang accepts; repros scratch/repro_keep/refctx/): return binding to
   `*p` or `hp.g` through a raw pointer parameter (pointee lifetime untracked), a global binding `{ p = *gptr }`, a
   global whose referent is a non-scalar temporary (clang 73), a field default binding another field of the frame
   (clang 9). Pre-existing: a named local aggregate returned (r12) and a nested explicit brace field (f04) escape the
   return gate; a lambda returning the aggregate lacks return-type inference (r13).
4. Full-expression temp destruction order landed DTORORD (b4466d12f). Pre-existing residue found by its reviews (master
   behaves the same; repros scratch/repro_keep/dtorord/):
   - Alias of a temporary landed ALIASTMP (8ad5ea039): the temp is extended to scope end; an alias of a reference that may
     come from a temporary is refused. NEEDS RULING: the refusal is conservative for opaque C++ functions - `alias r =
     pickref(C(2), 0)` returning a reference to a global is valid in clang (7) but refused; options: honour clang's
     `lifetimebound`, or inspect the inline body. Pre-existing: `alias r = *C(6).ptr();` (raw-pointer provenance) dangles.
     Repros scratch/repro_keep/aliastmp/.
   - A by-value copy of an lvalue is never destroyed when a later constructor in the same call throws (thrlv: 3210,
     clang 13210).
   - A conditional prvalue into a by-value param makes an extra moved copy clang elides (condbv 131, clang 31).
   - A by-value param that goes through the generated C++ wrapper (`bv1(C(1)) + ref2(C(2), C(3))`) is destroyed inside
     the call (132, clang 321).
   - A conditional mixing a prvalue and an lvalue arm (`c ? D(1) : y`) is refused; clang accepts.

