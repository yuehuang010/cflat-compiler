# An error thrown inside an armed '?.' chain leaves the outer 'nc_null' unterminated

Filed 2026-07-27, found during review of the `?.` interface-field guard fix
(`d1dfad8`). PRE-EXISTING: confirmed identical on master `28f3ed5` with an
ordinary error, so the `?.` fix did not introduce it - it merely adds one more
construct that can trigger it.

Severity: raw LLVM verifier dump instead of a clean diagnostic. Only reachable
through the `expect_error` harness, so it does not affect ordinary compiles.

## Repro

```cflat
expect_error("Undefined variable") { int r = tp?.arr[zzzundefined]; }
expect_error("Undefined variable") { int r2 = tp?.arr[alsoundefined]; }
```

Both legs PASS, then:

```
Module verification failed: Basic Block in function 'main' does not have terminator! label %nc_null
```

Two or more legs in one function are required - a single leg passes, which is
why this shape is easy to ship a bug behind.

## Root cause

`LogError` throws. When it throws from a position *inside* an already-armed `?.`
chain, the walk unwinds past that chain's end-of-chain merge, so the enclosing
`nc_null` block never gets its branch to `nc_resume`.

The recovery path at `cflat/MainListener.h:6446-6465` terminates only the
CURRENT insert block. It has no knowledge of enclosing `ncEnterGuard` frames, so
an outer chain's `nc_null` is left dangling.

Same root shape as the two self-inflicted bugs already hit this session: in this
codebase `LogError` throwing is a control-flow edge, and any IR bracket opened
before the call must be closed on the unwind path (compare the RAII on
`importCompileDepth_` in `dcb9003`, and the `CreateUnreachable` pair in
`d1dfad8`).

## Why it is contained today

- Normal compile mode: `LogError` reaches `exit(1)` before verification runs, so
  the dangling block is never seen.
- `--check` batch mode: the aborted file is contained; subsequent files in the
  same invocation were verified to compile clean.
- Only `expect_error` continues past the throw and then verifies the module.

## Fix direction

Track armed null-conditional frames on a stack in the backend rather than in
`ParsePostfixExpression` locals, and have the `expect_error` recovery path at
`MainListener.h:6446-6465` drain that stack - terminating every open `nc_null`
(and any other unterminated guard block) rather than just the current insert
block. An RAII frame guard that self-closes on unwind is the cleaner shape and
would fix the whole class rather than this one instance.

Regression test: extend `Test/errors/err_null_conditional_not_lvalue.cb` (or a
related error test) with two legs whose errors fire from inside an outer armed
chain - one leg does not reproduce it.
