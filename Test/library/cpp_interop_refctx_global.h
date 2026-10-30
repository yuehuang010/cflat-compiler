#pragma once

namespace cpprefctxg {
struct P { int a; int b; };
struct CR { const P& p; int k; };

inline P mkp() { return P{3, 4}; }
inline int readcr(CR value) { return value.p.a + value.p.b; }
}
