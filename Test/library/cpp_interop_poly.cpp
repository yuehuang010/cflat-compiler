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

    QualNames::QualNames() noexcept { lv = 55; cv = 66; bv = 77; }
    QualNames::~QualNames() noexcept {}
    int QualNames::l() const noexcept { return lv + 3000; }

    int read_constant(const constant* p) noexcept { return p->cv; }
    int read_class_base(const classBase* p) noexcept { return p->bv; }
    QualNames* make_qual() noexcept { return new QualNames(); }
    void destroy_qual(QualNames* p) noexcept { delete p; }

    Abstract::~Abstract() noexcept {}

    VBaseTop::~VBaseTop() noexcept {}
    int VBaseTop::t() const noexcept { return tv; }

    // ---- M41 ----
    static int g_slot_ctors = 0, g_slot_copies = 0, g_slot_moves = 0, g_slot_dtors = 0;
    void reset_slot_counts() noexcept
    { g_slot_ctors = g_slot_copies = g_slot_moves = g_slot_dtors = 0; }
    int slot_ctors() noexcept { return g_slot_ctors; }
    int slot_copies() noexcept { return g_slot_copies; }
    int slot_moves() noexcept { return g_slot_moves; }
    int slot_dtors() noexcept { return g_slot_dtors; }

    SlotVal::SlotVal(int value) noexcept : v(value) { ++g_slot_ctors; }
    SlotVal::SlotVal(const SlotVal& o) noexcept : v(o.v) { ++g_slot_copies; }
    SlotVal::SlotVal(SlotVal&& o) noexcept : v(o.v) { ++g_slot_moves; }
    SlotVal::~SlotVal() noexcept { ++g_slot_dtors; }

    Slot::Slot() noexcept { sv = 11; }
    Slot::~Slot() noexcept {}
    int Slot::primary() const noexcept { return sv; }
    int Slot::primary_val(SlotVal t) const noexcept { return sv + t.v; }

    Second::Second() noexcept { cv2 = 22; }
    Second::~Second() noexcept {}
    int Second::second() const noexcept { return cv2; }
    int Second::second_add(int x, int y) const noexcept { return cv2 + x + y; }
    int Second::second_val(SlotVal t) const noexcept { return cv2 + t.v; }
    SlotVal Second::second_ret() const noexcept { return SlotVal(cv2); }

    int read_second(const Second* p) noexcept { return p->cv2; }
    int read_slot(const Slot* p) noexcept { return p->sv; }
    TwoSlots<int>* make_two_slots() noexcept { return new TwoSlots<int>(); }
    void destroy_two_slots(TwoSlots<int>* p) noexcept { delete p; }

    CloneBase::CloneBase() noexcept { bvv = 55; }
    CloneBase::~CloneBase() noexcept {}
    CloneBase* CloneBase::clone() const noexcept { return new CloneBase(); }
    int read_clone(const CloneBase* p) noexcept { return p->bvv; }
    CloneDer<int>* make_clone_der() noexcept { return new CloneDer<int>(); }
    void destroy_clone(CloneBase* p) noexcept { delete p; }

    static int g_vbase_dtors = 0;
    void note_vbase_dtor() noexcept { ++g_vbase_dtors; }
    void reset_vbase_dtors() noexcept { g_vbase_dtors = 0; }
    int vbase_dtors() noexcept { return g_vbase_dtors; }
    VBaseUse<int>* make_vbase_use() noexcept { return new VBaseUse<int>(); }
    void destroy_vbase_use(VBaseUse<int>* p) noexcept { delete p; }
}
