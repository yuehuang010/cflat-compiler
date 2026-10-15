#pragma once

namespace t261 {
inline int ctor_count = 0;
inline int copy_count = 0;
inline int move_count = 0;
inline int dtor_count = 0;
inline void reset() { ctor_count = copy_count = move_count = dtor_count = 0; }
inline int ctors() { return ctor_count; }
inline int copies() { return copy_count; }
inline int moves() { return move_count; }
inline int dtors() { return dtor_count; }

struct Counted {
    int value = 0;
    Counted() { ++ctor_count; }
    explicit Counted(int v) : value(v) { ++ctor_count; }
    Counted(const Counted& other) : value(other.value) { ++copy_count; }
    Counted(Counted&& other) noexcept : value(other.value) { ++move_count; other.value = -1; }
    ~Counted() { ++dtor_count; }
};

// Throws when x is 1 (before the result is built); the callback guard catches it.
inline void boom(int x) { if (x == 1) throw 7; }
inline int bval(int x) { boom(x); return x; }
inline Counted mk(int x) { boom(x); return Counted(x + 10); }

typedef int (*Cb)(int);
inline int guarded(Cb f, int x) { try { return f(x); } catch (int e) { return -e; } }
}
