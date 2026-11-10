#pragma once
// Builtin pointer `<=>` across a non-primary base: both sides must name the same subobject.
namespace cppss
{
    struct A { long a = 0; };
    struct B { int b = 0; };
    struct M : A, B { int m = 0; };
    inline B* base(M* p) { return p; }
}
