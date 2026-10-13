#pragma once
#include <cstddef>
#include <cstdint>

namespace cpp_t111 {
using Nil = std::nullptr_t;

inline int pick(Nil) { return 111; }
inline int pick(int*) { return 112; }
inline int only(Nil) { return 113; }

struct FromNil {
    int tag;
    FromNil(Nil) : tag(181) {}
};

struct Value {
    int tag;
    Value(Nil) : tag(5) {}
};
inline int operator==(const Value& a, const Value& b) { return 500 + a.tag + b.tag; }

struct Implicit {
    int tag;
    Implicit(Nil) : tag(7) {}
};
inline int take_implicit(Implicit value) { return value.tag; }

inline int nref(const std::nullptr_t& p) { return p == nullptr ? 121 : 122; }
inline std::uintptr_t nref_address(const std::nullptr_t& p)
{
    return reinterpret_cast<std::uintptr_t>(&p);
}
inline int nref_r(std::nullptr_t&& p) { return p == nullptr ? 123 : 124; }
inline std::uintptr_t nref_r_address(std::nullptr_t&& p)
{
    return reinterpret_cast<std::uintptr_t>(&p);
}
}
