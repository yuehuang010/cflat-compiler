// T85: C++ calls refuse unrelated primitive pointees (int* into double*, long* into int*,
// float* into double*, int* into const char*). Overload sets fall to the void* or template
// candidate, as clang does.
namespace t85p {
inline int one(double*) { return 1; }
inline int ov(double*) { return 2; }
inline int ov(void*) { return 3; }
inline int tmp(double*) { return 2; }
template <class T> int tmp(T*) { return 4; }
inline int cc1(const char*) { return 5; }
inline int cc(const char*) { return 6; }
inline int cc(void*) { return 7; }
inline int cw(const int*) { return 8; }
inline int lw(int*) { return 9; }
inline int ff(double*) { return 10; }
inline int oi(int*) { return 11; }
inline int oi(double*) { return 12; }
inline int lx(long* p) { return 13; }
struct M { int take(double*) { return 14; } };
struct M2 { int take(double*) { return 15; } int take(void*) { return 16; } };
struct M3 { static int stake(double*) { return 17; } };
// Round 2: binary operators (member and free) and pointer-reference parameters.
struct MOp { int operator+(double*) { return 25; } };
struct MOpI { int operator+(int* p) { return *p; } };
struct FOp {};
struct FOpI {};
inline int operator+(FOp, double*) { return 26; }
inline int operator+(FOpI, int* p) { return *p; }
inline int rf(double* const&) { return 27; }
inline int rk(int* const& p) { return *p; }
struct C1 { int k; C1(double*) : k(18) {} };
struct C2 { int k; C2(double*) : k(19) {} C2(void*) : k(20) {} };
struct O1 { int operator()(double*) { return 21; } };
struct O2 { int operator()(double*) { return 22; } int operator()(void*) { return 23; } };
// Round 3: rvalue pointer references, reference-returning sources, compound operands, and the
// operator template that ranks once an unrelated-pointee member or free operator drops out.
inline int rr(double*&&) { return 30; }
inline int rri(int*&& p) { return *p; }
inline int*& src(int*& p) { return p; }
inline int**& srcpp(int**& p) { return p; }
inline int takeD(double*) { return 31; }
inline int takeI(int* p) { return *p; }
inline int takeDD(double**) { return 32; }
inline int takeII(int** p) { return **p; }
struct Acc { int k = 0; Acc& operator+=(double*) { k = 33; return *this; } };
struct AccI { int k = 0; AccI& operator+=(int* p) { k = *p; return *this; } };
struct RT { int operator+(double*) { return 34; } template <class T> int operator+(T*) { return 35; } };
struct RTV {
    int operator+(double*) { return 36; }
    int operator+(void*) { return 37; }
    template <class T> int operator+(T*) { return 38; }
};
struct AT {
    int k = 0;
    AT& operator+=(double*) { k = 39; return *this; }
    template <class T> AT& operator+=(T*) { k = 40; return *this; }
};
struct FT {};
inline int operator+(FT, double*) { return 41; }
template <class T> int operator+(FT, T*) { return 42; }
// Round 4: after a pointee drop clang ranks the rest; its ambiguity refusal is relayed (own
// namespace, so these templates never join the other t85p operator sets).
namespace amb {
struct A {
    int operator+(double*) { return 43; }
    int operator+(void*) { return 44; }
    template <class T> int operator+(T*) { return 45; }
};
template <class T> int operator+(A, T*) { return 46; }
struct F {};
inline int operator+(F, double*) { return 47; }
template <class T> int operator+(F, T*) { return 48; }
template <class T> int operator+(T, int*) { return 49; }
}
// Round 5: after the drop clang selects the template, whose BODY fails; that refusal is relayed
// even when its text reads like "invalid operands" (only one at the infix itself is exempt).
namespace body {
struct A {
    int operator+(double*) { return 50; }
    int operator+(void*) { return 51; }
    template <class T> int operator+(T* p) { return p + p; }
};
struct B {};
inline int operator+(B, double*) { return 52; }
inline int operator+(B, void*) { return 53; }
template <class T> int operator+(B, T*) { static_assert(sizeof(T) == 0, "invalid operands"); return 54; }
}
}
