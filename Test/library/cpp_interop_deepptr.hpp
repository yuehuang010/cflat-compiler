// Pointer depth 3 fixtures: a C++ signature that spells int*** must bind only at that depth.
#pragma once

namespace dpt {
struct Cell { int a; int b; };
inline int g_value = 5;
inline int* g_p = &g_value;
inline int** g_pp = &g_p;
inline int load3(int*** p) { return ***p; }
inline int load2(int** p) { return **p; }
inline int over(int** p) { return 2; }
inline int over(int*** p) { return 3; }
inline int*** top3() { return &g_pp; }
inline int cl_all(const int* const* const* p) { return ***p; }
inline int cl_inner(int* const** p) { return ***p; }
typedef int* IP;
typedef IP* IPP;
typedef IPP* IPPP;
inline int load_td(IPPP p) { return ***p; }
template <class T> inline T* tptr(T* p) { return p; }
struct Holder { int*** slot; };
inline int load_holder(Holder h) { return ***h.slot; }
inline int load3_ref(int***& r) { return ***r; }
inline int load3_cref(int*** const& r) { return ***r; }
inline int cl_c3(const int*** p) { return ***p; }
inline int*** g_slot = nullptr;
inline int***& ref_slot() { return g_slot; }
inline int***const& cref_slot() { return g_slot; }
using T3 = int***;
typedef int*** TD3;
using T3b = T3;
typedef T3b TD3b;
inline int load_alias(T3 p) { return ***p; }
inline int over2(void* p) { return 1; }
inline int over2(int*** p) { return 3; }
inline int over2d(void* p) { return 1; }
inline int over2d(int** p) { return 2; }
inline int over4(int*** p) { return 3; }
inline int over4(int**** p) { return 4; }
inline int vcv(void*) { return 1; }
inline int vcv(const void*) { return 2; }
inline int vcv_rev(const void*) { return 1; }
inline int vcv_rev(void*) { return 2; }
inline int cl2(void*) { return 1; }
inline int cl2(const int**) { return 2; }
inline int cl3(void*) { return 1; }
inline int cl3(const int***) { return 3; }
inline int qv0 = 5; inline int* qv1 = &qv0; inline int** qv2 = &qv1; inline int*** qv3 = &qv2;
inline const int*** qual3src(int*** p) { return const_cast<const int***>(p); }
inline int qual3(void*) { return 1; }
inline int qual3(const int* const* const*) { return 2; }
inline int ccell2(void*) { return 1; }
inline int ccell2(const Cell**) { return 2; }
inline int mcell2(const void*) { return 1; }
inline int mcell2(Cell**) { return 2; }
inline int cf3(void*) { return 1; }
inline int cf3(const int* const* const*) { return 3; }
inline int vref(void* const&) { return 1; }
inline int vref(int***) { return 3; }
inline int* const* mid2(int** p) { return p; }
inline int** const* mid3(int*** p) { return p; }
inline Cell* const* cmid2(Cell** p) { return p; }
inline int tg(int**) { return 1; }
inline int tg(Cell**) { return 2; }
inline int tg(void*) { return 3; }
inline int vol(void*) { return 1; }
inline int vol(volatile void*) { return 2; }
inline int crefcl(void*) { return 1; }
inline int crefcl(const int** const&) { return 2; }
struct Deep32Field { int x; int ********************************w; int y; };
inline Deep32Field mkdeep32() { Deep32Field d{}; d.y = 3; return d; }
inline int takevp(void* p) { return p == nullptr ? 1 : 2; }
struct Ctor2 { int v = 0; Ctor2(void*) : v(1) {} Ctor2(int**) : v(2) {} };
// [conv.qual]: volatile added below a non-const level is not viable; void* wins.
inline int volv(void*) { return 1; }
inline int volv(volatile int**) { return 2; }
typedef volatile int VolInt;
inline int tvolv(void*) { return 1; }
inline int tvolv(VolInt**) { return 2; }
inline int volok(void*) { return 1; }
inline int volok(int* volatile*) { return 2; }
struct CtorVol { int v = 0; CtorVol(void*) : v(1) {} CtorVol(volatile int**) : v(2) {} };
// A too-deep parameter of a function-pointer field is not the field's own depth.
struct CbDeep { int (*cb)(int ********************************); int y = 3; };
inline CbDeep mkcbdeep() { return CbDeep{}; }
inline int cbImpl(int ********************************) { return 8; }
struct CbLive { int (*cb)(int ********************************) = cbImpl; int y = 3; };
inline CbLive mkcblive() { return CbLive{}; }
inline int runcb(int (*f)(int ********************************)) { return f(nullptr); }
inline int cbpick(void*) { return 1; }
inline int cbpick(int) { return 2; }
// Volatile and restrict keep the legacy path: the pick master makes.
inline volatile int** dropvsrc(int** p) { return const_cast<volatile int**>(p); }
inline int dropv(void*) { return 1; }
inline int dropv(int**) { return 2; }
inline volatile int vsrc0 = 1;
inline volatile int* vsrc1 = &vsrc0;
inline volatile int** vsrc2 = &vsrc1;
inline volatile int*** vsrc3() { return &vsrc2; }
inline int exactv3(const volatile void*) { return 1; }
inline int exactv3(volatile int***) { return 2; }
inline int restr3(void*) { return 1; }
inline int restr3(int* __restrict__* __restrict__*) { return 2; }
// A depth-3 volatile source: an arm naming no qualifier does not match it exactly.
inline int d3v(void*) { return 1; }
inline int d3v(int***) { return 2; }
inline volatile int*** vsrc3g = &vsrc2;
inline int rfv(void*) { return 1; }
inline int rfv(volatile int***&) { return 2; }
inline int crfv(void*) { return 1; }
inline int crfv(volatile int*** const&) { return 2; }
inline int m3v(volatile int***) { return 1; }
inline int m3v(int***) { return 2; }
inline int m3v(void*) { return 3; }
struct VolF { volatile int** f2 = vsrc2; };
inline int qv3Storage = 1;
inline int* qv3a = &qv3Storage;
inline int** qv3b = &qv3a;
struct VolTop { int*** volatile f3 = &qv3b; };
inline VolTop volTop;
inline int tvq3(const volatile void*) { return 1; }
inline int tvq3(int***) { return 2; }
inline int******************************** rbImpl(int) { return nullptr; }
struct RbDeep { int******************************** (*rb)(int) = rbImpl; int y = 3; };
inline int pkv(void*) { return 1; }
inline int pkv(void**) { return 2; }
union DeepUnion { int ********************************w; int u; };
inline DeepUnion mkdeepunion() { DeepUnion d{}; d.u = 4; return d; }
// Depth 32 is the carrier limit: declaring it is fine, using it refuses with a located error.
inline int f32(int******************************** p) { return 1; }
// Depth 3 and 4: nullptr, references, class pointees, templates, function pointers, members.
inline int load4(int**** p) { return ****p; }
inline int g4v = 9; inline int* g4a = &g4v; inline int** g4b = &g4a; inline int*** g4c = &g4b;
inline int**** g4 = &g4c;
inline int**** top4() { return &g4c; }
using T4 = int****;
inline int t4(T4 p) { return ****p; }
inline int null3(int*** p) { return p ? 2 : 1; }
inline int null4(int**** p) { return p ? 2 : 1; }
inline int ref2(int**& r) { return **r; }
inline int setref3(int***& r, int*** v) { r = v; return 0; }
inline int rr3(int***&& p) { return ***p; }
inline int cls3(Cell*** p) { return (**p)->b; }
inline int ccls3(const Cell* const* const* p) { return (**p)->a; }
template <class T> T tf3(T*** p) { return ***p; }
template <class T> int tdepth(T*) { return 1; }
template <class T> int tdepth(T**) { return 2; }
template <class T> int tdepth(T***) { return 3; }
template <class T> int tsz(T***) { return (int)sizeof(T); }
typedef int (*Fp3)(int***);
inline int fpImpl(int*** p) { return ***p + 1; }
inline Fp3 getfp() { return fpImpl; }
inline int runfp(Fp3 f, int*** p) { return f(p); }
inline int fp4(int (*f)(int****), int**** p) { return f(p); }
inline int cvsel(int***) { return 1; }
inline int cvsel(const int***) { return 2; }
inline int over23(int**) { return 2; }
inline int over23(int***) { return 3; }
inline int pc3(int* const** p) { return ***p; }
inline int dflt3(int*** p = nullptr) { return p ? 2 : 1; }
inline int fw3(int*** a, int**** b) { return ***a + ****b; }
template <class... A> int fwd3(A... a) { return fw3(a...); }
inline int arr4(int**** a, int n) { int s = 0; for (int i = 0; i < n; i++) s += ***a[i]; return s; }
struct M3 {
    int*** m3 = nullptr;
    int v1 = 4;
    int* ptr1() { return &v1; }
    static inline int* s1 = nullptr;
    int over(int**) const { return 2; }
    int over(int***) const { return 3; }
    int take3(int*** p) const { return ***p + 100; }
};
template <class T> struct B3 { int m(T*** p) { return ***p + 1; } };
// One '*' is a recorded depth too: results, globals and static members refuse a deeper destination.
inline int* r1(int* p) { return p; }
inline void* vr1(void* p) { return p; }
template <class T> T* tr1(T* p) { return p; }
inline int take2(int** p) { return **p + 2; }
inline int ov1(int*) { return 1; }
inline int ov1(int**) { return 2; }
inline int ov1(void*) { return 3; }
struct Base1 { int b = 4; };
struct Der1 : Base1 { int d = 6; };
inline Der1 gder1;
inline Der1* mkder1() { return &gder1; }
}
