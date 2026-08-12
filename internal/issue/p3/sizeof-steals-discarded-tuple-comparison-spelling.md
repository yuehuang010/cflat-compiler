# `sizeof(a<b,c>d);` as a discarded statement is now read as a type, not a tuple

Filed 2026-08-06 by review round 1 of `fix/sizeof-closure`. A deliberately accepted, measured
cost of that fix - not a defect anyone is expected to hit, and recorded so the next change in this
area knows the ambiguity exists.

Severity: one spelling that compiled to a no-op now gets a hard error, and the error's wording is
wrong about what the user wrote. No wrong value, no crash, nothing accepted that should not be.

## The ambiguity

`fix/sizeof-closure` made the prefix-`sizeof` operand's type-name test admit `(`, `)` and `,` at
generic-bracket depth >= 1, so that `sizeof(Box<function<int(int)>>)` and `sizeof(Pair<int,float>)`
stop SIGSEGVing. `a<b,c>d` satisfies that test as well - and in cflat that text is a genuine
EXPRESSION: a two-element tuple of comparisons, `tuple__bool__bool`.

## Measured - PRE is `f24fb18`, POST is `fix/sizeof-closure`, all `-o` compiles

| Program | PRE | POST |
|---|---|---|
| `sizeof(a<b,c>d);` - value DISCARDED | rc 0, links, runs, prints `ok` | rc 1, `unknown type 'a<b,c>d'` |
| `i64 z = sizeof(a<b,c>d);` | rc 1, `cannot cast an aggregate value - a fixed array decays to a pointer to its first element` | rc 1, `unknown type 'a<b,c>d'` |
| `i64 z = sizeof(a<b,c>d) + 0;` | rc 1, `no overload of 'operator+' matches ... [0] tuple__bool__bool` | rc 1, `unknown type 'a<b,c>d'` |
| `tuple<bool, bool> t = (a<b, c>d);` - the tuple WITHOUT `sizeof` | rc 1, `Unknown identifier 'item0'` (line 5 - the tuple on line 4 is ACCEPTED) | IDENTICAL |

All with `int a = 1; int b = 2; int c = 3; int d = 4;` in scope.

Two things that table settles. The tuple reading is real - the PRE `operator+` diagnostic names
`tuple__bool__bool` for exactly this text. And the change is confined to the `sizeof` operand: the
bare tuple spelling is accepted identically on both binaries, so nothing outside `sizeof` was
taken.

So the ONLY behaviour actually lost is the value-discarded statement form, which computed a
`sizeof` and threw it away - a no-op. Every consuming form was already rejected on PRE, just with
a different message.

## The wording is also wrong

`unknown type 'a<b,c>d'` calls the user's tuple expression a type. It is accurate about what the
compiler tried to do and inaccurate about what was written. Any real disambiguation rule should
fix the message too.

## Why this was NOT fixed by falling back to the expression path

The obvious repair - if the type lookup fails, re-run the operand as an expression - walks
straight back into the crash `fix/sizeof-closure` closed. That fallthrough is precisely how a null
`Primary` reached `LLVMBackend::CreateCast` and SIGSEGVed the compiler with zero output. Trading a
located diagnostic on a discarded no-op for a re-entry into the null-`Primary` path is a bad
trade, and the stolen spelling has no realistic use.

## Status 2026-08-12 (q14 bucket close) - STILL DEFERRED, blocker gone, wording now worse

`sizeof-over-generic-instantiation-unresolved-while-alignof-resolves` was fixed in `bc53456`, so
the stated precondition is satisfied. It did NOT route the operand through the real `typeName`
rule, though: the prefix-`sizeof` arm in `MainListener_Expressions.cpp` still reconstructs the
type from `postFixCtx->getText()`. So the parser still cannot decide, and the safe re-offer to the
expression path this file asks for is still unavailable.

Re-measured on this bucket's binary: `sizeof(a<b,c>d);` now reports `unknown type 'a__b__c'` -
the MANGLED name, which is further from what the user wrote than the `'a<b,c>d'` recorded above.
Severity is unchanged (a discarded no-op), so this stays deferred rather than being special-cased.
Whoever routes the operand through `typeName` should fix the message in the same change.

## Fix direction

Leave it until the prefix-`sizeof` operand goes through the real `typeName` rule.
That issue's fix routes the `sizeof` operand through the real `typeName` rule instead of the
text-reconstruction handler, at which point the ambiguity has to be settled by the PARSER (which
can tell a `typeName` from a tuple expression) rather than by a character test - and the operand
that fails to be a type can be re-offered to the expression path safely, because the parser, not a
null value, decides. Resolve both together; do not add a special case for this text alone.

Related: [[sizeof-over-generic-instantiation-unresolved-while-alignof-resolves]], [[interface-issue-queue]]
