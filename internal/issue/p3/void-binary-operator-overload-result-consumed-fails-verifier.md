# Consuming the result of a `void` BINARY operator overload still dies in the module verifier

Filed 2026-08-09 while fixing [[direct-void-call-result-consumed-fails-verifier]]. That fix
funnels every direct CALL result (free function, method, interface vtable slot, generic
instantiation, namespace-qualified, aliased return type) and the UNARY operator overload through
one gate. The BINARY operator overload spelling is the one sibling left open.

Severity: **P3**. Declaring a binary operator overload `void` is a deliberate oddity - the
operator exists to produce a value - and the shape is not something a C programmer slips into
the way `int r = f();` is. Recorded because it is the last un-gated door on the same root cause.

## Repro

Measured Release, macOS arm64, warm `--init-local`, on the fixed binary
(`scratch/dv_n02_binop_void.cb`, `scratch/dv_n06_binop_stmt.cb`).

```cflat
struct V {
    int n = default;
    void operator+(V o) { }
};
extern int main() { V a; V b; int r = a + b; return r; }
```

```
Module verification failed:
Invalid bitcast
  %5 = bitcast void <badref> to i32
```

rc 1, NO location. The DISCARD spelling `a + b;` as a bare statement runs correctly (rc 0), so
the construct is genuinely usable and the gate must not simply reject the operator outright.

## Root cause

Identical to the direct-call defect: `TryBinaryOperatorOverload` hands back the void-typed
`CallInst` and the arithmetic machinery stores it. The reason the new gate does not reach it is
that `ResultUse` does not flow into the binary-expression parser at all - `ParseAdditiveExpression`
and its neighbours take no position argument, so there is no way to tell `a + b;` (a legal
discard, which works today) from `int r = a + b;` (the defect). Gating on the value alone would
falsely reject the statement form, which is the accept-set regression this repo is told to avoid.

## Fix direction

Thread `ResultUse` down the binary-expression chain (additive/multiplicative/shift/relational/
equality/bitwise/logical) the way it is threaded through assignment -> cast -> unary -> postfix,
forwarding it only on the single-child passthrough, then call
`MainListener::DiagnoseVoidResultConsumed` on the overload result exactly as
`ParseUnaryExpression` does. That is a mechanical but wide signature change, which is why it was
scoped out rather than attempted alongside the call gate.
