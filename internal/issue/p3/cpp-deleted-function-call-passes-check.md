# Call to a C++ `= delete` free function passes --check

Pre-existing on master; found by the T59 review 2 (2026-10-06). `namespace rv::a { int del(int) = delete; }`
then `rv.a.del(1)` passes `cflat --check`; clang refuses ("call to deleted function 'del'"). Not tried
with -o (may fail later at wrapper generation). Repro: scratch/repro_keep/t59_rev/.
Fix direction: a deleted overload chosen by resolution is a use-site error with clang's text ("clang: "
prefix), in --check too.
