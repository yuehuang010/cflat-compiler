# The C idiom `char[N] b = "literal";` has no working spelling

Filed 2026-07-31 while fixing [[fixed-array-copy-invalid-bitcast]]. BOTH spellings - the
declaration initializer and the assignment - are now REJECTED with their own diagnostics
(both previously miscompiled silently); what is filed here is that CFlat offers no direct
replacement for a very common C idiom.

Severity: a feature gap. Nothing lies to you any more - the compiler says what is wrong and
names three alternatives - but none of them is the idiom itself.

> **Correction (2026-07-31, same day).** The first version of this file said the silent
> miscompile had been removed, having only closed the DECL-INIT axis. The ASSIGNMENT axis
> (`char[8] b = default; b = "hello";`) still compiled and printed garbage on the branch that
> filed this file, and its indexed-read form still killed the compiler at exit 134. Both axes
> are closed as of the same commit; the lesson is to enumerate the axes before claiming a
> class of miscompile is gone.

## What master did on BOTH axes (measured, not assumed)

The DECL-INIT axis:

```cflat
extern int main(){ char[8] b = "hello"; printf("s=%s\n", b); return 0; }
```

Compiled, linked, ran, exit 0, printed GARBAGE. A string literal is an `llvm::Constant`, so
the bad `ptr` -> `[8 x i8]` cast folded into a **ConstantExpr**, which the module verifier
does not subject to the instruction-level check. The emitted body then does
`extractvalue [8 x i8] bitcast (ptr @2 to [8 x i8]), 0` per byte - i.e. it reads the bytes of
the POINTER VALUE, not of the string. This is the same Constant-vs-Instruction divergence the
primitive-array boxing record in [[interface-issue-queue]] describes.

The indexing form was worse - it crashed the compiler:

```cflat
extern int main(){ char[8] b = "hello"; printf("c=%c\n", b[0]); return 0; }
```

See [[fixed-array-storage-guards-miss-four-axes]] (renamed 2026-08-02).

The ASSIGNMENT axis behaved identically, and survived the decl-init fix by a full review
round - it was closed only after a later round enumerated it:

```cflat
extern int main(){ char[8] b = default; b = "hello"; printf("s=%s\n", b); return 0; }  // garbage
extern int main(){ char[8] b = default; b = "hello"; printf("c=%c\n", b[0]); return 0; } // exit 134
```

## Current behaviour

Rejected on both axes, each naming the spellings that do work (`char* b = "..."`,
`string b = "..."`, or a positional brace list). Covered by
`Test/errors/err_fixed_array_from_string_literal.cb` (decl-init) and
`Test/errors/err_fixed_array_assign_string_literal.cb` (assignment). Whole-array assignment
is separately rejected for every RHS - see `Test/errors/err_whole_fixed_array_assign.cb`.

## Fix direction

Lowering the ASSIGNMENT axis additionally requires whole-array assignment to exist at all,
which is its own feature (compound operators, field arrays, globals) and is currently
rejected outright. Do the declaration form first.

Lower `char[N] b = "literal";` the way the positional brace initializer already lowers - fill
the N slots from the literal's bytes, zero-filling the tail and rejecting a literal longer
than N with the existing "too many initializers" wording. `EmitPositionalFixedArrayInit`
(`cflat/MainListener.h`) is the machinery; the literal's bytes are available on the
`llvm::ConstantDataArray` behind the global, so no runtime copy is needed at global scope
either. Do NOT lower it as a pointer store - that is the bug that was just removed.
