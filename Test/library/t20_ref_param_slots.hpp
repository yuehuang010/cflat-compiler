#pragma once
namespace t20slot {
struct Big {
    long v[4] = {0, 0, 0, 0};
    Big(int x) { v[0] = x; }
    Big(const Big& other) { for (int i = 0; i < 4; ++i) v[i] = other.v[i]; }
    ~Big() {}
};
struct H {
    long refafter(Big b, Big& r) { return b.v[0] + r.v[0]; }
};
}
