#pragma once

// Copy-assignment parity: a const lvalue source binds operator=(const CV&), a non-const lvalue binds
// operator=(CV&), as clang selects them.
namespace cppcp {
struct CV {
    int v = 0;
    CV(int x) : v(x) {}
    CV& operator=(CV& o) { v = o.v + 10; return *this; }
    CV& operator=(const CV& o) { v = o.v + 20; return *this; }
};
}
