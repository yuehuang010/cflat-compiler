# Trailing const leftovers after c08 (const plan phase 8)

Filed 2026-10-19 from the c08 implementer report (landed 8b7c67afa). Items 1-3 (#8 field naming,
#10 location for generic interfaces, receiver on internal interface dispatch) fixed in c11.
1. Calling a free function declared after the struct with `this` from inside a method (`pokeT(this)`) fails with
   "no overload matches", const or not (pre-existing, unrelated to const).
2. An `alias` result into `this` from a const method is not refused (#7); matches phase 3 for `const alias` params.
Design note (not a bug): `T Read() const { return v; }` with T = int* is refused because const is transitive through
pointer fields (phase 2); C++ is shallow. Keep unless the maintainer rules otherwise.
