#pragma once
namespace cpprefres {
inline int defaultConst = 700;
inline int defaultRef = 800;
struct Pair { const int& c; int& r; int value; };
struct Defaulted { const int& c = defaultConst; int& r = defaultRef; int value; };
struct RefOnly { int& r; };
struct Holder { Pair items[2]; int tail; };
struct Holder2D { Pair items[2][2]; int tail; };
}
