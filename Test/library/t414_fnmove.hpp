#pragma once

#include <functional>
#include <utility>

namespace q {
using Fn = int (*)(int);
inline int plus(int x) { return x + 1; }
inline int useFn(Fn f) { return f(5); }

struct ByVal {
    Fn f;
    explicit ByVal(Fn value) : f(value) {}
    int call() const { return f(5); }
};

inline int useStd(std::function<int(int)> f) { return f(3); }
struct StdWrap {
    std::function<int(int)> f;
    explicit StdWrap(std::function<int(int)> value) : f(std::move(value)) {}
    int call() const { return f(5); }
};

inline std::function<int(int)> makeSmallStd() { return plus; }
template<class F> int useTplVal(F f) { return f(5); }
template<class F> int useTplRef(F&& f) { return f(5); }
inline int other(int x) { return x + 2; }
template<class F> int reseatTpl(F f, void* slot) { *static_cast<Fn*>(slot) = other; return f(5); }
struct PickCtor {
    int value;
    template<class F> PickCtor(F f, int) : value(f(5) + 100) {}
    template<class F> PickCtor(F&& f, const char*) : value(f(5) + 200) {}
};
struct TplRef {
    Fn f;
    template<class F> explicit TplRef(F&& g) : f(g) {}
    int call() const { return f(5); }
};
}
