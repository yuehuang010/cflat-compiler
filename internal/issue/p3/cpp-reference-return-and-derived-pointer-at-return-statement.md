# A C++ reference return (and a derived pointer) is refused at a `return` statement

Found 2026-09-17 by the review of fix/batch-cpp-bind1 (macOS arm64, Release). Pre-existing.

## Summary

After that fix, `C* p = ref_call();`, `p = ref_call();` and passing `ref_call()` to a `C*`
parameter all bind the C++ reference return (`C&` / `const C&`) as a borrowed pointer. The
RETURN position is the odd twin: `C* get() { return ref_call(); }` is refused with
`cannot return this value: its type does not match the return type of function 'get'`.
"Return converts like assignment" is a ratified rule, so this is a gap. The M6 derived-to-base
pointer adjust (`Base* get() { return derivedPtr; }`) is also not wired at the return site.

## Root cause

The refusal comes from a generic backstop in `cflat/LLVMBackend_MoveDataflow.cpp` (~1712) that
has no source `NamedVariable`. `LLVMBackend::CxxReferenceResultAsPointer` and
`AdjustCxxPointerForStore` are wired only at the declaration-initializer site
(`MainListener_Declarations.cpp`) and the assignment site (`MainListener_Expressions.cpp`).

## Fix direction

Wire both helpers into `EmitReturnExpression` (`cflat/MainListener_Statements.cpp` ~541), with
the return-ownership question answered explicitly (the returned pointer is a borrow of the
referent; it must not be marked owning). Legs in Test/test_cpp_interop.cb next to 1970-1973.
