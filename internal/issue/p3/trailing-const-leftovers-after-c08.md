# Trailing const leftovers after c08 (const plan phase 8)

Filed 2026-10-19 from the c08 implementer report (landed 8b7c67afa).
1. #8 inside a const method names 'this', not the field: `w.Mutate()` on a field `w` reads "on 'this'".
2. #10 for an implementer of a GENERIC interface points at the interface line, not the class.
3. Internal interface dispatch (for-in over an interface collection: count/get) passes no receiver to
   CallInterfaceMethod, so #8 does not judge it on a const receiver.
4. Calling a free function declared after the struct with `this` from inside a method (`pokeT(this)`) fails with
   "no overload matches", const or not (pre-existing, unrelated to const).
5. An `alias` result into `this` from a const method is not refused (#7); matches phase 3 for `const alias` params.
Design note (not a bug): `T Read() const { return v; }` with T = int* is refused because const is transitive through
pointer fields (phase 2); C++ is shallow. Keep unless the maintainer rules otherwise.
