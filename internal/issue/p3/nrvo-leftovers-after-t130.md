# NRVO leftovers after T130 (named return of a struct with C++ fields)

PRE-EXISTING (T130 review 1, 2026-10-12). T130 builds a returned function-scope local in the sret
slot when every return names it; these shapes still move where clang++ -std=c++20 elides:

1. A local declared in a nested block (or under a shadowed name) and returned: still moved
   (clang elides).
2. `construct_at(slot, mk1())`: 1 move + 1 dtor of a caller-side temporary (master 2/2, clang 0/0);
   the call result should be built directly in the slot.
3. `new S(mk1())` with S a CFlat struct holding a C++ field: "no overload of 'S'" on master and after
   T130 (clang accepts and elides).
