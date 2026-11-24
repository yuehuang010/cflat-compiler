#pragma once
// GRPLEAK: a dependency global bound to a lifetime-extended temporary.
int grpleak_ref_count = 0;
inline int grpleak_ref_next() { return ++grpleak_ref_count; }
const int& grpleak_ref_value = grpleak_ref_next();
namespace grpleak_ref {
struct X { int v; static int g(); };
int X::g() { return grpleak_ref_count * 10 + grpleak_ref_value; }
}
