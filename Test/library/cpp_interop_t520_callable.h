#pragma once
#include "cpp_interop_twin.h"

namespace t520 {
template<class F> inline int invoke_variadic(F&& f) { return f(7, 8); }
using TwinAlias = cpptw::Twin;
}
