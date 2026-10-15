# Explicit type arguments on a non-generic function are accepted and crash

Found by T195 (2026-10-14), pre-existing on master. A name followed by a type-argument list that names no generic
function (`wrap<int>` where `wrap` is an ordinary `int wrap(int a)`) compiles silently in value position and the
program crashes at run time (exit 133). Same for an ordinary call-free value bound to a slot, a ternary arm, and a
multiplicative operand.

Repro (compiles on master; expected: refused with "'wrap' is not a generic function" or similar):

    int wrap(int a) { return a + 2; }
    extern int main() { function<int(int)> g = wrap<int>; return g(1); }      // exit 133 on master

Root cause (not confirmed beyond the publish site): MainListener_PostfixExpression.cpp, the generic-instantiation
publish block (`if (isGenericFunc)` ~line 3520) is entered only for a generic function template. For a non-template
name with a type-argument list the block falls through without a CallerName or a diagnostic, so the value reaches the
slot as an unresolved function value.

Fix direction: at that publish site, when the name is neither a generic function template nor a generic type/struct
template (the `AnyGenericTypeTemplateNamed` / `genericClassTemplates` / `genericStructTemplates` checks), refuse with
"'<name>' is not a generic function". Check every consumer of genericIdentifier first (C++ imported templates, generic
types used as constructors and static qualifiers) so the refusal does not reject a legitimate form. Add an
`err_*.cb` leg with the repro.
