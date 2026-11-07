#pragma once
#include <initializer_list>

// Imported macro constants are C++ prvalues; the enum and brace shapes are constants in C++.
#define T98_N5 5
#define T98_SMALL 5
#define T98_BIG 70000
#define T98_UMAX 4294967295U
#define T98_ULLMAX 18446744073709551615ULL
#define T98_CH 'A'

enum { T98_ANON_VALUE = 17 };
using T98AnonEnum = decltype(T98_ANON_VALUE);
#define T98_ANON_MACRO T98_ANON_VALUE
enum T98NamedEnum { T98_NAMED_VALUE = 19 };
#define T98_NAMED_MACRO T98_NAMED_VALUE
#define T98_BOOL_MACRO true

namespace cpi_t98 {
enum Color { Red = 1, Green = 2 };
inline namespace t98_v1 { enum InlineEnum { InlineValue = 18 }; }
inline int ov(int& v) { return 100 + v; }
inline int ov(int&& v) { return 200 + v; }
inline int iref(int& v) { return v + 10; }
inline int irr(int&& v) { return v + 40; }
inline int cref(const int& v) { return v + 1; }
template <class T> int fwd(T&& v) { return __is_lvalue_reference(T) ? 80 + (int)v : 90 + (int)v; }
inline int commaCounter = 0;
inline int commaBump() { return ++commaCounter; }
inline int commaPick(int& v) { return 100 + v; }
inline int commaPick(int&& v) { return 200 + v; }
inline int commaCount() { return commaCounter; }
inline int derefCell = 40;
inline int* derefPointer = &derefCell;
// Shaped like std::vector: an initializer_list constructor over the element type.
struct Shorts {
    int n; int first;
    Shorts(std::initializer_list<short> l) : n((int)l.size()), first(*l.begin()) {}
};
struct Ints {
    int n; int first;
    Ints(std::initializer_list<int> l) : n((int)l.size()), first(*l.begin()) {}
};
// Overloads keyed on the constant's own type: int 11, unsigned long long 22, char 33, Color 44.
struct Pick {
    int n;
    Pick(int) : n(11) {}
    Pick(unsigned long long) : n(22) {}
    Pick(char) : n(33) {}
    Pick(Color) : n(44) {}
};
struct MacroAnonPick {
    int n;
    MacroAnonPick(T98AnonEnum) : n(71) {}
    MacroAnonPick(int) : n(72) {}
};
struct MacroInlinePick {
    int n;
    MacroInlinePick(InlineEnum) : n(73) {}
    MacroInlinePick(int) : n(74) {}
};
struct MacroNamedPick {
    int n;
    MacroNamedPick(T98NamedEnum) : n(75) {}
    MacroNamedPick(int) : n(76) {}
};
struct MacroBoolPick {
    int n;
    MacroBoolPick(bool) : n(77) {}
    MacroBoolPick(int) : n(78) {}
};
struct MacroAnonBox {
    int n;
    MacroAnonBox(std::initializer_list<T98AnonEnum> values) : n((int)*values.begin()) {}
};
struct MacroInlineBox {
    int n;
    MacroInlineBox(std::initializer_list<InlineEnum> values) : n((int)*values.begin()) {}
};
inline int macroTypePick(T98AnonEnum) { return 81; }
inline int macroTypePick(InlineEnum) { return 82; }
inline int macroTypePick(T98NamedEnum) { return 83; }
inline int macroTypePick(bool) { return 84; }
inline int macroTypePick(int) { return 85; }
template <class T> inline int macroTypeTemplate(T) {
    return __is_same(T, T98NamedEnum) ? 1 : (__is_same(T, int) ? 2 : 3);
}
// An unnamed-enum enumerator is a prvalue of the enum: T&& deduces T98AnonEnum (22).
template <class T> inline int macroAnonFwd(T&&) {
    return __is_same(T, T98AnonEnum&) ? 21 : (__is_same(T, T98AnonEnum) ? 22 : 29);
}
// A namespaced unnamed enum: its enumerator keeps the enum type in a brace list (clang 31).
enum { NsAnonValue = 23 };
using NsAnonEnum = decltype(NsAnonValue);
struct NsAnonPick {
    int n;
    NsAnonPick(NsAnonEnum) : n(31) {}
    NsAnonPick(int) : n(30) {}
};
inline int macroTypePick2(T98NamedEnum, int) { return 12; }
inline int macroTypePick2(int, int) { return 13; }
inline int macroCommaId(int x) { return x; }
struct Ulongs {
    unsigned long long n;
    Ulongs(std::initializer_list<unsigned long long> l) : n(*l.begin()) {}
};
}

#define T98_INLINE_MACRO cpi_t98::InlineValue
#define T98_DEREF (*cpi_t98::derefPointer)
