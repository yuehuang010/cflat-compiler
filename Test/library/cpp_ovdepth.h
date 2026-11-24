// OVDEPTH: an unnamed `&object` / ?: / ?? join argument of depth 1 never binds a `T**`
// parameter, so overload ranking picks the `T*` (or base `B*`) candidate like clang.
#pragma once
struct OvA { int a = 1; };
struct OvB { int b = 2; };
struct OvD : OvA, OvB { int d = 3; };
inline int ovd_ov(OvD** p) { return 1; }
inline int ovd_ov(OvB* p) { return p->b; }
inline int ovd_ovA(OvD** p) { return 1; }
inline int ovd_ovA(OvA* p) { return p->a + 10; }
inline int ovd_ovD(OvD** p) { return 1; }
inline int ovd_ovD(OvD* p) { return p->d; }
inline int ovd_ip(int** p) { return 1; }
inline int ovd_ip(int* p) { return *p; }
struct OvChoice {
    int v;
    OvChoice(OvD**) : v(1) {}
    OvChoice(OvB* p) : v(p->b) {}
};
// OVDEPTH2: imported function pointers whose parameter is a reference to a pointer.
inline int ovd_fl(OvD*& p) { return p->d + 10; }
inline int ovd_fc(OvD* const& p) { return p->d + 20; }
inline int ovd_fr(OvD*&& p) { return p->d + 30; }
inline int ovd_fpc(OvD** const& p) { return (*p)->d + 40; }
inline int ovd_fpr(OvD**&& p) { return (*p)->d + 50; }
using OvdFpL = int (*)(OvD*&);
using OvdFpC = int (*)(OvD* const&);
using OvdFpR = int (*)(OvD*&&);
using OvdFpPC = int (*)(OvD** const&);
using OvdFpPR = int (*)(OvD**&&);
inline OvdFpL ovd_get_fl() { return ovd_fl; }
inline OvdFpC ovd_get_fc() { return ovd_fc; }
inline OvdFpR ovd_get_fr() { return ovd_fr; }
inline OvdFpPC ovd_get_fpc() { return ovd_fpc; }
inline OvdFpPR ovd_get_fpr() { return ovd_fpr; }
// OVDEPTH2 r2: null-tolerant twins (a nullptr argument binds a null temporary) and `T**&`.
inline int ovd_fc0(OvD* const& p) { return p ? p->d + 20 : 91; }
inline int ovd_fr0(OvD*&& p) { return p ? p->d + 30 : 92; }
inline int ovd_fpc0(OvD** const& p) { return p ? (*p)->d + 40 : 93; }
inline int ovd_fpr0(OvD**&& p) { return p ? (*p)->d + 50 : 94; }
inline int ovd_fpl(OvD**& p) { return (*p)->d + 60; }
using OvdFpPL = int (*)(OvD**&);
inline OvdFpC ovd_get_fc0() { return ovd_fc0; }
inline OvdFpR ovd_get_fr0() { return ovd_fr0; }
inline OvdFpPC ovd_get_fpc0() { return ovd_fpc0; }
inline OvdFpPR ovd_get_fpr0() { return ovd_fpr0; }
inline OvdFpPL ovd_get_fpl() { return ovd_fpl; }
// OVDEPTH3: callees that rewrite the caller's slot through a reference to a pointer.
inline OvD* ovd_repl() { static OvD r; r.d = 9; return &r; }
inline OvD** ovd_repl_pp() { static OvD* q = ovd_repl(); return &q; }
inline int ovd_flw(OvD*& p) { int v = p->d; p = ovd_repl(); return v + 70; }
inline int ovd_fpw(OvD**& p) { int v = (*p)->d; p = ovd_repl_pp(); return v + 80; }
using OvdFpW = int (*)(OvD**&);
inline OvdFpW ovd_get_fpw() { return ovd_fpw; }
inline int ovd_kpl(const OvD**& p) { return 5; }
using OvdFpK = int (*)(const OvD**&);
inline OvdFpK ovd_get_kpl() { return ovd_kpl; }
inline int ovd_kpc(const OvD** const& p) { return 6; }
using OvdFpKC = int (*)(const OvD** const&);
inline OvdFpKC ovd_get_kpc() { return ovd_kpc; }
inline int ovd_ql(OvD* const*& p) { return 7; }
using OvdFpQL = int (*)(OvD* const*&);
inline OvdFpQL ovd_get_ql() { return ovd_ql; }
inline int ovd_kqr(const OvD* const*&& p) { p = nullptr; return 8; }
using OvdFpKQR = int (*)(const OvD* const*&&);
inline OvdFpKQR ovd_get_kqr() { return ovd_kqr; }
