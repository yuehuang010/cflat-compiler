#pragma once
namespace t17 {
struct Empty {
    int operator()(int a, int b) const { return a < b ? 1 : 0; }
};
struct Owner {
    Empty member() const { return {}; }
};
inline Empty free_value() { return {}; }
inline int consume(Empty value) { return value(1, 2); }
}
