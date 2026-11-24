#pragma once
// GRPLEAK: grouped with cpp_grpleak_brace_overload.hpp, so its brace request runs in the same TU first.
namespace grpleak_bra { struct In { int& r; int v; }; inline int take(In x) { return x.v + x.r; } }
