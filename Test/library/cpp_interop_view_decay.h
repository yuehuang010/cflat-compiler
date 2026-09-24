#pragma once
#include <span>
#include <vector>

namespace cppview
{
inline std::vector<int>& vector_ref()
{
    static std::vector<int> values{6, 9};
    return values;
}

inline const std::vector<int>& const_vector_ref()
{
    return vector_ref();
}

inline std::span<int> span_ref()
{
    return vector_ref();
}

struct Item
{
    int value;
};

inline std::vector<Item> make_items()
{
    return {{12}, {15}};
}

struct Base
{
    int value;
};

struct Derived : Base
{
};

inline std::vector<Derived> make_derived()
{
    return std::vector<Derived>(2);
}
}
