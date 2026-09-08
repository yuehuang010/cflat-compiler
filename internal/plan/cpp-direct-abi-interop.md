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
| `T&&` param | `move x` arg or a temporary; lvalue binds the `const T&` leg | Rvalue-alias param kind, ruling 2026-09-06 |
| `T&` return | `alias T` | Existing borrow keyword; caller never frees |
| `T&` field or local | not supported | Explicit error |
| `class C` / `struct C` | `C` (a foreign class) | Layout from Clang |
| `template<class T> class V` | `V<T>` | CFlat generic angle syntax |
| `template<int N>` | `V<T, N>` | Existing value generic parameters |
| `std::string`, any `std::` type | `std.string`, `std.T` | Just foreign types, no CFlat sugar (rulings 2026-09-06, 2026-09-07); `string_view` from `char*`; iterators are bound classes |
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

### M7 - Callbacks and reverse ABI entry points - DONE 2026-09-06 (uncommitted)

Generate CFlat function entries with the same foreign ABI arrangements used for calls.
Support eligible free-function callbacks, including aggregate arguments/results and
linkage declarations. Define callback lifetime and reject unsupported capturing closures.
Check that foreign code cannot silently unwind into an unprepared callback frame.

Exit: C++ calls a CFlat callback and receives correct aggregate results; callbacks nested
inside foreign calls preserve object lifetime and use matching ABI attributes at both ends.

Landed (uncommitted, 2026-09-06): clang CodeGen plans every C++ function-pointer pointee type
(`arrangeFreeFunctionType`, replayed through the header cache); a CFlat function handed to a
foreign fn-ptr slot whose recipe has lowering gets one internal `nounwind` reverse thunk per
(function, fn-ptr type) that unpacks sret/byval/coerce-pair/HFA slots, calls the natural
function, and repacks the result. Scalar C callbacks are unchanged (no thunk). Reference
callback params are spelled as pointers on the CFlat side. Refused with LogError: nontrivial
class by value (param or result) and `T&&` in a callback signature. Verified: Mixed/Large/Hfa
thunk signatures equal clang's; `visit_tracked` lifetime counters exact; M7 negative fixtures.
Also landed (uncommitted, 2026-09-06): `std.function<R(Args)>` - a bare function type
(`int(int)`) is a legal class-template argument (grammar rule `functionTypeArgument`, both
parser passes); construction from a plain CFlat function instantiates libc++'s converting
constructor with the function-pointer type and reuses the reverse-thunk path; `f(args)` binds
the imported `operator()`; `const std::function&` params bind the alias leg. A capturing
closure is refused with LogError (`err_cpp_std_function_capturing.cb`). Host verification:
test.sh 828/0/8, test_lsp.sh green, test_example.sh 45/0.

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

### M10 - CFlat-defined implementations of C++ classes - OUT OF MVP (ruling 2026-09-06)

Subclassing C++ types in CFlat: overrides, layout ownership, base construction, virtual
destructors, RTTI identity, vtable emission, cross-language `dynamic_cast`/`typeid`.

Exit: C++ owns a CFlat-defined derived object through a C++ base pointer, invokes
overrides, and destroys it correctly, including multiple inheritance if advertised.

## Sequence and release gates

- M0-M4, M5a, M6, M5b landed in that order; the M6 review fixes and the M5 review are next.
- MVP scope (ruling 2026-09-06): CFlat is a CONSUMER of C++ code only. M10 (CFlat-defined
  C++ classes) is out of the MVP. Order after the rvalue-reference work: finish the review
  debt (M6 fixes, M5 review, `T*&` index result), then M7 callbacks, then M8, then the M9
  cache and tooling items as rulings land.
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

Ruled 2026-09-06, rvalue-reference parameters (`T&&`). Root cause of the M5 finding: the type
mapper folds `&` and `&&` onto the same `alias T` (LLVMBackend_CInterop.cpp, reference
mapping in the C type mapper), so both legs spell one CFlat signature, and the instance-method
dedup keeps only the lvalue leg. `push_back(move t)` therefore silently copies. Ruling:
1. `T&&` is a distinct parameter kind, "rvalue alias": same ABI as `alias T` (address passed),
   different overload identity. The dedup keys on it, so both `push_back(const T&)` and
   `push_back(T&&)` are bound.
2. Resolution mirrors C++: a `move x` argument or a produced temporary prefers the rvalue leg;
   a plain lvalue binds the `const T&` / `T&` leg and never the rvalue leg. When only the
   rvalue leg exists an lvalue argument is an error naming `move`.
3. `move x` on a C++ value is a cast to rvalue, not a CFlat ownership transfer: `x` is
   compile-time consumed (use-after-move is an error, as today), it is NOT zeroed (CFlat cannot
   know what zero means for the type; the C++ move constructor decides the residual state), and
   its C++ destructor stays armed and runs at scope exit. Reassignment re-initializes `x`,
   which already holds for CFlat locals (probed 2026-09-06). No lost utility: every C++ idiom
   that reads a moved-from object (algorithm slot reuse, swap-based move assignment, partial
   member moves) lives inside C++ code that clang compiles into the companion module.
4. `T&&` never maps to CFlat `move T`: that kind suppresses the source destructor, which a C++
   move must not do. The compile-time consume is the only thing the two share.
5. Cost: the zeroing CFlat performs on a native move is folded away by mem2reg for a
   non-escaping local (probed at -O2, 2026-09-06); the kept C++ destructor on a moved-from
   value folds the same way once the companion bitcode is inlined.
6. `move p` on a `T*` binds the rvalue leg with the POINTEE's address (a `T*` lvalue already
   binds `const T&` through alias-by-pointer); the heap object is still destroyed at scope exit.
Implemented 2026-09-06 in the worktree (uncommitted): `IsRvalueRef` on the parameter
descriptor, keyed in the member dedup, serialized as `rr` in both caches; overload resolution
rejects lvalues with a diagnostic naming `move`; coverage in `Test/test_c_interop.cb` M10/M11 and
`Test/errors/err_cpp_rvalue_needs_move.cb`.

Review round 2026-09-06 (afternoon, on the uncommitted rvalue/T*&/M7/std.function work):
10 findings, all fixed and re-verified on macOS (test.sh 828/0/8, test_lsp.sh green,
test_example.sh 45/0). Notable: `T*&` PARAMETERS are lowered by address (the first fix only
covered results); fn-ptr plan keys normalize pointer depth on both sides; callback refusals
return after LogError; Ignore-slot callbacks are refused; bases reachable from an in-scope
record are collected; `functionTypeArgument` routes through ResolveForwardTypeArg in the
scanner; std.function copy-construct binds. Follow-up not done: passing `&p` (a `T**`) to a
`T* const&` parameter is accepted rather than diagnosed (the old M10 spelling silently
corrupted the stack before the tests were corrected) - add a type check.

Cleanup round 2026-09-06 (evening, the review's non-correctness feedback, no features):
explicit value category `NamedVariable::IsRvalue` set at value-producing sites (literals,
call/constructor results, lambdas) replaces the storage heuristic; a temporary attributed
to a live variable (`k.toString()` keeps `k` as owner name) is never consumed implicitly
(`IsConsumableTemporary`); rvalue preference stays restricted to aggregates. Grammar:
`memberNameToken` shared by member access and qualified names (the `if const` enum folder
now reads the rule node). One helper each for fn-ptr signature unpacking, std::function
spelling split, ABI description, fn-ptr plan queueing, reverse-thunk piece packing; dead
`fpTV` parameter dropped; `IsStdFunctionSpecialization` replaces four string tests;
capturing-closure refusal generalized to every thin fn-ptr constructor parameter;
operator() force-mark limited to requested records; std::function spellings memoized.
Codex reported three regressions as pre-existing; all were from the round and fixed on the
host. Verified: test.sh 828/0/8, test_lsp.sh green, test_example.sh 45/0, 18 err_cpp
fixtures. Snapshot scratch/cleanup_done_state.patch.

M8 design spike 2026-09-06 (read-only, scratch/M8_SPIKE.md): six call emission sites need
an invoke twin; per-function may-throw bit already serialized in both caches; today a C++
throw across a CFlat frame ABORTS (no unwind tables), it does not leak; `--run` registers
no EH frames on macOS/Linux; option (2) is medium effort and the only one meeting the exit
criterion. Five rulings listed at the end of the doc. Parked: the maintainer chose to
finish the Types table first (gap matrix scratch/TYPE_GAPS.md, round 1 brief
scratch/TYPES1_BRIEF.md).

Types round 1, 2026-09-06 (uncommitted, verified on the host: test.sh 834/0/8, LSP green,
examples 45/0, 21 err_cpp fixtures): `wchar_t`/`char16_t`/`char32_t`/`std::nullptr_t`
mapped; `__int128` and C++ `long double` refused; a dropped C++ signature keeps its refusal
reason (also in the header cache, version 20) and a call to it reports the unsupported
parameter type; reference fields and pointer-to-member types refused with fixtures; enum
width and signedness from Clang's underlying type; unscoped enumerators published as
`ns.E.A`; enums as template arguments; integer value template arguments (`Buf<int, 4>`,
`std.array<int, 4>`); `T&` returns of free functions are `alias T`; fixtures for nested
class/enum, union, anonymous union, bitfields (offset verification re-enabled), array
fields. Round 2 (in flight): class statics shadowed by a nested-type namespace (bug found
on the host), default arguments, enum constants as value arguments, operator whitelist and
conversion operators, typedefs of specializations and alias templates.

Types round 2 + 2b, 2026-09-06 (uncommitted, verified on the host: test.sh 836/0/8, LSP
green, examples 45/0, 22 err_cpp fixtures): class statics no longer shadowed by the
nested-type namespace; enum constants as value template arguments; CONSTANT default
arguments (integer/bool/enum/float/nullptr folded at extraction, carried in both caches,
header cache version 21; a non-constant default omitted at the call is refused,
`err_cpp_default_arg_nonconst.cb`); binary/compound/unary operator whitelist widened
(`<= >= % << >> & | ^` and compound forms, unary `-` `!`) with receiver guards; typedef of
a class-template specialization resolves to the one specialization identity
(`cppi.IntVec` == `std.vector<int>`). Not bound by ruling pending: `++`/`--` and conversion
operators (`operator bool`, `operator T`) - naive binding polluted primitive casts.
Found on the host: constructors of a TRIVIALLY COPYABLE class (`Counter`, no destructor)
are not bound (`cppi.Counter(3)` unknown) because the `T(args)` path is gated on
IsForeignNontrivialCxxClass; and a direct `Counter <= Counter` in the test file tripped an
unrelated `ML_SLOW` assertion that a standalone probe does not reproduce. Both are round-3
items (scratch/TYPES3_BRIEF.md) together with `__int128` -> i128/u128 (branch rebased onto
master 84da82a6), alias templates, and a minimal `std::unique_ptr` surface.

Types round 3, 2026-09-06 (uncommitted, branch rebased onto master 84da82a6; verified on
the host: test.sh 838/0/8, LSP green, examples 45/0, 22 err_cpp fixtures): `__int128` ->
i128/u128 (the unsupported-type fixture now uses `_Float16`); constructors of trivially
copyable classes bind for `T(args)` while the by-value ABI stays trivial; direct operator
assertions (`<= >= % << &`, unary `-`, `+=`) - the earlier `ML_SLOW` anomaly did not
reproduce; alias templates (`Vec<T> = std::vector<T>`) register onto the specialization
funnel (header cache version 22). NOT reached: `std::unique_ptr` - a header that merely
DECLARES a function returning `std::unique_ptr<Thing>` crashed the compiler (SIGSEGV in
getASTRecordLayout on the never-instantiated specialization); guarded in
EmitDefinedRecord (`isCompleteDefinition`), so the signature is now dropped with a bind
refusal instead. Round 4: instantiate named-but-uninstantiated specializations from a
signature (or bind them lazily when CFlat names the type), then `unique_ptr`,
`pair`/`optional`/`map`, `T (&)[N]`, conversion operators after the ruling.
Found on master while at it: the i128 wide-literal parser asserted in Debug on every
compile (it received char-literal text such as `'/'`); fixed on master, uncommitted.

Types round 4, 2026-09-06 (uncommitted; verified on the host: test.sh 840/0/8, LSP green,
examples 45/0, 23 err_cpp fixtures, header cache version 24): class-template
specializations that a signature names but the header never instantiates are completed by
one extraction retry with explicit `template class` instantiations appended (the bind
refusal stays when the retry cannot complete them); `std::unique_ptr<T>` minimal surface
(`get`, `release`, `reset`, scope destruction, `move` consumption, deleted copy refused by
`err_cpp_unique_ptr_deleted_copy.cb`); `std::pair<A,B>` by value with `first`/`second`;
`std::optional<T>` with `has_value`/`value`; `T (&)[N]` and `T (*)[N]` parameters map to a
pointer to the first element (extent kept in `ConstArraySize`, not enforced). `operator->`
and `if (p)` on `unique_ptr` are NOT bound (no CFlat-side hook yet, see rulings below).
LESSON (caught on the host, Codex reported it as "a suite stage issue"): the cold
extraction requested the specializations named by signatures, but neither header-cache
hit branch (in-memory `cFileSigCache_`, disk cache) did, so the SECOND file compiled in one
process refused `make_tracked_ptr` as "an indirect non-record argument". The pseudo-locale
discovery stage of `test.sh` compiles every error fixture in one process and exposed it (14
fixtures). Fix: `CSigEntry::retSpelling` (serialized `rspell`, cache v24) plus a
`RequestCxxSignatureTypes(const std::vector<CSigEntry>&)` replay before
`RegisterCSignatures` on both hit branches. Rule going forward: whatever the cold path
derives from an extraction result and registers must be re-derived on both cache-hit
branches from serialized fields; the two-file `--check` repro is the fast test.
Snapshot scratch/types4_done_state.patch + scratch/types4_untracked.txt.
Round 5 (scratch/TYPES5_BRIEF.md): `std::shared_ptr` surface, statics of a specialization,
`static constexpr` under declaration-only import, qualified typedef names, variadic member
functions (bind or refuse), `std::map` as a stretch.

Types round 5, 2026-09-06 (uncommitted; verified on the host: test.sh 842/0/8, LSP green,
examples 45/0, 24 err_cpp fixtures, two-file cache repro clean, header cache version 25):
`std::shared_ptr<T>` minimal surface (`get`, `use_count`, `reset`, copy shares ownership,
`move` consumption, exact destruction count); statics of a class-template specialization
(`cppi.Registry<int>.count`, `cppi.Registry<int>.bump()`); `static constexpr` data members
fold to compile-time constants under a declaration-only import (no symbol reference);
qualified typedef names (`cppi.Outer.Id`); variadic member functions refused at the use
site (`err_cpp_variadic_method.cb`). `std::map` stretch not started. Snapshot
scratch/types5_done_state.patch + scratch/types5_untracked.txt. Round 6
(scratch/TYPES6_BRIEF.md): `std::map<K,V>` minimal surface, non-constant default arguments
via companion forwarding wrappers, `std::vector` iteration surface (`begin`/`end` bound as
pointers where contiguous) as a stretch.

Types round 6, 2026-09-06 (uncommitted; verified on the host: test.sh 842/0/8, LSP green,
examples 45/0, 24 err_cpp fixtures, two-file cache repro clean, header cache version 26):
`std::map<int,int>` minimal surface through the specialization funnel (`size`, `count`,
`at`, `operator[]` read/write, `erase`, by-value return works on this ABI); non-constant
default arguments now forward through generated companion-module wrappers per omitted
suffix arity (`with_call_default(1)`), the refusal stays for defaults the wrapper cannot
reproduce (a private member default, `err_cpp_default_arg_nonconst.cb`). `std::vector`
`begin`/`end` stretch not started. Snapshot scratch/types6_done_state.patch +
scratch/types6_untracked.txt. Next: rebase past the master operator commits, then bind C++
`operator++()`/`operator--()`, `operator*`, `operator->`, `operator bool` onto the CFlat
hooks; `std::vector` iteration; rulings pending for `operator T` and `std::string_view`.

Types round 7, 2026-09-06 (uncommitted; verified on the host: test.sh 842/0/8, LSP green,
examples 45/0, 24 err_cpp fixtures, two-file cache repro clean, header cache version 26
unchanged): `std::vector<T>` `data()`/`begin()`/`end()` bound as `T*` (the iterator class
is never bound; a pointer loop iterates); `std::array<int, 4>` by value (`size`,
`operator[]` read/write, `data`) through the fixture typedef `cppi.IntArray4`. GAP found:
the CFlat spelling `std.array<int, 4>` parses as `std.array<int<4>>`, so a non-type template
argument cannot be written directly in a generic type spelling yet (integer template args
work when the header names the specialization); a later round needs the grammar/parser
path for `<T, N>` with an integer literal. Snapshot scratch/types7_done_state.patch +
scratch/types7_untracked.txt.
Branch rebased onto master 3c9344ed (operator++/--/*/->, operator bool, i128 literal fix)
2026-09-06 22:50: one conflict in MainListener_PostfixExpression.cpp because master moved
the operator-> forward-on-miss loop into the `ForwardOperatorArrow` helper (pointer
receivers deref first); the branch's only uncommitted delta there (the `false` argument to
`PrepareAliasCallResult`) was ported into the helper and the inline copy dropped. Debug
now compiles the interop test again (the literal fix is on master).
Debug assertion sweep 2026-09-06 23:32 (after the rebase, first time the interop test could
run under the assertion-enabled Debug build; all four were silent garbage in Release):
(1) CClangExtract CollectFields double-incremented its hand-counted field index after an
anonymous struct/union member, so every later field read the next slot's offset
(mathlib.h ML_Overlap) - replaced by clang's own `FieldDecl::getFieldIndex()`;
(2) `ParmVarDecl::getDefaultArg()` on a template member's uninstantiated default asserted -
classified "nonconst" before touching the expression; (3) enum constants of an unsigned
8-bit backing (`Small8::Value = 200`) built a signed APInt that does not fit - now truncated
to the backing width; (4) a `std::map<int,int>` member whose parameter maps to an LLVM type
that is not a legal function argument was declared anyway (llvm::FunctionType asserts) -
`RegisterCxxClassMembers` now validates every parameter with
`FunctionType::isValidArgumentType` and refuses the member with "takes unsupported type";
(5) the covariant-return check called `getBaseClassOffset` for an INDIRECT base
(direct-bases-only API) - it now walks the `CXXBasePaths` path and sums the offsets, and the
two direct-base layout sites use `getVBaseClassOffset` for a virtual base. Verified: Debug
interop compile+run exit 0, Debug err_cpp_virtual_base and test_operators green, Release
846/0/8, LSP, examples 45/0, 24 fixtures, two-file cache repro clean. Snapshot
scratch/rebase_done_state.patch + scratch/rebase_untracked.txt. Rule: from now on every
interop round runs the interop test under Debug as part of its bar.

Types round 8, 2026-09-07 00:45 (uncommitted; verified on the host: Debug interop
compile+run exit 0, Debug err_cpp_virtual_base and test_operators green, Release test.sh
848/0/8, LSP green, examples 45/0, 25 err_cpp fixtures, two-file cache repro clean; header
cache v27). Binds the C++ operators onto the master 3c9344ed hooks: `CClangExtract.cpp`
exports prefix-only `operator++`/`operator--` (the `(int)` postfix overloads are filtered),
unary and binary `operator*`, `operator->`, and a conversion `operator bool` (other
conversion operators stay unbound). `OperatorBoolFunctionNameForType` keeps the by-value
first-parameter rule for CFlat records and, for a registered C++ record only, also accepts
the pointer receiver a C++ member conversion emits. Covered by section M18 of
Test/test_c_interop.cb (Cursor ++/--, unique_ptr/shared_ptr `*` and `->` forwarding with
writes through the payload, `!p`, `(bool)p`, Truthy conditions) and
Test/errors/err_cpp_operator_bool_implicit.cb (`bool b = p` stays an error). Existing
RawCxxMember fields carry the new members through both cache branches. Snapshot
scratch/types8_done_state.patch + scratch/types8_untracked.txt.
Codex reported a Debug `--check` assertion (`M->isMaterialized()`) on the AbiPair extern
and correctly called it pre-existing: it reproduces on master with a three-line CFlat file.
Root cause: `--check` never materializes the lazily loaded core bitcode and
`llvm::Value::use_empty()` asserts on a non-materialized module. Fixed on master
(uncommitted) by using `materialized_use_empty()` at the four placeholder-proto sites in
LLVMBackend_ControlFlowAndFunctions.cpp and LLVMBackend_CodegenHelpers.cpp - our own protos
only ever have materialized uses, so this is exactly what Release already computed. Master
bar green (798/0/8, LSP, examples, Debug probe). The worktree picks it up at the next rebase.

Types round 9, 2026-09-07 03:25 (uncommitted; verified on the host: Debug interop
compile+run exit 0, Debug fixtures green, Release test.sh 848/0/8, LSP green, examples 45/0,
25 err_cpp fixtures, two-file cache repro clean; header cache v28). Ruling: "long double
should use the target os/platform, aka follow the C++ usage". The extractor now reads the
long double width, IEEE-double semantics and the triple from the clang invocation that
produced the AST; the facts ride the extraction result and the header cache (fields ldw,
ldieee, triple) and are replayed on the in-memory and disk cache-hit branches. Rule for C
and C++ alike: width 64 with IEEE double semantics -> CFlat double (Apple arm64, MSVC x64);
anything else (x86-64 SysV x87 80-bit, aarch64 Linux quad) -> recorded refusal naming width
and triple. Replaces the targetWindows_ special case and the unconditional C++ "x87"
LogError. Coverage: C++ long double param/return/pointer-out round-trip in
Test/test_c_interop.cb section O. No non-64-bit fixture: the CLI has no target-triple
option, so the refusal path is unexercised on this host. Snapshot
scratch/ldbl_done_state.patch + scratch/ldbl_untracked.txt.
Master side (worktree ../cflat-operators, branch feature/operator-parity, uncommitted,
verified 802/0/8): operator() and `?.` forwarding through operator-> (survey showed every
other member operator already worked), and `operator T` conversions as free functions
`T operator T(S s)` bound at explicit casts only. Next interop round after those commit:
rebase and widen isBindableOperator so C++ operator(), unary -, and conversion operators
bind onto the new hooks.

Types round 10, 2026-09-07 05:34 (opus subagent during a Codex quota dip; uncommitted;
verified on the host: Debug interop compile+run exit 0, Debug fixtures green, Release
test.sh 854/0/8, LSP green, examples 45/0, 26 err_cpp fixtures, two-file cache repro clean;
header cache v29). Branch stacked on feature/operator-parity 09de80a8 (operator(), `?.`
forwarding, `operator T` at explicit casts). Exported: every operator() overload on every
class (the requestedRecord gate is gone), operator~ (arity 0), and every conversion operator
(explicit and non-explicit alike - CFlat binds only at explicit casts anyway; new
RawCxxMember::isConversion, serialized "cvn"). Conversions publish as
"operator <CFlat spelling of the target>" derived at publish time on every path (so both
cache-hit branches replay without a hook); a target with no CFlat spelling stays a per-member
refusal. Master-side cast path accepts the pointer receiver for a registered C++ record (the
round-8 operator bool relaxation) and reports a refused conversion's recorded reason.
Unary -/+ needed nothing. Section M19 (725-739) and
Test/errors/err_cpp_operator_conversion_implicit.cb. Snapshot scratch/cxxops_done_state.patch.
Observation: the Debug interop compile grew from ~11 to ~37 minutes as main() gained
sections; the hot spot is still nulldf::ComputeControlDependence under the mimalloc debug
heap (deferred by ruling until interop is stable). Next queued: round 11
(scratch/TYPES11_BRIEF.md) - direct `std.array<int, 4>` spelling and static member writes.
On master the same evening (uncommitted, verified 796/0/8 Release, Debug operators test
green, LSP, examples): CFlat `operator bool` as a free function `bool operator bool(T)`,
firing only in `if`/`while`/`do`/`for` conditions, `?:`, `&&`/`||`, unary `!`, and the
explicit `(bool)` cast; never implicit elsewhere; a pointer to a struct keeps the null
test. C++ `operator bool` binds onto it after the branch rebases past that commit.
RULINGS STILL NEEDED before the next rounds: (a) CFlat has no user-definable `++`/`--`, no
unary `*` overload hook and no `->` hook on struct values, so binding C++ `operator++()`,
`operator*`, `operator->` (unique_ptr, iterators) needs a CFlat-side surface decision
(foreign classes only, or a general operator overload); (b) `operator T` conversions
(proposed: explicit casts only); (c) `std::string_view` parameters (touches the separate-
strings ruling: a CFlat `string` argument to a `string_view` parameter would be a borrow).

RULING 2026-09-06 (operators): CFlat adds `operator++`, `operator--` (postfix spelling
only, in place), unary `operator*` and `operator->` as struct member functions, built on
master first (scratch/OPFWD_BRIEF.md in the main checkout). `operator.` is NOT
overloadable: CFlat `.` is a smart forward - a direct member when the name belongs to the
struct, otherwise through `operator->` (chained). The C++ binding follows after the branch
rebases past that commit: `c++` -> C++ prefix `operator++()`, `*p` -> `operator*`,
`p.member` -> `operator->` forward. Ruling (a) above is thereby resolved.
Landed on master 2026-09-06 22:20 (uncommitted, verified 798/0/8 Release, Debug operators
test, LSP, examples 45/0): `operator++`/`operator--` (postfix, in place, expression value is
the updated object), unary `operator*` (arity 0), dot forward through `operator->` on value
AND pointer receivers, own members win, chains verified by probe. `?.` on a nullable pointer
receiver does not forward yet (needs chain control flow around the null guard); noted, not
blocking. Diagnostic for a missing overload: "struct T has no operator++; define void
operator++() on it or use a scalar".

RULING 2026-09-06 (increment): CFlat has no prefix `++`/`--` by design (a source of
human error). CFlat `c++` / `c--` on a foreign class binds the C++ PREFIX `operator++()` /
`operator--()` (in place, no copy); the postfix `operator++(int)` overloads are never bound.

RULING 2026-09-06 (strings): `std.string` and CFlat `string` stay separate types with no
conversion surface; users go through `c_str()` / a `std.string` constructor from `char*`.
Sugar may be considered later. The Types table row "explicit conversions" is superseded.

RULING 2026-09-07 (std types are just types): every `std::` type is a foreign C++ type
imported through the same funnel as any user class, with no CFlat-language surface built
around it. No `string` <-> `std.string` sugar, no `string` borrow into `string_view` (a
`std.string_view` is constructed from `char*` like in C++), no bespoke iteration surface
for containers: `begin()`/`end()` return the container's iterator TYPE as a foreign class
and the bound `++`, `*`, `->`, `==`/`!=` hooks drive the loop. The round-7 shortcut that
binds vector `begin`/`end` as `T*` is superseded (kept for `data()`); iterator classes
bind like any nested class-template specialization. Closes the string_view ruling and the
map-iteration question; remaining std work is coverage, not design.

Types round 12, 2026-09-07 (worktree ../cflat-cpp-interop on master 86c9befb, uncommitted;
verified on the host by the main session: Release test.sh 858/0/8, LSP green, examples 45/0,
28 err_cpp fixtures, two-file cache repro clean, Debug interop compile+run exit 0 in 82 s;
header cache v30, new field `ct` canonical base type). First round under the "std types are
just types" ruling. Iterators: `begin()`/`end()` publish the real iterator class
specialization (vector `__wrap_iter`, map `__map_iterator`) instead of the round-7 `T*`
bridge; `data()` stays `T*`. Generic funnel changes that made this work: iterator-returning
members request full nested foreign definitions; ADL free operator templates (vector
iterator `==`/`!=`) are ODR-used by the class request and registered by the free-function
extractor; a friend operator instantiated inside a class carries namespace semantic context
in Clang, so the collector accepts concrete non-member operators whose parameters name the
requested class; the class spelling is published before request signatures are mapped
(parameters otherwise decayed to `void*`). `std::string_view` bound as an ordinary class
(ctor from `char*` and `char*, len`, size/data/[]/substr, by-value pass/return); a CFlat
`string` argument is a plain overload error (`err_cpp_string_view_from_string.cb`).
`std::optional` `value_or`/`emplace`/`reset` bind through the member-template
materialization; libc++ exposes `has_value`/`reset`/`value` via public `using` from a
private base, so using-selected methods join the class surface and one linkage symbol may
carry several receiver types (CreateFunctionDeclaration dedups on full CFlat signature,
not just the LLVM type). Primitive `T&&` parameters: `move n` on a scalar direct call
argument carries the rvalue category and a direct call result is marked rvalue. Variadic
free function `sum_varargs` covered (section M20). Debug-only finds: `string_view::npos`
overflowed the signed APSInt getter (routed through the unsigned helper); the ODR-use probe
now forces only members needing a local definition, keeping user inline constructors.
Master-side probes (move on scalar, extern redeclaration, alias extern) behave the same as
the master binary. Report scratch/TYPES12_REPORT.md. Cost: 4h50m Codex at xhigh, 75 percent
model time. Perf follow-up by the main session the same evening (traces old vs new binary
on the same test): the Debug interop compile had grown 35 s -> 82 s and the Release check
9.7 s -> 22.8 s because type requests went 20 -> 52. Two filters landed in the same commit:
a signature no longer requests a plain qualified name the header itself registered (record
or enum; each such request was two Clang parses for nothing - 10 records and 3 enums here),
and `begin`/`end`/`operator*`/`operator->` request only the non-const overload's type
(registration keeps the non-const twin anyway, so `const_iterator` was requested and never
bound). Result: 52 s Debug, 15 s Release, 30 requests. Tried and reverted: implicit
(sizeof-based) instead of explicit instantiation in stage 1 - no measurable change in
either config, the cost is the libc++ parse itself, not member instantiation. Residual
structure: ~0.5 s Release / ~1.4 s Debug per request for two Clang frontends over the
import prologue. Next levers, largest first: (1) batch all requests of one header into a
single stage-1 and stage-2 TU (`__cflat_req_0..N`), turning ~60 frontends into ~4; (2) a
PCH of the import prologue per process, halving each frontend; (3) lazy iterator/pointee
requests on first CFlat use. Not started; needs a brief.
Remaining from the gap matrix: none open by design; not attempted: unordered_map, set,
tuple, variant, span, initializer_list (coverage rounds on the same funnel).

Real-world round: Dear ImGui headless, 2026-09-07 (main checkout, uncommitted; on master
a5b6a952). Goal per the maintainer: "keep looping until imgui is imported". Spike in
scratch/imgui_spike (not example/): `import cpp "imgui.h"` plus the four ImGui .cpp files,
CreateContext, GetIO by alias, two NewFrame/Begin/Text/Button/End/Render frames, GetDrawData.
Result: compiles, links, runs; frame 2 reports 1 command list, 80 vertices, 246 indices
(frame 1 is empty by ImGui's own first-frame auto-fit rule). Findings, all fixed in the
working tree and filed under internal/issue/cppinterop/ with their status:
- ImVector<T> fields dropped the record (12 records) -> opaque field blob of clang's
  size/alignment (`sz`/`al`/`bo` in the cache, v33); typed access to such a field is still open.
- `*` inside a template argument list counted as an outer pointer (ImGuiPlatformIO 112 vs
  120 bytes) -> only `*` after the last `>` is outer; a by-value spelling keeps resolving.
- Unrepresentable layout was an import-line LogError -> per-record `layoutRefusal`, replayed
  at the use site; bitfield check compares absolute bit positions; named-bitfield extraction
  site now fills `bitOffset`.
- Default-argument wrapper spelled `float (*)(void*, int) a1`; swallowed Sema errors reached
  CodeGen (placeholder-type unreachable) -> identity-template parameter spelling, error-carrying
  decls never reach CodeGen, dropped wrapper -> "unsupported" default (Codex; cache v34).
- `= NULL` on a pointer parameter is `__null` -> classified as nullptr.
- `alias T x = ref()` for a C++ class hit the local-slot path and demanded a destructor ->
  alias declarations bypass `TryDeclareForeignCxxLocal`.
- Overload tie between `Button(a, b = ImVec2(0,0))` and its exact-arity wrapper went to
  declaration order -> fewer default-filled parameters wins the tie in both tiers.
Filed, not fixed: mixed-type bitfields pack by MSVC rules on an Itanium target
(ImFontAtlasRectEntry, per-record refusal only). Verification on the working tree: Release
suite 862/0/8, LSP green, examples 45/0, err_cpp fixtures via test.sh, warm-cache second
pass, Debug interop compile (see session log).

Open: per-import `std` clause or CLI-only; exceptions option at M8 start; MSVC ABI pass.
Open from the M5 review (2026-09-06):
- LSP and template CodeGen: type requests still run stage-2 CodeGen under the LSP so the
  editor's member surface equals the compiler's. Either accept the cost or shrink the LSP
  surface to stage 1 only. Needs a ruling. Disk caching of type requests (beyond the
  in-memory request cache) is also pending a ruling on whether `cache` opts them in.
- RESOLVED 2026-09-06 (uncommitted): `T*&` results/params map to `alias T*` with
  `IsCxxRefToPointer` (serialized `crp`) so the call site materializes the slot; a native CFlat
  `alias T*` keeps its value semantics (the first attempt without the flag broke
  err_move.cb's unique-field borrow diagnostics under the discovery pass).

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
| 3a3a8509 | collapsed headline: direct ABI import M0-M4, M6 (supersedes the rows above) | 856 |
| 9424d6b1 | collapsed headline: inline definitions, templates, std::vector/std::string, review rounds (M5) | 856 |
| 86c9befb | collapsed headline: rvalue refs, M7 callbacks, types rounds 1-11, C++ operators, extractor fixes, header cache v29 | 856 |
| (round 12) | iterators as classes, std::string_view, std::optional completion, variadic free fn, request filters; header cache v30 | 858 |
| (imgui round) | Dear ImGui headless spike runs: opaque field blobs, template-arg pointer peeling, per-record layout refusal, default wrapper declarators, null defaults, alias of class refs, exact-arity overload tie-break; header cache v34 | 862 |

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
