# C++ implicit user-defined conversion at a call argument is not offered for some ctors

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
