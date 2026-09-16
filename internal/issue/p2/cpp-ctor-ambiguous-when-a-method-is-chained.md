# A C++ constructor call becomes ambiguous when a method is chained onto its result

## Summary

`c10.TensorOptions(c10.ScalarType.Float)` compiles on its own, but the SAME constructor call
with a fluent method chained onto the temporary is rejected:

```
probe3.cb(6,26): C++ class 'c10.TensorOptions' matches more than one constructor overload
        c10.TensorOptions g = c10.TensorOptions(c10.ScalarType.Float).requires_grad(true);
```

The error points at the CONSTRUCTOR, not at `requires_grad`. So the set of constructor
candidates the resolver considers depends on what else the expression requests from the class -
requesting `TensorOptions::requires_grad(bool)` materialises additional constructor overloads
(most likely `TensorOptions(caffe2::TypeMeta)` or the variadic `template<typename... Args>
TensorOptions(Args&&...)`), and the resolver then reports a tie instead of preferring the exact
match `TensorOptions(ScalarType)`. Found 2026-09-16 dogfooding libtorch
(scratch/dogfood/torch/linreg.cb); this is the spelling every libtorch tutorial uses for a
requires-grad leaf, so it is the first line a real user writes.

## Repro

Two one-screen programs, identical except for the chain (both under scratch/dogfood/torch/):

probe2.cb - COMPILES AND RUNS:
```c
import cpp "torch/torch.h";
extern int printf(const char* fmt, ...);
extern int main()
{
    c10.TensorOptions f = c10.TensorOptions(c10.ScalarType.Float);
    c10.TensorOptions l = c10.TensorOptions(c10.ScalarType.Long);
    printf("hd=%d %d\n", (int)f.has_device(), (int)l.has_device());
    return 0;
}
```

probe3.cb - FAILS:
```c
    c10.TensorOptions g = c10.TensorOptions(c10.ScalarType.Float).requires_grad(true);
```

Workaround: split the chain and mark the tensor afterwards instead -
`at.Tensor w = torch.zeros(sizes); w.requires_grad_(true);`. (Assigning the chain result to a
named local in two statements was not tried; the practical workaround was to avoid
TensorOptions entirely.)

## Root cause

Not investigated, but the probe pair pins it down: the candidate set for a C++ constructor is
request-set dependent, and the constructor overload ranking does not prefer an exact parameter
match over a match reached through a user-defined conversion. `ScalarType` -> `TypeMeta` is
exactly such a conversion, and `TensorOptions(ScalarType)` should win outright.

Related but distinct: internal/issue/p3/cpp-implicit-ctor-conversion-at-call.md (a needed
implicit conversion is NOT offered). This one is the opposite failure - a conversion IS offered
where it should have been outranked - and the two should be fixed together, since both come
from the same missing conversion-ranking step.

## Fix direction

Give the C++ constructor overload resolver clang's ranking: exact / qualification / promotion /
standard conversion beat a user-defined conversion, and only a genuine tie at the same rank is
an ambiguity error. Make the candidate set independent of which members the expression
happens to request, or at least make the ranking insensitive to it.

Regression coverage: an in-repo fixture class with both `Foo(Enum)` and `Foo(Wrapper)` where
`Wrapper` has a non-explicit `Wrapper(Enum)`, plus a fluent `Foo& flag(bool)`; assert that
`Foo(E::a)` and `Foo(E::a).flag(true)` both resolve to the same constructor.
