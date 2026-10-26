#pragma once
#include <memory>

namespace cpuq { class Widget; }

namespace cppt492 {
inline int ctorCount = 0;
inline int copyCtorCount = 0;
inline int moveCtorCount = 0;
inline int copyAssignCount = 0;
inline int moveAssignCount = 0;
inline int dtorCount = 0;
inline void reset() {
    ctorCount = copyCtorCount = moveCtorCount = copyAssignCount = moveAssignCount = dtorCount = 0;
}
inline int ctors() { return ctorCount; }
inline int copies() { return copyCtorCount; }
inline int moves() { return moveCtorCount; }
inline int copy_assigns() { return copyAssignCount; }
inline int move_assigns() { return moveAssignCount; }
inline int dtors() { return dtorCount; }

struct Counted {
    int value = 0;
    Counted() { ++ctorCount; }
    explicit Counted(int n) : value(n) { ++ctorCount; }
    Counted(const Counted& other) : value(other.value) { ++ctorCount; ++copyCtorCount; }
    Counted(Counted&& other) noexcept : value(other.value) {
        ++ctorCount; ++moveCtorCount; other.value = -1;
    }
    Counted& operator=(const Counted& other) {
        value = other.value; ++copyAssignCount; return *this;
    }
    Counted& operator=(Counted&& other) noexcept {
        value = other.value; other.value = -1; ++moveAssignCount; return *this;
    }
    ~Counted() { ++dtorCount; }
};

inline int counted_value(const Counted* value) { return value->value; }
inline bool unique_is_null(std::unique_ptr<cpuq::Widget>* value) {
    return value->get() == nullptr;
}
}
