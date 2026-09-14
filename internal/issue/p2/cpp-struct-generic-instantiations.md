# Generic `[cpp] struct` is refused: `struct Wrapper<T> : cppt.Holder<T>` cannot be written

## Summary

A `[cpp] struct` (explicit `[cpp]` or implicit through a C++ base clause) cannot carry a CFlat
generic parameter list. `cflat/MainListener_Aggregates.cpp` (~:77) refuses at the declaration
with `generic [cpp] struct is not supported yet`. Pinned by
`Test/errors/err_cpp_struct_generic.cb` (explicit `[cpp] struct BadCpp<T>`) and
`Test/errors/err_cpp_struct_tpl_base_generic_param.cb` (implicit, `struct Bad<T> :
cppt.m76.Holder<T>`). Filed 2026-09-13 as a p2 gap by maintainer ruling; not a design refusal.

## Repro

```
import cpp "cpp_interop_tpl.h";
struct Bad<T> : cppt.m76.Holder<T> { };      // error: generic [cpp] struct is not supported yet
```

C++ does this routinely (a class template deriving from a template base or from
`torch::nn::Module`); a reusable generic layer wrapper written in CFlat needs it. No libtorch
ladder rung (t1-t29) requires it today.

## Root cause

M10 v1 generates exactly ONE C++ class per `[cpp] struct` in the companion stub (fixed field
block, one ctor/dtor/move thunk set, one override thunk set, one header-cache row keyed by the
struct name). A generic CFlat struct is monomorphized per instantiation by the forward scanner,
so `Bad<int>` and `Bad<double>` would each need their own generated class; the plumbing keys
everything on the bare name, and the base spelling would see an unresolved `T`.

## Fix direction

- Key the generated class on the mangled instantiation identity (the same identity the
  monomorphizer already produces), not the bare struct name: `CppStructCxxName` /
  `CppStructThunkStem` take the instantiation.
- Resolve the base clause and base initializer through the instantiation's type map before the
  C++ type request (the resolver run F added for concrete specializations, both passes).
- Emit ctor/dtor/move/override thunks and `generatedCxxRecords_` entries per instantiation;
  header-cache rows keyed by the instantiation identity (bump the cache version).
- Move the check at ~:77 from "refuse at declaration" to "defer until instantiated"; keep the
  error for an uninstantiated generic body if the scanner has no instantiation to emit.
- Tests: M-series rows with two instantiations of one generic `[cpp] struct` deriving from
  `Holder<T>` (override dispatch through `call_doubled<int>` and `call_doubled<double>`, dtor
  counters, move), a generic `[cpp] struct` without a base, and the two existing err files
  rewritten to the remaining refused shape (or deleted if none remains).

Contained change on the order of run F (template bases); no redesign.
