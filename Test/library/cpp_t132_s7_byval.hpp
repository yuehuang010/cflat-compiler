#pragma once
#include "cpp_t87_null_zero.h"

namespace t132 {
inline int takeS7(S7 value) { return value.v; }
struct SC { int v; SC(char) : v(11) {} SC(int*) : v(22) {} };
}
