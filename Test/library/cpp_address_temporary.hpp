#pragma once

namespace addrtemp {
struct Box {
    int value;
    Box* p;
    constexpr Box(int v = 0) : value(v), p(nullptr) {}
    Box& operator=(const Box& other) { value = other.value; return *this; }
    Box operator+(const Box& other) const { return Box(value + other.value); }
    Box operator()(int add) const { return Box(value + add); }
    Box make() const { return Box(value + 100); }
};

struct Holder {
    Box field;
    constexpr explicit Holder(int v) : field(v) {}
};

inline Box make(int v) { return Box(v); }
inline Holder makeH(int v) { return Holder(v); }
inline Box& ref() { static Box value(17); return value; }
inline Box* array() { static Box values[2] = { Box(30), Box(40) }; return values; }
inline Box global_box(12);
inline int read_mutable(Box* value) { return value->value; }
inline int read_const(const Box* value) { return value->value; }
}
