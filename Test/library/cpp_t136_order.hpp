#pragma once
// T136 ORDER: one counter shared by the C++ globals and CFlat locals in test_cpp_interop.cb.
// Each construction records the next value, so the values read back show their order.
namespace t136ord {
inline int seq = 0;
inline int mark() { return ++seq; }
struct Stamp { int n; Stamp() : n(mark()) {} };
}
