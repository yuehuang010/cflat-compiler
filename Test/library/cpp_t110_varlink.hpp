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
