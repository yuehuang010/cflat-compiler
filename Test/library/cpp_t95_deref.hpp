#pragma once
// T95: unary '*' on a fixed array of C++ records decays to the element pointer (clang: builtin
// deref, never the element's operator*).
namespace t95d {
struct C { int n = 17; };
struct D { int n = 3; int& operator*() { return n; } };
}
