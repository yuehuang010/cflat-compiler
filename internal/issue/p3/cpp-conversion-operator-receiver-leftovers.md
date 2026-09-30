# C++ conversion-operator receiver and class-result leftovers

Status: active. The scalar conversion ranking fix in B15 resolves declaration initialization for `long` (exact beats bool-to-long), `int` (bool promotion wins), and preserves bool-context selection. Local, global-source, const-reference-result, temporary, and ternary-source probes were compared against clang++ at -O0/-O2.

Still open:

- C++ class-returning `operator W()` fails for declaration initialization (`W w = p`) and assignment (`w = p`) with an aggregate-cast refusal. A minimal explicit cast `(W)p` probe succeeds before and after B15 (value 37, conversion hit 1), so explicit-cast support is not a remaining fix for that shape. By-value result / sret and ctor/dtor counts remain unvalidated.
- `const Z&` selecting a non-const `operator bool` remains deferred to B20 const-receiver work.
- `template<class U> operator U()` is refused for all recorded destinations and positions.
- With `explicit operator bool()` and `operator int()`, explicit casts to `long`, `i64`, `char`, `double`, and `float` are refused although clang selects `operator int()`.

See `scratch/b15_matrix.md` and the probe corpus in `scratch/b15_matrix/` for measured results. The full class-result matrix remains incomplete.
