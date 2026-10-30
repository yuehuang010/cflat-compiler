#pragma once

namespace cpprefctx {
struct A { const int& r; };
struct B { int& r; int v; };

inline int ov(A value) { return 1; }
inline int ov(const B& value) { return 2; }
}
