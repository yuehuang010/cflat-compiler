# A C++ member OPERATOR returning a class-template specialization is never bound

A C++ member operator whose return type is a class-template specialization that has not been
requested yet (`Expr<W> operator*(double) const`) is not found:
`no operator '*' for type 'op3.W'`. The identical signature as a NAMED METHOD works - the lazy
refused-member retry (0911b337) requests the specialization and binds the method on use, even when
the program never spells the result type. Operator lookup never triggers that retry, and naming the
type does not help either (`op3.Expr<op3.W> e = a * 3.0;` fails the same way).

At import, `-v` shows the member refused (`C++ member op.W.operator* not bound: returns unsupported
type 'op::Expr<op::W>'`); the method `mul` is refused the same way at import but recovers on use.

Found 2026-09-26 probing Eigen: every arithmetic operator returns an expression template
(`CwiseBinaryOp<...>`, `Product<...>`). Eigen itself is no longer blocked by this: its operators
live on the `MatrixBase` CRTP base, and the inherited-member retry binds `(a + b).sum()`,
`(m * v).sum()` and `(a * 2.0).sum()`. A refused operator declared on the class itself (`op3.W`
below) still fails.

## Repro (standalone, ~1 s)

```cpp
// op3.h
#pragma once
namespace op3 {
template <class L> struct Expr { const L& l; double k; double eval() const { return l.v * k; } };
struct W {
    double v = 0; W() = default; W(double x) : v(x) {}
    template <class L> W(const Expr<L>& e) : v(e.eval()) {}
    Expr<W> mul(double k) const { return Expr<W>{*this, k}; }
    Expr<W> operator*(double k) const { return Expr<W>{*this, k}; }
    W operator+(double k) const { return W(v + k); }
};
}
```

```cflat
import cpp "op3.h";
extern int main()
{
    op3.W a = op3.W(2.0);
    double r1 = a.mul(3.0).eval();   // OK: method, result type never named - binds on use
    op3.W c = a + 1.0;               // OK: operator returning a non-template class
    double r2 = (a * 3.0).eval();    // FAILS: no operator '*' for type 'op3.W'
    return (r1 == 6.0 && r2 == 6.0 && c.v == 3.0) ? 0 : 1;
}
```

## Fix direction

Run the same on-use request + retry the named-method path uses when operator resolution
(`TryBinaryOperatorOverload()` in `MainListener.h`, and the unary / subscript / call operator
paths) finds a C++ operator candidate that was refused only because of an unrequested return-type
specialization. Find where the method path does it first (grep the refused-member retry introduced
by 0911b337) and share it, rather than adding a second mechanism.

## Notes for the fixing agent

- Regression: add `op3.h` to an existing `Test/library/cpp_interop_*.h` fixture with legs for the
  binary operator and at least one of unary `-`, `operator[]` and `operator()` returning a
  specialization. No new test files.
- Do NOT add Eigen to the test run (maintainer, 2026-09-26). Local check only:
  `bash scratch/probe3/run.sh` (e07 `a * 2.0`; e05 also needs the inherited-static fix).
