#pragma once

// MIX7: a C++ bool parameter takes a function pointer by value (a pointer-to-bool conversion):
// true for a set pointer, false for a null one.
namespace mix7fpb
{
    inline int take_bool(bool b) { return b ? 3 : 4; }
}
