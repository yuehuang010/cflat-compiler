# A `return` whose value has the wrong TYPE (not the wrong void-ness) dies in the module verifier

Filed 2026-08-09 while fixing
`return-value-void-mismatch-fails-module-verification` (the three VOID-NESS families), which
this file is the leftover of. Measured on `fix/retvoid` (Release, macOS arm64, warm
`--init-local`) both before and after that fix - **identical in both**, so it is a pre-existing
gap that fix neither caused nor closed.

Severity: **P2**. Same "no usable diagnostic" shape as its P1 sibling - a raw LLVM verifier dump
with no `file(line,col):` prefix - but the spellings are narrower: the value has to be a type
that is neither void nor convertible, which is a less natural slip than `return;` in an `int`
function.

## Repro

All three compile rc 1, emit no binary, and print no source location. Verbatim, pre- and
post-`fix/retvoid`:

```cflat
struct S { int n; };
S f() { return 5; }
extern int main() { S s = f(); return 0; }
```
```
Module verification failed:
Function return type does not match operand type of return inst!
  ret i8 5
 %S = type { i32 }
```

```cflat
int f() { return "x"; }
extern int main() { return f(); }
```
```
Function return type does not match operand type of return inst!
  ret ptr @27
 i32
```

A CONSTRUCTOR is the same defect wearing a different hat - its LLVM return type is the struct,
so a `return <value>;` in it is a type mismatch, not a void-ness one:

```cflat
struct S { int n; S() { n = 1; return 5; } };
extern int main() { S s = S(); return 0; }
```
```
Function return type does not match operand type of return inst!
  ret i8 5
 %S = type { i32 }
```

Not every mismatch reaches the verifier: `S* f() { return 5; }` COMPILES AND RUNS (the integer
is accepted into the pointer slot), which is its own hole and is where a fix would have to be
careful.

## Root cause

Same site as the P1 sibling and same omission, one axis over.
`MainListener::EmitReturnExpression` (`MainListener_Statements.cpp`) now proves and rejects a
VOID-NESS disagreement between the `ret` operand and the enclosing function's return type, and
lets everything else through to `CreateReturnCall`. Nothing compares the operand's non-void
LLVM type against the function's non-void LLVM return type, so a `%S`-returning function
happily gets `ret i8 5`.

## Fix direction

The accept set is the hard part, not the rejection: the return path legitimately reshapes the
operand after this point (interface fat-pointer boxing, thin/fat function-pointer coercion,
owned-struct move-out, integer widening/narrowing, the `auto` i64 placeholder). A predicate
placed where the void-ness gate now sits would see the UNCOERCED value and false-reject most of
those. Two workable shapes:

- Put the check at the sink, in `LLVMBackend::CreateReturnCall`
  (`LLVMBackend_MoveDataflow.cpp:1137`), immediately before `CreateRet(value)`, where every
  coercion has already run - but that site is also reached by ~20 SYNTHESIZED callers with no
  `errCtx` to blame, so it needs a source location threaded in or it re-creates the
  locationless dump it is meant to replace.
- Or keep it at the source site and prove only the cases the coercions cannot produce
  (aggregate return type vs. scalar operand, and vice versa), accepting everything else.

A constructor deserves its own wording either way ("a constructor has no return value"), since
"a function that returns 'S'" is true of the lowering and false of the language.

## Related

`return;` (value-less) in a constructor is diagnosed as of `fix/retvoid` - "cannot 'return'
without a value from a function that returns 'S'". That message is the lowering's word, not the
language's, and an early `return;` in a constructor is a reasonable thing to want to write; it
has never compiled (verifier dump before, diagnostic now), so supporting it is a FEATURE, not a
regression. Same wording fix applies.

## The `auto` void-inference asymmetry

Raised by the review of `fix/retvoid`. An `auto` function can infer `void` from a value-less
`return;` but not from a `return <void-expr>;`, and the second is now permanently rejected with
a message that points nowhere. Both were locationless verifier dumps on `680696c`, so neither is
a regression - but the asymmetry is real and the remedy text is wrong for this case.

Measured on the fix commit (Release, macOS arm64):

```cflat
int hits = 0;
auto f<T>(T x) { hits = hits + (int)x; return; }        // scratch/rv_au6_auto_valueless_only.cb
extern int main() { f<int>(4); printf("au6=%d\n", hits); return 0; }
```
Compiles, runs, prints `au6=4` - `auto` inferred `void`.

```cflat
void bump() { }
auto f<T>(T x) { return bump(); }                       // scratch/rv_au3_auto_voidexpr.cb
extern int main() { f<int>(1); return 0; }
```
```
cannot return a 'void' expression from a function that returns 'auto' - the expression
produces no value; call it as a statement and return a value
```

The advice is unfollowable: there is no value to return, because `void` is the right inference.
`return bump();` in an `auto` function should unify the return type to `void` exactly as
`return;` does - the auto-return capture (`LLVMBackend::IsAutoReturnCaptureActive`,
`LLVMBackend_MoveDataflow.cpp:1318`) would need a void site kind alongside its value sites. The
`fix/retvoid` gate rejects it only because a void OPERAND is provable without knowing the
function's real return type; once `auto` can carry a void site, that leg should become an
accept and the `'auto'` branch of the message can go.

Keep `auto f<T>(T x) { if (c) { return; } return x; }` rejected either way - mixing a value-less
and a valued return is already its own diagnostic (`'auto' return: cannot mix 'return;' and
'return <expr>;'`, measured unchanged on both binaries via `scratch/rv_au1_auto_valueless.cb`).
