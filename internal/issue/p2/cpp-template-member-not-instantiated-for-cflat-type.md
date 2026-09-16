# nn::Sequential cannot hold a CFlat-defined module (C++ template member not instantiated for a CFlat type)

## Summary

A CFlat `struct Scale : torch.nn.Module` can be registered as a child with `register_module`
(a C++ function template) but CANNOT be put into an `nn::Sequential`, because the templates on
that path are never instantiated for the CFlat type:

```
rnn.cb(89,4): no overload of 'push_back' matches the given arguments.
  Call arguments (3):
    [0] torch.nn.SequentialImpl <unnamed>
    [1] ptr <unnamed>
    [2] std.shared_ptr<__cflat_user.Scale> <unnamed>
  ...
  Argument mismatch detail (single resolved candidate: push_back):
    [0] arg=torch.nn.SequentialImpl  param=torch.nn.SequentialImpl*
    [1] arg=ptr                      param=std.string
    [2] arg=std.shared_ptr<__cflat_user.Scale>  param=torch.nn.AnyModule
```

Building the `AnyModule` by hand fails the same way:

```
rnn.cb(91,23): C++ class 'torch.nn.AnyModule' has no constructor whose parameter types match
these arguments ('std.shared_ptr<__cflat_user.Scale>')
```

Found 2026-09-16 dogfooding libtorch (scratch/dogfood/torch/rnn.cb). Sequential with
libtorch's own modules works (`seq->push_back("fc1", torch.nn.Linear(2, 4))`, ladder rung t22),
so this is specific to a CFlat-defined module type reaching a C++ member template.

## Repro

scratch/dogfood/torch/rnn.cb, the `nn::Sequential` section:

```c
struct Scale : torch.nn.Module { /* Linear child + forward */ };

torch.nn.Sequential seq = torch.nn.Sequential();
seq->push_back("scale", std.make_shared<Scale>(4));                 // no overload matches
torch.nn.AnyModule am = torch.nn.AnyModule(std.make_shared<Scale>(4)); // no ctor matches
```

Workaround: none. scratch/dogfood/torch/rnn3.cb drops Sequential and composes the sub-modules
by hand in `forward()`, which is what a user has to do today.

## Root cause (hypothesis)

`register_module` is `template<typename M> shared_ptr<M> register_module(string, shared_ptr<M>)`
and DOES instantiate for a CFlat type, so plain deduction over a CFlat struct works. The
Sequential path goes through `AnyModule`'s constructor template
`template<typename M> explicit AnyModule(shared_ptr<M>)`, which is SFINAE-constrained on the
module having a detectable `forward` (`torch::detail::check_not_lvalue_references`,
`ModuleType::forward` signature probing via `&M::forward`). A CFlat-defined struct's `forward`
is presumably not visible to that probe in the synthesized C++ declaration the binder feeds
clang, so the constrained constructor SFINAEs out and only the non-template candidates remain.

Second, smaller defect visible in the same diagnostic: the string literal argument prints as
`ptr` and does not convert to `std.string` on the non-template candidate - the same missing
implicit user-defined conversion as internal/issue/p3/cpp-implicit-ctor-conversion-at-call.md.

## Fix direction

Find out what the binder emits for a CFlat module type when clang instantiates a constrained
C++ template over it - specifically whether member functions (`forward`) are present on the
synthesized declaration. If they are not, emitting them is the fix and it unblocks every
SFINAE-on-members C++ API, not just `AnyModule`. If they are present, capture the substitution
failure clang reports and surface it in the diagnostic instead of silently dropping the
candidate, because "no constructor matches" gives the user nowhere to go.

Coverage: an in-repo fixture with a `template<typename T> struct Holder` whose constructor is
constrained on `T` having a member function, constructed from a CFlat struct - the libtorch
shape without libtorch.
