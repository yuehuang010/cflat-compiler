# Value (non-type) generic parameters: `struct Buf<T, int N>`

Status: **Design ratified 2026-08-26. Stages 0-5 IMPLEMENTED 2026-08-27; trailing-parameter DEFAULTS added 2026-09-03 (see as-built note below)** (`./test.sh Release`
720 passed / 0 failed / 8 skipped). Written after a review of C++ / C# / Rust prior art against
what CFlat already has; the "measured" claims below describe the state BEFORE implementation and
are kept as the record of why each decision was made.

**As-built notes, where the implementation refined the design:**

- **Stage 6 (2026-09-03): defaults on trailing value parameters.** `struct S<T, int N = <const-expr>>`
  parses (`valueParameterDeclaration : valueParameterType Identifier ('=' shiftExpression)?`, still
  100% rules - the default sits at SHIFT level for the same reason a use-site value argument does,
  so a `>` can never be read as an operator inside it). The folded default is stored per template in
  `gts.genericStruct/Interface/FunctionValueDefaults`, parallel to the existing `...ValueParams`
  vectors, filled by `ParseGenericTypeParameters` (both passes) and round-tripped through the
  `--init` cache as `value_param_defaults`. Two directions meet in the middle: instantiation FILLS
  omitted trailing arguments from the defaults (`FillGenericValueDefaults`), and
  `MangleGenericInstance` STRIPS a trailing argument spelled exactly as its default. Stripping
  rather than appending is what keeps `unique<T>`'s existing mangled name byte-identical while
  making `unique<T, 0>` name the same instantiation. Diagnostics: a default on a non-trailing
  parameter, a default that does not fold, and (only for templates that declare a default) a use
  site that omits an undefaulted parameter. Coverage: `Test/test_generics.cb::testGenericValueDefaults`,
  `Test/errors/err_generic_value_default.cb`. Known gap: the ForwardRefScanner does not register
  generic STRUCT templates at all, so it learns a struct's defaults only once the main pass (or the
  warm core cache) has registered them - a user struct whose default is spelled explicitly at a use
  site in the same file could mangle differently in the two passes. `unique` is a core template and
  is always registered first, so the shipped user of this feature is unaffected.

- The use-site value-argument alternative is `shiftExpression`, NOT `assignmentExpression`.
  Decision #5 said only that a comparison must be parenthesized; admitting a full
  `assignmentExpression` turned out to swallow ordinary code - `a < X && b > c` parsed as a
  generic instantiation, breaking `test_generics`, `test_hpc_kernels` and `test_c_interop`.
  Narrowing the alternative to shift level puts `<`, relational, equality, bitwise and logical
  operators structurally out of reach at the top level of an argument, which enforces decision
  #5 in the grammar instead of by convention. The cost is that a `>>` shift as a value argument
  needs parentheses, exactly as in C++.
- **The grammar stays 100% rule-driven.** An attempt to resolve the same ambiguity with
  `@parser::members` plus semantic predicates (a 256-token lookahead on `blockItem`, a
  float-literal sniff on `genericIdentifier`) was rejected by the maintainer on 2026-08-27. The
  fix for an ambiguous alternative is to NARROW the alternative, never to guard it. Do not
  reintroduce a predicate or an action block into `CFlat.g4`.
- `nonIntegralValueParameterType` (`float`/`double`/`string`/`void`) is in the grammar solely so
  `struct Buf<T, float N>` is REJECTED with a message naming the allowed set, rather than
  reported as unparseable token soup. Same parse-then-diagnose shape as the C-style array
  declarator.
- Value parameters are bound by `MainListener::GenericValueMacroScope`, which saves and restores
  only the names it binds. An earlier version copied the whole `compileTimeMacros` map on every
  instantiation, value parameters or not.
- **Seeded `const` globals live in their own map, NOT in `compileTimeMacros`.** The open item
  below recommended seeding them into `compileTimeMacros`; that recommendation was WRONG and is
  superseded. `MainListener::ParseIdentifier` consults `compileTimeMacros` BEFORE any scope
  lookup, so seeding a global there made it shadow same-named locals and parameters program-wide
  (silent wrong values, no diagnostic), narrowed every declared width to i32 (`const long BIG = 3;
  BIG << 40` lost its high bits), and stripped the global's storage so `&BIG` stopped compiling.
  All three were caught by an Opus review of the checkpoint commit and fixed on 2026-08-27:
  `LLVMBackend::SetConstGlobalInt` / `TryGetConstGlobalInt` back a separate `constGlobalInts_`
  map that ONLY the parse-tree folder reads. Regression coverage is
  `Test/test_basic.cb::testConstGlobalIsNotAMacro`. Do not merge the two maps back together.
- **The folder must accept a bare `genericIdentifier` node.** `Buf<int, CAP>` - decision #4's own
  example - was dead in BOTH passes at the checkpoint commit, because the macro lookup lived only
  in `FoldCompileTimeIntLeaf`'s `PrimaryExpressionContext` arm while the argument funnels hand the
  folder a bare `GenericIdentifierContext`. Only `Buf<int, (CAP)>` and `Buf<int, CAP * 2>` worked,
  and the suite missed it because the fixture only ever exercised `CAP * 2`. Fixed by
  `FoldCompileTimeIdentifier`, which both arms now share; the fixture asserts the bare spelling.
- **Gaps, deliberate:** value predicates do not work at INTERFACE scope (`interfaceDefinition`
  has no `whereClause` slot in the grammar); a value parameter cannot be combined with a
  parameter pack (rejected at the declaration, since the pack machinery binds every parameter
  through the TYPE substitution map); and a `const` integer inside a `namespace` does not fold in
  a value argument, reporting the kind-mismatch diagnostic rather than a "does not fold" one -
  see the file-scope-only note under `SeedConstIntGlobals`. All three are documented in
  `doc/LANGUAGE.md`.

Every "measured" claim below was run on `x64/Release/cflat` at the `-D` defines change.

The goal is a generic parameter that carries a compile-time VALUE, not a type, so a container
author can write `struct Buf<T, int N>` and a coder can write `Buf<int, 8>`. The coder's
surface must read exactly like the existing `simd<float, 8>` builtin - that requirement drove
most of the decisions below.

---

## Ratified decisions

| # | Decision | Chosen | Why |
|---|---|---|---|
| 1 | Declaration spelling | `struct Buf<T, int N>` | The type token discriminates a value param from a type param in ONE token. Matches C++ `template<size_t N>`. |
| 2 | No `const` marker | dropped | `const` is IMPLIED by the position: a generic parameter is a compile-time constant by construction, so there is no non-const reading of `N` to distinguish from. See "Why `const` is implied" below. |
| 3 | Value param types | primitives only | `bool` and `enum` are int-shaped in CFlat and come free. Keeps the mangling total (see #6). |
| 4 | Use-site argument | any constant expression that folds | `Buf<int, 8>`, `Buf<int, CAP>`, `Buf<int, CAP*2>`. |
| 5 | `>` closes the argument list | yes, C++ rule | A comparison must be parenthesized: `Buf<int, (A > B)>`. |
| 6 | Mangle the FOLDED VALUE | yes | `Buf<int, CAP*2>` and `Buf<int, 16>` are ONE instantiation. This is where C++ went wrong (structural expression equivalence) and where Rust punted (`generic_const_exprs`, still unstable). |
| 7 | Specialization mechanism | `if const` in the body | Including MEMBER scope (`CFlat.g4:842`), which is what makes C++ partial specialization unnecessary - an arm can select FIELDS, not just statements. |
| 8 | Value constraints | widened `where` | `where N > 0`. Mirrors C++'s `template<size_t N> ... requires (N > 0)` split: the parameter states the KIND, the clause states the CONSTRAINT. |

### Closed by design - do not revisit

- **A value parameter of a user-defined type** (`Foo N`). The spelling collides with the
  `unique T` ownership qualifier, which occupies the same leading-Identifier position in
  `typeParameterEntry`. Independently: instantiation identity needs equality plus a canonical
  symbol name, and user types have neither. This is Rust's `adt_const_params`, unstable for
  years for exactly this reason.
- **Float value parameters.** `NaN != NaN` and `-0.0 == 0.0` destroy instantiation identity.
  C++20 permits them and almost nobody uses them; Rust refuses them. Refuse them.
- **C++-style partial specialization** (`struct Buf<T, 0>`). Superseded by decision #7.
- **`where N : int` carrying the KIND** instead of a declaration-site marker. Rejected: it
  makes optional syntax load-bearing (dropping a constraint would silently reclassify N from
  a value to a type), puts the kind after the whole parameter list where a reader scanning
  `<T, N>` cannot see it, and degrades diagnostics (`Buf<int, 8>` would report "cannot find
  the type '8'"). C++, C#, and Rust all put the kind at the parameter and use
  `requires`/`where`/bounds only to constrain.
- **`const int N`.** `const` is implied by the position (see below), so this spells a no-op
  qualifier. **`const N`** additionally needs a "type omitted means int" defaulting rule that
  `int N` does not, and makes a later `string TAG` a second declaration form.
- **Bare `N`.** Undecidable: generic arguments are carried as TEXT (`typeArgs` is
  `std::vector<std::string>`, `MainListener_Generics.cpp:382`) and resolved at instantiation,
  so at `Buf<int, CAP>` the only source of truth for "is CAP a type or a value" is the
  declaration. Inferring the kind from body usage fails three ways: ordering (shells are
  pre-registered by ForwardRefScanner BEFORE the body is understood), cycles
  (`struct Wrap<T, N> { Buf<T, N> b; }` cannot classify N without first classifying Buf's),
  and underdetermination (a param used only in a method, only in a `where`, or not at all).

---

## Why `const` is implied

`const` is a MUTABILITY qualifier everywhere else in CFlat: `const int LANES8 = 8;` at file
scope is meaningful precisely because the unqualified `int LANES8 = 8;` is a different,
mutable thing. Inside a generic parameter list there is no such pair. A generic parameter is
substituted at monomorphization and burned into the mangled symbol name - `Buf__int__8` -
so it cannot be anything but a compile-time constant. There is no mutable reading of `N` for
`const` to rule out, which makes `const N` a no-op qualifier rather than a redundant one.

`int N` therefore declares the constant's TYPE, not its mutability. The two spellings are not
"long form vs short form" of the same statement: `int N` says something true and useful (N is
an int), while `const` in that position says something that could not have been false.

C++ reaches the same conclusion for the same reason - `template<std::size_t N>` carries no
`const`. Rust's `const N: usize` is not a counterexample: Rust needs a discriminator because
its `<>` list otherwise holds only types and lifetimes, so `const` there is doing the job
that the type token does here. C# has no value parameters at all, so it offers no evidence
either way.

---

## What exists today - measured

### 1. Value arguments do not parse at all

`typeParameterEntry` (`CFlat.g4:411`) is
`Identifier? typeSpecifier pointer? arrayTypeSuffix? Ellipsis?` - type arguments only. The
leading optional Identifier is the `unique` soft keyword, text-matched in the listener.

```cflat
struct Buf<T, N> { T[4] items; };
extern int main() { Buf<int, 8> b = default; return 0; }
```
```
error: cannot understand the code at 'extern int main() { Buf<int, 8'
```

### 2. `simd<T,N>` is the early form, and its value slot is literal-only

```antlr
simdTypeSpecifier
    : 'simd' '<' typeSpecifier ',' assignmentExpression '>'     // CFlat.g4:343
```

The grammar promises an expression. The implementation does not deliver one:
`TryParseSimdLaneCount` (`MainListener.h:926`) is handed
`assignmentExpression()->getText()` and runs `std::stoull` on the raw TEXT. Measured - both
of these are rejected:

```
simd<float, LANES>    // -DLANES=8            -> simd lane count must be an integer literal (got 'LANES')
simd<float, LANES8>   // const int LANES8 = 8 -> simd lane count must be an integer literal (got 'LANES8')
```

The grammar comment at `CFlat.g4:341` even claims "N is parsed as an expression and
constant-folded in the listener (mirrors arrayDimSpec)". It is not. This is the single
best argument for Stage 0 below: one shared evaluator retires the mismatch and makes
`simd<float, LANES>` work as a side effect.

`doc/HPC.md:159-163` documents the lane-count rule: power-of-two integer literal in `[2,64]`,
with a "did you mean simd<...,8>?" hint. That validation must survive verbatim.

### 3. `where` is narrower than its own documentation

```antlr
typeParameterConstraint
    : Identifier ':' Identifier          // CFlat.g4:418
```

Measured:

```cflat
where T : IShow        // parses
where T : ICmp<T>      // ERROR: found '<' but expected {'lock', '{'}
where T : int          // ERROR: found 'int' but expected Identifier
```

The second form is the example `doc/LANGUAGE.md:774` advertises
(`T maxOf<T>(T a, T b) where T : IComparable<T>`). **That was a shipped doc/grammar mismatch
independent of this plan.** It was filed, then fixed on 2026-08-27 by widening the constraint
target to `genericIdentifier`; the issue file is deleted. `where T : int` stays rejected - the
rule is a name-to-name binding and no use case needs a primitive there.

### 4. The constant-folding machinery already exists, in two copies

- `ForwardRefScanner::ScannerFoldIfConst` / `...Leaf` (`ForwardRefScanner.cpp:933`, `984`) -
  parse-tree based, no LLVM values. Handles the full binary chain (including the `'>' '>'`
  two-token shift join), ternaries, parens, integer literals, and identifier leaves resolved
  against `compilerLLVM->compileTimeMacros` (int-typed only). Rejects anything whose result
  leaves int32 range rather than folding it differently from codegen.
- `MainListener::TryFoldConstInt` (`MainListener_Expressions.cpp:9687`) - `llvm::Value` based,
  main pass only.

**This is the both-pass duality CLAUDE.md warns about, and it decides the architecture:**
mangled names are computed while instantiations are QUEUED, so the value argument must fold
in the SCANNER pass. `TryFoldConstInt` is unavailable there. Stage 0 therefore builds on
`ScannerFoldIfConst`, not on the LLVM-value folder.

`if const` folding is already proven over `const int` / `const u32` globals, enum members,
and shifts - `Test/test_basic.cb:1730` (`IfConstFlags.IFC_ON == 0`), `:1750`, `:1782`
(`(IF_CONST_BIG >> 1) == 0x7FFFFFFF`), `:1792` (`(IF_CONST_NEG8 >> 1) == -4`), and at FILE
scope over declarations at `:2012-2025`.

**RESOLVED 2026-08-26 - the two folders differ in reach, and it constrains Stage 0.**
`ScannerFoldIfConstLeaf`'s identifier leaf reads ONLY `compileTimeMacros`
(`ForwardRefScanner.cpp:1120`), and `SetCompileTimeMacro` has just three call sites (platform
macros, `__FILE__`, `-D` defines) - const globals are never inserted there. File-scope
`if const` over a `const` global still selects declarations correctly (measured), because
that fold is done by the MAIN pass: `TryFoldConstInt` carries a `constGlobals` set and reads
the global's initializer.

| Constant source | Scanner pass | Main pass |
|---|---|---|
| integer literal | folds | folds |
| `-D` define (in `compileTimeMacros`) | folds | folds |
| `const` global | **does NOT fold** | folds |

Mangled names are computed while instantiations are QUEUED, i.e. in the scanner. So without
a fix, `Buf<int, CAP>` (a define) would instantiate while `Buf<int, KONST>` (a const global)
would not - the same literal-only asymmetry as the simd bug in (2), reached from the other
direction. **Stage 0 must close it.** (As built, it does - but NOT the way this paragraph
recommended. Seeding into `compileTimeMacros` was tried and reverted; see the as-built note at
the top of this file. The seeded values live in a separate fold-only map.)

> **`const` is currently unenforced - deliberate, do not "fix" it here.** `const` on a global
> is accepted and then dropped: `const int KONST = 7;` emits `@KONST = global i32 7` (plain
> `global`, not `constant`), reads emit a `load`, and `KONST = 9;` compiles with no diagnostic
> and mutates at runtime (measured - prints 9). The same non-enforcement was measured on const
> locals, parameters, and struct fields. **Maintainer ruling 2026-08-26: leave as is** - const
> enforcement is a feature to be designed later, not a bug to file against this work.
>
> It does not block this plan: the main-pass folder reads the INITIALIZER, which is correct
> for any global that is never written. But Stage 0 must not be built on an assumption that a
> `const` global is immutable, and the form cannot be removed either way - `Test/test_basic.cb`
> reads const globals at 15 `if const` sites (`:1710-1792`, `:2012+`) plus `:1034-1037`,
> including the file-scope declaration-selection tests.

### 5. `-D` defines (landed, uncommitted) are the constant source

`--define`/`-D` puts int and string constants into `compileTimeMacros` before the first file
is parsed (`cflat/main.cpp`, `LLVMBackend_MoveDataflow.cpp` `SetUserDefines` /
`SetPlatformMacros`, wired in `LLVMBackend.cpp` `Compile`). Regression:
`Test/cli_defines_fixture.cb` plus the `cli_defines` block in `test.sh` / `test.bat`, which
already proves a define works as a fixed-array bound INSIDE a generic struct and as an
`if const` condition inside a generic body.

### 6. Mangling is a `__`-join over unnormalized spellings

`MangledGenericName` (`MainListener_Generics.cpp:402`) is `base + "__" + MangleTypeArg(arg)`
per argument. Per `internal/plan/funcptr-type-mangling.md`, type-argument spellings are NOT
normalized today - `Box<int>` and `Box<i32>` emit two distinct LLVM types of identical
layout. **Value arguments are the same class of problem** (`CAP*2` vs `16`), and decision #6
is the fix for the value half. The two plans should land compatible mangling; do not invent
a second scheme here.

---

## Stages

Each stage is independently reviewable. Stage 0 has standalone value and should land first
whether or not the rest proceeds.

### Stage 0 - one shared const-argument evaluator (+ simd retrofit)  [LANDED]

A three-part pipeline, callable from the scanner pass:

1. **Fold** an `assignmentExpression` to an int64 via `ScannerFoldIfConst`.
2. **Validate** with a per-consumer predicate. simd's stays exactly as it is
   (power-of-two in `[2,64]`, with the existing "did you mean" hint). A generic value param's
   is "must fold to an integer in range".
3. **Canonicalize** to the mangling token (Stage 2).

Replace the `std::stoull`-on-`getText()` call in `TryParseSimdLaneCount` with stage 1, keeping
stage 2 byte-identical so `doc/HPC.md`'s documented diagnostics do not move.

Acceptance: `simd<float, LANES>` with `-DLANES=8` compiles and runs; `simd<float, 7>` still
produces the power-of-two diagnostic verbatim; `simd<float, LANES>` with `-DLANES=7` produces
the same diagnostic; and a `const` global lane count (`simd<float, LANES8>`) folds too, which
requires closing the scanner/main-pass reach gap documented in (4) above.

### Stage 1 - grammar  [LANDED]

- `typeParameterEntry`: admit the value-param form. Primitive type token then Identifier, so
  the branch is decided on the first token and never collides with `unique T`.
- Use site: the argument slot must accept an `assignmentExpression` as well as a
  `typeSpecifier`, resolved by consulting the declaration's parameter kinds at instantiation
  (arguments are already carried as strings, so this is a listener decision, not a parse one).
- Adopt the C++ rule that `>` closes the argument list. Note `ScannerFoldIfConstLeaf` already
  rejoins `'>' '>'` into a shift - the same token hazard, already handled once.

### Stage 2 - mangling scheme  [LANDED]

Settle this BEFORE any code, because it must be collision-free against a future string value
param even though only ints ship:

- negatives need a token that is valid in a symbol: `Buf<int, -8>` -> `Buf__int__n8`
- a future string arg needs a length-prefixed or hashed form so it can never alias an int
  arg: e.g. `__s5_hello`
- keep compatible with `internal/plan/funcptr-type-mangling.md`

### Stage 3 - instantiation  [LANDED]

Bind the parameter as a SCOPED compile-time macro pushed for the duration of the
instantiation and popped after - the same `compileTimeMacros` entry shape a `-D` define
produces, differing only in lifetime. Everything downstream (`if const` at function, member
and file scope; fixed-array bounds; ordinary expression use) then works with no further
change, which is exactly what the `cli_defines` regression already demonstrates for defines.

### Stage 4 - widened `where`  [LANDED (struct/class/function scope; not interfaces)]

Extend `typeParameterConstraint` past `Identifier ':' Identifier` to cover, in one change:

- `T : IFace` (today)
- `T : IFace<...>` (documented at `doc/LANGUAGE.md:774`, does not parse - see above)
- `N > 0` and similar predicates over value params

A violated value constraint reports at the instantiation site. Note the body-level equivalent
already exists (`if const (...) { compile_error("..."); }`), so Stage 4 is a diagnostics and
readability improvement, not a capability gate - it can slip without blocking Stage 3.

### Stage 5 - diagnostics and tests  [LANDED]

Diagnostics to write (LogError only, ASCII only):

- kind mismatch: a type where a value is expected and vice versa, naming the parameter
- a value argument that does not fold, quoting the offending expression
- an out-of-range or non-integer value argument

Tests: extend `Test/cli_defines_fixture.cb` (already driven with `-D` by both suites) rather
than adding files - it is already the fixture for "a constant reaches generic code". Add the
value-param cases there and extend the `cli_defines` blocks in `test.sh` / `test.bat`.
Negative cases go in `Test/errors/` via `expect_error`.

---

## Prior art, condensed

| | Declaration | Argument expressions | Specialization |
|---|---|---|---|
| **C++** | `template<std::size_t N>`; `template<auto N>` deduces the type; defaults and deduction from function args both work | any constant expression; equivalence is STRUCTURAL, so `N+1` and `1+N` are different until resolved - the trap decision #6 avoids | full and partial specialization, plus `if constexpr` |
| **C#** | none - no non-type generic parameters at all, repeatedly proposed and never shipped because generics are REIFIED at runtime. Faked with marker types (`Buf<int, N8>`) or constructor args | n/a | none; JIT shares code for reference types |
| **Rust** | `struct Buf<T, const N: usize>`; integers, `bool`, `char` only; defaults allowed | anything past a bare literal or param needs braces (`{ N + 1 }`), and arithmetic in type position still needs unstable `generic_const_exprs` | none on stable; traits plus const folding instead |

C# is the cautionary data point, not a model: its objection to value parameters is a
consequence of reified runtime generics and does not apply to CFlat, which monomorphizes.

**Non-int value parameters in practice.** After integers, only one category has real
demonstrated demand: **strings** - C++20's flagship NTTP extension (CTRE's
`ctre::match<"[0-9]+">`, `Fixed<"name">` typed identifiers, compile-time field names for
serialization) and among the most-requested unstable Rust features. `bool` (`template<bool
IsConst> class iterator` unifying `iterator`/`const_iterator`) and `enum` (storage order,
endianness policies) are common in C++ SPELLING but are int-shaped in CFlat and cost nothing.
Function/member pointers (`template<auto F>` for zero-overhead delegates) are a real but
advanced category. Stage 2's mangling reserves room for strings; the rest stay closed.
