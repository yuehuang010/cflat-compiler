# Member type paths (`Spec.name`): spellings T37 left out

T37 (5837c4fa, 2026-10-05) made `Spec.name` name C++ member typedefs / nested types and CFlat struct `using`
members in every type position. These spellings still fail (all fail on master before T37 too):

- Member template in the tail: `Outer<int>.Inner<long>.type`.
- Closure-alias members: `struct P { using Cb = function<int(int)>; }` then `P.Cb`.
- CFlat static data members (T37 matrix cell n10).
- C++ static data members as value template arguments: `fixed<vec<int>.kind>` - was "does not fold" on master,
  now the member-type message.

Cells: worktree cflat-fix-t37-membertype scratch/t37_matrix.md "Out of scope" (worktree removed after landing;
the list above is the record).

## Fix direction

Extend the member-type path resolver (shared by both ParseDeclarationSpecifiers copies) to accept template
arguments on a non-final segment, closure aliases, and a static-member VALUE path that constant-folds into a
value template argument.
