#pragma once
#include <iterator>

namespace castglob {
struct Box {
    int value;
    Box(int x) : value(x) {}
    Box(const char* p) : value(static_cast<unsigned char>(p[0])) {}
    int get() const { return value; }
};
inline Box makeBox(int x) { return Box(x); }

using RefFn = char (&(*)())[4];
inline char (&buffer())[4] {
    static char value[4] = {97, 98, 99, 0};
    return value;
}
inline RefFn getfn() { return &buffer; }
struct Fn {
    int value;
    Fn(RefFn f) : value(static_cast<unsigned char>(f()[0])) {}
    int get() const { return value; }
};
}
