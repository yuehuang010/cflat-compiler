#pragma once

namespace cjnest
{
    struct Method
    {
        int pick(void*) { return 1; }
        int pick(const void*) { return 2; }
    };

    struct Ctor
    {
        int value;
        Ctor(void*) : value(1) { }
        Ctor(const void*) : value(2) { }
    };
}
