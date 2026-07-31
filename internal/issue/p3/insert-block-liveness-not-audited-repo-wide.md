# `GetInsertBlock() != nullptr` is used repo-wide as a liveness test, and it is not one

Filed 2026-07-31, from the `fix/ifconst-ir` P1 fix. This records the CONFUSION CLASS the `if const`
bug turned out to be one instance of. It is not a repro; it is an unfinished audit.

Severity: P3. **There is no live repro today.** Every spelling in the `if const` family that
reached the confusion is fixed, and no other spelling was found that reaches a remaining site.
This is filed so the class is greppable, not because something is known to be broken. Do not
re-rank it upward without an actual repro - see "Why P3" below.

## The confusion

`builder->GetInsertBlock() != nullptr` does **not** mean "there is a live block to emit into".

Nothing clears the IRBuilder insert point when a function definition finishes. The end-of-function
sequence is `CreateBlockBreak(nullptr, true)` followed by `ClearCurrentSubprogram()`
(`LLVMBackend.h:9058`), and `ClearCurrentSubprogram` clears only the DWARF debug location - not the
insert point, and not `currentFunction`. So once ANY function body has been emitted, the builder at
declaration scope still points at the last, already-terminated block of the PREVIOUSLY emitted
function. `SetInsertPoint(BB)` positions at `BB->end()`, i.e. AFTER the terminator, so a site that
mistakes non-null for live appends instructions past a `ret` in a function the user was nowhere
near.

The correct predicate already exists: `IsInsertBlockLive()` (`LLVMBackend.h:3078`) - non-null AND
`getTerminator() == nullptr`.

## Worked example (fixed - use it to recognise the signature)

`if const` leaf emission, fixed on `fix/ifconst-ir` by swapping the predicate at
`MainListener.h:7891`. A file-scope `if const` whose condition must EMIT (a const-global load, an
enum member; literals and `__PLATFORM__` fold without emitting and never noticed) produced:

```
Module verification failed:
Basic Block in function '_Test_int_charPtrstringstring_' does not have terminator!
label %ifResume
```

The named function is `Test(const char*, string, string)` from `Test/test_helper.cb`, emitted long
before the `if const` and completely unrelated to it. The actual IR:

```
ifResume:                        ; the LAST block of the previously emitted function
  call void (ptr, ...) @printf(ptr @32, ptr %6, ptr %8, ptr %10)
  ret i32 0
  %11 = load i32, ptr @GI_NEVER, align 4     <- leaked here from the file-scope `if const`
```

LLVM reports an instruction after the terminator as "does not have terminator!", which is why the
diagnostic points at an innocent function. **That is the signature: a verifier terminator
complaint naming a function nobody touched.**

## Evidence that the omission is an oversight, not a designed invariant

Two independent sites, written by different changes, encode the belief that the insert block is
NULL at declaration scope:

- `MainListener.h:7899` (before this fix): "No usable insert block (declaration / member /
  interface scope)".
- `LLVMBackend.h:12486`: "Requires an active insert point (no builder block at file scope) - fall
  back to a zero-length wrap rather than crash."

The belief is TRUE until the first function body is emitted (an `IRBuilder` starts with no insert
block) and false forever after, which is exactly why it survives casual testing: the first
file-scope construct in a compile behaves as documented.

No code was found that deliberately relies on the stale insert point surviving end-of-function -
the nested-function / lambda paths use explicit `SaveBuilderState` / `RestoreBuilderState` rather
than falling through to whatever was left behind. **That is an absence-of-evidence argument, not a
proof**: it was established by reading, not by making the change and running a sweep.

## The site set - much smaller than it first looked

An earlier draft of this file said "roughly 30 sites to audit". **That was wrong, and wrong in the
expensive direction** - it makes the deferral read as a sweep nobody will start. The real numbers,
counted rather than estimated:

- `GetInsertBlock()` uses in `LLVMBackend.h`: **49**.
- Of those, DIRECT null-compares: **3** - `4558`, `4587`, `12488`.
- Of those 3, on the SHARED `builder`: **1** - `12488`.

The other 46 uses cannot exhibit the confusion: they are `GetInsertBlock()->getParent()` forms
(e.g. `2128`, `2146`, `3225`, `14068`) that need a block to exist at all, or they assign to a local
and then ask a stronger question. So **the emit-hazard set on the shared builder is essentially
`12488` alone** - the site already named below as the worst sibling. This is a one-site check plus
a handful of lower-risk reads, not a 30-site sweep.

Classified:

- **`LLVMBackend.h:12488`** - the runtime string-literal wrap. The one real sibling: it EMITS on the
  shared builder, and its `== nullptr` guard carries the same false premise in its comment. If a
  string literal needed a runtime-length wrap at declaration scope after any function had been
  emitted, the guard would not fire and the `CreateInsertValue` sequence would land past a
  terminator. See "Why P3" for why it cannot be reached today.
- `LLVMBackend.h:4558`, `4587` (`EmitUniqueArrayFieldRelease` and its sibling) - guard a SEPARATE
  `llvm::IRBuilder<>& b` parameter, not the shared one, so the stale-point mechanism does not apply
  to them. Lower risk; still unverified.
- `LLVMBackend.h:1282` (`RecordPendingReturnDangleCheck`), `18385`
  (`MarkVariableExplicitlyMovedNull`), `18523` (`RecordMoveEvent`), `18549` (`RecordNullEvent`) -
  RECORD-ONLY. They null-test a local and store the block pointer in an analysis log rather than
  emitting into it, so a stale block corrupts an analysis key, not the IR: wrong answer, not
  verifier error. Different severity, same root confusion.
- Already correct, worth knowing so nobody "fixes" them: `3078` is `IsInsertBlockLive` itself,
  `3122` already calls it, and `19628` (`ReopenAfterTerminator`) explicitly tests
  `getTerminator() != nullptr` - it exists precisely because the stale-terminated-block state is
  real and sometimes has to be handled rather than avoided.

## Why P3, explicitly

The `if const` family is fixed and swept: a differential corpus sweep over all 512 `.cb` under
`Test/`, `example/` and `cflat/core/` (real `-o` codegen plus program stdout, both binaries) found
zero behavioural differences.

More importantly, there is a REASON the one real sibling is unreachable, not just a failed search.
The only declaration-scope entry into `WrapStringLiteralAsString` (`LLVMBackend.h:12488`) is a
global initializer, and every non-literal spelling is rejected EARLIER by the constant-initializer
check:

```cflat
char[8] raw = "abc";
const char* craw = "def";
string g  = raw;        // global variable initializer must be a compile-time constant
string g2 = craw;       // same
string g3 = (char*)0;   // same
```

A literal initializer takes the folded path above the guard and never needs the runtime wrap. So
the guard's false premise is currently unobservable by construction, not by luck. That is what
justifies P3 over P1 - re-rank it when someone produces a spelling that reaches `12488` (or a
second instance of the class elsewhere), not on the strength of the story.

Note also that this commit REMOVES one route to `12488`: an `if const` condition performing a
string conversion now runs with a live scratch block rather than at the stale point.

## Fix direction, and the trap

Two options, and the tempting one is the dangerous one.

1. **Audit the three null-compares** (`12488` on the shared builder; `4558` / `4587` on a separate
   one) and decide, per site, whether each means "non-null" or "live", switching the latter to
   `IsInsertBlockLive()`. Nearly free given the real numbers above, and blast-radius-free: each
   change only diverts a case that would otherwise have emitted into a terminated block, which is
   always invalid IR. This is what `fix/ifconst-ir` did for one site. Optionally also decide whether
   the four record-only sites should log a stale block at all.

2. **Clear the insert point at end-of-function** so non-null and live coincide. This is the
   one-liner and it is the trap. It changes what EVERY site sees, including the ones that are
   currently correct by accident, and any path that implicitly relies on the stale point (error
   unwind, `AbortFunctionBlocks` at `LLVMBackend.h:9065`, anything that re-enters emission after a
   function completes) would change behaviour silently. If anyone takes this route it needs its own
   full differential sweep and its own review, not a drive-by commit.

Option 1 has the right guard polarity for this repo: it only ever diverts something already
provably illegal. Option 2 asserts an invariant nobody has established.

## Related

[[interface-issue-queue]] (design record: `fix/ifconst-ir`),
[[expect-error-leaves-outer-nullcond-block-unterminated]]
