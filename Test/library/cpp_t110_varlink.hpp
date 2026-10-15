#pragma once
#include <cstdarg>

inline int t110_global(void* expected, int fixed, ...) {
  va_list ap; va_start(ap, fixed);
  int a = va_arg(ap, int), b = va_arg(ap, int);
  double c = va_arg(ap, double); void* p = va_arg(ap, void*);
  va_end(ap); return fixed + a + b + (int)c + (p == expected ? 10000 : 0);
}

namespace t110 {
inline double namespaced(double fixed, ...) {
  va_list ap; va_start(ap, fixed);
  double a = va_arg(ap, double); int b = va_arg(ap, int);
  va_end(ap); return fixed + a + b;
}
struct Worker {
  inline int member(int fixed, ...) {
    va_list ap; va_start(ap, fixed);
    int a = va_arg(ap, int); int b = va_arg(ap, int);
    va_end(ap); return fixed + a + b;
  }
  inline static double stat(double fixed, ...) {
    va_list ap; va_start(ap, fixed);
    double a = va_arg(ap, double); int b = va_arg(ap, int);
    va_end(ap); return fixed + a + b;
  }
};
// ov(int) and ov(int, ...) are distinct overloads: ov(3, 4) picks the variadic one, ov(3) is ambiguous.
struct Overloaded {
  inline int ov(int a) { return 1000 + a; }
  inline int ov(int a, ...) {
    va_list ap; va_start(ap, a); int x = va_arg(ap, int); va_end(ap); return 2000 + a + x;
  }
};
}

// T144: an sret return with scalar varargs passes the tail as clang does.
struct T144Pod8 { int a; int b; };
struct T144Pod16 { long long a; long long b; };
struct T144Big { long long a, b, c, d; };
namespace t144v {
inline T144Big inl_sret0(int n, ...) { return T144Big{n, 0, 0, 4}; }
T144Big out_sret0(int n, ...) { return T144Big{n, 0, 0, 4}; }
inline T144Big inl_sret(int n, ...) {
  va_list ap; va_start(ap, n); long long x = va_arg(ap, long long); int y = va_arg(ap, int);
  double z = va_arg(ap, double); va_end(ap); return T144Big{n, x, y, (long long)(z * 2)};
}
T144Big out_sret(int n, ...) {
  va_list ap; va_start(ap, n); long long x = va_arg(ap, long long); int y = va_arg(ap, int);
  double z = va_arg(ap, double); va_end(ap); return T144Big{n, x, y, (long long)(z * 2)};
}
inline int take8(int n, ...) {
  va_list ap; va_start(ap, n); T144Pod8 p = va_arg(ap, T144Pod8); va_end(ap); return n + p.a * 10 + p.b;
}
inline int take16(int n, ...) {
  va_list ap; va_start(ap, n); T144Pod16 p = va_arg(ap, T144Pod16); va_end(ap);
  return n + (int)(p.a * 10 + p.b);
}
inline int takeBig(int n, ...) {
  va_list ap; va_start(ap, n); T144Big p = va_arg(ap, T144Big); va_end(ap);
  return n + (int)(p.a * 1000 + p.b * 100 + p.c * 10 + p.d);
}
struct Ret {
  int k = 7;
  inline T144Big m0(int n, ...) { return T144Big{n, 0, k, 4}; }
  inline T144Big m3(int n, ...) {
    va_list ap; va_start(ap, n); long long x = va_arg(ap, long long); int y = va_arg(ap, int);
    double z = va_arg(ap, double); va_end(ap); return T144Big{n, x, y + k, (long long)(z * 2)};
  }
  T144Big plain(int n, long long x) { return T144Big{n, x, k, 4}; }
};
}

// T204: by-value aggregate varargs use Clang's call-site ABI arrangement.
struct T204S1 { unsigned char a; };
struct T204S8 { int a, b; };
struct T204S12 { int a, b, c; };
struct T204S16 { long long a, b; };
struct T204S24 { long long a, b, c; };
struct T204Hfa { float a, b, c, d; };
struct T204Ret { long long a, b, c, d; };
struct T204Nontrivial {
  int a;
  T204Nontrivial() : a(1) {}
  T204Nontrivial(const T204Nontrivial& x) : a(x.a) {}
  ~T204Nontrivial() {}
};
inline int t204_s1(int n, ...) {
  va_list ap; va_start(ap, n); T204S1 x = va_arg(ap, T204S1); va_end(ap); return n + x.a;
}
inline int t204_s8(int n, ...) {
  va_list ap; va_start(ap, n); T204S8 x = va_arg(ap, T204S8); va_end(ap); return n + x.a * 10 + x.b;
}
inline int t204_s12(int n, ...) {
  va_list ap; va_start(ap, n); T204S12 x = va_arg(ap, T204S12); int y = va_arg(ap, int);
  va_end(ap); return n + x.a * 100 + x.b * 10 + x.c + y;
}
inline int t204_s16(int n, ...) {
  va_list ap; va_start(ap, n); T204S16 x = va_arg(ap, T204S16); va_end(ap);
  return n + (int)(x.a * 10 + x.b);
}
inline int t204_s24(int n, ...) {
  va_list ap; va_start(ap, n); T204S24 x = va_arg(ap, T204S24); va_end(ap);
  return n + (int)(x.a * 100 + x.b * 10 + x.c);
}
inline int t204_hfa(int n, ...) {
  va_list ap; va_start(ap, n); T204Hfa x = va_arg(ap, T204Hfa); va_end(ap);
  return n + (int)(x.a * 1000 + x.b * 100 + x.c * 10 + x.d);
}
inline T204Ret t204_sret(int n, ...) {
  va_list ap; va_start(ap, n); T204S12 x = va_arg(ap, T204S12); int y = va_arg(ap, int);
  va_end(ap); return T204Ret{n, x.a * 100 + x.b * 10 + x.c, y, 4};
}
struct T204Member {
  int k = 7;
  inline int take(int n, ...) {
    va_list ap; va_start(ap, n); T204S8 x = va_arg(ap, T204S8); va_end(ap);
    return n + k + x.a * 10 + x.b;
  }
};
// T210: an ellipsis-only overload has no fixed parameter before its variadic tail.
inline int t210_none(...) { return 2113; }
inline int t210_none(int n) { return 2114 + n; }
template <class T> struct T210TMember {
  T k;
  inline long long take(int n, ...) {
    va_list ap; va_start(ap, n); T204S8 x = va_arg(ap, T204S8); va_end(ap);
    return n + k + x.a * 10 + x.b;
  }
};
inline int t204_nontrivial(int n, ...) { return n; }

// T161: out-of-line variadic C++ members (declared in the class, defined outside it) bind like
// their in-class siblings: the scalar tail takes the default promotions and an sret return works.
struct T161Big { long long a, b, c, d; };
namespace t161 {
struct Out {
  int k = 7;
  T161Big f_int(int n, ...);
  T161Big f_float(int n, ...);
  T161Big f_char(int n, ...);
  T161Big f_short(int n, ...);
  T161Big f_ptr(int n, ...);
  T161Big zero(int n, ...);
  T161Big mix(int n, ...);
  T161Big cmix(int n, ...) const;
  static T161Big s_mix(int n, ...);
  static T161Big s_agg(int n, ...);
  int ov(int n, long long x);
  int ov(int n, ...);
};
template <class T> struct Tpl {
  T base;
  T161Big m(int n, ...);
};
}
T161Big t161::Out::f_int(int n, ...) { va_list ap; va_start(ap, n); int x = va_arg(ap, int); va_end(ap); return T161Big{n, x, 3, 4}; }
T161Big t161::Out::f_float(int n, ...) { va_list ap; va_start(ap, n); double x = va_arg(ap, double); va_end(ap); return T161Big{n, (long long)(x * 2), 3, 4}; }
T161Big t161::Out::f_char(int n, ...) { va_list ap; va_start(ap, n); int x = va_arg(ap, int); va_end(ap); return T161Big{n, x, 3, 4}; }
T161Big t161::Out::f_short(int n, ...) { va_list ap; va_start(ap, n); int x = va_arg(ap, int); va_end(ap); return T161Big{n, x, 3, 4}; }
T161Big t161::Out::f_ptr(int n, ...) { va_list ap; va_start(ap, n); int* p = va_arg(ap, int*); va_end(ap); return T161Big{n, *p, 3, 4}; }
T161Big t161::Out::zero(int n, ...) { return T161Big{n, 0, 3, 4}; }
T161Big t161::Out::mix(int n, ...) {
  va_list ap; va_start(ap, n); long long x = va_arg(ap, long long); int y = va_arg(ap, int);
  double z = va_arg(ap, double); va_end(ap); return T161Big{n, x, y + k, (long long)(z * 2)};
}
T161Big t161::Out::cmix(int n, ...) const {
  va_list ap; va_start(ap, n); double z = va_arg(ap, double); va_end(ap);
  return T161Big{n, (long long)(z * 2), k, 6};
}
T161Big t161::Out::s_mix(int n, ...) { va_list ap; va_start(ap, n); int x = va_arg(ap, int); va_end(ap); return T161Big{n, x, 5, 6}; }
T161Big t161::Out::s_agg(int n, ...) {
  va_list ap; va_start(ap, n); T161Big x = va_arg(ap, T161Big); va_end(ap);
  return T161Big{n, x.a, x.b, x.c + x.d};
}
int t161::Out::ov(int n, long long x) { return (int)(n * 100 + x); }
int t161::Out::ov(int n, ...) { va_list ap; va_start(ap, n); int x = va_arg(ap, int); va_end(ap); return n + x; }
template <class T> T161Big t161::Tpl<T>::m(int n, ...) {
  va_list ap; va_start(ap, n); T x = va_arg(ap, T); va_end(ap); return T161Big{n, (long long)x, (long long)base, 8};
}

// T161 (r3): a virtual out-of-line variadic member called through a base pointer or reference takes
// the same default promotions as clang: float widens to double, char/short/bool widen to int.
namespace t161 {
struct VBase { virtual int vf(int n, ...); };
struct VDerived : VBase { int vf(int n, ...) override; };
inline VBase* vbase() { static VDerived d; return &d; }
}
int t161::VBase::vf(int n, ...) { return 5; }
int t161::VDerived::vf(int n, ...) {
  va_list ap; va_start(ap, n);
  int x = va_arg(ap, int); double f = va_arg(ap, double); int c = va_arg(ap, int);
  int s = va_arg(ap, int); int b = va_arg(ap, int); va_end(ap);
  return n + x * 10 + (int)(f * 10) + c * 100 + s * 1000 + b * 10000;
}
