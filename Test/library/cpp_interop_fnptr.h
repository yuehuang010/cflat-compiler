#pragma once
#include <functional>
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
inline int inline_callback(int (*f)(int), int x) { return f ? f(x) : -1; }
inline int std_function_callback(std::function<int(int)> f, int x) { return f(x); }
}
