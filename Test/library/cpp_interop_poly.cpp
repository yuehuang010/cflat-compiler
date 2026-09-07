// Definitions for cpp_interop_poly.h. Compiled by the host C++ driver and linked into the final
// image; the CFlat side never parses this file. Every virtual function is out-of-line on purpose:
// the vtable needs a key function, and CFlat refuses a virtual member that has no symbol.
#include "cpp_interop_poly.h"

namespace cpppoly
{
    static int g_shape_dtors = 0;
    static int g_circle_dtors = 0;
    static int g_square_dtors = 0;

    void reset_poly_counts() noexcept { g_shape_dtors = 0; g_circle_dtors = 0; g_square_dtors = 0; }
    int shape_dtors() noexcept { return g_shape_dtors; }
    int circle_dtors() noexcept { return g_circle_dtors; }
    int square_dtors() noexcept { return g_square_dtors; }

    Shape::Shape() noexcept { tag = 1; }
    Shape::~Shape() noexcept { ++g_shape_dtors; }
    int Shape::area() const noexcept { return 0; }
    int Shape::plain() const noexcept { return tag + 100; }

    Circle::Circle() noexcept { tag = 2; radius = 5; }
    Circle::~Circle() noexcept { ++g_circle_dtors; }
    int Circle::area() const noexcept { return radius * 10; }

    Square::Square() noexcept { tag = 3; side = 7; }
    Square::~Square() noexcept { ++g_square_dtors; }
    int Square::area() const noexcept { return side * side; }

    Shape* make(int kind) noexcept
    {
        if (kind == 0) return new Circle();
        return new Square();
    }
    void destroy(Shape* s) noexcept { delete s; }

    Left::Left() noexcept { lv = 11; }
    Left::~Left() noexcept {}
    int Left::l() const noexcept { return lv; }

    Right::Right() noexcept { rv = 22; }
    Right::~Right() noexcept {}
    int Right::r() const noexcept { return rv; }

    Both::Both() noexcept { lv = 33; rv = 44; }
    Both::~Both() noexcept {}
    int Both::l() const noexcept { return lv + 1000; }
    int Both::r() const noexcept { return rv + 2000; }
    int Both::both_only() const noexcept { return lv + rv; }

    int read_right(const Right* p) noexcept { return p->rv; }
    int call_right(const Right* p) noexcept { return p->r(); }

    Abstract::~Abstract() noexcept {}

    VBaseTop::~VBaseTop() noexcept {}
    int VBaseTop::t() const noexcept { return tv; }
}
