# `for (string s in arr)` aliases each element, and its loop variable is destructed once

## Summary

A range-based `for` over a fixed `string[N]` binds the loop variable by bit-copying the element's
`{ptr,len,owned}` pair, so the loop variable and the element both own - and both free - the same
buffer: rc 133 at teardown.

## Repro

rc 133 on bfb5943 and after the fixed-array element READ fix (measured both):

```cflat
extern int main()
{
    string[2] dst;
    dst[0] = "ab" + "cd";
    dst[1] = "ef" + "gh";
    int n = 0;
    for (string s in dst)
        n += s == "abcd" || s == "efgh" ? 1 : 0;
    printf("n=%d\n", n);            // prints n=2, then rc 133
}
```

## Root cause

The range-`for` fixed-array leg in `MainListener_Statements.cpp` (the `isFixedArray` branch of the
`declarationSpecifiers() && In()` case) is hand-rolled: it emits the two-index GEP, a raw
`CreateLoad`, and `CreateAssignment(elemVal, elemAlloca)` directly. No `NamedVariable` is built for
the element, so none of the ownership arms - including the new `IsFixedArrayStringElementRead`
deep-copy - is reachable from it.

## Why it was not fixed alongside the other read positions

The loop-variable ALLOCA is hoisted once for the whole loop, and its `string.dtor` is emitted ONCE,
in `forRangeResume` (confirmed in `--out-lli`: a single `call void @string.dtor(ptr nonnull %s)` in
the resume block). Deep-copying per iteration would therefore trade the double free for a leak of
N-1 buffers. A correct fix has to give the loop variable a per-iteration destruct first, or clear
the loaded value's owned bit so the copy is an explicit borrow.

## Fix direction

Two options, in preference order:

1. Clear the owned bit on the loaded element (`ClearStringOwnedBit`) so the loop variable is a
   plain BORROW for the duration of the body - matching what an interface/container `get` already
   hands back, and what the once-only destructor can safely handle. Cheap and allocation-free.
2. Deep-copy per iteration AND emit the loop variable's destruct at the end of the loop BODY
   rather than at `forRangeResume`. Correct but changes loop-variable lifetime for every element
   type, so it needs its own accept-set sweep (owning structs, closures, interface elements).

The same question applies to the `isFaceType` and `count()/get()` legs of the same construct; both
go through a `get` that already clears the borrow bit, so they are probably already correct -
measure before assuming.
