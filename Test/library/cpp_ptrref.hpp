#pragma once
#include <cstring>

// `const char *const &` beside `T (&)[N]`, a by-value pointer and a template, on the free,
// member and constructor paths (PTRREF). Each result is 100 * overload + strlen when it reads p.
namespace cpp_ptrref {
inline int len(const char* const& p) { return p ? (int)std::strlen(p) : 9; }
struct Sv { const char* d; Sv(const char* s) : d(s) {} };
inline int f3(const char* const& p) { return 100 + len(p); }
inline int f3(const char (&)[4]) { return 200; }
inline int f4(const char* const& p) { return 100 + len(p); }
inline int f4(const char* p) { return 200; }
inline int f5(const char* const& p) { return 100 + len(p); }
template<class T> int f5(const T&) { return 200; }
inline int f6(const char (&)[4]) { return 100; }
inline int f6(const char*) { return 200; }
inline int f7(char (&)[4]) { return 100; }
inline int f7(const char* const& p) { return 200 + len(p); }
inline int f8(const char* const& p) { return 100 + len(p); }
inline int f8(Sv) { return 200; }
template<class T> int f8(const T&) { return 300; }
inline int f9(const char* const& p, int) { return 100 + len(p); }
inline int f9(const char (&)[4], long) { return 200; }
inline int fb(const char* const& p) { return 100 + len(p); }
inline int fb(const char (&)[4]) { return 200; }
inline int fb(Sv) { return 300; }
struct S {
    int m3(const char* const& p) { return 100 + len(p); }
    int m3(const char (&)[4]) { return 200; }
    int m4(const char* const& p) { return 100 + len(p); }
    int m4(const char* p) { return 200; }
    int m6(const char (&)[4]) { return 100; }
    int m6(const char*) { return 200; }
    int m7(char (&)[4]) { return 100; }
    int m7(const char* const& p) { return 200 + len(p); }
    int m9(const char* const& p, int) { return 100 + len(p); }
    int m9(const char (&)[4], long) { return 200; }
    int ma(const char (&)[4]) { return 100; }
    int ma(Sv) { return 200; }
    int mb(const char* const& p) { return 100 + len(p); }
    int mb(const char (&)[4]) { return 200; }
    int mb(Sv) { return 300; }
};
struct C3 { int r; C3(const char* const& p) : r(100 + len(p)) {} C3(const char (&)[4]) : r(200) {} };
struct C4 { int r; C4(const char* const& p) : r(100 + len(p)) {} C4(const char*) : r(200) {} };
struct C6 { int r; C6(const char (&)[4]) : r(100) {} C6(const char*) : r(200) {} };
struct C7 { int r; C7(char (&)[4]) : r(100) {} C7(const char* const& p) : r(200 + len(p)) {} };
struct C9 { int r; C9(const char* const& p, int) : r(100 + len(p)) {} C9(const char (&)[4], long) : r(200) {} };
struct Ca { int r; Ca(const char (&)[4]) : r(100) {} Ca(Sv) : r(200) {} };
struct Cb { int r; Cb(const char* const& p) : r(100 + len(p)) {} Cb(const char (&)[4]) : r(200) {} Cb(Sv) : r(300) {} };
inline int cv_array_pick(int (&)[3]) { return 1; }
inline int cv_array_pick(const int (&)[3]) { return 2; }
struct CvArrayMember {
    int pick(int (&)[3]) { return 1; }
    int pick(const int (&)[3]) { return 2; }
};
struct CvArrayCtor {
    int r;
    CvArrayCtor(int (&)[3]) : r(1) {}
    CvArrayCtor(const int (&)[3]) : r(2) {}
};
// A literal at a template `const char *&&` that keeps its address: the caller's slot, as clang.
__attribute__((noinline)) inline int scribble() { volatile char b[2048]; for (int i = 0; i < 2048; ++i) b[i] = 0x5a; return b[0]; }
__attribute__((noinline)) inline int consume(const char* const* p, int) { return (int)std::strlen(*p); }
template<class U> const char* const* rr(const char*&& p, const U&) { return &p; }
// A constructor keeping the address of a literal bound at `const char *const &` beside a template.
struct Keep { const char* const* q; Keep(const char* const& p) : q(&p) {}
              template<class T> Keep(const T&) { static const char* z = "compete"; q = &z; } };
}
