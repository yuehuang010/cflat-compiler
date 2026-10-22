# Nullable types: finish `T?` and tie it into the safety checks

Status: DRAFT 2026-10-22. Ruling 2026-10-22: `T?` stays and gets finished. Not started; section 6
needs rulings before P1.

## 1. Where it stands (measured 2026-10-22, Release, macOS arm64)

Plumbing exists, semantics do not. `T?` is `T*` with a flag nobody reads for safety.

- Grammar `typeSpecifier '?'? pointer? arrayTypeSuffix?` (`CFlat.g4:283`). So `Box?`, `Box?*`,
  `Box?[]` parse; `Box*?` does not; `Holder<Box?>` (generic ARGUMENT) does not parse (probe p5).
- `IsNullable` on `TypeAndValue`/`NamedVariable` (`LLVMBackend.h:865,1383`), set with
  `Pointer = true` in both `ParseDeclarationSpecifiers` copies (`ForwardRefScanner.cpp:667`,
  `MainListener_Declarations.cpp:2121`); primitives refused ("nullable '?' is not allowed on
  primitive type"); serialized in the `--init` cache; default-null with no initializer
  (`MainListener_Declarations.cpp:10030`); `?` vs `*` in one diagnostic
  (`LLVMBackend_CodegenHelpers.cpp:2017`).
- `f(Box?)` + `f(Box*)` is a REDEFINITION (probe) - nullability is not in the signature. Keep.
- Behaviour (probes p1/p2/p6): `Box* p = q;` and `Box? r = p;` both silent; `q->v`, `q.get()` on
  a `Box?` compile unchecked; `take(Box* p)` called with a null `Box?` compiles and SIGSEGVs.
  `?.`/`??` work identically on plain `T*`. `unique Box? w = new Box();` compiles (meaning
  undefined). `Holder<T> { T? item; }` with `T=Box` works.
- Uses: 37 in tests (Test/ 24, errors/ 13), 0 in core and example/. Every positive use reads via
  `?.`/`??` or a `nullptr` compare (test_basic:4340, test_operators:865-888, test_move:684, ...).
- Existing null analysis to build on: `nulldf` (`MoveDataflow.h:379`,
  `RunNullDerefDataflow` in `LLVMBackend_MoveDataflow.cpp:193`). It is the OPPOSITE polarity:
  it reports a deref only when a `move` PROVABLY nulled the value (control-dependence
  containment, false-positive-free by construction). `T?` needs a must-NON-null proof that
  fails closed. Reuse its event-log + end-of-body hook + noreturn set; do not reuse its rule.
- C import: `_Nonnull`/`_Nullable`/`_Null_unspecified` are STRIPPED textually at ~8 sites in
  `LLVMBackend_CInterop.cpp` (1824, 2082, 2180, 2313, 4220, 5290, 5701, 24858) - nullability is
  discarded today. macOS SDK: `stdio.h`/`stdlib.h`/`string.h` carry 0 annotations,
  `pthread.h` 36; 68 of 3127 `usr/include` headers are annotated; Foundation headers ~1570
  `nullable` tokens.
- Blast-radius counts (grep, raw `->` tokens are an upper bound on deref sites):

  | tree | `->` | `?.` | `??` | `== / != nullptr` | `return nullptr` |
  |---|---|---|---|---|---|
  | cflat/core (68 files) | 842 | 2 | 0 | 506 | 108 |
  | Test/ (49 files) | 3777 | 286 | 353 | 870 | 14 |
  | example/ (125 files, recursive) | 1869 | - | - | - | 16 |

  Core: 197 pointer-returning function definitions, 49 of them contain `return nullptr`
  (ui_native 27, os 22, the allocators ~24, arena_channel 6, threadpool 5, numa 5, xml 3).

## 2. Goal and semantics (recommended model)

**Two pointer kinds, Kotlin-platform-type shaped:**

- `T?` = CHECKED nullable pointer to `T`. Same representation as `T*` (zero codegen change,
  same ABI, same mangling). The compiler refuses any dereference it cannot prove non-null.
- `T*` = C pointer, nullability UNKNOWN (Kotlin's `T!`). Unchecked, exactly as today. `T*` is
  NOT re-ruled as non-null - CFlat is a C extension, and the 2026-09-04 delete-form ruling keeps
  `T* p = new T[n]` C-compatible. Flipping the default is P3's question, recommended NO.
- There is no separate non-null pointer spelling. "Proven non-null" is a FLOW fact on a `T?`
  binding, not a type (Kotlin smart cast / TypeScript narrowing, not Swift's `T` vs `T?`).

Conversions:

| from -> to | rule |
|---|---|
| `T*` -> `T?` | implicit (widening; always safe) |
| `nullptr` -> `T?` | implicit |
| `T?` -> `T*` (init, assign, arg, return, field store) | only where the value is proven non-null at that point; else ERROR. Explicit `(T*)q` cast is the assertion escape hatch (no new `!!` token; matches the pointer-not-number ruling's explicit-cast style) |
| `T?` -> `T?` | implicit |
| `T?` -> `bool` / `== nullptr` | allowed, and is how narrowing starts |

Per position:

- **Locals / params**: narrowable (section 3). `T?` param = callee must check; `T*` param =
  caller may pass anything (unchecked) but may not pass an unproven `T?` (no implicit
  narrowing at calls - consistent with the 2026-09-04 implicit-narrowing-at-calls ruling).
- **Returns**: `T? find()` forces callers to check; `return q;` from a `T*` function needs `q`
  proven. The return-follows-assignment ruling applies unchanged.
- **Fields** (`struct Node { Node? next; }`): `?` legal, default null. Field paths are NOT
  narrowed in P1 (Kotlin rule: mutable properties never smart-cast) - read into a local, or use
  `?.`. P1b may add TypeScript-style path narrowing (Q4).
- **Globals**: never narrowed (any call may rewrite them; threads).
- **`move T?`**: legal param/sink. After `move x`, `x` is definitely-null in the flow state
  (explicit-move-nulls-source: readable-as-null); its TYPE does not change. `T*` keeps nulldf.
- **`unique` / handles**: `unique<T>` already has an empty state; `?` on it is redundant. P1
  refuses `?` on a `unique`/`shared`/view declaration (Q5) so a meaning can be assigned later
  without a break.
- **Views / arrays**: `Box?[]` = view of nullable pointers; `arr[i]` yields a `T?` that is not
  narrowable (element path) - copy to a local or `?.`.
- **`T?*`**: pointer to a `T?` slot, `*pp` an unstable `T?`; `T*?` stays unparsable.
- **Generics**: `T?` substitutes literally: `T=Box` -> `Box?`; `T=Box*` -> nullable `Box**`;
  `T=int` -> the primitive refusal, reported at the instantiation with its trace. `Holder<Box?>`
  must parse as a generic argument (P1 grammar fix; generic arg is a type, so it is a rule
  change, no predicate).
- **`?.` / `??`**: stay legal on both `T?` and `T*`. On `T?` they are the canonical checks:
  `q?.f` where `f` is a pointer field yields `T?`; `q ?? &b` with a proven non-null right side
  yields a non-null result (assignable to `T*`). `??` on integers (zero = empty) is unrelated to
  nullability and unchanged.
- **`string?`**: stays "nullable pointer to string" (p4 option 1). An owning optional value is a
  different feature - `optional<T>` library struct in P4 (Q6), not a second meaning of `?`.

## 3. Safety-check tie-in: flow-sensitive narrowing (P1 core)

New analysis `nndf` (must-non-null), sibling of `nulldf`, same plumbing: AST-time event log per
`llvm::Function`, solved at end of body over the emitted CFG, noreturn set shared.

Tracked: `T?` locals/params. An `&q` / `alias` of `q` drops tracking for the rest of the function
(every later deref refused, fail-closed, the escape named in the diagnostic).

Facts (per CFG edge, forward must-analysis, meet = intersection):

- NonNull after: init/assign from `new`, `&x`, `this`, a string/array address, a narrowed
  `T?`, a `T*` value (platform: trusted, Q3), `a ?? b` with `b` non-null.
- Null after: `= nullptr`, `move q`, default-init.
- Maybe after: assign from an unproven `T?`, a `T?`-returning call, `q?.f` of pointer type.
- Branch refinement on `CondBr` edges, recorded by the condition emitter (no IR pattern
  matching): `q != nullptr`, `q == nullptr`, `q`, `!q`, through `&&`/`||` short-circuit blocks
  and the ternary. Early exit (`return`/`break`/`continue`/noreturn call) in the null arm narrows
  the fall-through - this falls out of the CFG for free.
- Loops: fixed point over the CFG (the lattice is 3 values per name, so it terminates).

Refused (each a LogError, earliest per name per function, like nulldf):

- `q->f`, `q.f` (auto-deref), `*q`, `q[i]`, method call through `q`, `q(...)` for a `T?`
  function pointer, while `q` is not proven non-null.
- `T?` -> `T*` flows listed in section 2.

Diagnostic text (drafts; `LogErrorContext` pointing at the declaration as the second location):

- `'q' may be null here (declared 'Box?'); test it with 'if (q != nullptr)', or use 'q?.v' /
  'q ?? fallback', or assert with '(Box*)q'`
- `cannot pass 'q' of type 'Box?' to parameter 'p' of type 'Box*' - 'q' may be null here`
- `'q' is null here (it was moved on line 12)` when the flow fact is definitely-null.
- `'q' may be null here: its address was taken on line 9, so a check cannot be trusted`

Not refused: compare, `?.`, `??`, `delete` (null is a no-op), pass to a `T?` param, store to `T?`.

Ownership: `move q` is a Null event in nndf; nndf owns `T?` names, nulldf keeps `T*` names (no
double report). `IsOwning` is orthogonal: `Box? b = new Box();` owns; scope-end delete of null is
a no-op.

Known residue to CLOSE in P1, not record: end-of-body checks run only on the named-function path
(fix-issue-lessons "An end-of-body deferred check runs only in the named-function path"). For a
fail-closed check that hole means a lambda body silently skips the refusal, so P1 must run nndf
on lambda bodies too, with an acceptance leg.

Cost: no codegen change (byte-identical `--out-lli`, the null-interface ruling's bar). No
`CFLAT_NULL_DF_OFF`-style kill switch: a fail-closed type rule must not be switchable.

## 4. Prior art and which model to follow

- **Kotlin**: `T?` vs `T`, smart casts on stable values only (locals, same-module `val`s), `?.`,
  `?:`, `!!`. Java types are platform types `T!` (unchecked) unless annotated `@Nullable` /
  `@NonNull`, which Kotlin enforces as ERRORS. Closest fit: CFlat `T*` = `T!`, `T?` = `T?`.
- **Swift**: `Optional<T>` enum, `if let`/`guard let` bind a non-optional name, `!` traps. ObjC
  imports went from implicitly-unwrapped `T!` to `_Nonnull`/`_Nullable` audits plus
  `NS_ASSUME_NONNULL` - imports are the hard part, and annotation audits are incremental.
- **C# 8 nullable reference types**: flow analysis, `!` suppression, `[NotNullWhen]`; rolled out
  as WARNINGS behind `#nullable enable` because flipping `T` to non-null broke the world. CFlat
  has no warnings, so that rollout path does not exist - the decisive argument against a
  non-null `T*` default.
- **TypeScript strictNullChecks**: opt-in flag; narrows property paths (`a.b`) and does NOT
  invalidate them across calls - ergonomic, unsound.
- **Rust**: `Option<&T>` / `Option<Box<T>>` use the null niche (pointer-sized, `None` = 0); `&T`
  never null; narrowing by `if let Some(p)`. Confirms `T?` can share `T*` representation.
- **clang**: `_Nonnull`/`_Nullable`/`_Null_unspecified`, `#pragma clang assume_nonnull`; all
  diagnostics are warnings (`-Wnonnull`, `-Wnullable-to-nonnull-conversion`).

**Follow Kotlin**: errors not warnings, a fully checked nullable type, an unchecked platform
pointer for C, foreign annotations enforced when present. Take Rust's zero-cost niche and
Swift's import audit; reject C#'s warning rollout and TypeScript's unsound path narrowing (P1).

## 5. Phases (each lands alone; tests extend existing files, negatives are new `Test/errors/err_*.cb`)

**P0 - ruling, doc, grammar gaps (small).** Section in `doc/LANGUAGE.md` "Nullable types" under
Null-Safety (semantics of section 2). Grammar: `Holder<Box?>` as a generic argument; refuse `?`
on `unique`/handle types. Resolve Q1-Q7. Acceptance: legs in `test_generics.cb` (`Holder<Box?>`
round-trip, `T? item` with `T=Box`), `err_declarations.cb` leg for `unique Box?`.
Blast: 0 existing uses affected (no Test/ use spells `unique T?` or a generic `T?` argument).

**P1 - checked `T?` (the feature).** nndf analysis, conversion refusals, lambda-path hook,
diagnostics of section 3. Files: `MoveDataflow.h` (new namespace), `LLVMBackend_MoveDataflow.cpp`
(hook beside `RunNullDerefDataflow`), condition emitter in `MainListener_Statements.cpp` /
`MainListener_Expressions.cpp` (edge facts), postfix deref sites in
`MainListener_PostfixExpression.cpp` (Deref events), assignment/arg/return conversion sites.
No new `TypeAndValue` field (flow state is transient, cleared in `ResetForReanalysis`), so no
`--init` serializer change; if one IS added, same-change round-trip (CLAUDE.md rule).
Acceptance: positive legs in `test_basic.cb` `testNullableTypes` (if/else, early return,
`&&` chain, while loop, ternary, `??` non-null, reassign in loop, noreturn guard, lambda body);
`Test/errors/err_nullable_deref.cb` (unchecked `->`, `.`, `*`, `[]`, call; `T?`->`T*` at
init/assign/arg/return/field; after `move`; after `&q`; deref inside a lambda; narrowing lost
by reassignment in a loop). Byte-identical `--out-lli` on test_basic/test_operators. Green
`./test.sh Release`, `test_example`.
Blast: Test/ positive uses need 0 edits (all checked, section 1); core/example 0 (no `T?`).

**P1b (optional, gated on Q4) - field-path narrowing** (`if (n->next != nullptr) n->next->v`),
invalidated by any store to the path OR any call. Acceptance in the same two files.

**P2 - C/C++ import nullability.** Read the clang qualType BEFORE the ~8 strip sites:
`_Nullable` -> `T?` (callers must check), `_Nonnull` -> `T*` plus refusal of `nullptr` / an
unproven `T?` argument, unannotated / `_Null_unspecified` -> `T*` (platform). C++ `T&` already
non-null. The C/C++ header disk cache must carry the bit (verify the cache entry round-trips it
- import-cache-implied ruling makes every import cached). Acceptance: in-repo header fixture
(tests-no-third-party-libs) with annotated + `assume_nonnull` prototypes, legs in `test_c.cb`
and an `err_` file. Blast: only annotated headers (68/3127 SDK headers; pthread 36, libc core 0),
so stdio/stdlib calls are unaffected; Foundation/Cocoa bindings in `core/ui_*` and macOS
examples are the exposure - measure by compiling `example/` with P2 on and counting new errors
before landing. Optional P2b: CFlat-side curated nullable list for unannotated libc returns
(`fopen`, `malloc`, `getenv`, `strchr`, ...) - Q3.

**P3 - core API adoption (replaces "flip the `T*` default").** Retype the 49 core functions
that `return nullptr` (and pointer fields documented as optional) to `T?`, one module per
change, fixing callers. Measure first per module: callers in Test/ + example/ that deref
without a check = new errors. Acceptance: suite green per module, no `(T*)` assertion casts
added in Test/ (casts would hide the point). Flip of `T*` itself to non-null: NOT planned (Q1).

**P4 - optional values.** Close p4 `owning-nullable-string` with option 1 (documented borrow). Owned
optional values, if wanted: library `optional<T>` (value + present bit, `?.`/`??` via operator
forwarding, like `unique<T>`) - separate plan.

## 6. Open questions for the maintainer

**Q1. Is `T*` non-null by default?** (a) no - `T*` stays unchecked platform pointer, `T?` is
the checked opt-in; (b) yes, per-file opt-in (`#pragma nullable` / file attribute) like C#
`#nullable enable` but as errors; (c) yes globally behind a flag, then default. Blast for (c):
842 `->` in core, 3777 in Test/, 1869 in example/, plus every C import. Recommend (a): no
warnings exist to stage (b)/(c), a per-file dialect splits the language, and C compatibility of
`T*` is ruled. Safety grows by P3 adoption instead.

**Q2. Escape hatch** for "known non-null": (a) cast `(T*)q`; (b) postfix `q!` (Swift/C#/TS); (c)
`q!!` (Kotlin). Recommend (a): no new token, greppable, same explicit-cast habit as pointer->int.

**Q3. Unannotated C pointers.** (a) platform `T*` (unchecked); (b) treat all as `T?` (every C
call result needs a check - Kotlin abandoned this for Java in 2012); (c) (a) plus curated list
of known-nullable libc returns. Recommend (a) in P2, (c) as P2b if wanted.

**Q4. Narrow field paths?** (a) locals/params only (Kotlin); (b) TypeScript-style paths, killed
by any call or store; (c) paths, kept across calls (TS exact, unsound). Recommend (a) for P1,
(b) as P1b only after measuring how many `n->next` patterns P3 produces.

**Q5. `?` on `unique`/handles.** (a) refuse, redundant; (b) accept as doc; (c) gate the handle's deref.
Recommend (a) now (keeps (c) open).

**Q6. `string?`.** (a) nullable borrow forever, empty `string` for "no text"; (b) owning
optional `string` (p4 option 2) - makes `?` mean two things; (c) (a) now plus generic
`optional<T>` library type later. Recommend (c).

**Q7. Moved-from retyping.** Does `move x` on a `T*` make `x` checked like `T?` afterwards? (a) no,
nulldf's must-null report stays the `T*` rule; (b) yes. Recommend (a): (b) is the fail-closed
`T*` checking Q1 declines, made history-dependent.

## 7. Side findings (probes; not this plan, file separately if confirmed)

- `--run` null deref (p2, p9) prints the ICE banner; `Box* p;` local (p12) -> "Undefined variable p.".
