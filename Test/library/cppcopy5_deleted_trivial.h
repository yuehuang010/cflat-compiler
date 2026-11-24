#pragma once

namespace cppcopy5
{
struct A
{
    int v = 1;
    A() {}
    A(const A&) = delete;
    A(A&&) = default;
};
}
