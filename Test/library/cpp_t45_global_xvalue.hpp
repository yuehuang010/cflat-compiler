#pragma once

#include <cstdint>
#include <utility>

// T45: a default-constructible C++ class, so a file-scope source global is constant-initialized
// and only the std::move initializer under test is the non-constant one.
namespace cpp_t45 {
    struct Movable
    {
        int v = 7;
        Movable() noexcept = default;
        Movable(const Movable& other) noexcept : v(other.v) {}
        Movable(Movable&& other) noexcept : v(other.v) { other.v = -1; }
        ~Movable() noexcept {}
    };
    inline Movable&& pass(Movable& m) noexcept { return std::move(m); }

    // Round 2: a negative enumerator (signed division / remainder folds) and a class whose
    // invariant is its own address (a relocated object reports ok() == 0).
    enum Neg { neg_five = -5, three = 3 };
    struct SelfAddr
    {
        std::uintptr_t self;
        int v;
        SelfAddr(int x = 0) noexcept : self(reinterpret_cast<std::uintptr_t>(this)), v(x) {}
        SelfAddr(const SelfAddr& o) noexcept : self(reinterpret_cast<std::uintptr_t>(this)), v(o.v) {}
        SelfAddr(SelfAddr&& o) noexcept : self(reinterpret_cast<std::uintptr_t>(this)), v(o.v) { o.v = -1; }
        ~SelfAddr() noexcept {}
        int ok() const noexcept { return self == reinterpret_cast<std::uintptr_t>(this) ? 1 : 0; }
        int get() const noexcept { return v; }
    };
}
