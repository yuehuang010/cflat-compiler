# P2: implicit conversion between pointers to different integer types is accepted

Summary: `long* -> int*`, `int* -> u64*`, `u8* -> char*`, `i8* -> char*` (any pair of distinct integer
pointees, any depth `int** -> u64**`) convert silently at call arguments, assignment, declaration init,
return, ternary arms and field default initializers. Reading through the result reinterprets memory
(width) or changes meaning (signedness; char signedness differs by target - signed on x86 and Apple
arm64, unsigned on Linux/Android arm64).

## Ruling (maintainer, 2026-09-30)
Block on safety grounds; an explicit cast is the workaround (verified: `(i8*)&c`, `(char*)&u`,
`(int*)&l` compile and run). char <-> i8/u8: maintainer leaning to block too, advice given = block, after
retyping core text APIs (`string.data()`, filesystem `data()`) from `i8*` to `char*`. Confirm before
building the char-family stage.

## Splash (scratch/ptrint/ptrint_report.md, 2026-09-30, probe patch scratch/ptrint/probe.patch)
287 unique accepted sites in test.sh + examples + test_libs t1-2: 285 char-family (280 `i8* -> char*`,
5 `char* -> i8*`; 108 in core, mostly from `string.data()` returning `i8*`; 52 at C-import calls such as
`sprintf(buf, ...)`), 2 width (`long* -> int*`), 0 signedness-only. C++ imports: 0.

## Stages
1. Non-char pairs: error at every context listed above, LogError with a cast hint
   ("cannot convert 'long*' to 'int*' implicitly; the pointee types differ - use '(int*)p'"). Fix the 2
   suite sites with casts. Array views (`u8[]` etc.) and C++ references keep their own rules.
2. (after confirmation) char family: retype core text APIs to `char*`, then extend the error to
   char <-> i8/u8; fix remaining sites with casts.
Acceptance: error legs in the existing Test/errors/err_pointer_to_integer_store.cb (or the nearest
pointer-conversion err test); suites green; no C++-import call newly refused.

## Stage 1 status (V4, 2026-10-01)
Landed: every context above refused for non-char pairs; `long`/`ulong` equal the target-width iN/uN
(C headers bind C `long` that way) but rank below an exactly spelled overload. Gaps found by the V4
review, still accepted (probes cflat-fix-v4 scratch/rev/r3/, archived to scratch/repro_keep/v4_rev/):
- brace init with a field name: `S s = { p = &x };` (int* field, long x) - r3/bi.cb.
- global initializer: `long g; int* gp = &g;` - c_global_init.cb.
- nit: an `int*` into a C `long*` param names the param `'i64*'`, not `long*` (r2/c_ci1.cb).
- array decay skips the check: `i64[] v; int* p = v;`, `take(v)` with `take(int*)`, `i64[2] b; int* p = b;`
  (`from.IsArrayView` early return in IsImplicitIntegerPointeePointerConversion; fixed arrays decay first) - rv/t4.cb, t5.cb.
- ternary with one view arm: `int* r = t ? intView : &i64Var;` - the arm skip needs the view treated as its element pointer (rv/t3.cb).

## Stage 1 gaps closed (V4b, 2026-10-01)
Array decay (views and fixed arrays as their element pointer), one-view-arm ternary, brace init with a
field name (nested too), and global initializers are refused. Brace lists for arrays of pointers
(local and global, `int*[2] ptrs = { &y, &y };` with i64 y) are refused since V9 19018a0c.
The C-import param spelling nit ('i64*' for C long*) is unchanged.

RULED 2026-10-01 (maintainer): char family staged: (1) retype core text APIs (`string.data()`, filesystem `data()`, ...) from `i8*` to `char*`; (2) then block `i8* <-> char*` like the rest (explicit cast is the workaround). Stage 2 only after stage 1 leaves no core-induced sites.
