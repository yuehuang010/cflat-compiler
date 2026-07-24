# '?:' (ternary) evaluates BOTH arms unconditionally, not just the selected one

Opened while testing the cross-block moved-pointer deref diagnostic (a same-block
dereference-of-explicitly-moved-pointer deref guard). Tripped over directly while writing
regression coverage for that fix; unrelated to it and not caused or fixed by it.

## Summary

`cond ? trueExpr : falseExpr` (the ternary form of `ConditionalExpression`, as opposed to
`??`, which does branch through nullcoal_* blocks) always
computes BOTH `trueExpr` and `falseExpr` before selecting between them with LLVM's
`select` instruction. It never branches. This means:

1. A `move` inside either arm executes UNCONDITIONALLY, regardless of the runtime value
   of `cond`.
2. A dereference (or any other side-effecting/unsound operation) inside the arm that is
   NOT selected still executes and can still crash.

Both are silent: no compile-time diagnostic, no runtime indication other than the wrong
behavior / the crash itself.

## Repro

```cflat
struct R { int v = 0; };
extern int main() {
    unique R* a = new R();  a->v = 5;
    bool cond = false;
    unique R* b = cond ? move a : nullptr;
    printf("a null? %d\n", (int)(a == nullptr));   // prints 1 - 'a' was moved even though cond is false
    return 0;
}
```

```cflat
struct R { int v = 0; };
extern int main() {
    unique R* k = new R();
    // ... k becomes null via some earlier control flow ...
    unique R* dummy = nullptr;
    // (see deref-of-moved-pointer-guard-inside-callee.md for a full move-then-branch
    // setup that leaves k null here)
    int r = (k == nullptr) ? -1 : k->v;   // k->v is still COMPUTED even when the '-1' arm is
                                           // the one actually selected - segfaults if k is null
    return r;
}
```

The second shape is masked ONLY in the narrow case where the move and the ternary share the
SAME basic block AND the arm that is actually selected is the non-dereferencing one: LLVM's
optimizer sees the pointer is statically null at that program point and constant-folds the
`k->v` load away before the module is even emitted. That is why a same-block
`a == nullptr ? 0 : a->v` right after a same-block move runs correctly - which in turn is
why the deref guard is suppressed inside ternary arms (rejecting it would be a false
positive on code that works).

Do NOT read that as "same-block ternaries are safe". When the DEREFERENCING arm is the
selected one, the same-block shape crashes too, with no diagnostic (rc=139, verified
against both the fixed and the pre-fix compiler - this is not a regression):

```cflat
unique T* p = new T();
unique T* q = move p;
int flag = 1;
int r = flag == 1 ? p->v : 0;   // segfault, no diagnostic
```

Same for a nested ternary and for a `unique <interface>` local (`1 == 1 ? a.g() : 0`). So
the ternary-arm suppression leaves a real false-negative hole: the deref guard does not
fix the original bug for ternary spellings at all. That is an accepted trade - the
alternative is rejecting the legitimate null-guard form above - and it disappears if
ternary is ever made to branch, which is the fix direction below.

## Root cause

`ParseConditionalExpression` (MainListener.h, the branch handling `ctx->expression()` /
`ctx->conditionalExpression()` as the true/false arms) computes both arms via
`ParseExpression(expressionTrueCtx)` and `ParseConditionalExpression(expressionFalseCtx)`
unconditionally, in the current block, then combines them with `compiler->CreateSelect(...)`
(a thin wrapper over `llvm::IRBuilder::CreateSelect`). No basic blocks are created and no
branch is emitted for the ternary form. Contrast `??` (`QuestionQuestion()`), a few lines
above in the same function, which DOES branch: it creates `nullBlock`/`notNullBlock`/
`resumeBlock` and only evaluates the RHS inside `nullBlock`.

## Fix direction

Either:
- Make `?:` branch like `??` does (three basic blocks, only the true arm evaluated on the
  true path, only the false arm on the false path), which would also make it consistent
  with `if`/`else` for anything relying on block identity (e.g. the same-block
  dereference-of-explicitly-moved-pointer guard - once fixed, that guard's `?:`-arm
  suppression logic in `suppressExplicitNullDerefGuard_` becomes unnecessary and could be
  removed). This changes codegen shape for every ternary in the codebase; needs a careful
  pass over existing ternary-heavy code (see e.g. the type-coercion logic living directly
  after the current eager-evaluation call site) to make sure per-arm coercions still apply
  correctly on each branch.
- Short of full branching, at minimum diagnose (LogError) a `move` or a provably-unsound
  dereference appearing inside a ternary arm, so the unconditional-move and blind-deref
  cases are caught at compile time instead of silently misbehaving.

Not attempted here - likely a larger, more invasive change than a one-off patch, and not
in scope for the dereference-of-explicitly-moved-pointer work that surfaced it.
