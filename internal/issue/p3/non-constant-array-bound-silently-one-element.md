# A fixed-array bound that is not a compile-time constant is silently one element

READY TO BUILD (type C, adds a rejection, full mode). Ruled, no question open, no dependency on another run.

## Summary

`int[n] a;` where `n` is not a compile-time constant compiles. The array gets ONE element: `sizeof(a)` is
`sizeof(T)`, and `a[2] = 3` writes past the slot with no diagnostic. Measured 2026-11-18 (Release, macOS arm64,
master 289499697) for a local bound (`int n = argc + 2; int[n] a;`) and a mutable C++ static bound
(`int[rs.Mut.n] a;`, `static int n;` or `static const int rt = getn();`).

RULED 2026-11-18 (maintainer): COMPILER ERROR until the runtime-sized array feature
(`internal/issue/p4/runtime-sized-arrays-vla.md`, C VLA, optional) is built. A fixed-array declaration whose
bound is not a compile-time constant is refused with a located error naming the bound. Applies to every
non-constant bound (local variable, parameter, mutable C++ static, call result). Constant bounds are unchanged,
including the constexpr / const C++ statics STATMEM (289499697) folds. Rejected for now: accepting it as a
runtime-sized array (that is the p4 feature; it later lifts this refusal for block-scope locals and flips these
`err_` legs to value legs). Not asked, do not invent: any change to a zero / negative constant bound, to
array-view (`T[]`) declarations, or to `new T[n]`.

## Repro

Native (`cflat a.cb -o a.out && ./a.out x y`), master prints `3 4`, exit 0:
```cflat
int main(int argc, char** argv) { int n = argc; int[n] a; for (int i = 0; i < 3; i++) { a[i] = i + 1; } printf("%d %d\n", a[2], (int)sizeof(a)); return 0; }
```
C++ static (header `sm.hpp`, then `cflat b.cb -i . -o b.out`), master prints `3 4`:
```cpp
namespace rs { struct Mut { static int n; }; inline int Mut::n = 3; }
```
```cflat
import cpp "sm.hpp";
int main() { int[rs.Mut.n] a; for (int i = 0; i < 3; i++) { a[i] = i + 1; } printf("%d %d\n", a[2], (int)sizeof(a)); return 0; }
```
After the fix both are refused at the declaration.

## Where to look (pointers, not a verified root cause)

- `cflat/MainListener_Initializers.cpp`: `EvalGlobalArrayDim` already refuses a global ("global array size must be
  a compile-time constant"). The local path in the same file folds with `TryFoldConstInt` and returns nullptr when
  the bound does not fold (`if (!ok) return nullptr;`); find what each caller does with that nullptr - the
  one-element array comes from there.
- Existing refusals at the local dimension site: `Test/errors/err_array_dim_float_variable.cb` ("array dimension
  'f' must have an integer type"), `err_array_dim_function_name.cb`. The new error belongs at the same site.
- Both passes: the ForwardRefScanner and MainListener copies of `ParseDeclarationSpecifiers` / the array
  dimension code must agree.

## Fix direction

One located `LogErrorContext` at the dimension expression, e.g. "array size 'n' must be a compile-time constant
(runtime-sized arrays are not supported yet)". Keep the existing global text as is. Audit every caller of the
local fold (local declaration, struct field, parameter array, array inside a generic, `auto` forms, multi-
dimensional `int[n][4] a` and `int[4][n] a`) and report per site what it does today.

## Accept-set (build and freeze BEFORE writing the guard)

Every bound that folds today must keep compiling with the same `sizeof`: integer literal, `const int` local and
global, enum value, `sizeof(...)`, arithmetic / ternary / `&&` of those, a generic value parameter
(`struct Buf<int N> { int[N] d; }`), an `if const` selected constant, a C macro constant from an imported
header, a C++ `static constexpr` / `static const` member and a C++ `constexpr` global (STATMEM legs in
`Test/test_cpp_interop_template.cb`), a C++ enumerator. Corpus sweep: the fix binary over every `.cb` in
`Test/` and `example/` plus `./test_libs.sh -j 2` - zero new refusals.

## Acceptance

- New `Test/errors/err_array_bound_not_constant.cb` (a new rejection may add an `err_` file): legs for a local
  variable bound, a parameter bound, a call-result bound, each dimension of a 2-D array, a struct field bound.
  The mutable C++ static bound goes in the closest existing C++ interop error test.
- No pre-existing test leg changes. `bash test.sh Release`, `bash test_example.sh`, `./test_libs.sh -j 2` green.
- This file is deleted in the fix commit; the p4 file keeps the feature.
