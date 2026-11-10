#pragma once
#include <cstddef>
// CVAMB: a by-value class parameter reached only through a class conversion operator ranks with a
// `const T&` one (clang: tie, refused in Test/errors/err_cpp_constref_byvalue_ambiguous.cb). The
// counters tally CvcW constructions and destructions for the leg's clang oracle.
inline int cvcBornCount = 0;
inline int cvcDiedCount = 0;
inline int cvc_born() { return cvcBornCount; }
inline int cvc_died() { return cvcDiedCount; }
inline void cvc_reset() { cvcBornCount = 0; cvcDiedCount = 0; }
struct CvcW
{
    int v;
    CvcW(int n) : v(n) { ++cvcBornCount; }
    CvcW(const CvcW& o) : v(o.v) { ++cvcBornCount; }
    ~CvcW() { ++cvcDiedCount; }
};
struct CvcPU { int v; operator CvcW() const { return CvcW(v); } };
struct CvcNC { int v; operator CvcW() { return CvcW(v); } };
struct CvcTwo { int v; operator CvcW() const { return CvcW(v); } operator int() const { return v; } };
inline int cvc_one(CvcW w) { return w.v + 3; }
inline int cvc_two(CvcW) { return 4; }
inline int cvc_two(int) { return 5; }
inline int cvc_ref(const CvcW&) { return 6; }
inline int cvc_ref(int) { return 7; }
inline int cvc_nc_one(CvcW) { return 8; }
inline int cvc_tie(const CvcW&) { return 1; }
inline int cvc_tie(CvcW) { return 2; }
inline int cvc_nc(CvcW) { return 9; }
inline int cvc_nc(const CvcW&) { return 10; }
inline int cvc_mix(CvcW) { return 11; }
inline int cvc_mix(int) { return 12; }
// CVAMB r1: a by-value class parameter via a conversion operator beside a second, const& overload (accept leg).
inline int cvc_multi(CvcW, int) { return 1; }
inline int cvc_multi(const CvcW&, long) { return 2; }
// CVAMB r2: several candidates beat the CFlat winner; the best of them is the pick, a tie among them is ambiguous.
inline int cvc_pick(const CvcW&, long) { return 2; }
inline int cvc_pick(CvcW, int) { return 1; }
inline int cvc_pick(CvcW, short) { return 3; }
inline int cvc_ptie(const CvcW&, long) { return 2; }
inline int cvc_ptie(CvcW, int) { return 1; }
inline int cvc_ptie(CvcW, int, int = 0) { return 3; }
