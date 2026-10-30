# C++ reference-member leftovers after REFMEM

T47 items: designated init of a C++ aggregate with reference members, literal/rvalue/C++-class-prvalue binding with clang's
lifetime extension, `{ r = move x }`, and nested aggregates landed in REFMEM (791f1281e). The const array-reference write
(T104) and const C& result ranking already matched clang on master. All below are pre-existing (master behaves the same);
clang++ -std=c++20 is the oracle. Repros: scratch/repro_keep/refmem/ and scratch/repro_keep/t47/.

1. Wrong code: a reference member in a BASE class, positional init: `struct B { int& r; }; struct D : B { int v; };`
   `D d = { { x }, 5 }` does not make d.r alias x (master and REFMEM exit 1, clang 0).
   Attempt REFBASE (branch fix/refbase, b1f84a38e, parked 2026-10-28; review in its worktree scratch/briefs/refbase_review1.md)
   fixed single/multi/nested bases but regressed an empty base clause `{ { }, x, 5 }` (`{}` parses without initializerList(),
   so it is not seen as a base -> dangling ref) and mapped derived fields as elementIndex - baseCount, wrong when a later base
   is brace-elided. Next: one flattened-field cursor (braced base clause advances by the base's flattened field count, elided
   clause by 1) and detect a braced clause by its `{` token.
2. A function-reference member is refused: "'__c_fn_ptr' has no C++ spelling" (the wrapper parameter spelling for function
   references exists but is unreachable).
3. Refused contexts (clear diagnostics; clang accepts): designated C++ aggregate with reference members as a global, as
   `return { r = x }`, as a call argument `f({ r = x })`, as a CFlat struct field initializer; named fields inside array
   elements (`{ a = { { r = x, v = 1 }, ... } }`); a bare `nullptr` element ("brace-list element type '' has no C++ spelling").
4. Call-argument C++ temporaries are destroyed in construction order; clang destroys them in reverse:
   `int z = pick(C(1), C(2)).v;` -> order 12, clang 21.

Fix direction: item 1 in the positional brace wrapper's base-subobject lowering (reference target metadata for base fields);
item 4 in the full-expression temp ledger flush order.
