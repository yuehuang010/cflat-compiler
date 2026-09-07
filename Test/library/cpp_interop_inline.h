// C++ fixture for M7 - HEADER-ONLY C++. There is deliberately no sibling .cpp: every function,
// method, constructor, destructor and static below is defined INLINE here, so the only way any of
// it can be called is for clang CodeGen to emit the definitions into the companion module cflat
// links in (linkonce_odr bodies, the vtables of the polymorphic classes, guard variables for the
// static locals, and the inline static data members).
//
// Coverage: an inline free function, an inline function that calls another one (transitive
// emission), an inline function with a static local, inline static data members used as lifetime
// counters, an inline static method, an all-inline class with a nontrivial constructor/destructor,
// and an all-inline VIRTUAL hierarchy with a virtual destructor (no key function anywhere).
// `may_throw` has no noexcept specification on purpose: it arms
// Test/errors/err_cpp_inline_may_throw.cb, since emitting a body does not make unwinding safe.
#pragma once

namespace cppinl
{
    inline int helper(int v) noexcept { return v * 3; }
    // Calls another inline function: proves the callee is emitted transitively, not just the
    // declaration cflat asked for by name.
    inline int twice(int v) noexcept { return helper(v) + helper(v); }

    // A static local inside an inline function needs the guard variable and the zero-initialized
    // slot to be emitted alongside the body.
    inline int next_id() noexcept { static int counter = 100; return ++counter; }

    inline int may_throw(int v) { return v + 1; }

    // Nontrivial (user-provided destructor), entirely inline. The lifetime counters are inline
    // STATIC DATA MEMBERS - no out-of-line definition exists, so their storage comes from the
    // companion module too, as does the inline static method that clears them.
    class Box
    {
    public:
        static inline int ctors = 0;
        static inline int dtors = 0;
        static inline void reset() noexcept { ctors = 0; dtors = 0; }
        inline Box(int start) noexcept : value_(start) { ++ctors; }
        inline ~Box() noexcept { ++dtors; }
        inline int get() const noexcept { return value_; }
        inline void add(int v) noexcept { value_ += v; }
        inline int scaled() const noexcept { return helper(value_); }
        int value_;
    };

    // All-inline polymorphic hierarchy: no key function, so the vtable of each class must be
    // emitted here (linkonce_odr, COMDAT) or virtual dispatch has nothing to read.
    class Base
    {
    public:
        static inline int base_dtors = 0;
        static inline int derived_dtors = 0;
        static inline void reset_dtors() noexcept { base_dtors = 0; derived_dtors = 0; }
        inline Base(int tag) noexcept : tag_(tag) {}
        inline virtual ~Base() noexcept { ++base_dtors; }
        inline virtual int area() const noexcept { return 10; }
        inline int plain() const noexcept { return tag_ + 1; }
        int tag_;
    };

    class Derived : public Base
    {
    public:
        inline Derived(int side) noexcept : Base(2), side_(side) {}
        inline ~Derived() noexcept { ++derived_dtors; }
        inline int area() const noexcept override { return side_ * side_; }
        int side_;
    };
}
