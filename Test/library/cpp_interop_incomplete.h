// A C++ type that no translation unit ever defines, so a std container of it can be named
// (through a pointer) but its iterator-returning members cannot be instantiated.
#pragma once
#include <deque>

namespace cpin
{
    struct Opaque;
}
