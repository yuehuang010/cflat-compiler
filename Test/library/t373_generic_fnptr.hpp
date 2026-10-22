#pragma once
namespace t373 {
struct W { int value; W(int x) : value(x) {} };
inline int freeAmb(int (*f)(int)) { return f(1) + 10; }
inline int freeAmb(long (*f)(long)) { return (int)f(1) + 20; }
inline int freeFirst(int (*f)(int)) { return f(1) + 10; }
inline int freeFirst(double (*f)(double, int)) { return 99; }
inline int freeSecond(double (*f)(double, int)) { return 99; }
inline int freeSecond(int (*f)(int)) { return f(1) + 10; }
inline int freeNone(int (*f)(int, int)) { return 1; }
inline int freeNone(double (*f)(double, double)) { return 2; }
inline int prevAmb(long a, int (*f)(int)) { return f(1) + 10; }
inline int prevAmb(long a, long (*f)(long)) { return (int)f(1) + 20; }
inline int defAmb(int (*f)(int), int k = 0) { return f(1) + 10 + k; }
inline int defAmb(long (*f)(long), int k = 0) { return (int)f(1) + 20 + k; }
inline int defSole(double (*f)(double, int)) { return 99; }
inline int defSole(int (*f)(int), int k = 0) { return f(1) + 10 + k; }
struct M {
  int both(int (*f)(int)) { return f(1) + 10; }
  int both(long (*f)(long)) { return (int)f(1) + 20; }
  int first(int (*f)(int)) { return f(1) + 10; }
  int first(double (*f)(double, int)) { return 99; }
  int second(double (*f)(double, int)) { return 99; }
  int second(int (*f)(int)) { return f(1) + 10; }
  int none(int (*f)(int, int)) { return 1; }
  int none(double (*f)(double, double)) { return 2; }
  int defBoth(int (*f)(int), int k = 0) { return f(1) + 10 + k; }
  int defBoth(long (*f)(long), int k = 0) { return (int)f(1) + 20 + k; }
};
}
