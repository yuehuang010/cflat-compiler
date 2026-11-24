// A user header in a different directory than Test/library, listed after a system header.
#pragma once
namespace grpsub {
struct Sub { int v; };
inline int operator+(const Sub& s, int x) { return s.v * 10 + x; }
inline int subFree() { return 77; }
}
