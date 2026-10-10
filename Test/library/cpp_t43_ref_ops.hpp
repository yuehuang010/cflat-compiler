#pragma once

namespace t43 {
struct RefOps {
    int value;
    RefOps(int v = 0) : value(v) {}
    RefOps operator*(const RefOps& rhs) const { return RefOps(value * rhs.value); }
    RefOps operator+(const RefOps& rhs) const { return RefOps(value + rhs.value); }
    const RefOps& operator<<(const RefOps& rhs) const { return value >= rhs.value ? *this : rhs; }
    const RefOps& operator>>(const RefOps& rhs) const { return value >= rhs.value ? *this : rhs; }
    const RefOps& operator&&(const RefOps& rhs) const { return value >= rhs.value ? *this : rhs; }
    const RefOps& operator||(const RefOps& rhs) const { return value >= rhs.value ? *this : rhs; }
    const RefOps& operator==(const RefOps& rhs) const { return value >= rhs.value ? *this : rhs; }
    const RefOps& operator<(const RefOps& rhs) const { return value >= rhs.value ? *this : rhs; }
};
template<class T> int ptr_kind(T) { return 9; }
template<class T> int ptr_kind(T*) { return 2; }
struct Kinder {
    int base = 0;
    template<class T> int mk(T) { return base + 9; }
    template<class T> int mk(T*) { return base + 3; }
};
}
