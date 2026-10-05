#pragma once
#include <string>

namespace cflat_t16 {
inline int free_bi(bool) { return 1; }
inline int free_bi(int) { return 2; }
inline int free_bs(bool) { return 3; }
inline int free_bs(const std::string&) { return 4; }
inline int free_lone(bool) { return 11; }
inline int free_bv(bool) { return 5; }
inline int free_bv(const void*) { return 6; }
inline int free_bc(bool) { return 7; }
inline int free_bc(const char*) { return 8; }

struct Member {
    int choose(bool) const { return 21; }
    int choose(int) const { return 22; }
};

struct Constructed {
    int value;
    Constructed(bool) : value(31) {}
    Constructed(int) : value(32) {}
};

struct Operator {
    int operator+(bool) const { return 41; }
    int operator+(int) const { return 42; }
};
}
