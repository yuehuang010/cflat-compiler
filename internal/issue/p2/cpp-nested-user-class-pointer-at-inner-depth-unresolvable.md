# Bucket p2. `std.vector<std.vector<user.C*>>` - a POINTER to a user class at inner depth is unresolvable

Found 2026-09-17 while fixing
internal/issue/p2/cpp-nested-std-specialization-in-file-scope-signature-still-unresolved.md
(fix/cpp-nested-sig, macOS arm64, Release). Pre-existing on master. Listed there as a related gap
("refused on both sides"); measured here and confirmed to be a DIFFERENT root cause from the
signature pre-pass defect that issue was about.

## Summary

`std.vector<std.vector<nest.Cell*>>` is refused. Unlike the signature defect, it is refused as a
LOCAL as well, so the ForwardRefScanner signature path is not involved and the inner-before-outer
request order does not fix it.

## Repro

scratch/nsg_p8b_innerptr_local.cb and scratch/nsg_p8_innerptr_param.cb in the fix worktree:

```
import cpp "vector";
import cpp "Test/library/cpp_interop_nest.h";
extern int main()
{
    std.vector<std.vector<nest.Cell*>> v = default;   // refused
    return (int)v.size();
}
```

Local: `'std::vector<std::vector<nest::Cell *>>' does not name a C++ class type in the imported
headers`. As a file-scope parameter: `unknown type 'std.vector<std.vector<nest.Cell*>>'`
(displayed as "cannot find the type ..."), pinned by Test/errors/err_cpp_nested_ptr_sig.cb.

## What was measured

Each of these BINDS and runs, so the refusal is specific to a pointer-to-USER-class at inner depth:

- `std.vector<nest.Cell*>` (depth 1, user class, pointer) - fixture leg 1944.
- `std.vector<std.vector<int*>>` (depth 2, pointer, PRIMITIVE element) - scratch/nsg_q8b_intptr.cb.
- `std.vector<std.vector<nest.Cell>>` (depth 2, user class, NO pointer) - fixture leg 1930.
- `std.vector<std.shared_ptr<nest.Cell>>` - fixed by the signature change, fixture section M103.

`-v` on the local form shows the INNER `std::vector<nest::Cell *>` request succeeding in the
<vector>+cpp_interop_nest.h group (its stage-2 key names `Fstd.vector$.p$nest.Cell`), and the
outer `std::vector<std::vector<nest::Cell *>>` request then reaching clang and returning zero
records - the `raw.records.empty()` arm of `RequestCxxForeignType`
(cflat/LLVMBackend_CInterop.cpp around 8196 and 8253). So it is an extraction/record-matching
failure, not an owner-group failure.

## Fix direction

Start from the zero-record result: dump the probe source the outer request hands clang for this
spelling and compare it against the working `std::vector<std::vector<nest::Cell>>` one. Suspect
the spelling round-trip through `CxxSpellingForCflatType` for a mangled inner that carries a
pointer (`std.vector$.p$nest.Cell` -> `std::vector<nest::Cell *>`), and the record match against
clang's canonical form.

## Related

A refused file-scope signature reports TWICE - the generic "unknown type" from the
ForwardRefScanner signature pre-pass, then the request's own message from the codegen walk. Only
the first is visible in a normal compile (it aborts), but it means a SCOPED `expect_error` block
around a refused signature is armed for both and cannot be satisfied by any single expectation.
Test/errors/err_cpp_nested_ptr_sig.cb uses the bare file-scope form for that reason.
