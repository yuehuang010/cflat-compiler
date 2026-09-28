# `(b ?? a)->v` fails with "Undefined variable v"

Pre-existing on master 64c7a9ed (found 2026-09-27 by the fix/coalesce-arm review, probe
scratch/revAK_y/revAK_y.cb (main checkout)). A parenthesized `??` result used
with `->` does not resolve the member; `T* t = b ?? a; t->v` works. Fix direction: the postfix `->` on a
parenthesized coalesce expression must take the struct type from the join's result type.
