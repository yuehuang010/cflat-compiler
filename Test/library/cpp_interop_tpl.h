// C++ fixture for M5b - CLASS TEMPLATES instantiated from CFlat. Header-only on purpose: a
// specialization CFlat asks for has no symbol anywhere until the compiler instantiates it, so
// every body below has to be emitted into the companion module clang CodeGen hands back.
//
// Coverage: a constructor taking T, get/set, a static instance counter (an inline static data
// member, one per specialization), operator[] returning T by value, operator==, a specialization
// over a nontrivial C++ class (cppi::Tracked), and a class whose destructor is DEFAULTED but
// nontrivial because a member has one.
#pragma once

#include "cpp_interop_basic.h"

namespace cppt
{
    template <typename T>
    class Box
    {
    public:
        explicit Box(T v) noexcept : v_(v) { ++live_; ++ctors_; }
        Box(const Box& o) noexcept : v_(o.v_) { ++live_; ++ctors_; }
        ~Box() noexcept { --live_; ++dtors_; }

        T get() const noexcept { return v_; }
        void set(T v) noexcept { v_ = v; }
        // Index is ignored: a one-slot box, present only so CFlat's index expression has an
        // operator[] to bind to. Returns T BY VALUE (the by-reference form is covered by
        // std::vector's operator[] in section M9).
        T operator[](int index) const noexcept { return v_; }
        bool operator==(const Box& o) const noexcept { return v_ == o.v_; }

        static int live() noexcept { return live_; }
        static int ctors() noexcept { return ctors_; }
        static int dtors() noexcept { return dtors_; }
        static void reset() noexcept { ctors_ = 0; dtors_ = 0; }

    private:
        T v_;
        inline static int live_ = 0;
        inline static int ctors_ = 0;
        inline static int dtors_ = 0;
    };

    // Counter readers as FREE inline functions. A specialization's static member can only be
    // named in a DECLARATION today (`cppt.Box<int> b`), which is what drives instantiation, so a
    // test cannot write `cppt.Box<int>.live()` in expression position.
    inline int box_int_live() noexcept   { return Box<int>::live(); }
    inline int box_int_ctors() noexcept  { return Box<int>::ctors(); }
    inline int box_int_dtors() noexcept  { return Box<int>::dtors(); }
    inline void box_int_reset() noexcept { Box<int>::reset(); }
    inline int box_double_live() noexcept { return Box<double>::live(); }

    // The destructor is DEFAULTED, so there is no body in the header at all - but the member has
    // one, which makes the implicit destructor nontrivial and REQUIRED. As a template, the
    // specialization request forces Sema to define it and CodeGen to emit it.
    template <typename T>
    class Holder
    {
    public:
        // Takes an int and builds the member from it: a by-reference or by-value parameter of a
        // nontrivial class is not selectable at a CONSTRUCTOR yet (see M5B_REPORT.md).
        explicit Holder(int v) noexcept : t_(v) {}
        ~Holder() = default;
        int value() const noexcept { return t_.value(); }

    private:
        T t_;
    };
}
