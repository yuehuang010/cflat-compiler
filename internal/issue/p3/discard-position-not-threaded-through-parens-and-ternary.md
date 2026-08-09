# A discard position stops being one behind a `?:`, so a value-less call is falsely rejected

Filed 2026-08-09 during the review of `fix/voidcall`. Two gates now key on the `ResultUse`
threaded down from the statement: the return-block value-context check (pre-existing, master)
and the void-closure-call check (new). Both share one hole - the `Discard` handed to
`ParseAssignmentExpressionNamed` rides only the **pure single-child passthrough chain**
(`assignment -> cast -> unary -> postfix`). A parenthesized primary or a `?:` re-enters the
expression parser with the default `ResultUse::Value`, so a bare-statement discard behind
either one is rejected as a value consumption. **The paren half was fixed in round 2 of the
same commit (see below); only the `?:` half is still open**, which is what this file now
tracks.

Severity: **P3**. The diagnostic is located, correct in wording ("call it as a statement") and
the fix is to delete the parentheses, so nothing is silent and nothing miscompiles. It is
recorded because `fix/voidcall` turned three previously-COMPILING spellings into errors, which
is a (small) accept-set regression rather than a pure pre-existing gap.

## Repro

Measured Release, macOS arm64, warm `--init-local`, merge base `75b4275`
(`/Users/felixhuang/source/cflat-compiler/x64/Release/cflat`) vs `fix/voidcall`.

Newly rejected by the void-closure gate - all three ran correctly (rc 0) on the base:

```cflat
Lambda<void()> g = () => { bump(); };
(g());                              // scratch/rev_p05_paren_stmt.cb   base rc 0 -> now rejected
((g()));                            // scratch/rev_r04_double_paren_stmt.cb
cond ? g() : h();                   // scratch/rev_r01_ternary_stmt.cb
Lambda<void()> f = () => (g());     // scratch/rev_r03_lambda_exprbody_paren.cb
```

```
rev_p05_paren_stmt.cb(6,5): call through function value 'g' returns 'void', so it produces no
value to consume - call it as a statement
```

The SAME hole is already visible on master through the return-block gate, which is why this is
one issue and not two - the discard simply does not survive either wrapper:

```cflat
inline bool RbA(int a) { return { if (a < 0) { return false; } }; }
RbA(1);              // scratch/rev_r06_rb_plain.cb    base rc 0, fine
(RbA(1));            // scratch/rev_r05_rb_paren.cb    base rc 1: "cannot be called in a value context"
c > 0 ? RbA(1) : RbA(2);  // scratch/rev_r07_rb_ternary.cb  base rc 1: same
```

Discard positions that DO work and must keep working (all rc 0 on both binaries):
bare statement, for-init, for-increment, `while`/`do` body, `if const` block, `lock` block,
nested lambda body, void `=> expr` body without parens
(`scratch/rev_p10_ifconst_stmt.cb`, `rev_p11_lock_stmt.cb`, `rev_p14_dowhile_stmt.cb`,
`rev_p15_forinit_stmt.cb`, plus `testVoidClosureCallDiscarded()` in `Test/test_function_ptr.cb`).

## Root cause

`MainListener::ParseAssignmentExpressionNamed` (`MainListener_Expressions.cpp:3`) forwards its
`ResultUse` only on the single-child fast path. `'(' expression ')'` is a `primaryExpression`
alternative (`CFlat.g4:45`) whose inner expression is parsed at
`MainListener_PostfixExpression.cpp:5581` with the `ResultUse::Value` default; a conditional
expression likewise re-enters each arm at the default.

## PARENTHESES: FIXED in the same commit (2026-08-09, round 2)

`ParsePrimaryExpression` now takes the `ResultUse` and forwards it through the
`'(' expression ')'` alternative only; `ParsePostfixExpression` hands its own `use` down when the
primary IS the whole postfix (`childLimit == 1`) and `Value` otherwise, so a suffix
(`(mk())()`, `(p).f`) still consumes the primary as a value. Re-measured against the merge base:

| spelling | base | before round 2 | now |
|---|---|---|---|
| `(g());` | rc 0 | rejected | rc 0 |
| `((g()));` | rc 0 | rejected | rc 0 |
| `() => (g())` | rc 0 | rejected | rc 0 |
| `cond ? g() : h();` | rc 0 | rejected | **still rejected** |

Accept legs `vdc_paren_statement`, `vdc_double_paren_statement`, `vdc_void_expr_body_paren`,
`vdc_paren_then_call` in `Test/test_function_ptr.cb`; mutation-proven (drop the forward and
`vdc_paren_statement` goes red).

**Return-block delta, shipped deliberately.** The same threading also closed the pre-existing
return-block paren hole: `(RbA(1));` was rc 1 on the merge base and is rc 0 now. Verified
semantically rather than by exit code - `scratch/vc_rb1.cb` runs the plain and parenthesized
spellings side by side over both polarities with a trace counter, and the parenthesized form is
byte-identical to the plain one (`pos=1 trace=1`, `neg=0 trace=0`), i.e. the inlined `return`
still exits the CALLER. `c ? RbA(1) : RbA(2);` (`rev_r07`) is unchanged, rejected on both.

## What remains: the `?:` leg

## Fix direction

(The paren half of this section is DONE - see the round-2 section above. The `childLimit == 1`
guard it ships with admits nothing on its own: dropping it leaves `(g())();`, `(g())[0];` and
`(g()).n;` rejected, but their messages degrade to "unknown function '(g())'" and "Undefined
variable n". Keep it for the diagnostic, not for soundness.)

The `?:` leg IS a semantic decision - a ternary in statement position discards BOTH arms - so
handle it separately and only if wanted: thread the use into both arms when the ternary itself
is in a `Discard` position, and keep `Value` for the arms of a ternary that is being consumed.
Do not thread it into the CONDITION, which is always a value.

Whatever gate lands for [[direct-void-call-result-consumed-fails-verifier]] inherits this hole,
so converge the two together.

Round 2 attempted the paren leg only. The `?:` leg was **abandoned deliberately**, not missed:
the arms are parsed through a different API (`ParseConditionalExpression` /
`ParseTernaryBranches` / `ParseExpression`, all `TypedValue`-based, plus the eager
constant-context fallback and the `UnifyTernaryArmTypes` / join-ledger tail), so threading a
position into them means changing four more signatures and deciding what a `Discard` arm means
for the join that follows. That is the re-enumeration shape this repo is told to stop at. The
cost/benefit is also poor: `cond ? g() : h();` as a bare statement is a rare idiom, its
diagnostic is located and its wording already names the fix, and the return-block twin
(`c ? RbA(1) : RbA(2);`) has been rejected on master since long before this work.
