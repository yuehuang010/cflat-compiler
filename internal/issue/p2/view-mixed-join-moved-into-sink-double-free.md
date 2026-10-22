# p2: owning view from a mixed `?:` join moved into a sink double-frees when the borrowed arm was taken

Pre-existing on master; found by the T362 review (2026-10-20). Case f00 in scratch/repro_keep/box1021/t362_rev/p.cb.
`S[] w = new S[3]; S[] v = c ? new S[2] : w; sink(move v);` (sink takes `move S[]`) with the BORROWED arm taken -> rc 133, ASan
double free (sink frees w's block, then w frees it again). The non-join form `S[] v = w; sink(move v);` is refused at compile time
("move of a borrowed view"), the mixed join is not.
Fix direction: after T362 the view carries a runtime owns flag; `move v` of a view whose ownership is runtime-decided must either be
refused at compile time (consistent with the plain borrowed form) or transfer only when the flag is set (sink receives ownership
conditionally - needs a design note: a `move S[]` sink assumes ownership unconditionally). Refusal is the conservative choice.
