# Direct C++ ABI interop

Status: proposed implementation plan. No compiler changes are part of this document.

## Objective and architectural decision

CFlat imports C++ headers and uses their namespaces, functions, classes, and template
specializations through normal language expressions. Generated code calls the actual C++
ABI entry points, including constructors, destructors, and virtual dispatch. Users do not
write bindings, and generated extern-C wrappers are not the implementation strategy.

Clang owns C++ parsing, semantic decisions, layout, mangling, and target ABI knowledge.
CFlat owns its syntax, type checking outside foreign expressions, control flow, and
ownership rules. A narrow adapter joins these systems; do not implement a second C++
compiler inside the existing C type-spelling mapper.

Direct ABI does not mean every call is a fixed symbol call: virtual calls use dispatch,
inline definitions need emission, and ABI-required adjustment thunks remain valid.
Clang may emit C++ definitions into a companion LLVM module, linked before optimization.
This is definition emission, not a generated wrapper boundary.

## Existing foundation

- CMakeLists.txt already links clangAST, clangSema, clangCodeGen, and tooling libraries.
- The documented LLVM 23 source build enables clang and lld.
- cflat/CClangExtract.cpp already uses an AST visitor, but exports plain C spellings.
  VisitFunctionDecl uses unqualified names and excludes declarations without an ordinary
  identifier; this cannot represent constructors, destructors, or operators correctly.
- cflat/CClangExtract.h exposes value-only extraction results. Its existing extraction
  lifetime is unsuitable for later template instantiation and overload resolution.
- cflat/LLVMBackend_CInterop.cpp owns driver arguments, C type mapping, registration,
  and header caching. Its existing C++ mode is used for UUID harvesting, not full interop.
- cflat/LLVMBackend_EmitAndLink.cpp is the integration point for native link/runtime work.
- MainListener.h contains both declaration passes; type parsing changes must reach both.

## Design contracts

1. Add a compilation-owned C++ session retaining ASTContext, Sema, declarations, and
   code generation state. Use opaque session/type/declaration IDs across the adapter;
   keep Clang headers confined to dedicated translation units. Explicitly define LLVM
   context/module ownership and reset order. Never persist raw pointers in caches.
2. Keep foreign canonical type identity, cv qualifiers, reference kind, value category,
   namespace, access, template arguments, and source location. Distinguish identity from
   ABI storage: two foreign types with identical layout are not interchangeable.
3. Ask Clang to resolve foreign overloads and initialization using CFlat operand types
   and value categories. Do not rank C++ candidates using CFlat's existing overload rules.
   Specify mixed native/foreign candidate behavior and conversions before enabling it.
4. Obtain ABI arrangements from Clang: LLVM parameter/result types, calling convention,
   hidden parameters, attributes, coercions, expansion, alignment, and indirect passing.
   A mangled name plus an LLVM FunctionType is not a complete call-lowering contract.
5. Use Clang layout for foreign records, including bases, padding, bitfields, and overlap.
   Do not copy the existing CFlat struct or interface layout onto C++ objects.
6. Keep construction, copy, move, destruction, and borrowing explicit in CFlat's internal
   representation. Reject unsupported operations with LogError/LogErrorContext before IR
   emission; never silently treat nontrivial objects as memcpy-compatible values.
7. Select a target toolchain profile: triple, ABI, CPU/features, C++ standard, sysroot,
   standard library, runtime, packing, defines, and relevant compiler compatibility flags.
   An LLVM/Clang install alone does not supply every target's C++ SDK or standard library.

## Milestones

### M0 - Prove the Clang integration seam

Inspect the installed LLVM 23 headers and source before selecting APIs. Prototype the
adapter under scratch/: retain a semantic session, resolve a declaration, obtain its
mangled name and ABI arrangement, and emit a direct call from CFlat-owned IR.

Exercise a scalar free function, an aggregate result with hidden return storage, and a
nontrivial class parameter. Compare against Clang-generated callers for the same target.
Inventory public CodeGenABITypes/CGFunctionInfo support and the gaps for expression
emission, constructors, virtual dispatch, and EH. Installed APIs may not expose all of
Clang's internal CodeGen machinery.

If needed, propose a small version-pinned Clang CodeGen extension that emits resolved
foreign operations into the current function with explicit operand and cleanup contracts.
Document source/install changes and upgrade cost. Do not assume arbitrary LLVM values can
be handed to public Clang APIs, copy large internal implementation files, or switch to
wrappers to bypass the gate.

Exit: executable proof of direct calls plus a written adapter contract covering later
milestones. Any required Clang patch and rebuild is identified before broad implementation.

### M1 - Import sessions and foreign symbol/type identity

Add explicit C++ import mode and toolchain configuration, including unambiguous handling
of .h files. Preserve existing C import behavior. Parse headers strictly enough that an
error-recovered AST cannot become a callable binding. Import namespaces, aliases, scoped
enums, overload sets, and incomplete/complete class declarations on demand.

Retain the AST for the compile and reset it for reanalysis. Specify declaration merging
across imports and translation units without merging conflicting macro configurations.
Provide source-based diagnostics for unsupported requested declarations; unrelated
unsupported declarations in a large header must not prevent supported uses.

Exit: duplicate names in different namespaces and overloads remain distinct; qualifiers,
references, and incomplete types survive registration; C interop continues to pass.

### M2 - Direct free-function calls and basic native linking

Implement actual C++ linkage names, ABI lowering, and Clang overload selection for scalar,
enum, pointer, and reference arguments/results. Support extern-C declarations encountered
inside C++ headers without changing their linkage. Begin with noexcept calls; reject
potentially throwing calls until M8, even if a declaration happens not to throw at runtime.

Compile .cpp inputs and link compatible prebuilt objects/libraries with the selected C++
runtime. Include visibility, dllimport where applicable, and useful unresolved-symbol
diagnostics. Support basic reference binding and const correctness without implying that
a reference return extends the referent's lifetime.

Exit: CFlat directly calls overloaded namespaced functions in a separately compiled C++
library. Runtime values and emitted call attributes match Clang callers, without wrappers.

### M3 - Record layout and trivial aggregate ABI

Import size, alignment, field offsets, and triviality. Support public field access and
trivial records passed/returned by value, including ABI coercion, splitting, and indirect
storage. Respect packed and over-aligned types; reject unsupported layouts explicitly.
Keep storage layout separate from parameter/result ABI layout.

Exit: bidirectional calls exchange small, large, mixed integer/floating, packed, and
over-aligned aggregates correctly. Clang observes the same fields, sizes, and alignments.

### M4 - Classes, lifetime, and nontrivial values

Add direct constructor/destructor calls, nonvirtual/static methods, access control,
cv/ref-qualified methods, and stack-owned foreign objects. Handle constructor/destructor
ABI variants and hidden arguments through the adapter, not suffix conventions.

Integrate copy/move construction, assignment, temporary materialization, and destruction
on normal scope exit and every early control-flow exit. Support nontrivial by-value
parameters/results only with correct caller/callee destruction responsibility. Encode
copy elision rules; do not assume std::move guarantees a move or destroys the source.
Add foreign allocation/deallocation through the appropriate C++ operators, including
alignment; never pair arbitrary foreign allocation with a CFlat deallocator.

Exit: instrumented C++ counters prove exactly-once destruction and correct copy/move
behavior for nested scopes, returns, breaks, and temporaries. Deleted/private operations
and invalid reference binding are diagnosed. Throwing operations remain gated by M8.

### M5 - Inline definitions, operators, and templates

Use retained Sema to instantiate explicit class/function specializations first, then
deduced function templates, default arguments, operators, conversions, and ADL. Map
CFlat concrete types to eligible C++ template arguments; reject types without a defined
representation. Keep CFlat generic mangling separate from C++ template identity.

Emit required inline/template definitions, static data members, vtables/RTTI when needed,
and compiler-required thunks through Clang CodeGen. Preserve linkage, COMDAT/ODR rules,
target attributes, and module flags when combining IR. Include header-defined static
objects and initialization/finalization scheduling; do not skip bodies needed by Sema.

Exit: a header-only templated container with nontrivial elements and an overloaded
operator works across multiple imports without missing or duplicate definitions.

### M6 - Inheritance and virtual dispatch

Support use of C++-defined hierarchies: public base conversions, multiple and virtual
inheritance, adjusted this pointers, virtual method/destructor calls, and covariant
returns. Delegate layout and ABI dispatch details to Clang; do not map C++ vtables to
CFlat interface tables. Add member pointers only when their ABI representation and
invocation are implemented; ordinary function pointers are insufficient.

Exit: objects created in C++ can be used and destroyed through supported base views in
CFlat, including a case where the base address differs from the complete object address.

### M7 - Callbacks and reverse ABI entry points

Generate CFlat function entries with the same foreign ABI arrangements used for calls.
Support eligible free-function callbacks, including aggregate arguments/results and
linkage declarations. Define callback lifetime and reject unsupported capturing closures.
Check that foreign code cannot silently unwind into an unprepared callback frame.

Exit: C++ calls a CFlat callback and receives correct aggregate results; callbacks nested
inside foreign calls preserve object lifetime and use matching ABI attributes at both ends.

### M8 - Exceptions and unwind-safe cleanup

Define the language contract for foreign exceptions: propagation, catch syntax or existing
construct mapping, typed catches, catch-all/rethrow, and noexcept termination. Implement
target-specific personality, invoke/unwind edges, cleanup pads/landing pads, and destructor
ordering. Cover partially constructed objects, temporary arguments, callback frames, and
destructors during unwinding. Do not allow exceptions through CFlat frames until they have
correct cleanup support. Different target EH models require separate lowering paths.

Exit: C++ throws across a CFlat frame and is caught in C++, with every live CFlat and C++
owned object cleaned up correctly. Also verify a CFlat-side catch and noexcept termination
in a subprocess. Only then lift the potentially-throwing-call restriction.

### M9 - Production imports, runtime support, cache, and tooling

Validate realistic standard-library use (string, vector, unique_ptr) through direct C++
types. Specify explicit CFlat conversions and borrowed-view lifetime behavior. Verify
toolchain selection and runtime linking for supported ABI/standard-library profiles;
record incompatible or unverified profiles accurately rather than promising universal
C++ binary compatibility.

Complete persistent caches keyed by transitive headers, macros, standard, target, ABI,
SDK/runtime identity, and exact Clang build. Reload ASTs through Clang serialization or
rebuild sessions; never deserialize opaque IDs as live declarations. Persist requested
specializations and artifacts with dependency invalidation. Every earlier milestone
must already invalidate affected caches or bypass unsupported persistent caching.

Support --check, native emission, and --run where runtime loading and initialization can
be honored; otherwise issue a precise mode diagnostic. Add foreign hover, completion,
definition navigation, and useful Clang diagnostics at CFlat call sites. Verify reanalysis
after failed imports cannot leak stale declarations, value categories, or cleanup state.

Exit: representative header-only and separately compiled libraries work from cold and
warm caches, and header/config changes force correct regeneration.

### M10 - CFlat-defined implementations of C++ classes

Treat subclassing C++ types in CFlat as a separate advanced milestone. Specify overrides,
layout ownership, base construction, virtual destructors, RTTI identity, vtable emission,
and cross-language dynamic_cast/typeid behavior. This requires foreign class-definition
support, not just the imported-class use completed in M6.

Exit: C++ owns a CFlat-defined derived object through a C++ base pointer, invokes overrides,
and destroys it correctly, including multiple inheritance if advertised as supported.

## Sequence and release gates

- M0 is mandatory before implementation estimates; the major uncertainty is access to
  Clang CodeGen operations, not whether AST parsing is available.
- M1 -> M2 -> M3 -> M4 forms the first useful direct-ABI release, restricted to supported
  noexcept operations. M5 and M6 expand that release; M7 adds the reverse direction.
- M8 depends on M4 cleanup representation and the M0 target EH design. It can be developed
  before M5-M7 if throwing libraries are the priority. M9 requires their integrated behavior.
- M10 is the final advanced expansion, not a prerequisite for using ordinary C++ libraries.
- Every milestone ships an explicit capability boundary and rejects unsupported uses.
  Do not advertise unrestricted C++ interop based on successful scalar calls.

## Verification and repository constraints

For each implementation milestone, extend related existing tests (such as
Test/test_c_interop.cb and Test/test_c.cb) and existing suitable error fixtures. Do not
create new test files without explicit instruction. Use scratch/ for prototype C++
libraries, reference callers, IR, objects, logs, and temporary executables. Persistent
fixture needs must be resolved before claiming a milestone has durable regression coverage.

Use Clang-compiled callees/callers as the ABI oracle. Assert observable values, offsets,
dispatch targets, and lifetime counters, not just successful compilation. Include negative
tests and cold/warm cache cases as each feature lands. Verify emitted IR before execution.
Cross-target IR comparisons are useful, but do not substitute for native runtime results.

After compiler changes, build Release and run the current host's full suite:
./cmake_build.sh release and ./test.sh Release on macOS/Linux; the corresponding batch
scripts on Windows. Run applicable LSP checks for symbol-registration/LSP changes.
No build or suite is needed for this plan-only change.

Preserve C interop. Apply declaration-specifier changes to both passes. Serialize every
new analysis-relevant field in the compiler cache round-trip in the same change. Use ASCII
and LogError/LogErrorContext. Do not modify root vcpkg.json or commit changes without the
authorization required by repository instructions. For substantial implementation, use
the repository's delegated implementation and main-session review workflow.

## References and API caution

Repository anchors: CMakeLists.txt; cflat/CClangExtract.h/.cpp;
cflat/LLVMBackend_CInterop.cpp; cflat/LLVMBackend_EmitAndLink.cpp;
cflat/LLVMBackend_Overloads.cpp; cflat/LLVMBackend_OwnershipTemps.cpp;
cflat/MainListener.h; internal/llvm-from-source-build.md; internal/testing-notes.md.

- [Clang tooling interfaces](https://clang.llvm.org/docs/Tooling.html)
- [Clang CodeGen ABI API](https://clang.llvm.org/doxygen/CodeGenABITypes_8h.html)
- [Clang CodeGen type lowering](https://clang.llvm.org/doxygen/classclang_1_1CodeGen_1_1CodeGenTypes.html)
- [MSVC compatibility](https://clang.llvm.org/docs/MSVCCompatibility.html)

Online Doxygen follows development Clang and is architectural reference only. M0 must
verify exact API availability and behavior against the repository's LLVM 23 installation.
