#pragma once
#include <cstddef>

namespace cpp_t38 {
struct Base { char base; };
struct Derived : Base { double own; };
struct NonStandard { virtual ~NonStandard() = default; int value; };
struct MemberNamed { int offsetof; };
}
