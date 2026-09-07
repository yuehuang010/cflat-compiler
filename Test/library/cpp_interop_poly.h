// C++ fixture for M6 - inheritance and virtual dispatch. Everything here is declared only;
// definitions live in the sibling cpp_interop_poly.cpp so every virtual function has a real
// out-of-line symbol and the vtable has a key function to anchor it.
//
// Shapes cover single inheritance: virtual dispatch through a base pointer, a virtual
// destructor reached through `delete`, and an inherited field.
// Left / Right / Both cover MULTIPLE inheritance: `Right` is a NON-PRIMARY base of `Both`, so
// its subobject sits at a non-zero offset and every `Both* -> Right*` crossing must adjust.
// Abstract exists only to arm Test/errors/err_cpp_abstract_ctor.cb.
// VirtBase exists only to arm Test/errors/err_cpp_virtual_base.cb.
#pragma once

namespace cpppoly
{
    class Shape
    {
    public:
        Shape() noexcept;
        virtual ~Shape() noexcept;
        virtual int area() const noexcept;
        int plain() const noexcept;
        int tag;
    };

    class Circle : public Shape
    {
    public:
        Circle() noexcept;
        ~Circle() noexcept override;
        int area() const noexcept override;
        int radius;
    };

    class Square : public Shape
    {
    public:
        Square() noexcept;
        ~Square() noexcept override;
        int area() const noexcept override;
        int side;
    };

    // kind 0 -> Circle, anything else -> Square. Built on the C++ side, so the object's vptr
    // was written by C++ and the CFlat side only reads it.
    Shape* make(int kind) noexcept;
    void destroy(Shape* s) noexcept;

    void reset_poly_counts() noexcept;
    int shape_dtors() noexcept;
    int circle_dtors() noexcept;
    int square_dtors() noexcept;

    struct Left
    {
        Left() noexcept;
        virtual ~Left() noexcept;
        virtual int l() const noexcept;
        int lv;
    };

    struct Right
    {
        Right() noexcept;
        virtual ~Right() noexcept;
        virtual int r() const noexcept;
        int rv;
    };

    struct Both : Left, Right
    {
        Both() noexcept;
        ~Both() noexcept override;
        int l() const noexcept override;
        int r() const noexcept override;
        int both_only() const noexcept;
    };

    // Takes the NON-PRIMARY base. Passing a Both* here is only correct if the caller added
    // Right's base offset; reading rv proves it did.
    int read_right(const Right* p) noexcept;
    int call_right(const Right* p) noexcept;

    // M6b - class names that CONTAIN a declarator keyword as a substring ("const" inside
    // "constant", "class" inside "classBase"). Stripping the keyword as a substring loses the
    // class identity, the parameter decays to void*, and the base adjustment silently vanishes.
    // Both are NON-PRIMARY bases of QualNames, so reading their fields proves it happened.
    struct constant { int cv; };
    struct classBase { int bv; };

    struct QualNames : Left, constant, classBase
    {
        QualNames() noexcept;
        ~QualNames() noexcept override;
        int l() const noexcept override;
    };

    int read_constant(const constant* p) noexcept;
    int read_class_base(const classBase* p) noexcept;
    QualNames* make_qual() noexcept;
    void destroy_qual(QualNames* p) noexcept;

    class Abstract
    {
    public:
        virtual ~Abstract() noexcept;
        virtual int pure() const noexcept = 0;
    };

    struct VBaseTop { virtual ~VBaseTop() noexcept; virtual int t() const noexcept; int tv; };
    struct VBaseMid : virtual VBaseTop { int mv; };
}
