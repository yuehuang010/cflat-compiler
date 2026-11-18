# C++ brace-overload ambiguity leftovers after T314

The function-template ranking issue and the global explicit-template lookup item were fixed in the
BRACETPL round. The following notes remain out of scope.

Cosmetic: the ambiguity list names a member unqualified (`'m'`); Test/errors pins that spelling.

## Ruled 2026-11-10: function name refused on the brace route in declaration order B

A C++ function name against a function-pointer parameter whose type really differs (`const int*`
vs `int*`, noexcept, nested return) comes back Unranked, so the braced call is refused. Master picked
the rival there only because a tie goes to the first-declared candidate; the same program in
declaration order A is refused by master too. Ruled acceptable by the maintainer (16 reviewer cells,
CVTIE review 5); a clean refusal, never a wrong pick. Closing it needs a real type compare of the
two signatures instead of spellings.

    inline void g(const int*) { }
    inline int f(bool, IL(long)) { return 1; } inline int f(void (*)(int*), IL(long)) { return 2; }
    // f(g, {1, 2}): clang 1, CFlat "ambiguous call to 'f'"
