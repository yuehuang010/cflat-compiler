# Nested tuple element type is not resolved: `((int,int),int)` fails "cannot find the type '(int,int)'"

Summary: a tuple whose element is itself a tuple fails to resolve, as a local and as a parameter, on master
d3681293f and after const phase 0 (found by the c00 round-3 review, 2026-10-17). Single-level tuples work.

Repro (scratch/repro_keep/const_c00/tuple_plain_nested.cb):
`int f(((int,int),int)* p){return (int)sizeof(*p)-12;} extern int main(){((int,int),int) p=default;return f(&p);}`
-> error "cannot find the type '(int,int)'". Expected: compiles, exit 0 (clang analogue: a struct of a struct).

Fix direction: tuple element resolution (ParseTupleTypeSpecifier and the scanner's tuple-shell registration,
ForwardRefScanner.cpp tuple callback) looks the element up by its spelled text; recurse into a nested
tupleTypeSpecifier instead. Same shared resolver (ResolveCanonicalTypeSpecifier, MainListener.h) as const
wrappers, so `const(((int,int),int))*` should be covered by the same leg.
