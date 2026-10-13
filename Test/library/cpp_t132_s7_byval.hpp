#pragma once
#include "cpp_t87_null_zero.h"

namespace t132 {
inline int takeS7(S7 value) { return value.v; }
struct SC { int v; SC(char) : v(11) {} SC(int*) : v(22) {} };
}
namespace t140 {
struct SCL { int v; SCL(char) : v(11) {} SCL(long) : v(33) {} };
// Mac-visible mirror of SCL: a long argument ties long long (both 64 bits on LP64, as int ties long on Windows).
struct SLL { int v; SLL(char) : v(11) {} SLL(long long) : v(33) {} };
struct EX { int v; explicit EX(long) : v(11) {} EX(void*) : v(22) {} };
struct Recv {
    int takeSC(t132::SC value) { return value.v; }
    int takeSCL(SCL value) { return value.v; }
    int takeEX(EX value) { return value.v; }
};
inline int takeK(t132::K value) { return value.was_null; }
}
