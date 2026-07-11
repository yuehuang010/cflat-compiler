# return inside a nested block emits a mid-block terminator (LLVM verifier error)

Created: 2026-07-11 (found while adding file-scope `else if const`)

## Summary

A `return` inside a NESTED scope that is followed by more statements in the same
function emits `ret` into the current basic block and then keeps emitting into
that same block. LLVM rejects the module:

```
Module verification failed:
Terminator found in the middle of a basic block!
label %entry
Error: module verification failed.
```

Not specific to `if const`, and not a regression from the `else if const` work -
a plain compound block reproduces it. Both a plain `{ ... }` block and an
`if const` branch inline their statements straight into the enclosing basic
block, so a `return` inside them terminates the block the caller is still
writing to. A real `if` / loop masks the bug because it creates its own blocks.

The compiler crashes out of LLVM instead of reporting a source-level error.

## Repro

Fails (statements after the nested block):

```cflat
int probe()
{
    if const (__PLATFORM__ == 64)
    {
        return 64;
    }

    printf("after\n");   // emitted after `ret` in the same block
    return 0;
}
```

```cflat
int probeA()
{
    {
        return 64;
    }
    printf("after\n");
    return 0;
}
```

Works (nothing follows the nested block, so nothing is emitted after `ret`):

```cflat
int probeC()
{
    {
        return 64;
    }
}
```

Related but distinct: a bare `return 64; printf("dead");` at function top level
is rejected with "Function 'probeB' with non-void return type is missing a return
statement." - the missing-return check looks at the trailing statement, so dead
code after a return confuses it. Clean error, no crash; noted here because a fix
in this area should keep it in mind.

## Root cause

`return` codegen writes a terminator into the current insert block but does not
start a fresh block afterwards. `ParseBlockItemList` in `cflat/MainListener.h`
already carries an "unreachable-code hint (LSP only)" whose comment asserts that
codegen "already emits into a dead block harmlessly" - that assumption holds only
when the return sits at the level where a new block gets created, not when it is
nested inside a compound / `if const` arm.

## Fix direction

After emitting a terminator (`return`, and check `break` / `continue` too), start
a fresh continuation block and set the insert point to it, so any following
statements are emitted into a legal, dead block. Per CLAUDE.md, the LLVM assert
should also become a proper `LogErrorContext` diagnostic (unreachable code after
`return`) rather than a verifier failure.

## Status

Diagnosed, deferred. No test currently covers it.
