#pragma once
// Overload set for the [lex.icon] ladder: an unsuffixed literal's C++ type must survive template
// deduction. On LP64 0x8000000000000000 is unsigned long (pick returns 2).
namespace t76 {
inline int pick(long) { return 1; }
inline int pick(unsigned long) { return 2; }
inline int pick(unsigned long long) { return 3; }
template <class T> int deduce(T x) { return pick(x); }
}
