# Pointer depth carrier: volatile / restrict levels and `new const T*(...)` remain

RULED 2026-11-04: carry pointer depth like C, up to the 31-level carrier limit.
Steps 1 (spelled native types), 2a (native `new`, generics, aliases) and 2b (C and C++ interop)
have landed. Step 2b: C / C++ depth 3+ is recorded by the type mapper (header cache 336), and a
C or C++ result, global, field, method result or static member spelled with one '*' records
depth 1 like a native `T*` (header cache 351), so `int** q = cfn_returning_int_ptr();` refuses
like clang instead of binding silently.

Remaining (pre-existing, separate root causes; re-measured 2026-11-18 on the C++ header repros, unchanged by step 2b):

- `new const int*(&cx)` fails to parse.
- Overload ranking does not model volatile or restrict at pointer depth >= 2: a pointer naming either keeps the legacy pick, so a valid arm can lose. Repro: `int f(void*) {return 1;} int f(int* volatile*) {return 2;}` called with an `int**` picks 1, clang picks 2. Same family: `volatile int* const*`, `const volatile int* const*`, `int* restrict*`.
- A depth >= 3 parameter naming volatile or restrict accepts a source that does not name it, where clang and pre-carrier CFlat refused. Repro: `int s3(volatile int***p) {return p?4:5;}` called with an `int***` compiles and returns 4; clang: no matching function. Same for `int* volatile**` and `int* __restrict__* __restrict__*`.
- Data-pointer literal-0 accept legs at Test/test_basic.cb around the data-pointer
  tests sit outside the function-slot literal-0 ruling. CONFIRMED 2026-11-18 (maintainer: "land the rest", on the main session's recommendation): a plain
  literal `0` into a DATA pointer (`int* z = 0;`) stays accepted, as C and C++; the legs stay value legs. The
  function-slot refusals (`(void*)0`, `(int*)0`, `(int*)nullptr`, ruled the same day) do not reach data pointers.
