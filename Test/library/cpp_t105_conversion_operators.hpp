#pragma once
#include <utility>
#include "t70_xvalue_overloads.h"

namespace cppi {
inline int t70_conv_value(T70Conv* value) { return value->value; }
inline int t70_inh_value(T70Inh* value) { return value->value; }
inline int t70_tpl_value(T70Tpl* value) { return value->value; }
inline int t70_der_value(T70Der* value) { return value->value; }
}

namespace t105 {
struct Trivial { int value; };
struct TrivialSource {
    int value;
    operator Trivial() const;
};
inline int trivialHits = 0;
inline TrivialSource::operator Trivial() const { ++trivialHits; return Trivial{value + 1}; }
inline TrivialSource makeTrivialSource() { return TrivialSource{10}; }

struct AmbiguousSource;
struct AmbiguousTarget {
    int value;
    AmbiguousTarget(int value) : value(value) {}
    AmbiguousTarget(const AmbiguousSource& source);
};
struct AmbiguousSource {
    int value;
    operator AmbiguousTarget() const { return AmbiguousTarget{value + 1}; }
};
inline AmbiguousTarget::AmbiguousTarget(const AmbiguousSource& source)
    : value(source.value + 300) {}
inline AmbiguousSource makeAmbiguousSource() { return AmbiguousSource{10}; }

struct W {
    int value;
    inline static int ctor, copy, assign, dtor, op;
    W(int value = 0) : value(value) { ++ctor; }
    W(const W& other) : value(other.value) { ++copy; }
    W& operator=(const W& other) { value = other.value; ++assign; return *this; }
    ~W() { ++dtor; }
};
struct P {
    int value;
    operator W() const { ++W::op; return W{value + 10}; }
};
inline P makeP() { return P{27}; }
inline void resetW() { W::ctor = W::copy = W::assign = W::dtor = W::op = 0; }
inline int wOps() { return W::op; }
inline int wCtors() { return W::ctor; }
inline int wCopies() { return W::copy; }
inline int wAssigns() { return W::assign; }
inline int wDtors() { return W::dtor; }

struct PointerSource {
    int value = 31;
    operator PointerSource*() {
        static PointerSource result;
        result.value = 73;
        return &result;
    }
};
inline int pointerPick(PointerSource* value) { return value->value; }
inline int pointerPick(void*) { return 91; }
inline int pointerOnly(PointerSource* value) { return value->value; }

struct TemplatePointerSource {
    int value = 31;
    template<class T> operator T*() {
        static TemplatePointerSource result;
        result.value = 73;
        return &result;
    }
};
inline int templatePointerPick(TemplatePointerSource*) { return 1; }
inline int templatePointerPick(void*) { return 2; }
}
