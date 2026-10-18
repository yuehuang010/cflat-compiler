# expect_error statement block catching an error inside a lambda body crashes the compiler

Summary: a statement-scope `expect_error("...") { ... }` whose expected error is raised inside a lambda body leaves the
half-built lambda function in the module; RunMoveDataflow then aborts (SIGABRT). Reproduces on master for any error
inside a lambda body (e.g. an undefined variable). Found by the const phase 1 implementer, 2026-10-17; the c01 legs
moved to file-scope expect_error blocks to avoid it.

Repro (scratch/repro_keep/const_c01/p/lam_ee.cb, verified on master e4e7fcf4c):
`extern int main() { expect_error("Undefined variable zz") { Lambda<int()> f = () => { return zz + 1; }; } return 0; }`
-> prints "PASS: expected error received", then the compiler crashes (rc 139; the implementer saw SIGABRT in
RunMoveDataflow on its variant).

Fix direction: when an expect_error scope swallows an error raised while emitting a lambda (or any nested function),
erase the partially emitted llvm::Function (and its pending dataflow state) before the scope closes, as the
function-scope error path already does.
