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

## Fix direction

1. Enumerate the remaining axes that can store a Constant into fixed-array storage - field
   arrays, globals, compound assignment, brace-init through an alias, a `?:` join of literals
   - and confirm whether any still reaches the fold.
2. Per CLAUDE.md's convention, an LLVM fatal error reachable from plain source is never an
   acceptable outcome: whatever shape remains needs a front-end `LogError` before codegen.
3. Consider the general guard instead of chasing spellings: refuse to emit a cast from a
   pointer-typed Constant to an aggregate type at all, so the fold cannot be created in the
   first place. That is the one place all of these converge.
