# A C++ scoped enum argument silently converts to an unrelated enum, picking the wrong overload

## Summary

`module->to(c10.ScalarType.Double)` compiles and binds `torch::nn::Module::to(c10::Device,
bool)` instead of `to(c10::ScalarType, bool)`. `c10::ScalarType::Double` is 7; it arrives as
`c10::DeviceType(7)` = FPGA, and the program dies at runtime:

```
libc++abi: terminating due to uncaught exception of type c10::Error:
PyTorch is not linked with support for fpga devices
frame #5: torch::nn::Module::to(c10::Device, bool)
```

`c10::ScalarType` and `c10::DeviceType` are both SCOPED enums (`enum class ... : int8_t`), so
C++ allows no implicit conversion between them in either direction and the call is
unambiguous in real C++. Found 2026-09-16 dogfooding libtorch (scratch/dogfood/torch/rnn3.cb).

Severity: this one compiles clean and gives a wrong-code runtime failure a long way from the
call site. Any C++ API with sibling overloads over different enums is exposed.

## Repro

```c
import cpp "torch/torch.h";
struct Scale : torch.nn.Module { /* any CFlat or libtorch module */ };
...
std.shared_ptr<torch.nn.Module> m = ...;
m->to(c10.ScalarType.Double);        // binds to(c10.Device, bool); crashes at runtime
```

Workaround: use the three-argument overload, which cannot be confused -
`m->to(c10.Device(c10.DeviceType.CPU), c10.ScalarType.Double, false);`

## Root cause (hypothesis)

Imported C++ enums are presumably mapped to their integer underlying type for
conversion-ranking purposes, so ScalarType -> int -> DeviceType -> Device (Device has a
non-explicit `Device(DeviceType)`) becomes a viable chain. C++ scopedness is being dropped.
Two separate defects hide here: (1) a scoped enum must not convert to an integer or to another
enum implicitly; (2) even for unscoped enums, this chain is two user-defined/standard steps and
should not have been offered.

## Fix direction

Carry `EnumDecl::isScoped()` from the clang AST dump into the bound enum type and refuse any
implicit conversion from a scoped enum to an integer, to another enum, or to a class with a
converting constructor from another enum. Keep explicit `(int)` casts working. Then confirm
`m->to(c10.ScalarType.Double)` resolves to `to(ScalarType, bool)`.

Regression coverage: a fixture header with `enum class A : int { x = 7 };`,
`enum class B : int { y = 7 };`, `struct Dev { Dev(B); };` and overloads `f(Dev)` / `f(A)`;
assert `f(A::x)` picks `f(A)`, and that `f(A::x)` where only `f(Dev)` exists is an error, not a
silent reinterpretation. A `Test/errors/err_*.cb` row for the second half.
