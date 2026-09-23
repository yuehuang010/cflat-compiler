# C++ implicit user-defined conversion at a call argument is not offered for some ctors

Also blocks (2026-09-20): the libtorch line
`c10.TensorOptions(c10.ScalarType.Float).requires_grad(true)`. It was filed as
`p2/cpp-ctor-ambiguous-when-a-method-is-chained.md`; that ambiguity no longer reproduces (12
in-repo cells + libtorch, all three filed hypotheses contradicted) and the file was closed. What
stops the line now is this issue.

## Summary

`torch.optim.Adam(params, 0.01)` fails with "C++ class 'torch.optim.Adam' has no constructor
whose parameter types match these arguments ('std.vector<at.Tensor, ...>', 'double')", while
`torch.optim.SGD(params, 0.1)` binds (rung t30). In C++ both rely on the non-explicit
`XOptions(double lr)` converting constructor. Found 2026-09-16 in rung t31
(scratch/ladder/torch/t31.cb); workaround `torch.optim.Adam(params, torch.optim.AdamOptions(0.01))`.

## Repro

scratch/ladder/torch/t31.cb with the Adam line reverted to the bare double.

## Root cause

Not investigated. First question: why the SGD overload set admits the double (a by-value
`SGDOptions` parameter reached through the M-series one-step implicit conversion?) and the Adam
one does not (AdamOptions may take the lr through a different constructor shape, or the
candidate is by-reference and the by-value/by-reference distinction added in timebox 2 excludes
it).

## Fix direction

Decide whether one user-defined implicit conversion at a C++ call argument is offered
uniformly (clang's rule) or never; make SGD and Adam behave the same either way, and add a
fixture row against an in-repo header (Test/library/cpp_interop_tpl.h style options class).

## Measurements 2026-09-17 (fix/cpp-explicit-ctor) - the filed asymmetry does NOT reproduce

Reproduced the SHAPE in-repo (`Test/library/cpp_interop_explicit.h`, namespace `cppexp`): a
`Options` class with a NON-explicit `Options(double)`, taken by value, by `const Options&`, by
`Options&&`, by non-const `Options&`, through a one-parameter class constructor (`OptOwner`),
through the Adam-style two-parameter constructor (`Optim(std::vector<int>, Options)`), through the
const-ref variant (`OptimCref`), and as a free operator operand. Measured on master 0fbe6115:

| parameter kind | bare `0.5` accepted? | clang | verdict |
|----------------|----------------------|-------|---------|
| by value | yes (50) | ok | correct |
| `const Options&` | yes (51) | ok | correct |
| `Options&&` | yes (52) | ok | correct |
| ctor param, 1 param | yes (57) | ok | correct |
| ctor param, 2 params (Adam shape) | yes (1050) | ok | correct |
| ctor param, `const Options&` | yes (2050) | ok | correct |
| operator operand | yes (40) | ok | correct |
| non-const `Options&` | yes (53) | **error** | WRONG - over-accepted |

So the one implicit user-defined conversion IS offered uniformly at every parameter kind C++
allows, including the constructor shape the issue blames. The SGD/Adam split is therefore NOT this
code path in general - it needs a libtorch-specific cause (option-class ctor shape, template
constructor, or a signature clang refuses to harvest), and tests may not use libtorch, so the
issue stays filed with no in-repo repro. These rows are now value legs 2213-2220 in
`Test/test_cpp_interop.cb`.

The non-const `Options&` row WAS fixed on fix/cpp-explicit-ctor (`CanImplicitlyConstructCxxClass`
now refuses a user-defined conversion into a non-const lvalue reference).
