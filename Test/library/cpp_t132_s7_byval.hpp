#pragma once
#include "cpp_t87_null_zero.h"

namespace t132 {
inline int takeS7(S7 value) { return value.v; }
struct SC { int v; SC(char) : v(11) {} SC(int*) : v(22) {} };
}
namespace t140 {
struct SCL { int v; SCL(char) : v(11) {} SCL(long) : v(33) {} };
// Mac-visible mirror of SCL: a long argument ties long long (both 64 bits on LP64, as int ties long on Windows).
struct SLL { int v; SLL(char) : v(11) {} SLL(long long) : v(33) {} };
struct EX { int v; explicit EX(long) : v(11) {} EX(void*) : v(22) {} };
struct Recv {
    int takeSC(t132::SC value) { return value.v; }
    int takeSCL(SCL value) { return value.v; }
    int takeEX(EX value) { return value.v; }
};
inline int takeK(t132::K value) { return value.was_null; }
// T180 CTORTIE: same-width distinct C++ types beside a char constructor tie in clang (ambiguous).
struct SUS { int v; SUS(char) : v(11) {} SUS(unsigned short) : v(33) {} };
struct SUI { int v; SUI(char) : v(11) {} SUI(unsigned int) : v(33) {} };
struct SUH { int v; SUH(char) : v(11) {} SUH(short) : v(33) {} };
enum EL : long { elv = 5 };
struct SEL { int v; SEL(char) : v(11) {} SEL(long long) : v(33) {} };
// Accept controls (clang: 33): an alias or typedef of the same type, an exact short or int, a
// single unsigned short constructor, and an unscoped enum promoting to its own long.
using USAlias = unsigned short;
typedef unsigned short USTypedef;
struct SUA { int v; SUA(char) : v(11) {} SUA(USAlias) : v(33) {} };
struct SUT { int v; SUT(char) : v(11) {} SUT(USTypedef) : v(33) {} };
struct SSH { int v; SSH(char) : v(11) {} SSH(short) : v(33) {} };
struct SUO { int v; SUO(unsigned short) : v(33) {} };
struct SIN { int v; SIN(char) : v(11) {} SIN(int) : v(33) {} };
struct SEN { int v; SEN(char) : v(11) {} SEN(long) : v(33) {} };
}
