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

// T144: an sret return with scalar varargs passes the tail as clang does; a by-value aggregate in
// the `...` tail is refused at the call (err_cpp_variadic_aggregate.cb).
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
