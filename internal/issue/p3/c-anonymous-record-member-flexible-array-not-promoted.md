# C anonymous record member: its flexible array field is not reachable from the containing record

Found 2026-09-28 by run A6 round 2 (pre-existing on master 86ccc218). A C record with an anonymous
struct/union member whose tail is a flexible array (`struct AnonymousFlex { int n; struct { int k; int data[]; }; };`)
does not expose `data`: `a->data[0]` reports no matching `operator[]` for 'AnonymousFlex' on master and on
fix/c-flex-array. Repro header: scratch/repro_keep/a6/neighbor.h (main checkout).
Hypothesis: the anonymous child is registered as a synthetic record, but its fields are not promoted into
the containing record's member lookup (check whether plain `int` fields of an anonymous member are
promoted - if they are, only the flexible-array path is missing the promotion).
Fix direction: trace clang anonymous-field flattening; register promoted fields at the containing
record's offsets, keeping IsFlexibleArrayMember.
