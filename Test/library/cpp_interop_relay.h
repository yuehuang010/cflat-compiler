#pragma once
#include <type_traits>
namespace rv {
inline int present(int x) { return x; }
namespace a { namespace b { inline int present(int x) { return x; } } }
struct C { static int present(int); int member = 0; };
template<class T> struct Box { static int present(int); };
}
