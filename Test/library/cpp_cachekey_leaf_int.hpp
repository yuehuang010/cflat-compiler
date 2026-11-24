#pragma once
// CACHE-KEY twin of cpp_cachekey_leaf_dbl.hpp: same class name, int val().
namespace ckeyl {
struct Leaf {
    int v;
    Leaf(int x) : v(x) {}
    int val() const { return v; }
};
}
