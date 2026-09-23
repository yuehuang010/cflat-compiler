Bucket: p3 (codegen; found 2026-09-23 by the raw-owning-pointer review, reproduces on master before that change)

# Chained pointer assignment fails module verification ("Invalid bitcast")

`q = p = new PS();` and `g = p = new PS();` (local or global left-hand side, PS a struct with a
destructor) abort with `module verification failed: Invalid bitcast`. Single assignments work.
Chained assignment of scalars is fine. Probe shapes: scratch/rv/*.cb from the review of
fix/raw-owning-pointer-runtime-flag (chained cells), pre and post binaries alike.

Root cause: not yet located. The value produced by the inner assignment is presumably the stored
pointer wrapped in the wrong LLVM type (owning-flag / release plumbing or the struct-pointer store
result) before it is stored again.

Fix direction: make the assignment expression yield the stored value with the declared pointer
type; add a leg to Test/test_move.cb (chained assignment between owning pointer locals must
transfer ownership once and destroy once) and an ASan run.
