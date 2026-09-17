#pragma once

template <class T>
struct BadT
{
    typename T::nested n;
};

using BadAlias = BadT<int>;

namespace cppbadns
{
    template <class T>
    struct BadT
    {
        typename T::nested n;
    };

    using BadAlias = BadT<int>;
}
