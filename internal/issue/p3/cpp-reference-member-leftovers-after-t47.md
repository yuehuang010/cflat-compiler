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
3. Refused contexts (clear diagnostics; clang accepts): designated C++ aggregate with reference members as a global, as
   `return { r = x }`, as a call argument `f({ r = x })`, as a CFlat struct field initializer; named fields inside array
   elements (`{ a = { { r = x, v = 1 }, ... } }`). A bare `nullptr` element in a C++ brace list landed REFFN.
4. Call-argument C++ temporaries are destroyed in construction order; clang destroys them in reverse:
   `int z = pick(C(1), C(2)).v;` -> order 12, clang 21.

Fix direction: item 4 in the full-expression temp ledger flush order.
