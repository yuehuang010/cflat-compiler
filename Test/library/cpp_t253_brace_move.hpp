#pragma once

namespace t253 {
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
    Counted(const Counted& other) : value(other.value + 100) { ++ctor_count; ++copy_count; }
    Counted(Counted&& other) noexcept : value(other.value + 1000) {
        ++ctor_count; ++move_count; other.value = -1;
    }
    ~Counted() { ++dtor_count; }
};

struct NoMove {
    int value = 0;
    explicit NoMove(int v) : value(v) { ++ctor_count; }
    NoMove(const NoMove& other) : value(other.value + 100) { ++ctor_count; ++copy_count; }
    NoMove(NoMove&&) = delete;
    ~NoMove() { ++dtor_count; }
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdefaulted-function-deleted"
struct Fallback {
    NoMove item;
    explicit Fallback(int v) : item(v) {}
    Fallback(const Fallback&) = default;
    Fallback(Fallback&&) = default;
    ~Fallback() = default;
};
#pragma clang diagnostic pop
}

// T258: a C++ class whose MOVE constructor throws when armed; the callback guard catches it.
namespace t258 {
inline int ctor_count = 0;
inline int move_count = 0;
inline int dtor_count = 0;
inline int throw_at = -1;
inline void reset() { ctor_count = move_count = dtor_count = 0; throw_at = -1; }
inline void set_throw(int n) { throw_at = n; }
inline int ctors() { return ctor_count; }
inline int moves() { return move_count; }
inline int dtors() { return dtor_count; }

struct Throwy {
    int value = 0;
    Throwy() { ++ctor_count; }
    Throwy(const Throwy& other) : value(other.value + 100) { ++ctor_count; }
    Throwy(Throwy&& other) : value(other.value + 1000) {
        if (move_count + 1 == throw_at) throw 7;
        ++ctor_count; ++move_count; other.value = -1;
    }
    ~Throwy() { ++dtor_count; }
};

typedef int (*Cb)(int);
inline int guarded(Cb f, int x) { try { return f(x); } catch (int e) { return -e; } }
}
