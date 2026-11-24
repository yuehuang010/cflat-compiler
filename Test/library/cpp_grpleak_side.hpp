#pragma once
// GRPLEAK: a dependency global whose initializer also stores another global.
int grpleak_side_count = 0;
struct GrpleakSideP { int a; int b; };
inline GrpleakSideP grpleak_side_mk() { return { 4, 5 }; }
auto [grpleak_side_p, grpleak_side_q] = grpleak_side_mk();
int grpleak_side_a = ++grpleak_side_count, grpleak_side_b = grpleak_side_count * 7;
namespace grpleak_side {
struct X { int v; static int g(); };
int X::g() { return grpleak_side_count * 1000 + grpleak_side_p * 100 + grpleak_side_a * 10 + grpleak_side_b; }
}
