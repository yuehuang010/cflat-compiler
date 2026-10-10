#pragma once

// T40: names a scoped enumerator shares with other entities. A scoped enumerator is never
// declared in its enclosing scope, so the unscoped enumerator, the variable and the global
// `using enum` re-export below are the bindings C++ unqualified lookup finds.
namespace t40c
{
    enum class S { T40Collide = 1, T40CppVar = 3, T40CfVar = 4 };
    inline int choose(int) { return 10; }
    inline int choose(S) { return 11; }
}

namespace t40g
{
    enum class GS { T40GlobalUse = 2 };
    inline int pick(int) { return 20; }
    inline int pick(GS) { return 21; }
}

using enum t40g::GS;
enum T40U { T40Collide = 77 };
inline int T40CppVar = 88;
