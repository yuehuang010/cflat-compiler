#pragma once
#include <functional>
#include <cstddef>
#define CPPFN_ZERO 0
#define CPPFN_ZEROL 0L
#define CPPFN_NULL __null
namespace cppfnptr {
using Cb = int (*)(int);
typedef int (*CbAlias)(int);
using VoidCb = void (*)();
inline int plus_one(int x) { return x + 1; }
inline int callback_present(Cb f) { return f ? 1 : 0; }
inline int callback_call(CbAlias f, int x) { return f ? f(x) : -1; }
inline Cb callback_return() { return &plus_one; }
inline int void_callback(VoidCb f) { if (f) { f(); return 1; } return 0; }
struct Source {
    static int callback(int x) { return x + 40; }
};
struct Sink {
    Sink() = default;
    explicit Sink(Cb f) : value(f ? f(4) : -1) {}
    int member(Cb f) { return f ? 1 : 0; }
    int value;
};
struct FunctionSink {
    int value;
    explicit FunctionSink(std::function<int(int)> f) : value(f ? 1 : 0) {}
    FunctionSink(std::function<int(int)> f, int v) : value(f ? v : -v) {}
};
struct PointerSink {
    int value;
    explicit PointerSink(int* p) : value(p ? 1 : 0) {}
};
inline int inline_callback(int (*f)(int), int x) { return f ? f(x) : -1; }
inline int std_function_callback(std::function<int(int)> f, int x) { return f(x); }
inline int std_function_present(std::function<int(int)> f) { return f ? 1 : 0; }

inline int null_slot_pick(std::function<int(int)> f) { return f ? 71 : 72; }
inline int null_slot_pick(int (*f)(int)) { return f ? 81 : 82; }
inline int null_slot_pick(int value) { return 91; }
inline int null_slot_pick_data(std::function<int(int)> f) { return f ? 101 : 102; }
inline int null_slot_pick_data(int* p) { return p ? 111 : 112; }

inline int null_ctor_born = 0;
inline int null_ctor_died = 0;
inline int null_ctor_fn_pick = 0;
inline int null_ctor_int_pick = 0;
inline int null_ctor_copy_pick = 0;
inline int null_ctor_multi_fn_pick = 0;
inline int null_ctor_multi_int_pick = 0;

inline void reset_null_ctor_counts()
{
    null_ctor_born = null_ctor_died = null_ctor_fn_pick = null_ctor_int_pick = null_ctor_copy_pick = 0;
    null_ctor_multi_fn_pick = null_ctor_multi_int_pick = 0;
}

struct NullFunctionCtorProbe {
    int value;
    NullFunctionCtorProbe(std::function<int(int)> f, int bump = 3)
        : value((f ? 201 : 200) + bump) { ++null_ctor_born; ++null_ctor_fn_pick; }
    NullFunctionCtorProbe(int n) : value(300 + n) { ++null_ctor_born; ++null_ctor_int_pick; }
    NullFunctionCtorProbe(const NullFunctionCtorProbe& other) : value(other.value)
        { ++null_ctor_born; ++null_ctor_copy_pick; }
    ~NullFunctionCtorProbe() { ++null_ctor_died; }
};

struct NullFunctionMultiCtorProbe {
    int value;
    NullFunctionMultiCtorProbe(int tag, std::function<int(int)> f, int bump = 4)
        : value(tag * 100 + (f ? 10 : 0) + bump) { ++null_ctor_born; ++null_ctor_multi_fn_pick; }
    NullFunctionMultiCtorProbe(int tag, int n, int bump = 4)
        : value(tag * 100 + n + bump) { ++null_ctor_born; ++null_ctor_multi_int_pick; }
    ~NullFunctionMultiCtorProbe() { ++null_ctor_died; }
};
}
