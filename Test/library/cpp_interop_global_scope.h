#pragma once

template <class T>
struct NestedGlobalBox
{
    T value;
};

namespace global_scope_fixture
{
    struct Holder
    {
        NestedGlobalBox<int> box;
    };

    inline NestedGlobalBox<int> echo(NestedGlobalBox<int> value) noexcept
    {
        return value;
    }
}
