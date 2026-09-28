# CFlat structs holding nontrivial C++ fields lower the C++ way

Status: RULED 2026-09-27 (design A). Not started. Replaces the decision section of
`internal/issue/p2/cpp-struct-list-field-relocated-bitwise.md`; the elision issues
`p3/cpp-prvalue-to-cflat-byvalue-param-extra-copy.md` and
`p3/cpp-typed-local-extra-copy-from-param-and-ctor-arms.md` are folded in as phase 4.

## Ruling

A CFlat struct (plain or `[cpp]`) that transitively contains a field of a nontrivial imported C++ class
(`HasForeignNontrivialCxxField`) behaves exactly as the same aggregate would in C++:

- it is itself nontrivial: the compiler synthesizes member-wise copy ctor, move ctor, copy/move
  assignment and destructor, each calling the field's C++ special member (trivial fields bit-copy);
- it is constructed in place (C++17 guaranteed elision: `T h = T();`, `return T();`);
- it is returned through sret and passed by value through a caller-owned temporary, never as an LLVM
  first-class value;
- a move is the member-wise move constructor, never a load/store of the whole aggregate.

Structs with only trivial fields keep the direct first-class ABI. Copy vs move is inferred by the
compiler as everywhere else (borrow by default, no user annotation). Prior art: C++ implicit special
members ([class.copy.ctor]/[class.dtor]) and Itanium C++ ABI 3.1.2 (non-trivial for the purposes of
calls -> indirect). Rust's answer (every move is a memcpy, self-reference needs `Pin`) is exactly what
breaks `std::list` here, so it is not followed.

## Measured today (issue repro)

`std.list<int>` field in a plain CFlat struct: `Plain h = Plain()`, `Plain h;`, `= default`, `h = Plain()`,
returning a `[cpp] Holder` from `make()`, and move/by-value transfer all abort (exit 134, expected 52).
The `[cpp]` default-construction spellings are already correct (Test/test_cpp_interop_bridge.cb
3580-3589).

## Code anchors (2026-09-27)

- Construction: `MainListener::EmitCppStructConstructorThunk` (MainListener_Aggregates.cpp ~4593) and the
  plain ctor path (~5009) end in `CreateLoad(structLLVMType, thisAlloca)` + return by value.
- `LLVMBackend::IsForeignNontrivialCxxReturnClass` (LLVMBackend.h ~8335) documents that CFlat-defined
  `[cpp]` structs keep the direct ABI; only imported classes use the foreign sret convention.
- sret machinery to reuse: `prepareCxxSret`, `AbiSlot::SRetReturn`, `BuildCFlatSRetFunctionType`
  (LLVMBackend_ControlFlowAndFunctions.cpp ~539-822, ~1684-1725) - today only for calls INTO C++.
- By-value C++ argument temp: LLVMBackend_Overloads.cpp ~4395-4430 (`cxx.argtemp`).
- Destruction is already member-wise: `GetOrCreateFullDestructor` (LLVMBackend_CodegenHelpers.cpp ~1175)
  calls the bound C++ dtor of each C++ field. `[cpp]` dtor thunk: `EmitCppStructDestructorThunk`.
- Predicate: `HasForeignNontrivialCxxField` (LLVMBackend_CodegenHelpers.cpp ~1678) - recomputed per call,
  not cached on `StructData`.

## Phases

0. **Stopgap refusal (option B, small).** Until phase 2 lands, refuse the still-wrong cells with one
   clean `LogError` instead of a runtime abort: return by value, by-value parameter, `move`, and
   whole-struct assignment of a struct with a nontrivial C++ field. Legs in `Test/errors/`. Each later
   phase removes the refusal for the shapes it makes correct.
1. **Classification.** Cache "nontrivial for the purposes of calls" on `StructData`
   (`HasNontrivialCxxMember`), set at struct completion, including generic instantiations and nested
   CFlat structs. Add it to the `--init` cache round-trip in the same change (CLAUDE.md serializer rule).
   Mangling/ABI of trivial structs unchanged.
2. **Construction + return.** Functions returning such a struct get an sret parameter (reuse
   `BuildCFlatSRetFunctionType`); ctor thunks construct into the sret/destination slot instead of
   loading the aggregate; `T h = T();`, `T h;`, `= default`, `return T();` construct in place. Covers the
   issue's construction and `make()` cells.
3. **Copy, move, assign.** Synthesize member-wise copy ctor / move ctor / copy-assign / move-assign
   (per field: C++ special member for nontrivial C++ fields, recursive for nested nontrivial CFlat
   structs, bit-copy otherwise). Route by-value parameters, `move`, assignment, container element
   store and ternary joins through them. Partial-construction unwind reuses `UnwindPartialScope`.
   Covers the move/by-value cells; `consume(make())` binds the prvalue to the parameter slot with no
   copy.
4. **Elision parity.** Close the two elision issues (prvalue into by-value parameter, typed local from
   parameter/ctor/ternary arms) on top of phases 2-3; count copy/move ctor calls against clang++.
5. **Docs.** doc/LANGUAGE.md bridge section; remove the issue files.

## Acceptance

- Every cell in the issue repro matches clang++ -std=c++20 for the equivalent C++ aggregate (value and
  count of copy/move/dtor calls), cold and warm cache.
- Trivial CFlat structs: IR for return/param/move unchanged (spot-check `--symbol-dump-ir`).
- Gate from `internal/issue/Queue.md` (test.sh, examples, test_libs tiers 1-2, torch -j 1) green at each
  phase.
