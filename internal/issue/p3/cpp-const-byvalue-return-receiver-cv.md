# Const C++ by-value return loses receiver cv

## Summary
A const-qualified C++ class returned by value loses its top-level const when used as a member-call receiver. Overload resolution then chooses the non-const method, unlike clang.

## Repro
```cpp
struct R { int f() { return 1; } int f() const { return 2; } };
const R make();
int main() { return make().f(); } // clang 2; master and T122 return 1
```

## Fix direction
Preserve the top-level const qualification of a C++ by-value call result in receiver-cv tracking before overload ranking.
