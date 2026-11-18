# C++ braced-list overload: template beside a non-template still picked wrongly

CVTIE (2026-11-10) fixed the conversion-operator ties, the braced-route ranking review findings, the
global-namespace template callee (`tq({1,2})` was "Undefined variable" for ANY global template, braces
were not the cause) and the `std.` spelling in the candidate list. What is left:

Repro header (global or inside a namespace, `nn.` prefix on the call; `IL(T)` = `std::initializer_list<T>`):

    template <class T> int tp(IL(T)) { return 2; }   int tp(IL(long)) { return 1; }
    template <class T> int ts(T, IL(T)) { return 2; } int ts(int, IL(long)) { return 1; }
    template <class T> int tr(IL(T), int) { return 2; } int tr(IL(long), int) { return 1; }
    template <class T> int tn(IL(T)) { return 2; }   int tn(IL(int)) { return 1; }

Calls and answers (clang++ -std=c++20 vs CFlat):
- `tp({1,2})`: clang 2 (deduced `il<int>` is identity, `il<long>` a conversion), CFlat 1.
- `ts(1,{1,2})`: clang 2, CFlat 1. `tr({1,2},3)`: clang 2, CFlat 1.
- `tn({1,2})`: clang 1 (equal ranks, the non-template wins), CFlat 2.

## Root cause

A template beside non-templates is not ranked against them. The brace scan in
RequestCxxBraceFunction walks `cxxFunctionSignatures_` (declared non-templates) only, while the
template route requests the template alone, so each route answers without seeing the other.

## Fix direction

Give the brace scan a deduced candidate per function template: element identity for an `il<T>` slot
(homogeneous list), the plain argument's identity for a `T` slot, conflicts not viable; then rank it with
`argumentRanks` and prefer the non-template on an equal rank ([over.match.best] 2.5). A list whose
elements disagree deduces nothing and stays with the non-template.

Also: explicit `tq<int>({1,2})` on a global-namespace template still reports "unknown generic function";
the namespace form `nn.tq<int>({1,2})` works.

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
