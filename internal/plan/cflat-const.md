# Enforced `const` for CFlat

Status: RULED 2026-10-17 for iteration 1 (native CFlat only; C++ interop = phase 5, later). R1 B,
R2 A, R3 B for now (inferred; trailing `func() const` later), R4 A, R5 A, R6 A, R7 A, R8 A, R9 fix in
place, R10 superseded, R11 whole-word spelling; interface method calls on a const receiver are
allowed until trailing const lands. Review-draft legs (gitignored):
scratch/const_iter1/const_section.cb (append to Test/test_basic.cb, call testConstWholeWord from main).
PHASE 0 LANDED 2026-10-17 (b4b7e3448): spellings, ConstMask carry/cache/--symbol-dump, east const and
'* const' refused everywhere; known limit: `const(function<...>)` wrapper does not resolve yet.
PHASE 1 LANDED 2026-10-17 (b4b7e3448): const objects read-only (#1 #4 #11 #12), &G, rodata scalar/array globals,
lexical const-local folding (dims, value generic args, captures). Struct globals stay mutable `global` until phase 4.
PHASE 2 LANDED 2026-10-18 (b4b7e3448): const fields (#2), whole-assign refusal (#13), transitive const through a const
struct's pointer fields (#4), field access through a pointer to const (#3, incl. call/ternary/union bases), explicit cast
drops pointee const at member access, is_assignable(T) + guarded core containers (R5a). Left for phase 3: bare `*p`/`p[i]`,
pointers copied out of const objects, `*((int*)box.p) = 2`. hpc/btree.cb and com.cb HResult<T> still assign live slots.
PHASE 3 LANDED 2026-10-18 (b4b7e3448): writes through a pointer to const at any level (#3); qualification conversions
(#5 #6 #7 #16, C++ multi-level, void*, views, pointer aliases, compound pointer assign); C params compare their exact
spelled levels; casts add or drop const; `using X = const W` parses. Deferred: overloads differing only in pointee const
(R1 B: duplicate), function<> const params (p3 function-type-cannot-carry-pointee-const), string `_ptr` borrow cast
(p3 string-ptr-drops-const-from-borrowed-literal, phase 6), C auto-extern return const (phase 5).
PHASE 4 LANDED 2026-10-18 (ecf28e477): read-only methods inferred (receiver scan + fixed point over codegen's chosen
overloads, return provenance; core cached as `cms`, cache v15); #8 on const receivers, deferred to module end (an
expect_error scope owns its entries; its verdict waits for module end); const dtor-free non-owning struct globals are
`constant`. Limits: a read-only method's alias/pointer RESULT is not const-qualified on a const receiver (needs trailing
const, later iteration); pointer writes deeper than one level stay read-only (R11 one level).
PHASE 6 LANDED 2026-10-18 (ecf28e477): string literals are `(const char)*` (#3 #5 #6 #7 via the phase-3 predicate; auto
deduces it); imported C struct fields keep their spelled pointee const (header cache v265); core/Test/example/test_libs and
Windows-only sites migrated (static sweep, Windows not compiled - verify on the final Windows gate). Part B (string `_ptr`
/ `data()` const) stopped at its threshold: p3 string-ptr-drops-const-from-borrowed-literal.
PHASE 7 LANDED 2026-10-18 (ecf28e477): #14 (const on a generic type argument, incl. aliases naming a const word);
`const T` whole-word per instantiation; pointee-const type args mangle distinctly (`.c<N>$`, existing symbols unchanged;
depth-1 only); deduction keeps pointee const, strips top-level; doc/LANGUAGE.md "Const" section; review-draft remainder
appended as testConstWholeWord. ITERATION 1 COMPLETE (phases 0-4, 6, 7).
ITEM 10 (C side) LANDED 2026-10-19 (fa1d8e755): C header / .c globals, C struct/union fields (top-level const too), typedef'd
spellings and C auto-extern results carry ConstMask like the native declaration; DeclaredConstSlot sees C globals; header
cache v266. C function-pointer results/params stay unconst (p3 function-type-cannot-carry-pointee-const, needs ruling).
TRAILING CONST RULED 2026-10-19 (R3 -> C), PHASE 8 LANDED 2026-10-19 (8b7c67afa; constructors cannot be const;
leftovers p3 trailing-const-leftovers-after-c08, p3 duplicate-function-definition-accepted): C++ spelling `int get() const`; explicit const asserts
(#9), inference stays for unmarked methods, interface methods must spell const (#10). RULED 2026-10-19: function types carry pointee const, exact-match identity; string data() -> (const char)*, data_mut()
copies a borrowed string first (rulings in the two p3 issue files). Phase 5 (C++) ON HOLD. Open: is_const(T), string `_ptr`
(p3), function<> const params (p3), Windows gate (box offline 2026-10-19).
Written 2026-10-16 against master 753d9b22f. Supersedes, once ruled, the
2026-08-26 "const stays unenforced" ruling and the "(CFlat exposes no const)" clause of the
2026-09-27 C++-const ruling. Probes: `scratch/const_spike/p01-p19.cb`
(gitignored; re-create from the repros below if gone).

North star check: CFlat is borrow-by-default with compiler-inferred copy/move. `const` is the one
place a USER annotation is unavoidable (it states intent the compiler must not infer away), so the
plan keeps it C/C++-shaped and opt-in, and infers everything that is not intent (method
read-only-ness, R3).

## 1. Today (measured, Release cflat built 2026-10-15)

### Grammar (`cflat/CFlat.g4`)

- `typeQualifier : 'const' ;` (:531), reached from `declarationSpecifier` (:284),
  `specifierQualifierList` (:503), `pointer : ('*' typeQualifierList?)+` (:565/:568). So
  `const T x`, `const T* p`, `T* const p`, `const T* const* pp` all PARSE.
- No trailing-const member function slot: `functionDefinition` (:852) - `int get() const {}` is a
  syntax error (p04).
- `list<const int>` is a syntax error (p07): generic arguments take no qualifier.
- `if const` (:695, :768, :892, :1074) is unrelated compile-time selection; untouched by this plan.

### Where const is read, and where it is dropped

Both `ParseDeclarationSpecifiers` copies end identically - `ForwardRefScanner.cpp:551-558` and
`MainListener_Declarations.cpp:1919-1926`: they set `IsCxxPointeeConst` for a pointer to a C++
record and `IsSpelledPointeeConst = hasConst && Pointer`, then drop const. The pointer
declarator's `typeQualifierList` is never read (`PointerDepthOf`, `MainListener.h:606`, counts
stars only), so `T* const` is silently a plain `T*`.

`DeclSpecHasConst` (`MainListener_Declarations.cpp:3439`) callers - the piecemeal const that
already exists:

| Site | Effect |
|------|--------|
| :4100, :9918 | `NamedVariable::IsTopLevelConst` on funcptr params / locals only |
| :6847-6852 | C++ record value -> `IsCxxConstRef`; C++ record pointer -> `IsCxxPointeeConst` |
| :8200 | `auto` C++ locals |
| :9755, :9980 | `MarkCxxConstFieldAccess` on a const C++-record global / local slot |
| :9826 | const `string` global -> `RecordCompileTimeStringConstant` |
| :9836, :9844 | `constFoldableGlobals_`, `SetConstGlobalFP` |

File-scope const integer globals fold: `ForwardRefScanner::SeedConstIntGlobals`
(`ForwardRefScanner.cpp:999-1060`) -> `constGlobalInts_` (`LLVMBackend.h:4549`), FP into
`constGlobalFPs_`. Read at `MainListener.h:1192-1200` (array dims, `if const`). Globals are still
emitted as mutable `global`, never `constant` (`LLVMBackend_VariablesAndIR.cpp:202`).

### Behaviour matrix (probes)

| Probe | Result today |
|-------|--------------|
| `const int x = 1; x = 2; x++;` local (p01) | accepted, mutates |
| `const int G = 7;` global, `G = 9;` (p01) | refused, but by accident: "Left side of assignment is not an addressable lvalue." (folded rvalue) |
| `&G` on that global (p11) | refused: "Unable to get an Address-of an object without a Storage." |
| `namespace ns { const int K = 2; }` `ns.K = 4;` | refused, same accidental lvalue text |
| `const float F`, `const char* NAME`, `const S CS` globals (p14) | all writable |
| `const int L` local, `int* p = &L; *p = 6;` (p15) | accepted, mutates |
| const field `const int a = 1;` write from outside and from a method (p01) | accepted |
| `const S cs; cs.b = 4; cs.set();` (p01) | accepted |
| by-value `const int v` param write (p01) | accepted |
| `const int* p; *p = 5; int* q = p; w(p);` (p02) | accepted |
| `int* const p; p = &b;` (p03) | accepted |
| `const P* p` param, `p.x = 3;` / `const alias P p`, `p.x = 4;` (p12) | accepted |
| `const int* -> int*` via `(int*)p` (p13) | accepted (fine - keep) |
| `const int* get(); int* p = get(); *p = 8;` (p10) | accepted |
| `const char* c = "hi"; take(c)` where take writes `s[0]` (p05) | compiles; SIGBUS rc 138 - literals live in read-only memory |
| `const R r; sink(move r);` (p06) | accepted |
| `const V a` passed to `int use(const IGet g)` calling a mutating `get()` (p18) | accepted, mutates |
| local `const int M = 3; int[M] b` (p08) | works (VLA path; p3 array-dimension-constant-lookup-leftovers) |
| C++ `const Box cb; cb.which()` with `which()` / `which() const` twins (p16) | picks the const twin (2) - already works |
| C++ `b.ref() = 3` through `const int&` result (p17) | refused (2026-09-27 ruling text) |
| CFlat `const int* cp` into C++ `void takes_ptr(int*)` (p16) | accepted, C++ writes through it |
| C++ `const char* cname()` into CFlat `char* n` (p16) | accepted |

### C / C++ const already tracked (the machinery to generalize)

- `TypeAndValue` (`LLVMBackend.h:834`): `IsCxxConstRef` :865, `IsCxxPointeeConst` :868,
  `IsCxxPointeeVolatile`, `IsSpelledPointeeConst` :873, `IsCxxConstField` :877,
  `IsCxxMutableField`. Mirrored in `SerializedTav` (:1278-1283, copies :1351/:1424).
- `FuncPtrParam::IsPointeeConst` (:967/:1315); `NamedVariable::IsTopLevelConst` (:1574);
  `FunctionSymbol::CxxConst` (:2186, const C++ member); `CGlobalEntry::isConst`,
  `CRecordFieldEntry::isConst` (:4167, :4234).
- Read-only storage: `cxxConstGlobalSymbols_`, `cGlobalSymbols_`, `IsCxxConstFieldAccess`,
  `IsCxxConstReferent`; `CxxConstReceiverKind` (`LLVMBackend_VariablesAndIR.cpp:732`) is the one
  "is this receiver const" query; `CxxPointeeConstRelation` (`LLVMBackend.h:11754`) + ranking in
  `LLVMBackend_Overloads.cpp` (~:1501, :6650, :6703).
- C auto-extern (in-process clang, not a JSON dump): `MapCTypeToTypeAndValueImpl` strips cv
  (`LLVMBackend_CInterop.cpp:2277-2281`); `MapRawSig` re-adds pointee const for C `const T*`
  PARAMS (:4204-4213, T183); a C `const char*` RETURN loses it unless `r.isCxx` (:4114).
- String literals: LLVM constant globals (`CreateGlobalString`,
  `LLVMBackend_VariablesAndIR.cpp:2227`) typed plain `char*` (`MainListener_Declarations.cpp:13434`,
  `LLVMBackend_Overloads.cpp:296`); only diagnostics spell them `const char*`.
- Existing const diagnostics (reuse their shape): `MainListener_Expressions.cpp:3801/3806`
  ("cannot assign to '{}': it is a const C object, so its storage is read-only. ..."), :28222,
  :28248, :28271 ("cannot call non-const member '{}' on '{}': {}, so it is read-only ...");
  `LLVMBackend_Overloads.cpp:6674`, :6724; `LLVMBackend_CInterop.cpp:29405`.

### Mangling and cache

- `cflat/TypeMangling.h/.cpp`: `MangleType` (:732), `DemangleType`, `MangleFunctionSymbol`.
  `TypeSpelling` (`TypeMangling.h:9-24`) has pointerDepth + view/alias/unique/value/closure/move,
  NO cv. (The `$`-separated scheme of the 2026-09-02 ruling lives here; its plan file is gone.)
- `--init` cache (`LLVMBackend.cpp`): `SerializeTav` :6995 / `DeserializeTav` :7086 (`cpc`, `spc`
  keys at :7016-7018 / :7106-7108), `SerializeAnnotations` :7168, `SerializeDtav` :7208,
  `SerializeFuncSym` :7350, StructData save :7664-7716 / load :8263-8313.
- C-header cache (`LLVMBackend_StateAndImports.cpp`): `TvToJson` :2116 / `TvFromJson` :2193
  (`ccr`, `cpc`, `cpv`, `spc`, `ccf`), `GlobalToJson` :2516, `FieldToJson` :2574.

### Spelling census (regex, comments / strings / `if const` excluded; approximate)

| Dir | const qualifiers | `const T*` params | non-ptr `const T x` (col 0 / indented) | const in extern lines |
|-----|-----|-----|-----|-----|
| `cflat/core` (110 files) | 602 | 281 | 209 / 97 | 125 |
| `Test/*.cb` | 222 | 84 | 33 / 70 | 10 |
| `Test/errors` | 32 | - | - | - |
| `example` | 165 | 52 | 101 / 0 | 9 |

`T* const`, trailing-const methods, const fields: 0 real sites. Const return types: ~4.
Non-pointer const params: 2 (Test). Top files: `core/ui_native/win32.cb`, `core/os.cb`,
`core/os.posix.cb`, `core/cruntime.cb`, `core/sci/constants.cb`; `Test/test_cpp_interop.cb`,
`Test/test_basic.cb`. String-literal fallout: `char* x = "..."` 5 core / 52 Test / 16 example;
extern lines taking a mutable `char*` 20 / 13 / 52 (mostly `printf(char* fmt, ...)`).

Breakage estimate: write-refusal phases break little (const is mostly on read-only globals and
`const T*` params that already behave). The two expensive phases are pointee-const conversion
(every `const T*` arg flowing into a `T*` parameter, core prototypes included) and string literals
as `const char*`.

Open issues this plan closes (delete on landing, move durable records to fix-issue-lessons digest
first): `p3/c-const-leftovers-after-t183`, `p3/cpp-const-pointee-dropped-at-pointer-init-and-const-result-writes`,
`p3/array-dimension-constant-lookup-leftovers` (const-local leg). Touches:
`p3/cpp-const-byvalue-return-receiver-cv`, `p3/cpp-typed-null-operand-loses-pointee-const-in-operator-ranking`.

## 2. Semantics (proposed)

Rule of thumb: C++ meaning, CFlat diagnostics, no user-visible const in type identity beyond
pointee const. Prior art: C++ (`const T`, `T* const`, const members, `mutable`, `const_cast`,
overload on const); Rust (immutable by default, `let mut`, `&T`/`&mut T`, `*const T`/`*mut T`,
interior mutability via `Cell`). CFlat is a C dialect and already has hundreds of `const`
spellings, so opt-in C++ const is the only migration-feasible shape; Rust's default-immutable is
noted and rejected (would touch every local in the tree).

1. **const object (local, global, namespace global, by-value param).** Any write to the object or
   a sub-object (field, element) is refused: `=`, compound assignment, `++`/`--`, address taken as
   a mutable pointer, non-const method call, passing to a mutable `alias` / `T*` parameter. Must
   be initialized at declaration (`= value`, `= default`, brace-init) - a const without an
   initializer is refused (C++; C allows it but it is useless). Destructor still runs (C++).
   Globals with a constant initializer emit as LLVM `constant` (rodata; enables folding).
   `&G` on a const global yields `const T*` (fixes p11's accidental refusal; the fold map keeps
   folding reads). Local `const` integer with a constant initializer folds like a file-scope one
   (array dims, value generic args) - closes the local-dimension leg of the p3 issue.
2. **const field.** Writable only during construction: field default, brace-init element, the
   constructor's field-init phase and constructor body. Elsewhere refused, including inside
   methods. A struct with a const field cannot be whole-assigned (`s = t;`) - C++ deletes copy
   assignment; init / copy-construct / move-construct stay legal. (R5.)
3. **Pointer to const vs const pointer.** `const T* p` (= `T const* p`): `*p`, `p.f`, `p[i]` are
   read-only; `p` itself reassignable. `T* const p`: `p` fixed, `*p` writable. Per-level, C++
   qualification rules:
   - `T* -> const T*` implicit everywhere (init, assign, arg, return, brace element).
   - `const T* -> T*` refused everywhere except an explicit cast.
   - Multi-level: `T** -> const T**` refused, `T** -> const T* const*` allowed (C++ [conv.qual]).
   - `const T* ->` integer stays governed by pointer-is-not-a-number; `bool` exempt as today.
   - `void*`: `const T* -> const void*` ok, `-> void*` refused.
4. **const receiver.** On a const object (or through `const T*` / `const alias T`) only
   read-only methods are callable; inside a read-only method `this` is `const S*`. Read-only-ness
   of a native method is INFERRED by default (R3): the ForwardRefScanner already body-scans params
   for writes (`IsWriteInferredSink`, `LLVMBackend.h:901`); the same scan over `this` decides it.
   An optional trailing `const` asserts it (refused if the body writes) and is REQUIRED where
   there is no body: interface methods, extern / C++-bound declarations.
5. **Parameters.**
   - By-value `const T v`: callee may not write `v`. Top-level const is NOT part of the signature
     (C++): `f(const int)` and `f(int)` are the same function.
   - `const T* p` / `const alias T p`: callee may not write through it; caller may pass a mutable
     object. With borrow-by-default this also tells the inference "never needs a copy" - a const
     struct param can never become a write-inferred sink.
   - `const` on a `move` parameter: legal, the callee owns but cannot mutate (destructor runs).
6. **Ownership / move.** (R4.) Implicit last-use consume of a const local is allowed (nothing can
   observe the source again). Explicit `move x` on a const local / param is REFUSED, because
   explicit move nulls the source and leaves it readable-as-null (ruling) - that is an observable
   write. C++ meanwhile turns `std::move(const T)` into a copy; Rust allows the move because the
   binding dies. Copy out of a const (`R r3 = r2;`) follows the normal copy rules.
7. **Generics.** (R6.)
   - `const` is NOT a legal generic type argument at top level (`list<const int>` stays refused,
     now with a real message): containers must own mutable slots (C++'s `vector<const T>` is
     ill-formed for the same reason). Pointee const is legal: `list<const char*>`.
   - Deduction strips top-level const (C++ by-value deduction): `id(c)` with `const int c` gives
     `T = int`. Pointee const is kept: `id(cp)` gives `T = const int*`.
   - `const T` inside a generic body applies const to T itself: with `T = int*`, `const T` is
     `int* const`, NOT `const int*` (the C++ gotcha; document it).
   - `is_const(T)` intrinsic for `if const` (pairs with `is_pointer`, `is_unique`); answers for
     pointee const of a pointer T. Optional, phase 7.
8. **Interfaces.** Interface methods declare `const` explicitly (no body to infer from). A method
   implementing a `const` interface method must be read-only (inferred or explicit). Calling
   through a `const IFoo` / `const IFoo*` needs a const interface method. Vtable layout unchanged.
9. **Operator overloads.** Same rules as methods/params: a member operator on a const LHS needs a
   read-only operator; `const alias V` params accept anything. C++ operators: unchanged (const
   twins already ranked).
10. **C interop.** Extern prototypes keep their const: a CFlat `const T*` argument into an extern
    `T*` parameter is refused (2026-09-04 "extern C calls follow CFlat rules"). C auto-extern
    already re-adds pointee const on params; returns gain it too (fix :4114). `--c-include`
    globals: `CGlobalEntry::isConst` becomes plain CFlat const.
11. **C++ interop.** CFlat const maps 1:1 onto clang const. The record-only flags
    (`IsCxxConstRef`, `IsCxxPointeeConst`, `MarkCxxConstFieldAccess` on CFlat const slots) become
    readers of the general const bits, so C++ and CFlat records behave identically (bridge
    transparency, 2026-09-17). A CFlat `const int*` into a C++ `int*` parameter is refused
    (today accepted, p16). A C++ `const char*` result into CFlat `char*` is refused (p16).
    CFlat functions crossing to C++ already spell pointee const (`IsSpelledPointeeConst`).
12. **Casts.** No `const_cast` keyword. An explicit C cast may add or drop const at any level
    (C rule; the explicit cast is the escape hatch, like pointer-is-not-a-number). (R7.)
13. **String literals.** (R2.) Type becomes `const char*`. `char* s = "x";` and a literal into a
    mutable `char*` parameter are refused (C++11). Literal -> `string` and literal -> C++
    `const char*` converting ctor are unaffected.

## 3. Representation

- `TypeAndValue` gains `uint8_t ConstMask`: bit `i` = const at pointer level `i`, where level 0 is
  the innermost pointee and level `PointerDepth` is the declared object itself. `const int* const p`
  = 0b11; `const int x` = 0b1; `int* const p` = 0b10. Arrays: const applies to the element
  (C rule), stored on the element level. `alias` / ref-shaped: the referent level.
- Migration of existing flags: `IsSpelledPointeeConst` == `ConstMask & 1 && Pointer` (keep the
  field as a derived accessor first, delete in phase 5); `IsCxxPointeeConst` / `IsCxxConstRef`
  stay as C++-origin markers until phase 5 folds them into `ConstMask`. Do NOT keep two sources of
  truth past phase 5.
- `NamedVariable::IsTopLevelConst` becomes `TypeAndValue.ConstMask` top bit (one place, so
  the 5 current uses and `CxxConstReceiverKind` read it).
- `DeclTypeAndValue` (fields, params) carries `ConstMask` through the same struct.
- `FunctionSymbol` gains `bool IsConstMethod` (explicit or inferred) next to `CxxConst`; interface
  method contracts carry it too.
- Mangling (`TypeMangling.cpp`): top-level const is NEVER mangled (C++). Pointee const IS mangled
  as one marker per const level in `TypeSpelling` so `f(int*)` / `f(const int*)` can coexist and
  demangle back (R1); receiver const mangles for explicit-const twins. Every user-facing demangle
  surface prints `const`. If R1 rules "no overload on const", skip the mangling change and refuse
  the duplicate instead - cheaper.
- `--init` serializer rule (load-bearing): `ConstMask` round-trips in `SerializeTav` /
  `DeserializeTav` (new key, e.g. `cm`) and `SerializeDtav`; `IsConstMethod` in `SerializeFuncSym`;
  struct field const via the field `DeclTypeAndValue` in the StructData block; C-header cache
  `TvToJson` / `TvFromJson`. SAME change as the field - else warm-cache `expect_error` tests stop
  firing. Every phase's acceptance runs the suite cold AND warm.
- Both `ParseDeclarationSpecifiers` copies (`ForwardRefScanner.cpp:551`,
  `MainListener_Declarations.cpp:1919`) fill `ConstMask` identically, including the pointer
  declarator's `typeQualifierList` (today ignored). Abstract declarators (casts, `sizeof`, funcptr
  types) go through the same helper.

## 4. Diagnostics (new `LogError` / `LogErrorContext` format strings; locales regenerate)

Shape follows the existing const family ("cannot X 'name': reason. hint").

1. `cannot {} '{}': it is declared const` - `{}` = assign to / modify / increment / decrement.
2. `cannot {} '{}': field '{}' is const and can only be set during construction`
3. `cannot {} '{}': it is reached through a pointer to const ('{}')`
4. `cannot {} '{}': '{}' is const` (sub-object of a const object: field, element)
5. `cannot convert '{}' to '{}': the conversion discards const. Use an explicit cast if the
   object is really writable`
6. `cannot pass argument {} ('{}') to parameter '{}' ('{}'): the conversion discards const`
7. `cannot return '{}' as '{}': the conversion discards const`
8. `cannot call non-const member '{}' on '{}': it is declared const` (reuse the :28271 text with
   this reason; no second sentence family)
9. `method '{}' is declared const but writes '{}'`
10. `'{}' implements const interface method '{}.{}' but writes '{}'`
11. `const variable '{}' must be initialized`
12. `cannot move out of const '{}': 'move' nulls the source, which is a write. Let the last use
    consume it, or drop 'const'`
13. `cannot assign a whole '{}': field '{}' is const`
14. `'const' is not allowed on a generic type argument; use it on the pointee ('const T*') or on
    the declaration`
15. `'const' after a parameter list is only allowed on a member function`
16. `cannot take a mutable pointer to '{}': it is declared const` (`int* p = &L;` gets #5 when the
    target type is spelled; this one covers `&L` passed to a `T*` parameter / mutable alias)

String literal fallout reuses #5/#6 with the source spelled `const char*`. The accidental p01/p11
texts for const globals ("not an addressable lvalue", "without a Storage") are replaced by #1 and
a working `&G`.

## 5. Phases

Each phase: one Codex brief, extend existing tests (`Test/test_basic.cb`, `Test/test_generics.cb`,
`Test/test_cpp_interop.cb`, `Test/test_c_interop.cb`), negatives as new
`Test/errors/err_const_*.cb` (the sanctioned form), `./test.sh Release` cold + warm green,
`scratch/libs_perf.sh` vs previous master (no compile-time regression), `test_lsp` when
`MainListener.h` symbol registration changes. Migration edits to core / Test / example land in the
SAME phase as the rule that needs them (core edits need redeploy + `--init-local`).

| # | Phase | Scope | Acceptance |
|---|-------|-------|------------|
| 0 | Parse + carry | `ConstMask` on TypeAndValue/DeclTypeAndValue/NamedVariable, both ParseDeclarationSpecifiers copies + pointer `typeQualifierList` + abstract declarators; cache round-trip; `IsSpelledPointeeConst` derived. No new errors. | Suite green cold+warm; `--symbol-dump` shows const in the type; zero behaviour diff in `--out-lli` for test_basic. |
| 1 | const objects | Locals, globals, namespace globals, by-value params: write refusal (#1, #4, #11, #16), `&G` works as `const T*` source, `constant` globals, local const folding as array dim / value generic arg. | p01/p11/p14/p15 refused with #1/#16; `&G` compiles; `err_const_local_write.cb`, `err_const_global_write.cb`, `err_const_uninitialized.cb`; test_basic legs for folding + `&G`. Migration: expected ~0 core sites (audit `const` locals that are written - grep result goes in the brief). |
| 2 | const fields + const struct objects | #2, #13; field writes through a const object; construction-phase allowance. | p01 field legs refused; ctor/field-default/brace-init still set them; `err_const_field_write.cb`. |
| 3 | Pointee const | Qualification conversions at init/assign/arg/return/brace/ternary (#3, #5-#7), `T* const`, multi-level rule, `void*`, explicit cast drops (p13 stays legal). Extern CFlat prototypes enforced. | p02/p03/p10/p12 refused; `err_const_pointer_discard.cb`, `err_const_pointer_write.cb`. Migration: every core `const T*` flowing into a `T*` (expect libc wrappers in `os*.cb`, `cruntime.cb`, `win32.cb`); fix the core prototype or callee, never add casts in bulk. Biggest phase - split per directory if the brief exceeds one run. |
| 4 | const receivers | Inferred read-only-ness (scan `this` writes; reuse write-inferred-sink scan), optional trailing `const` (grammar: `functionDefinition` member alt, 100% rules, no predicates; `ForwardRefScanner` no-op), interface `const` methods, #8-#10, #15; mangle receiver const if R1. | p01 `cs.set()`, p18 `use(a)` mutating `get()` refused; `err_const_method_call.cb`, `err_const_method_writes.cb`; test_basic leg: read-only method callable on const without annotation. |
| 5 | Interop unification | Fold `IsCxxConstRef`/`IsCxxPointeeConst`/CFlat-slot `MarkCxxConstFieldAccess` into `ConstMask`; delete `IsSpelledPointeeConst`; C auto-extern return const; CFlat const into C++ `T*` refused; C++ const results into CFlat mutable refused. | p16 `takes_ptr(cp)` and `char* n = cname()` refused; existing C++-const tests unchanged (texts may unify - update expectations only where the text family merged, never weaken); the 2-3 p3 issues above deleted. |
| 6 | String literals | Literal type `const char*`; core/Test/example `char* x = "..."` and `extern ...(char* fmt` prototypes migrated (counts in section 1). | p05 refused at compile time; `err_const_string_literal.cb`; test_example green. |
| 7 | Generics + polish | #14, deduction stripping, `const T` on pointer T, `is_const(T)`, doc/LANGUAGE.md section, remove "const unenforced" notes in docs. | test_generics legs; `err_const_generic_arg.cb`. |
| 8 | Trailing const (iteration 2, R3 C) | Grammar: optional `const` after the parameter list of a member function definition, a struct-body method declaration, an interface method and a member operator (100% rules, no predicates; ForwardRefScanner records it); on a free / extern / generic free function #15. Explicit const: `this` is `const S*` in the body, so a write is #9 and returning a pointer/alias into `this` as mutable is #7 (closes the phase-4 result limit for marked methods); the flag joins `IsConstMethod` (explicit wins, inference unchanged for unmarked). Interfaces: a call on a const interface receiver (`const IFoo`, `const IFoo*`, `const alias IFoo`) needs a const interface method (#8, ends the iteration-1 allowance); an implementer of a const interface method must be read-only, inferred or explicit (#10). Receiver const is not part of the signature (R1 B: `get()` + `get() const` = duplicate); vtable layout unchanged. Cache: explicit flag in SerializeFuncSym + interface table round-trip. Migration: core / Test / example interface methods called on const receivers gain `const` (count in the brief, from a build with the rule on). `mutable` stays out. | explicit-const write, implementer-writes and const-interface-receiver legs refused (#9 #10 #8) in `err_const_method_writes.cb` / `err_const_method_call.cb`; test_basic legs: marked and unmarked read-only methods on const receivers, const interface method via const receiver, marked method returning `(const int)*` into this; test_lsp green; cold == warm verdicts. |

Phases 1, 2 can run in parallel after 0 (disjoint diagnostics; both touch assignment checks -
stack 2 on 1 if the brief overlaps `MainListener_Expressions.cpp` assignment paths). 3 before 4
(receiver = pointer-to-const `this`). 5 after 3. 6 last among breaking phases.

Perf risk: low. One byte per TypeAndValue; qualification check is a bitmask compare on paths that
already compare pointer depth; read-only inference rides the existing body scan (no new pass). The
C++ path loses work (fewer special flags). Watch: phase 4 inference must not add a pass over
every core method body on a warm cache - cache `IsConstMethod` in `SerializeFuncSym`.

Brief constraints (every phase): both-pass ParseDeclarationSpecifiers, LogError-only, ASCII,
no new non-err test files, existing tests read-only except the named legs, serializer rule, never
edit `cflat/locales/`, shell-safety line, token-efficiency directive.

## 6. Rulings

RULED 2026-10-17 (maintainer): every recommendation below adopted; R3 = B for iteration 1.

R1. **Overload on pointee const** (`f(int*)` and `f(const int*)` both declared).
  A: allowed, ranked like C++ (needs mangling markers). B: refused as a duplicate; const only
  constrains. Recommend **A for C++-imported functions (already ranked), B for native CFlat** -
  cheapest, no mangling change, revisit if a library needs twins. Receiver twins likewise: native
  `get()` / `get() const` refused as duplicate.

R2. **String literal type.** A: `const char*` now (phase 6), migrate ~73 `char* = "..."` and ~85
  extern lines. B: stays `char*`; writes through literals remain a runtime SIGBUS. C: `const char*`
  but a literal may still bind an EXTERN prototype's `char*` parameter (C compatibility hole).
  Recommend **A** - p05 shows the crash is real and B leaves the hole the plan exists to close.

R3. **Method read-only-ness.** A: explicit trailing `const` only (C++). B: inferred only. C:
  inferred by default, explicit `const` optional (assert) and required without a body (interface,
  extern, C++). Recommend **C**: 0 core methods spell trailing const today, so A would make every
  core method uncallable on a const receiver until annotated; inference matches the north star.
  Cost: a body change can silently change a public method's const-ness - acceptable inside a
  module, and interface methods are explicit anyway.
  AMENDED 2026-10-19 (maintainer): iteration 2 adopts **C** (phase 8). Surface: C++ trailing `const` after the
  parameter list; explicit const that writes is an error; unmarked methods keep inference; interface methods
  must spell `const` to be callable on a const interface receiver.

R4. **Explicit `move` of a const.** A: refused (#12), implicit last-use consume allowed.
  B: allowed (Rust: binding dies). C: becomes a copy (C++ `std::move(const T)`).
  Recommend **A**: explicit move's readable-as-null source is an observable write; C silently
  copies, against "no implicit copy of non-trivial types".

R5. **Whole-assignment of a struct with a const field.** A: refused (C++). B: allowed as a
  re-init. Recommend **A**.

R5a. **Containers of a const-field T** (RULED 2026-10-17). A compile-time intrinsic `is_assignable(T)` (false when T
  has a const field anywhere: embedded structs, fixed-array fields, union arms) guards the assigning methods of core
  containers (`list.sort` etc.) with `if const`; relocation into fresh storage is construction, not assignment. So
  `list<S>` with a const-field S works and only calling an assigning method fails (C++ `std::sort` behaviour).

R6. **`const` as a generic argument** (`list<const int>`). A: refused, pointee const only.
  B: allowed, const propagates into members. Recommend **A** (containers need mutable slots;
  C++ forbids it for the same reason).

R7. **Dropping const.** A: any explicit C cast drops it. B: a dedicated spelling
  (`const_cast<T*>(p)` builtin) required, plain casts may not drop const. Recommend **A** (C
  dialect, matches the pointer-not-a-number escape hatch); B is additive later if wanted.

R8. **Must a const be initialized?** A: yes, refused otherwise (C++). B: C rule (zero-filled).
  Recommend **A**.

R9. **Scope of phase 3 migration.** Fix core prototypes to be const-correct (a `const T*` param
  where the callee only reads) in the same phase, or allow a temporary core-only exemption.
  Recommend **fix in place, no exemption** (an exemption is a second rule to retire).

R10. **Ruling text supersession.** Confirm the 2026-08-26 ruling and the "(CFlat exposes no
  const)" clause of 2026-09-27 are superseded on ruling this plan, and record it in the
  fix-issue-lessons digest.

R11. **Spelling: whole-word `const`** (proposed 2026-10-17, maintainer-preferred direction; grammar
  spiked, see below). Fixes the "is `const int*` the pointer or the int" confusion: a CFlat type
  is one word (`int*`, `list<int>`, `T`), and `const` applies to the whole word, transitively
  (D-style): everything reached through it is read-only.
  - `const int x` - value read-only (unchanged).
  - `const int* p` = `const (int*)`: `p` cannot be reseated AND `*p` / `p[i]` / `p.f` are read-only.
    ConstMask: every level at and below the declared object (0b11).
  - `(const int)* p` and `const(int)* p` (both accepted, same meaning): const on the inner word
    only - `p` reseatable, `*p` read-only (0b01). Nests: `(const int*)* pp` = pp reseatable,
    `*pp` fixed, `**pp` read-only; `const(int)** pp` = pp and `*pp` reseatable, `**pp`
    read-only; `const(const(int)*)*` is the same type as `(const int*)*`. Spelled masks are always
    downward closed (bit k set => every inner bit set): it is a "const depth". Display: all
    levels -> `const W`, none -> `W`, else `(const Inner)` + the remaining `*`s.
  - `const S s` with a pointer field: `*s.p = 5` refused too (transitive). C++ is shallow here
    (`s.p` becomes `int* const`, `*s.p` writable) and gets deep const only from const-overloaded
    accessors; CFlat containers get it for free.
  - Generics: `const T` with `T = int*` is exactly the written-out `const int*` - the C++
    `const T` -> `int* const` gotcha of section 2 item 7 disappears.
  - `using` captures the const word whole: `using CStr = (const char)*;` is a reseatable view,
    `using Fixed = const int*;` is fully const; `const Alias` applies const to the whole aliased
    word (`const IntPtr` with `IntPtr = int*` is `const int*`), the generic rule.
  - Parameters follow the variable rules exactly (2026-10-17 direction: a parameter declaration
    reads like a variable declaration). `const int* p` param = p fixed in the body and `*p`
    read-only; `(const int)* p` = reseatable view; `const int v` / `const S b` by value = the
    callee may not write the copy (transitively for S). Top-level const stays out of the
    function type, so callers pass a `const int*` and a `(const int)*` alike, and an extern
    prototype's `const char* s` needs no change.
  - Iteration 1 scope (2026-10-17): variables, parameters, fields, string literals, `using`, and
    METHOD CALLS on a const receiver - read-only-ness inferred from the body (R3 B for now; a
    write through a pointer field counts, transitive const). The explicit trailing
    `func() const` spelling (R3 C's assert form, required on interface / extern / C++-bound
    declarations) moves to a later iteration; until then interface methods called on a const
    receiver need a ruling of their own.
  - Refused spellings: east const `int const*` and trailing `int* const` (0 real sites in
    core / Test / example; comments only). A C++ type with a const pointer level and no CFlat
    spelling (`int* const` field, `char* const&` param) stays enforced through ConstMask and is
    displayed in C++ spelling.
  - Supersedes: section 2 item 3 (`const T*` = pointee-only, `T* const`), the section 3 example
    masks (`const int*` becomes 0b11, `(const int)*` is the old 0b01), the item 7 generic gotcha.
  - Migration: existing `const T*` declarations whose pointer is reseated (`s++`, `p = next`)
    become `(const T)*`. Regex upper bound 46 of 465 `const T*` decls (top:
    `example/ui/03-canvas-win32/win32_shot.cb` 8, `Test/test_cpp_interop.cb` 5,
    `Test/test_basic.cb` 4, `example/hpc/heat_stencil.cb` 4, `cflat/core/os.posix.cb` 2); the
    exact list comes from a build with the rule on. Signatures are unaffected: top-level const is
    not part of a function type, so a `const char* s` param still matches clang's `const char*`.
  - C++ interop open sub-question: a C++ const method may write through a member pointer (shallow).
    Binding one on a const C++ object must follow clang's rule, so CFlat records are transitive
    and C++ records shallow - a bridge-transparency (2026-09-17) exception to rule on. `mutable`
    has no CFlat spelling yet; needed if a `lock()` mutex field must work in a read-only method.
  - Grammar (spiked in `scratch/const_whole/`, ANTLR 4.13.2 Java target, LL exact-ambiguity
    detection, no predicates):
    ```
    typeSpecifier
        : ...
        | 'const' '(' typeName ')'      // const(T)
        | '(' 'const' typeName ')'      // (const T)
        ;
    castExpression
        : '(' castTypeName ')' castExpression
        | ...
    castTypeName                        // top-level const needs a declarator
        : typeSpecifier* abstractDeclarator?
        | typeQualifier typeSpecifier* abstractDeclarator
        ;
    ```
    Without the cast narrowing, `(const int)* p;` as a statement is a real blockItem ambiguity
    (declaration vs. cast of `*p`); with it the ambiguity is gone. Cost: `(const int)x` /
    `(const int)*p` stop being casts (a cast yields a value, so the const was meaningless; 0
    real sites); the listener should still diagnose them - today it is a raw ANTLR error.
    `(const char*)x` (96 sites) is untouched. `const (int, float) t` (const tuple) and
    `if const (...)` still parse. Pre-existing exact ambiguities (parameterDeclaration
    declarator vs abstractDeclarator, `= default` initializer) are unchanged by the edit.
    The listener reads casts through `castTypeName` instead of `typeName` (both
    `ParseDeclarationSpecifiers` copies + cast / abstract-declarator helpers).
    Corpus check: all 1059 `.cb` under core / Test / example parse to the IDENTICAL tree (modulo
    the `castTypeName` wrapper) with identical syntax-error and exact-ambiguity counts.
