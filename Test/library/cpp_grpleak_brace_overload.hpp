#pragma once
// GRPLEAK: a later brace argument whose int literal targets a non-record parameter type.
namespace grpleak_brb {
struct A { const int& r; };
struct B { int& r; int v; };
inline int ov(A value) { return 1; }
inline int ov(const B& value) { return 2; }
}
