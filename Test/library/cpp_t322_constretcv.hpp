#pragma once

namespace cppi322
{
    struct R
    {
        int f() { return 1; }
        int f() const { return 2; }
        int onlyConst() const { return 3; }
    };

    inline const R make() { return R{}; }
    inline R make2() { return R{}; }
    inline const R& ref() { static R value; return value; }
    inline const R* ptr() { static R value; return &value; }

    struct Holder
    {
        const R get() const { return R{}; }
    };
    inline Holder holder;

    struct Only
    {
        int f() { return 7; }
    };
    inline const Only makeOnly() { return Only{}; }
}
