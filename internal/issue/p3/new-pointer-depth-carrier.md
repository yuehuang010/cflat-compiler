# Deep pointers in C and C++ interop remain unsupported

RULED 2026-11-04: carry pointer depth like C, up to the 31-level carrier limit.
Step 1 carries spelled native types. Step 2a carries native `new` element and result
pointer depth, including generic substitutions and aliases, and removes the old
8-level const-deduction limit.
Step 2a round 2 (2026-11-05) also landed: an explicit depth-2 const type argument
(`run<(const int*)*>`, `run<(const int)**>`) keeps its const; `new (const T)**[n]` and
`new (const T)*(v)` (inline parenthesized const as the new type) parse; a depth-2 array
view keeps its element const through raw, view and `auto` forms; a direct `new T[n]`
argument names its element in the array-view diagnostic.

Step 2b remains: C and C++ interop types deeper than 2, including C header
`int***` parameters and `std.vector<nest.Cell**>.data()`. The existing
err_cpp_diag_display_names and err_cpp_unsupported_param_type tests pin the
current refusal.

Pre-existing separate issues:

- Pointer-depth mismatches at initialization, assignment, and return can be accepted (also a view passed to a param of a different element depth, `int** e = &f2[0]`), and overloads on `int*[]` / `int**[]` / `int***[]` are not told apart - every call picks the first (NEWDEPTH2 review 2, repros scratch/repro_keep/newdepth2/).
- `new const int*(&cx)` fails to parse.
- Data-pointer literal-0 accept legs at Test/test_basic.cb around the data-pointer
  tests sit outside the function-slot literal-0 ruling; confirm the intended behavior.
