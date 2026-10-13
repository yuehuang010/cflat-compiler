#pragma once
// T97: a counted C++ class with no default constructor, held as a field of CFlat structs.
namespace cpp_t97 {
inline int ctor_count = 0, copy_count = 0, move_count = 0, dtor_count = 0;
struct Dl
{
    int v;
    Dl(int a) : v(a) { ++ctor_count; }
    Dl(const Dl& o) : v(o.v) { ++copy_count; }
    Dl(Dl&& o) : v(o.v) { ++move_count; }
    ~Dl() { ++dtor_count; }
};
inline void reset() { ctor_count = copy_count = move_count = dtor_count = 0; }
inline int made() { return ctor_count + copy_count + move_count; }
}
