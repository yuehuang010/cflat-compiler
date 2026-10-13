# Returning a named local struct holding a C++ class field moves it (no NRVO)

Summary: `S1 mk() { S1 s = { c = 5 }; return s; }` where S1 holds a C++ class field C (logging
ctor/move/dtor, converting `C(int)`) moves the field into the return slot: value 1005 (moved-from
marker), 1 move, on master and T125 alike; clang++ -std=c++20 elides (NRVO): value 5, no move. The
array form `struct H { C[2] arr; }; H mkH() { H h = { arr = { 5, 6 } }; return h; }` gives 4 ctors /
4 dtors since T125 (master 6/6), clang 2/2. `construct_at(p, mkH())` inherits it.

Found: T125 review 1 (2026-10-11), pre-existing.

Fix direction: when every return in a function returns the same named local of the return type,
allocate that local in the sret slot (C++ NRVO); the cflat-struct C++ field lowering ruling (rule A,
lower like C++, sret, in place) already points here.
