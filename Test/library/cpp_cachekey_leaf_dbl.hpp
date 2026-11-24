#pragma once
// CACHE-KEY twin of cpp_cachekey_leaf_int.hpp: same class name, double val().
namespace ckeyl {
struct Leaf {
    int v;
    Leaf(int x) : v(x) {}
    double val() const { return v + 0.5; }
};
}
