# C++ static members and member operators inherited from a base class are not found

Instance methods inherited from a C++ base work (`v.norm()`, `v.sum()` on Eigen). Two other
inherited kinds do not:

1. A static member function reached through the derived class name: `'K' is not a member of
   namespace 'sb.D'.`
2. A member operator declared in the base (CRTP included): `no operator '+' for type 'op.V'`.

Found 2026-09-26 probing Eigen: `Eigen.Matrix3d.Identity()`, `Eigen.Matrix2d.Zero()` (statics of
`DenseBase` / `MatrixBase`) and `a + b` on `Eigen.Vector3d` (operator of `MatrixBase`).

## Repro (standalone, ~1 s)

```cpp
// sb.h
#pragma once
namespace sb { struct B { static int K() { return 7; } }; struct D : B { int x = 0; }; }
```

```cflat
import cpp "sb.h";
extern int main()
{
    return sb.D.K() == 7 ? 0 : 1;   // 'K' is not a member of namespace 'sb.D'.
}
```

```cpp
// op.h
#pragma once
namespace op {
template <class D> struct Base {
    D operator+(const D& o) const { D r; r.v = static_cast<const D*>(this)->v + o.v; return r; }
};
struct V : Base<V> { double v = 0; V() = default; V(double x) : v(x) {} };
struct Own { double v = 0; Own() = default; Own(double x) : v(x) {}
             Own operator+(const Own& o) const { return Own(v + o.v); } };
}
```

```cflat
import cpp "op.h";
extern int main()
{
    op.V a = op.V(1.0);
    op.V b = op.V(2.0);
    op.V c = a + b;                 // no operator '+' for type 'op.V'
    return c.v == 3.0 ? 0 : 1;
}
```

Controls that work: `op.Own` (operator declared on the class itself); a static declared on the
class itself (`sm.Mat3.Zero()`, where `Mat3` is a typedef of a class template specialization);
a static on a plain typedef'd class. The typedef is not the problem - `sm.Mat<double, 3>.Identity()`
fails the same way when `Identity` lives in the CRTP base.

## Fix direction

Static member lookup through a type name and operator lookup (`TryBinaryOperatorOverload()` in
`MainListener.h`) must walk public bases the way instance member lookup already does (find that
walk first and reuse it; for a CRTP base the `this` adjustment is zero, for a non-first base it is
not - use the base-adjusted pointer the instance path uses).

## Notes for the fixing agent

- Regression: add `sb.h` / `op.h` shapes to an existing `Test/library/cpp_interop_*.h` fixture +
  legs in `Test/test_cpp_interop.cb`. No new test files. Include a non-first-base case for the
  operator to cover the `this` adjustment.
- Do NOT add Eigen to the test run (maintainer, 2026-09-26). Local check only:
  `bash scratch/probe3/run.sh` (e02/e05/e08/e09/e10 statics, e04 `+`). e07 `a * 2.0` additionally
  needs `cpp-operator-returning-unrequested-specialization-not-bound.md`, because Eigen's operators
  return expression-template types.
