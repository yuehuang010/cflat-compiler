# Ternary arm lvalue leftover after T475: string-literal arms bind a C++ `const char*&` parameter

Found by T325 review 1 (2026-10-16), PRE-EXISTING on master. T475 (2026-10-24) fixed the scalar postfix arm
(`ibump(t ? i++ : j)`, `t ? j : i++`, `t ? i-- : j` now refused like clang; legs Test/errors/err_cpp_ternary_prvalue_arm_lvalue_ref.cb).

Still open: `cbump(t ? "ab" : "cd")` with a C++ `void cbump(const char*& r)` (Test/library/cpp_t67_postinc.hpp) is
accepted; clang refuses (array-to-pointer conversion makes the conditional a pointer prvalue). Master passes the pointer
PHI through `%cxx.refptr.arg`. T475 tried marking a pointer join with a string-literal arm as a prvalue (sourceType
`const char*`, fresh) in ParseTernaryBranches near the storage-join publish; no diagnostic fired after three attempts -
the C++ reference-argument binder (`cxx.refptr.arg` path) does not consult that flag. Fix direction: find where a
pointer rvalue is materialized into `cxx.refptr.arg` for a `T*&` parameter and refuse a non-lvalue (no storage) source
there, the way plain `cbump("ab")` is refused (probe that first).

Also open (T475 review, PRE-EXISTING): `int* p = &(t ? i++ : j);` takes the address of `i` (master and T475); clang refuses
("cannot take the address of an rvalue"). The `&` path does not go through scalarLvalueJoin.
