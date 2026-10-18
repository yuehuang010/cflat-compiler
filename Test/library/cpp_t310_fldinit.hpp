#pragma once
// T310 FLDINIT: C++ class types used as CFlat struct field defaults and in constructor ties.
// clang: `IntC c = 5` runs IntC(int); `KP k = 0` takes KP(int*) (was_null 1); a deleted char
// constructor and a private char constructor tie with the long one for an int argument.
namespace t310 {
struct IntC { int v; IntC(int x) : v(x) {} };
struct KP { int was_null; KP(int* p) : was_null(p == nullptr ? 1 : 0) {} };
struct CT { int v; inline static int live = 0; CT(int x) : v(x) { live++; } ~CT() { live--; } };
inline int liveCount() { return CT::live; }
struct DCh { int v; DCh(long x) : v(1) {} DCh(char) = delete; };
struct PRv { int v; PRv(long x) : v(1) {} private: PRv(char) : v(2) {} };
inline int takePRv(PRv value) { return value.v; }
struct HideOk { int v; HideOk(int) : v(33) {} HideOk(long) : v(1) {} private: HideOk(char) : v(2) {} };
inline int takeHideOk(HideOk value) { return value.v; }
struct PTc { int v; PTc(long x) : v(1) {} protected: PTc(char) : v(2) {} };
}
