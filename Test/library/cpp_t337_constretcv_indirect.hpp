#pragma once
namespace cppi337
{
    struct R { int value; };
    inline const R make() { return R{12}; }
    inline int const_arg_pick(R&&) { return 11; }
    inline int const_arg_pick(const R&&) { return 12; }
}
