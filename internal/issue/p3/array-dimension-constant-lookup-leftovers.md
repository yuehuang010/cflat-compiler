# Array dimension constant lookup leftovers (after T193)

Pre-existing (T193 Opus r1, 2026-10-14; same on master). Probes: cflat-fix-t193 scratch/rev_t193_opus_probe.sh (copied to scratch/repro_keep/t193_rv1/).
- A function name used as an array dimension (`int K(){...} int f(){ int[K] a; }`) fails with "module verification failed"
  ("Alloca array size must have integer type"); clang refuses. Fix: LogError in the array-dimension emission path when the
  dimension value is not an integer constant (or not integer-typed).
- A local `const int K = 5;` shadowing a file-scope `const int K = 1` sizes `int[K]` as 1 (clang 5), with or without a namespace.
- A non-const `int K = 7;` in namespace N next to a file-scope `const int K = 1` sizes `int[K]` in N as 1 (clang: not a constant
  expression). The T193 folder returns nullopt there, but the emission path still picks the file-scope const.
