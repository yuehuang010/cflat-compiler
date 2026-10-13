# C++ conversion-operator receiver and class-result leftovers

Status: active. The scalar conversion ranking fix in B15 resolves declaration initialization for `long` (exact beats bool-to-long), `int` (bool promotion wins), and preserves bool-context selection. Local, global-source, const-reference-result, temporary, and ternary-source probes were compared against clang++ at -O0/-O2.

Class-result item resolved in T105. A non-explicit `operator W()` now copy-initializes `W` locals
and assignments through clang's copy-initialization wrapper. T105 checks value 37, one conversion,
one constructor, no copies, one assignment, and exact destructor totals for both forms.

Still open:

- `const Z&` selecting a non-const `operator bool` remains deferred to B20 const-receiver work.
- `template<class U> operator U()` is refused for all recorded destinations and positions.
- With `explicit operator bool()` and `operator int()`, explicit casts to `long`, `i64`, `char`, `double`, and `float` are refused although clang selects `operator int()`.

See `scratch/b15_matrix.md` and the probe corpus in `scratch/b15_matrix/` for measured results. The full class-result matrix remains incomplete.

T105 review 1 (pre-existing, reproduce on master and af5afa448):
- Inherited `operator W()` (struct PD : PB, PB declares it): `W w = pd` is refused; clang gives value 37, one operator call, one ctor, no copy. `HasImplicitClassConversionOperator` (LLVMBackend_CInterop.cpp ~28360) scans only the class's own members. Repro scratch/repro_keep/t105/c_w_inh.cb.
- Explicit `(W)p` makes one extra copy + dtor (37 1 1 1 0 1 vs clang 37 1 1 0 0 0); earlier "explicit-cast support is not a remaining fix" checked value and op count only. Repro scratch/repro_keep/t105/c_w_cast.cb.
