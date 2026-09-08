// Order-independence fixture, part A. The macro below must NOT reach a type request made for
// cpp_interop_reqscope_b.h: each `import cpp` statement is its own translation unit, so a header
// imported earlier can never change the member surface a later import's template exposes.
#pragma once

#define CFLAT_REQ_SCOPE_LEAK 1

namespace reqa {

template <class T>
struct AHolder
{
    T value;
    T get() const noexcept { return value; }
    void set(T v) noexcept { value = v; }
};

}
