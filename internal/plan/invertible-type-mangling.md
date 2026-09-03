# Invertible type-name mangling: every customer-facing surface demangles before it prints

Opened 2026-09-02, promoted from `internal/issue/p4/user-facing-type-names-print-mangled.md`
(deleted with this file). Maintainer ruling the same day: **proper long-term fix** - the mangled
name must be invertible by a pure function, and every surface a user reads (`typeof`,
diagnostics, overload candidate lists, LSP hover/completion, `--symbol-dump*` banners, crash
dumps) demangles before printing. No side table of display names.

Related, partly landed: [funcptr-type-mangling.md](funcptr-type-mangling.md) canonicalized the
`function<>`/`Lambda<>` namers; its Stage B (qualified struct keys) is the open remainder and
folds into Stage 1 here. Both plans share the rule: one funnel, string equality, no bespoke
comparison predicates.

## Why the current scheme cannot be inverted

The scheme balances readability against uniqueness (`array__i32`, `list__string`) and was never
designed to be read back. Measured on this tree (probes in `scratch/mg.cb`):

1. **Separator collision.** `__` is legal inside an identifier (`Nondigit: [a-zA-Z_]`), so
   `A<B>` and a user `struct A__B` mangle to the same name. Confirmed: both `typeof` to `A__B`
   and the compiler reports no redefinition - a silent shared type.
2. **No nesting or arity.** `GBox<Inner<int>>` flattens to `GBox__Inner__i32`; splitting on
   `__` yields `GBox<Inner, i32>`. `DisplayNameOfMangledType` bails to the raw name whenever the
   segment count disagrees with the template arity.
3. **Suffix codes are plain letters.** `*` -> `ptr`, `[]` -> `view`: `IShape2ptr` is not
   distinguishable from a type of that name. Primitives stay canonical (`i32`), so even the
   happy path prints `array<i32>`, not `array<int>`.
4. **Function symbols have the same flaw, with no separator at all.**
   `ComputeMangledName` (LLVMBackend_ControlFlowAndFunctions.cpp:1426) concatenates
   `ToUniqueString()` per parameter: `_set_void_array__IShape2Ptri32IShape2_`. Overload candidate
   lines print this symbol verbatim.

Blast radius, counted 2026-09-02:

| Surface | Count |
|---|---|
| compiler files that build or parse a mangled name | 18 |
| string-splice sites on `"__"` (incl. prefix checks such as `array__`, `list__`, `unique__`) | 69 |
| Test files pinning a mangled spelling (`typeof`, `expect_error` text, symbol names) | 17 |
| docs describing the scheme (THREADING, HPC, noalias-arrayview, parallel-helpers, c-interop-anon-records, fix-issue-lessons) | 6 |
| LSP server | 1 file |

## Design

**Reserved separator.** `$` - not in the identifier lexer rule, and battle-tested in object
files: MSVC, Swift and Java all emit `$` in symbols, so lld-link/COFF, ld64/Mach-O, ELF, nm and
debuggers accept it. `<`, `>`, `,` are legal in LLVM quoted names but are an unexercised COFF risk
and are NOT used.

**Scheme (corrected 2026-09-03, see "Stage 1b").** A type is `base` followed by exactly
`arity(base)` argument types, each `$`-prefixed. Reference kinds and qualifiers are CODE tokens
placed BEFORE the type they decorate (Itanium `P` order), and every non-type token starts with a
`.` so it can never be read as an identifier: `.p` pointer (one per level), `.v` array view,
`.a` alias qualifier, `.m` move parameter, `.va` varargs, `.8` / `.n8` numeric value argument.
Arity recursion then needs no brackets and no slot-kind knowledge.

| Source | Old (`__`) | New |
|---|---|---|
| `array<int>` | `array__i32` | `array$int` |
| `array<IShape2*>` | `array__IShape2ptr` | `array$.p$IShape2` |
| `array<int>*` (as a parameter) | `array__i32Ptr` | `.p$array$int` |
| `list<string*>*` vs `list<string**>` | `list__stringptrPtr` vs `list__stringptrptr` | `.p$list$.p$string` vs `list$.p$.p$string` |
| `int[]` view arg | `i32view` | `.v$int` |
| `GBox<Inner<int>>` | `GBox__Inner__i32` | `GBox$Inner$int` |
| `Pair<int, string>` | `Pair__i32__string` | `Pair$int$string` |
| `list<alias Circle*>` | `list__alias_Circleptr` | `list$.a$.p$Circle` |
| `unique<IS>` | `unique__IS` | `unique$IS` |
| `ns.Item` | `ns.Item` | `ns.Item` (a base never starts with `.`) |
| struct `A__B` | `A__B` | `A__B` - collision gone |
| `Buf<8>` / `Buf<-8>` | `Buf__8` / `Buf__n8` | `Buf$.8` / `Buf$.n8` |
| `list<Lambda<int(int)>>` | `list____fatfn_1_3_i32_3_i32` | `list$fatfn$.1$int$int` (kind, param count, return, params) |
| function `int f(list<string*>* a)` | `_f_i32_list__stringptrPtr_` | `_f$int$.1$.p$list$.p$string` |

Why postfix codes were wrong (Stage 1 as first landed): `list$string$p$p` cannot say whether the
second `$p` decorates `string` or `list<...>`; `f(list<string*>*)` and `f(list<string**>)`
collided and reported a false redefinition (repro scratch/amb.cb).

**Canonical spelling on the way out.** Aliases are folded on the way in (they already are:
`ResolveManglingAlias`), primitives are canonical keywords on the way out (`i32` -> `int`, `f64` ->
`double`, `u8` -> `u8`). So `array<MyInt>`, `array<i32>`, `array<int>` are one instantiation and
all print `array<int>` - the C++/Rust rule (aliases folded, canonical names printed).

**Function symbols.** `ComputeMangledName` becomes `_<name>$<ret>$<n>$<arg>...` on the same
encoder, so `_set_void_array__IShape2Ptri32IShape2_` becomes
`_set$void$3$array$IShape2$p$this$int$IShape2` (exact shape to be fixed in Stage 1; the
requirement is that the demangler prints `set(array<IShape2*>* this, int, IShape2)`). `M`
(move) and the `this` receiver become codes, not suffix letters.

**The pair.** `MangleType(const TypeAndValue&) -> std::string` and
`DemangleType(std::string_view) -> TypeAndValue` (or a spelling string) live together in one
header, are total inverses on every value the compiler can produce, and are the ONLY code that
knows the scheme. `MangleTypeArg`, `MangledGenericName`, `CanonicalWrapperTypeName`,
`ToUniqueString`, `BuildEncodedClosureName`, `DisplayNameOfMangledType`,
`DisplayNameOfCoreUniqueType`, `MangledGenericNameIsAmbiguous`, `IsCoreArrayType`'s prefix
match, and the 69 `"__"` splice sites collapse onto them. A prefix check like
`rfind("array__", 0) == 0` becomes `DemangleBase(name) == "array"`.

**No new persisted state.** Because the name inverts, `--init` cache round-trip needs nothing
new; the cache is invalidated once by the scheme change (`--init-clear-local`).

## Surfaces that must demangle (acceptance list)

- `typeof(expr)` - three `CreateGlobalString("typeof", ...)` sites in
  MainListener_PostfixExpression.cpp (~6857-6884); pointer/view suffix appended after.
- Every `LogError*` that formats a `TypeName`: audit `std::format(... TypeName ...)` and
  `LocalizeMessage(..., { ...TypeName... })` in `MainListener_*.cpp` and `LLVMBackend_*.cpp`.
  Mechanical rule: a `TypeName` never reaches a format argument raw; it goes through
  `Spelling(tv)`. Enforce with a grep in `utilities/extract_diagnostics.py --report` (fail the
  report if a format argument is a bare `.TypeName`).
- Overload-resolution failure candidate lines (`ComputeMangledName` symbol today).
- LSP: hover type, completion detail, signature help, symbol index kinds (`LspServer.cpp`,
  `LspSymbolIndex.cpp`); `test_lsp.sh` pins the new spellings.
- `--symbol-dump` / `--symbol-dump-ir` / `--symbol-dump-opt` banners and `function:<name>`
  selector: selector accepts the SOURCE spelling (`function:set` or `function:array<int>.init`)
  and the banner prints it; the raw symbol stays accepted for IR readers.
- CompilerManager crash-dump state (struct registry listing).
- `compile_error(...)` messages in core that interpolate a type (grep `core/*.cb`).

Non-surfaces (stay mangled, by design): LLVM IR type/function symbols, `.ll` output, object
files, the `--init` cache JSON.

## Staging - tree green at every step

Status 2026-09-03: Stage 0 and Stage 1 LANDED in worktree unique-type-prototype-4ba544
(uncommitted): `cflat/TypeMangling.{h,cpp}` owns the `$` scheme, function symbols are
`_name$ret$argc$args`, cache metadata bumped 9 -> 10, the `A<B>` / `A__B` collision has a positive
leg in Test/test_generics.cb. Stage 2 LANDED 2026-09-03: 120 diagnostic sites re-routed through SpellType /
SpellFunctionSymbol, typeof / LSP / symbol-dump / crash dump spell source names, and
utilities/extract_diagnostics.py fails the report on a bare `.TypeName` or `mangled*` format
argument. Stage 1b (prefix `.x` codes) applied after the postfix-code collision above was found. The Windows link of `$` symbols (test.bat Release) is the remaining proof.

**Stage 0 - funnel (behaviour-neutral, largest step).** Introduce the mangle/demangle pair with
the CURRENT `__` scheme and prove it by round-tripping every instantiation the suite produces
(debug assert in `QueueGenericInstantiation`: `Mangle(Demangle(name)) == name`). Move all 69
splice sites, `ToUniqueString`, `BuildEncodedClosureName` and the prefix checks onto it. No test
expectation changes. This is mechanical and is the Codex-sized half; it also completes
funcptr-type-mangling Stage B.

**Stage 1 - flip the scheme to `$`.** One-line change inside the pair plus the test/doc
migration: 17 test files, 6 docs, LSP fixtures, pseudo-locale catalog regenerated by the
documented pipeline (never hand-edited). Delete `MangledGenericNameIsAmbiguous`, the core-unique
special cases and `mangledTypeDisplayNames`. Add `Test/errors/err_mangling_collision.cb`
retirement: the `A__B` vs `A<B>` program now compiles with two distinct types (positive leg in
`Test/test_generics.cb`).

**Stage 2 - surfaces.** Route the acceptance list through `Spelling(tv)`; `typeof` expectations
in `Test/test_reflect.cb` move to `array<int>` / `array<double>`; overload-candidate and
LSP expectations updated; the extract_diagnostics guard added. Warm-cache second pass over
`Test/errors` to prove every `expect_error` still fires.

## Decisions (settled from compiler usage, 2026-09-02)

1. Function-symbol shape: internal detail, fixed by Stage 0 when it writes the pair; the only
   requirements are injectivity (round-trip assert) and that the demangler prints
   `set(array<IShape2*>* this, int, IShape2)` for the candidate line.
2. `typeof` of a pointer to an instantiation prints `array<int>*` - the same no-space suffix
   rule `typeof` already uses for `int*` (pinned in Test/test_reflect.cb).
3. `--symbol-dump function:` accepts both the raw symbol and the source spelling.
4. Separator `$` - ruled with the plan; the Windows link is the proof (see Verification).

## Verification

- `bash test.sh Release` 0 failed on every stage; `bash test_example.sh` 45/0;
  `bash test_lsp.sh Release` (MainListener.h and LspServer.cpp change).
- Stage 0 assert: zero round-trip failures across the full suite in Debug (assertions-on LLVM).
- Stage 1: `git diff --stat cflat/locales/` contains ONLY generator output.
- Windows: `test.bat Release` - the `$` in COFF symbols is the one concrete cross-platform risk;
  a Release link of `Test/test_generics.cb` and `example/` on Windows is the proof.
