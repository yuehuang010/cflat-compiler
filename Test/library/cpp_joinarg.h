// JOINARG: a C++ class pointer ?: / ?? join passed as a call argument, and rows / whole
// multi-dimensional arrays of a derived class (JaB sits at a non-zero offset in JaD).
#pragma once
struct JaA { int a = 1; };
struct JaB { int b = 2; };
struct JaD : JaA, JaB { int d = 3; };
struct JaX : JaA { int x = 7; };
inline int ja_take_b(JaB* p) { return p->b; }
inline int ja_take_bpp(JaB** p) { return (*p)->b; }
inline int ja_take_dpp(JaD** p) { return (*p)->d; }
inline int ja_take_void(void* p) { return p != nullptr ? 4 : 0; }
inline int ja_take_x(JaX* p) { return p->x; }
inline int ja_ov2(JaB** p) { return 1; }
inline int ja_ov2(JaD** p) { return 2; }
inline int ja_ov3(JaB* p) { return 1; }
inline int ja_ov3(JaX* p) { return 2; }
inline int ja_ov4(JaA* p) { return 1; }
inline int ja_ov4(JaD* p) { return 2; }
inline int ja_ov6(JaD** p) { return 1; }
inline int ja_ov6(JaB** p) { return 2; }
// JOINARG2: C++ reference arms joined with pointer arms in a call argument.
inline JaD ja_global_d;
inline JaD& ja_ref_d() { return ja_global_d; }
inline JaB& ja_ref_b() { return ja_global_d; }
inline int ja_ov_ab(JaA* p) { return 1; }
inline int ja_ov_ab(JaB* p) { return 2; }
