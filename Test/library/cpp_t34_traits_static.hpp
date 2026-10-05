#pragma once

// allocator_traits shape: a static non-template member beside static member templates of the
// same name and a higher arity (libc++ allocate(a, n) / allocate(a, n, hint)).
namespace t34traits {
struct Alloc {
    int base = 100;
    int take(int n) { return base + n; }
};

template <class A>
struct traits {
    static int allocate(A& a, int n) { return a.take(n); }
    template <class B = A, int = 0>
    static int allocate(A& a, int n, const void*) { return a.take(n) + 1000; }
    template <class B = A, long = 0>
    static int allocate(A& a, int n, const void*, int extra) { return a.take(n) + 2000 + extra; }
};
} // namespace t34traits
