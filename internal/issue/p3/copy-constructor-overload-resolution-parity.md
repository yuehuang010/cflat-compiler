# CFlat return into same-type initializer adds a copy

Summary: A CFlat-defined function returning a C++ class by value is copied into a same-type
initializer, where clang++ elides that copy. A named local returned from a CFlat function
(`CV x(7); return x;`) is likewise copied where clang++ elides it (NRVO).

Repro: `CV z = f(CV(7));`, with `CV` imported from a C++ header and CFlat defining
`CV f(CV a) { return CV(a.v); }`.

Measured on this worktree: value 7, k=1, b=3 (k = copy-ctor runs, b = ctor runs). clang++ -std=c++20:
value 7, k=0, b=2. The copy is a non-const lvalue copy (T(T&) pick) on the returned temporary.
Named-local NRVO repro: `CV local_ret() { CV x(7); return x; }`, `CV z = local_ret();` gives b=2 on
CFlat, b=1 on clang++ (k=0 on clang++).

Root cause direction: the initializer's C++ return-slot elision path is not armed for this
nested CFlat call expression, so the call result is materialized in a return temporary and then
copy-constructed into the declared object. Fix direction: route the CFlat call's return storage
directly to the initializer destination without consuming an outer slot for an inner call; a
named local returned by value should be built in the caller's sret slot (NRVO).

Fixed (copypar2): a returned parameter or `return move x;` with no move constructor copies through
the const copy constructor first (clang's CWG1579 rvalue-first pick), not T(T&).
Open: a class with no move constructor and a forwarding template constructor. clang's rvalue pick is the
template, CFlat takes the const copy (`return a;` v=10 cc=1, clang v=13 tc=1; master mc=2, also wrong); same
for a prvalue argument and `return move h->m;`. Route through the clang pick when classInfo->hasCtorTemplate.
