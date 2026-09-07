# Direct C++ ABI interop

Status: in progress on branch `feature/cpp-interop` (worktree `../cflat-cpp-interop`).
Milestone status is tracked in the "Milestones" section. This document merges the original
ABI plan and the language-surface supplement (2026-09-06); it is the single source of truth.

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
Clang emits C++ definitions into a companion LLVM module, linked before optimization.
This is definition emission, not a generated wrapper boundary.

### Why direct ABI

| Approach | Used by | Cost | What the user sees |
|----------|---------|------|--------------------|
| Hand-written C shim | Rust `bindgen` + manual, Zig, Go cgo | Low compiler cost, high user cost | Writes and maintains a shim per API |
| Generated extern-C wrappers | Rust `cxx`/`autocxx`, SWIG, D `dpp` | Medium | Declares what to bridge; templates need explicit instantiation lists; extra call layer; a `.cpp` is compiled per import |
| Direct ABI via Clang | Swift ClangImporter (shipped), Carbon (experimental), Google Crubit (experimental) | High compiler cost, near-zero user cost | Header in, calls out; inline/template code emitted on demand |

Direct ABI is chosen because CFlat's value proposition is "extended C with no binding
layer", and the C side already delivers that (auto-extern from a `.c`, header binding with
no prototypes). Swift proves the direct route works and also proves its cost: it needed
private Clang CodeGen access. M0 established that the installed LLVM 23 exposes enough
public API (below), so no Clang patch is carried. If that ever changes, the fallback is
generated wrappers behind the SAME surface described here; the surface must not leak which
strategy is underneath.

## Design contracts

1. A compilation-owned C++ session retains ASTContext, Sema, declarations, and code
   generation state for as long as an import group can request work. Opaque session /
   type / declaration IDs cross the adapter; Clang headers stay confined to dedicated
   translation units (`CClangExtract.cpp`). LLVM context/module ownership and reset order
   are explicit. Never persist raw pointers in caches.
2. Keep foreign canonical type identity, cv qualifiers, reference kind, value category,
   namespace, access, template arguments, and source location. Distinguish identity from
   ABI storage: two foreign types with identical layout are not interchangeable.
3. Ask Clang to resolve foreign overloads and initialization using CFlat operand types
   and value categories. Do not rank C++ candidates using CFlat's existing overload rules.
   Foreign and native functions never form one overload set.
4. Obtain ABI arrangements from Clang: LLVM parameter/result types, calling convention,
   hidden parameters, attributes, coercions, expansion, alignment, and indirect passing.
   A mangled name plus an LLVM FunctionType is not a complete call-lowering contract. The
   emitted FunctionType is compared structurally against Clang's; a mismatch refuses the
   declaration rather than miscompiling.
5. Use Clang layout for foreign records, including bases, padding, bitfields, and overlap.
   Do not copy the existing CFlat struct or interface layout onto C++ objects; do not map
   C++ vtables onto CFlat interface tables.
6. Keep construction, copy, move, destruction, and borrowing explicit in CFlat's internal
   representation. Reject unsupported operations with LogError/LogErrorContext before IR
   emission; never silently treat nontrivial objects as memcpy-compatible values.
7. Select a target toolchain profile: triple, ABI, CPU/features, C++ standard, sysroot,
   standard library, runtime, packing, defines, and relevant compiler compatibility flags.
   An LLVM/Clang install alone does not supply every target's C++ SDK or standard library.

## Language surface

### Import spelling

The `cpp` keyword goes in front of the path and selects C++ mode. Extension never selects
the mode: `.h` under `import cpp` is C++, `.hpp` under plain `import` is the existing C
header path. `cpp` is a soft keyword through the same grammar alternative `program` uses;
no grammar change was needed.

```c
import cpp "widgets.h";                       // C++ header, any extension
import cpp "impl.cpp";                        // compiled by clang++ and linked, like .c today
import cpp "vector";                          // system header through the C++ driver's paths
import cpp package "fmt/core.h" lib "fmt.lib"; // prebuilt C++ library
import cpp { "a.hpp", "b.hpp" };              // grouped, one translation unit, one macro state
import cpp "big.hpp" cache;                   // existing cache clause applies unchanged
```

- `cpp` composes with every trailing clause (`lib`, `define`, `cache`, `framework`, `as`).
- A CFlat file may mix `import "x.h"` (C) and `import cpp "y.h"` (C++); they are separate
  sessions and a name declared in both is an error at the use site, not at import.
- `extern "C"` functions inside a C++ header keep C linkage and land in the C
  registration path.
- Toolchain profile comes from CLI flags (`--cpp-std`, `--cpp-stdlib`, existing
  `--c-include` / `--c-define`). Defaults per host: MSVC on Windows, libc++ on macOS,
  libstdc++ on Linux.

### Name lookup

C++ `a::b::c` is spelled `a.b.c`; foreign namespaces are ordinary CFlat namespaces
holding foreign members. Global-namespace C++ names join file scope as C header names do.
A foreign name colliding with a CFlat name at the same scope is an error at use naming
both origins; `import cpp "x.h" as X;` roots the header under `X.`. Nested types and
static members are members of the class name (`Outer.Inner`, `Widget.count()`); scoped
enums are `Color.Red`. Overload sets are foreign overload sets ranked by Clang.

### Types

| C++ | CFlat spelling | Notes |
|-----|----------------|-------|
| `int`, `long long`, `bool`, `char` | `int`, `i64`, `bool`, `char` | Width by target |
| `T*` | `T*` | Borrow by default |
| `const T*` | `T*` | const dropped (ruling 2026-08-26) |
| `T&` param | `T` lvalue arg, passed by address | No new syntax |
| `const T&` param | any `T` arg, temporaries allowed | |
| `T&&` param | `move x` arg required | |
| `T&` return | `alias T` | Existing borrow keyword; caller never frees |
| `T&` field or local | not supported | Explicit error |
| `class C` / `struct C` | `C` (a foreign class) | Layout from Clang |
| `template<class T> class V` | `V<T>` | CFlat generic angle syntax |
| `template<int N>` | `V<T, N>` | Existing value generic parameters |
| `std::string` | `std.string` | Distinct from CFlat `string`; explicit conversions |
| `enum class E` | `E` | Scoped; `E.A` |
| `void(*)(int)` | existing function pointer type | |
| member pointers | not supported | Precise error |

Template arguments accept CFlat primitives, pointers, and foreign types. A CFlat struct or
CFlat generic as a template argument is rejected. CFlat generic mangling and C++ template
identity never meet: `list<std.vector<int>>` is a CFlat instantiation whose element is a
foreign type.

### Construction, calls, and members

```c
import cpp "widgets.h";

int main()
{
    ui.Widget w = ui.Widget("title", 3);      // stack object, ctor overload picked by Clang
    ui.Widget d = default;                    // default ctor; error if deleted
    ui.Widget e;                              // same as = default
    w.resize(10, 20);                         // member call; virtual dispatch is automatic
    int n = w.count();
    ui.Widget* p = new ui.Widget("heap");     // operator new + ctor
    unique ui.Widget* q = new ui.Widget("x"); // owned; dtor + operator delete at scope exit
    delete p;                                 // dtor + operator delete (deleting dtor if virtual)
    return 0;                                 // w destroyed here, once
}
```

- `T(args)` is the construction expression; `= default` (or no initializer) calls the
  default constructor rather than zero-filling.
- `new`/`delete` on foreign types route to the C++ operators, never to CFlat's allocator.
  `unique T*` composes unchanged.
- C++ operators bind to CFlat's operator overloading table for foreign operand types;
  `operator[]` binds to CFlat's index expression. Unbound operators are a precise error.
- Static data members and header constants are readable; access control is enforced from
  Clang (private members are diagnosed as private, not as missing).
- Derived-to-base pointer conversion is implicit for public nonvirtual bases, with the
  correct `this` adjustment; inherited fields and methods are visible on the derived type.

### Lifetime and ownership

Foreign classes with ctor/dtor are owning types under the "ownership at function
boundaries" rule; nothing new is tracked.

1. `=` is total. For a copyable foreign type, `dest = src` invokes the C++ copy assignment
   (copy ctor at declaration). For a move-only foreign type plain `=` is an error;
   `dest = move src;` is required.
2. `move x` on a foreign object: `x` is compile-time consumed (use-after-move diagnostics
   apply) and its C++ destructor still runs at scope exit on the moved-from state. This
   is the one place the "readable-as-null" property of the move ruling does not hold.
3. `_ = move x;` runs the destructor immediately and marks `x` consumed.
4. By-value parameters: a C++ `void f(T)` called with a CFlat lvalue copies unless the
   argument is `move x`; the caller owns the temporary and destroys it after the call
   (Itanium). Borrow-into-sink slot rule applies: no `move` of a borrowed `T*` into `T`.
5. `alias T` returns spell `T&` returns with the existing no-lifetime-extension meaning.
6. const overloads: an lvalue prefers the non-const overload; const is chosen only when it
   is the sole candidate or the argument is a temporary. Revisit when the prototype matures.
7. Copy elision follows C++; CFlat must not insert a copy a C++ caller would not.
8. A foreign class never implements a CFlat interface and a CFlat struct never derives from
   a foreign class before M10. `is`/`as` on a foreign pointer is an error until
   `dynamic_cast` support lands.

### Errors, diagnostics, and modes

- Every unsupported construct is a `LogError` at the CFlat use site, carrying Clang's
  diagnostic text after a `-` separator. Every user-facing name is demangled.
- Potentially-throwing calls are an error until M8: "call to 'ns.f' may throw - C++
  exceptions are not supported yet". Detection is Clang's `noexcept` analysis. The CLI
  flag `--cpp-assume-noexcept` downgrades the gate (a thrown exception terminates); it
  exists so libc++ containers are usable before M8 and is off by default.
- `--check`, `--out-lli`, `-o`, and `--run` all work; companion-module static initializers
  run under `--run`.

### Exceptions - surface options for M8

CFlat has no `try`/`catch`. Options, cheapest first: (1) contain at the boundary, foreign
exceptions never enter CFlat frames, terminate with a diagnostic; (2) propagate through
CFlat frames with correct cleanup, catch only in C++ (the M8 exit criterion); (3) add
`try`/`catch` for foreign types. Recommendation: ship 1 with the flag above, upgrade to 2
at M8, keep 3 out of scope until a library demands it. RULING at M8 start.

## Milestones

Status legend: DONE (landed on feature/cpp-interop, verified by the host suite), PARTIAL,
OPEN. Commits are listed in "Landed history".

### M0 - Prove the Clang integration seam - DONE

Probe and contract in `scratch/cpp_abi_proof/` of the main checkout (`CONTRACT.md`).
Verdict: direct ABI through PUBLIC installed APIs, no Clang patch. `arrangeFreeFunctionType`
/ `CGFunctionInfo` / `ABIArgInfo`, `CodeGenerator::GetAddrOfGlobal` (structors with
Clang's own arrangement), `GetAddrOfVTable`, `ItaniumVTableContext::getMethodVTableIndex`,
`HandleTopLevelDecl`/`HandleTranslationUnit` emission all work against LLVM 23.1.0. Only
`CodeGenModule.h` is not installed; its structor arrangement is substituted by
`GetAddrOfGlobal`. LLVM 23 API drift recorded there (`TargetParser/Host.h`,
`getCanonicalTagType`, `CanQual<FunctionProtoType>`, `llvm::Triple`).

### M1 - Import sessions and foreign symbol/type identity - DONE

`import cpp` mode; qualified dotted names; mangled linkage names; `extern "C"` passthrough;
anonymous-namespace skip; scoped enums; overloads distinct across namespaces; C interop
unchanged. Header disk cache keyed on C++ mode.

### M2 - Direct free-function calls and basic native linking - DONE

Scalars, enums, pointers, references-as-pointers; `noexcept` gate with LogError at the
call and at function-pointer binding; `.cpp` inputs compiled by clang++; C++ standard
library and runtime selected at link (libc++ on macOS, msvcprt on Windows, untested there).

### M3 - Record layout and trivial aggregate ABI - DONE

Per-function ABI plan from Clang (`RawAbi`), Clang-free, serialized in the header disk
cache and the `--init` cache; Direct, Extend, Indirect (sret/byval), Ignore supported;
Expand, CoerceAndExpand, InAlloca refused. Record layout padded to Clang's field offsets
and verified for size, alignment, and every offset; packed and over-aligned records pass.

### M4 - Classes, lifetime, and nontrivial values - DONE

Members extracted (ctors, dtor, methods, statics, access, triviality). Construct-into-slot
path (`TryDeclareForeignCxxLocal`): `T(args)`, `= default`, no initializer, copy, move,
sret return straight into the slot. Destructor through the existing owning-local cleanup,
proven with counters on scope exit, nested scopes, early return, break, continue,
discarded temporaries, `_ = move x`. Copy/move assignment. Nontrivial by-value params and
returns. `new`/`delete` through the global C++ operators. Refused: class-specific
`operator new`/`delete`, by-value args from non-addressable expressions.

### M5 - Inline definitions, operators, and templates - DONE (acceptance met)

M5a: companion bitcode per import group emitted by Clang CodeGen (inline functions,
methods, structors, statics, keyless vtables and RTTI), linked before optimization;
header-only libraries work; `--run` executes them; disk cache stores the blob.
M5b: class templates instantiated from CFlat spelling through a per-request
explicit-instantiation TU (`RequestCxxForeignType`: stage 1 extracts members and
signatures, stage 2 ODR-uses every member and adopts the companion bitcode); identity is
Clang's canonical type, so two spellings of one specialization share a registration;
defaulted/implicit special members are defined by Clang; `operator[]` binds to the index
expression and `operator==` to the overload table. Acceptance met on libc++:
`std.vector<int>`, `std.vector<double>`, `std.string` (ctor from literal, append, size,
c_str, compare), `std.vector<std.string>`, `std.vector<cppi.Tracked>` with every element
destroyed exactly once. `--cpp-assume-noexcept` (off by default, honoured by the LSP)
makes non-noexcept libc++ members callable until M8; tests opt in through a first-line
`// cflat-args:` comment read by `test.sh`/`test.bat`. The `String` token is now legal
after a dot in the grammar so `std.string` parses.

Not covered, refused with precise errors: nontrivial class parameters at C++
constructors; free operator templates (call-site deduction, e.g. libc++ string
`operator==`); specializations in expression position. The per-request TU is the interim
mechanism; the plan's live Sema session remains the eventual home and is what call-site
deduction needs.

### M6 - Inheritance and virtual dispatch - DONE

Polymorphic classes with vptr and nonvirtual base subobjects from Clang layout; virtual
calls through Clang's vtable slot; deleting destructor on `delete`; derived-to-base
adjustment at args, `this`, and stores; inherited members; covariant returns only at
offset zero. Refused: virtual bases, abstract construction, `is`/`as`, member pointers,
thunk-requiring covariants. Two review rounds (2026-09-06) resolved; the non-primary-base override finding was
refuted against the clang oracle and pinned by a regression test.

### M7 - Callbacks and reverse ABI entry points - OPEN

Generate CFlat function entries with the same foreign ABI arrangements used for calls.
Support eligible free-function callbacks, including aggregate arguments/results and
linkage declarations. Define callback lifetime and reject unsupported capturing closures.
Check that foreign code cannot silently unwind into an unprepared callback frame.

Exit: C++ calls a CFlat callback and receives correct aggregate results; callbacks nested
inside foreign calls preserve object lifetime and use matching ABI attributes at both ends.

### M8 - Exceptions and unwind-safe cleanup - OPEN

Define the language contract for foreign exceptions (see surface options above).
Implement target-specific personality, invoke/unwind edges, cleanup pads/landing pads, and
destructor ordering. Cover partially constructed objects, temporary arguments, callback
frames, and destructors during unwinding. Different target EH models require separate
lowering paths.

Exit: C++ throws across a CFlat frame and is caught in C++, with every live CFlat and C++
owned object cleaned up correctly; a CFlat-side catch and noexcept termination verified in
a subprocess. Only then lift the potentially-throwing-call restriction and retire
`--cpp-assume-noexcept`.

### M9 - Production imports, runtime support, cache, and tooling - OPEN

Validate realistic standard-library use (`string`, `vector`, `unique_ptr`) beyond the M5b
acceptance: explicit CFlat conversions and borrowed-view lifetime behaviour. Verify
toolchain selection and runtime linking for supported ABI/standard-library profiles;
record incompatible or unverified profiles accurately. Complete persistent caches keyed by
transitive headers, macros, standard, target, ABI, SDK/runtime identity, exact Clang
build, and requested specializations. Foreign hover, completion, definition navigation,
and Clang diagnostics at CFlat call sites. Reanalysis after failed imports must not leak
stale declarations, value categories, or cleanup state.

Exit: representative header-only and separately compiled libraries work from cold and
warm caches, and header/config changes force correct regeneration.

### M10 - CFlat-defined implementations of C++ classes - OPEN

Subclassing C++ types in CFlat: overrides, layout ownership, base construction, virtual
destructors, RTTI identity, vtable emission, cross-language `dynamic_cast`/`typeid`.

Exit: C++ owns a CFlat-defined derived object through a C++ base pointer, invokes
overrides, and destroys it correctly, including multiple inheritance if advertised.

## Sequence and release gates

- M0-M4, M5a, M6, M5b landed in that order; the M6 review fixes and the M5 review are next.
- After M5b: M9's cache and tooling items, then M7, then M8. M10 is the final expansion.
- Every milestone ships an explicit capability boundary and rejects unsupported uses. Do
  not advertise unrestricted C++ interop based on successful scalar calls.
- Windows/MSVC is unverified for everything past M1: structor variants, vtable layout under
  multiple inheritance, deleting destructors, and allocator spellings differ. A Windows
  pass with `test.bat` is required before the branch is considered cross-platform.

## Rulings

Ruled 2026-09-06: import spelling (`import cpp` before the path); prototype order
(primitives and pointers, then trivial records, classes, polymorphism, templates);
move-from (consumed, dtor still runs); const overloads (prefer non-const, revisit);
construction spelling (`T(args)`, `= default`); `--cpp-assume-noexcept` as the interim
exception stance.

Open: per-import `std` clause or CLI-only; exceptions option at M8 start; MSVC ABI pass.
Open from the M5 review (2026-09-06):
- Rvalue-reference parameters (`push_back(T&&)`): today `push_back(move t)` binds the copy leg
  because the dedup collapses the `T&&` overload; the source's destructor still runs, so
  ownership stays correct but a move is silently a copy. Mapping `T&&` to CFlat `move T`
  would suppress the source destructor, which a C++ move must not do. Needs a spelling
  ruling: a distinct parameter kind that selects the rvalue overload and leaves the source
  destructor armed.
- LSP and template CodeGen: type requests still run stage-2 CodeGen under the LSP so the
  editor's member surface equals the compiler's. Either accept the cost or shrink the LSP
  surface to stage 1 only. Needs a ruling. Disk caching of type requests (beyond the
  in-memory request cache) is also pending a ruling on whether `cache` opts them in.
- `v[0]` on `std.vector<char*>` yields the reference rather than the element (`int&` decays,
  `char*&` does not); repro in scratch/findings_m5/p3t.cb. Fix direction: treat `T*&` like
  `T&` at the index-expression result.

## Landed history

| Commit | Content | Suite |
|--------|---------|-------|
| 56907bc3 | `import cpp`, primitives and bare pointers (M1, M2) | 796 |
| c08294f3 | trivial records by value, Clang calling convention (M3) | 796 |
| de9eee05 | class members, methods, statics, access control (M4a) | 800 |
| 103c89f0 | construction, destruction, copy/move, by-value, new/delete (M4b) | 806 |
| 05668d44 | polymorphic classes, virtual dispatch, base conversions (M6) | 810 |
| 8c134f5c | inline definition emission through Clang CodeGen (M5a) | 812 |
| 4439a456 | class templates, std::vector and std::string (M5b) | 818 |
| 76c3962c | resource-dir bake-in and LSP sweep flag (M5b follow-up) | 818 |
| 52d8537c | first review round resolved (reset leaks, token stripping, demangle) | 818 |
| 952cbe32 | second review round: cache mode keys, ABI sink RAII, template arg pointers, request cache, namespace gating, blob budget, data-layout check | 818 |

## Verification and repository constraints

For each milestone, extend `Test/test_c_interop.cb` (sections M, M3-M7 so far) and the
`Test/library/cpp_interop_*` fixtures; error fixtures are `Test/errors/err_cpp_*.cb`. Use
`scratch/` for prototype libraries, oracles, IR, and logs. Clang-compiled callees/callers
are the ABI oracle (`clang++ -S -emit-llvm -O0 -target arm64-apple-macosx11.0.0`). Assert
observable values, offsets, dispatch targets, and lifetime counters, not just successful
compilation. Include negative tests and cold/warm cache cases as each feature lands.

After compiler changes, build Release and run the current host's full suite
(`./cmake_build.sh release`, `./test.sh Release`; the batch scripts on Windows), plus
`test_lsp.sh` for symbol-registration changes and `test_example.sh`.

Preserve C interop byte-for-byte. Apply declaration-specifier changes to both passes.
Serialize every new analysis-relevant field in the header disk cache and the `--init`
round-trip in the same change. ASCII only; LogError/LogErrorContext only. Do not modify
root `vcpkg.json`. Substantial implementation goes through the delegated implementation
and main-session review workflow; every milestone is verified by re-running the suite in
the main session before it is committed.

## References and API caution

Repository anchors: `CMakeLists.txt`; `cflat/CClangExtract.h/.cpp` (extraction, ABI plans,
member tables, definition emission); `cflat/LLVMBackend_CInterop.cpp` (registration,
class info, destructor hook, virtual dispatch, base adjustment); `cflat/
LLVMBackend_ControlFlowAndFunctions.cpp` (recipe from Clang plan);
`cflat/LLVMBackend_MoveDataflow.cpp` (`EmitAbiLoweredCall`);
`cflat/LLVMBackend_EmitAndLink.cpp` (companion link, native link);
`cflat/LLVMBackend_StateAndImports.cpp` (header disk cache);
`cflat/MainListener_Declarations.cpp` (`TryDeclareForeignCxxLocal`);
`internal/llvm-from-source-build.md`; `internal/testing-notes.md`.

- [Clang tooling interfaces](https://clang.llvm.org/docs/Tooling.html)
- [Clang CodeGen ABI API](https://clang.llvm.org/doxygen/CodeGenABITypes_8h.html)
- [Clang CodeGen type lowering](https://clang.llvm.org/doxygen/classclang_1_1CodeGen_1_1CodeGenTypes.html)
- [MSVC compatibility](https://clang.llvm.org/docs/MSVCCompatibility.html)

Online Doxygen follows development Clang and is architectural reference only; verify
exact API availability against the repository's LLVM 23 installation.
