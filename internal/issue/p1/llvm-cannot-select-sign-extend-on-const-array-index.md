# Indexing a fixed array whose storage came from a folded ConstantExpr kills the compiler

Filed 2026-07-31 while fixing [[fixed-array-copy-invalid-bitcast]]. PRE-EXISTING on
`4097959`.

Severity: COMPILER CRASH, exit 134. An LLVM fatal error, not a diagnostic.

> **Correction (2026-07-31, same day).** The first version of this file claimed the crash had
> "no live repro" because the declaration was rejected earlier, and marked the reachability
> "UNVERIFIED". Both statements were wrong, and wrong for a reason worth recording: only the
> DECL-INIT axis had been closed, and the ASSIGNMENT axis was never enumerated. The assignment
> spelling still crashed the compiler on the very branch that filed this file. Enumerate the
> axes (decl-init, assignment, compound assignment, parameter, return, field) before writing
> "no live repro" about anything.

## Repros

Both crash the compiler with exit 134 on `4097959`. The FIRST also crashed on
`fix/array-shape` until the whole-array assignment reject landed in that same commit.

```cflat
// assignment axis - survived the decl-init fix
extern int main(){ char[8] b = default; b = "hello"; printf("c=%c\n", b[0]); return 0; }

// decl-init axis - closed by the decl-init reject
extern int main(){ char[8] b = "hello"; printf("c=%c\n", b[0]); return 0; }
```

```
=== LLVM fatal error: Cannot select: 0x87cbd02a0: i64 = sign_extend 0x87cbd05b0
In function: main ===
```

The non-indexed form of each (`printf("%s", b)`) did not crash - it compiled, ran, exit 0,
and printed garbage. See [[char-array-from-string-literal-has-no-spelling]].

> **Correction (2026-08-02, `fix/array-storage` round 2).** Re-measured on the verified
> `ca5a02a` Release binary. Two changes to the record:
>
> 1. **The exit-134 fatal-error TEXT above could not be reproduced in any spelling.** Every
>    crashing program measured aborts with a bare **SIGSEGV, exit 139, zero output** - no
>    `Cannot select` line, no `LLVM fatal error` banner. The `sign_extend` node is therefore
>    NOT confirmed as the specific failing node on `ca5a02a`; treat the quoted text as
>    historical (it was recorded against `4097959`, a different build). Record the live
>    symptom as "SIGSEGV exit 139 on a row assign followed by a CONSTANT-index element read".
> 2. **Both repros listed above are now front-end rejected on `ca5a02a`** by the whole-array
>    assignment and decl-init guards, so neither is live. The spelling that IS live is the
>    ROW receiver, which those guards cannot see:
>
>    ```cflat
>    // exit 139 on ca5a02a - closed on fix/array-storage
>    extern int main(){ char[2][8] b = default; b[0] = "hello"; printf("c=%c\n", b[0][1]); return 0; }
>    extern int main(){ char[2][2][8] e = default; e[0][1] = "hello"; printf("c=%c\n", e[0][1][2]); return 0; }
>    ```
>
>    The trailing read is the discriminator, and it must use a CONSTANT index. `printf("%s", b[0])`
>    (whole-row read) and `b[0][argc]` (runtime index) both compile rc 0 and MISCOMPILE instead.
>    Compiling to IR only (`-l`, no object) also succeeds - the crash is in the backend.
>
> Lesson: quote repros verbatim. Substituting `%s` for `%c` in the trailing read turns this
> crash into a silent wrong value, and a reviewer who made that substitution concluded the
> whole premise did not reproduce.

## Root cause

Not fully diagnosed; only the front-end shapes that FEED it are closed. Both spellings stored
a string literal into `[8 x i8]` storage. A literal is an `llvm::Constant`, so the bad
`ptr` -> `[8 x i8]` cast folded into a **ConstantExpr**, which the module verifier does not
subject to its instruction-level check - it verified clean and detonated in SelectionDAG.
Indexing that storage produced an `i64 sign_extend` node that could not be legalized. (Same
Constant-vs-Instruction divergence as the primitive-array boxing record in
[[interface-issue-queue]].)

## Status

Both KNOWN front-end shapes are now rejected before codegen, so there is no known live repro
on `fix/array-shape`. That is NOT the same as the unselectable node being unreachable -
whether any other spelling can still fold a Constant into array storage has not been
enumerated, and the last time that distinction was glossed over it produced the correction
at the top of this file.

On `fix/array-storage` the ROW-receiver axis (item 2 of the corrections above) is closed as
well, by a reject in `derefAssign`, plus a `CreateCast` backstop that refuses any
non-aggregate -> aggregate conversion. The UNION array-field axis turned out not to need a
reject at all: it was a stale store type (the whole FIELD type used for an indexed slot) and
is now lowered correctly. The file stays open because the backend node itself is still
undiagnosed and no exhaustive enumeration of Constant-into-array-storage folds exists.

## Fix direction

1. Enumerate the remaining axes that can store a Constant into fixed-array storage - field
   arrays, globals, compound assignment, brace-init through an alias, a `?:` join of literals
   - and confirm whether any still reaches the fold.
2. Per CLAUDE.md's convention, an LLVM fatal error reachable from plain source is never an
   acceptable outcome: whatever shape remains needs a front-end `LogError` before codegen.
3. Consider the general guard instead of chasing spellings: refuse to emit a cast from a
   pointer-typed Constant to an aggregate type at all, so the fold cannot be created in the
   first place. That is the one place all of these converge.
