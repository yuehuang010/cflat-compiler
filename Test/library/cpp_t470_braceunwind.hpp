#pragma once

namespace t470 {
inline int ctor_count = 0;
inline int copy_count = 0;
inline int copy_attempts = 0;
inline int move_count = 0;
inline int dtor_count = 0;
inline int live_count = 0;
inline int counted_dtors = 0;
inline int counted_live = 0;
inline int tc_dtors = 0;
inline int tc_live = 0;
inline int throw_copy = 0;

inline void reset() {
    ctor_count = copy_count = copy_attempts = move_count = dtor_count = live_count = 0;
    counted_dtors = counted_live = tc_dtors = tc_live = throw_copy = 0;
}
inline void set_throw(int value) { throw_copy = value; }
inline int ctors() { return ctor_count; }
inline int copies() { return copy_count; }
inline int copyAttempts() { return copy_attempts; }
inline int moves() { return move_count; }
inline int dtors() { return dtor_count; }
inline int live() { return live_count; }
inline int countedDtors() { return counted_dtors; }
inline int countedLive() { return counted_live; }
inline int tcDtors() { return tc_dtors; }
inline int tcLive() { return tc_live; }

struct Counted {
    int value = 0;
    Counted() { ++ctor_count; ++live_count; ++counted_live; }
    Counted(const Counted& other) : value(other.value) { ++copy_count; ++live_count; ++counted_live; }
    Counted(Counted&& other) noexcept : value(other.value) {
        ++move_count; ++live_count; ++counted_live; other.value = -1;
    }
    ~Counted() { ++dtor_count; --live_count; ++counted_dtors; --counted_live; }
};
inline void destroyCounted(Counted* value) { value->~Counted(); }

struct TC {
    int value = 0;
    TC() { ++ctor_count; ++live_count; ++tc_live; }
    TC(const TC& other) : value(other.value) {
        ++copy_attempts;
        if (throw_copy) throw 7;
        ++copy_count; ++live_count; ++tc_live;
    }
    TC(TC&& other) noexcept : value(other.value) {
        ++move_count; ++live_count; ++tc_live; other.value = -1;
    }
    ~TC() { ++dtor_count; --live_count; ++tc_dtors; --tc_live; }
};

struct BraceHost_CTZ_U { Counted c; TC t; int z; BraceHost_CTZ_U() : z(3) {} };
struct BraceHost_TCZ_U { TC t; Counted c; int z; BraceHost_TCZ_U() : z(3) {} };
struct BraceHost_TZC_U { TC t; int z; Counted c; BraceHost_TZC_U() : z(3) {} };
struct BraceHost_CTZ_N { Counted c; TC t; int z = 3; };
struct BraceHost_TCZ_N { TC t; Counted c; int z = 3; };
struct BraceHost_TZC_N { TC t; int z = 3; Counted c; };

inline int bval(int value) { if (value == 1) throw 7; return value; }
using Callback = int (*)(int);
inline int guarded(Callback callback, int value) {
    try { return callback(value); }
    catch (int error) { return -error; }
}
}
