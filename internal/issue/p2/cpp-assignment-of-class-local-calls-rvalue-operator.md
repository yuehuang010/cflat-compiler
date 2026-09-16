# Assigning one C++ class local to another performs an in-place copy instead of rebinding

## Summary

`a = b;` where `a` and `b` are locals of an imported C++ class calls the RVALUE-ref-qualified
`operator=` instead of the lvalue one. For `at::Tensor` that is
`Tensor& operator=(const Tensor&) &&`, which does `this->copy_(rhs)` - it writes THROUGH the
handle into the shared storage instead of rebinding the handle. Every other owner of that
storage silently sees the new data, and in autograd the write becomes a `CopyBackwards` node
that then fails the version counter. Found 2026-09-16 dogfooding libtorch
(scratch/dogfood/torch/reassign_probe.cb, scratch/dogfood/torch/rnn2.cb).

This breaks the most ordinary loop in machine learning - carrying RNN hidden state:

```
libc++abi: terminating due to uncaught exception of type c10::Error: one of the variables
needed for gradient computation has been modified by an inplace operation:
[CPUFloatType [192, 16]], which is output 0 of torch::autograd::CopyBackwards, is at version 5;
expected version 4 instead.
```

## Repro

scratch/dogfood/torch/reassign_probe.cb, compiles clean, runs, prints the wrong answer:

```c
import cpp "torch/torch.h";
extern int printf(const char* fmt, ...);
extern int main()
{
    at.Tensor a = torch.zeros({2});
    at.Tensor alias = a;     // shares a's TensorImpl (local-to-local copy ctor: correct)
    at.Tensor b = torch.ones({2});
    a = b;                   // C++: rebind a to b's impl; alias keeps the zeros
    printf("aSum=%f aliasSum=%f\n", a.sum().item().toDouble(), alias.sum().item().toDouble());
    return 0;
}
```

```
aSum=2.000000 aliasSum=2.000000 verdict=in-place copy_ (wrong)     <- observed
aSum=2.000000 aliasSum=0.000000                                    <- what C++ does
```

An explicit `move` does NOT rescue it: scratch/dogfood/torch/reassign_probe2.cb writes
`a = move b;` and prints the same `aliasSum=2.000000`, so there is no spelling that gets C++
rebind semantics today.

The full-program form is scratch/dogfood/torch/rnn2.cb (`h = torch.tanh(...)` inside the time
loop). scratch/dogfood/torch/rnn3.cb works around it by unrolling the loop into distinct
locals `h0..h5`, which is not a workaround a user can apply to a variable-length sequence.

## Root cause (hypothesis)

CFlat has no notion of C++ ref-qualifiers (`&` / `&&` on a member function), so both
`operator=` overloads look identical in the overload set and the wrong one is picked - in
practice the last one declared. `at::Tensor` declares, in order,
`operator=(const TensorBase&) &` (rebind), `operator=(TensorBase&&) &`,
`operator=(const Tensor&) &&` (copy_), `operator=(Scalar) &&` (fill). Picking the trailing
`&&` one matches the observed behaviour exactly, and also explains `CopyBackwards` appearing
in the graph.

Same failure shape must be checked for any ref-qualified member, not just `operator=` -
`std::optional::value() &&`, `std::move_iterator`, any fluent builder with `&&` overloads.

## Fix direction

Record the ref-qualifier on each imported C++ member function in the binder and use it in
overload resolution: an lvalue object argument may only bind `&`-qualified or unqualified
members, an rvalue (a call result, an explicit `move`) may only bind `&&`-qualified or
unqualified ones. Until that lands, an lvalue receiver should at minimum PREFER an unqualified
or `&`-qualified candidate over an `&&`-qualified one.

Related ruling to respect: "C++ std types are just types" - assignment of an imported C++
class must mean C++ assignment, so this is not an assignment-transparency question.

Regression coverage: an in-repo fixture class with `Self& operator=(const Self&) &` that sets
a tag and `Self& operator=(const Self&) &&` that sets a different tag; assert an lvalue
assignment picks the `&` one. Add a `Test/test_cpp_interop.cb` row.

## Confirmed with an in-repo header (2026-09-16, no libtorch needed)

scratch/dogfood/probe/refq.h declares `Box& operator=(const Box&) &` (lastRan = 1) and
`Box& operator=(const Box&) &&` (lastRan = 2, v += 100). `refq.Box a = ...; a = b;` prints
`ran=2 v=102`: the lvalue assignment selected the rvalue-ref-qualified overload. Root cause is
therefore that member ref-qualifiers (`&` / `&&`) are ignored in operator overload selection;
libtorch's `Tensor::operator=(const Tensor&) &&` (which does an in-place copy_) is just the
visible victim. Fix: record the ref-qualifier per member in the extraction and exclude
`&&`-qualified candidates when the receiver is an lvalue (and `&`-qualified ones for a
temporary). Regression row: put a refq-style class into Test/library/cpp_interop_tpl.h.
