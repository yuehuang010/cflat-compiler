# A reinterpreting cast CONSUMES an owning pointer, so the original name is moved-from

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 04).
Reproduced on `cd847a3`, Release.

## Repro (measured)

```cflat
struct Item { i64 a = 0; };
extern int main() {
    u8* buffer = new u8[64];
    Item* items = (Item*)buffer;   // MOVES 'buffer'
    items[0].a = 1;
    printf("%p\n", buffer);        // error: use of moved variable 'buffer'
    delete[] buffer;
    return 0;
}
```

```
t_cast3.cb(6,4): use of moved variable 'buffer'
```

Narrowing: the operand must be OWNING. The same shape over a non-owning pointer is accepted -
`u8* buffer = (u8*)malloc(64); Item* items = (Item*)buffer; use(buffer);` compiles clean, because
`malloc` hands back a plain `u8*` with no ownership. So the trigger is a cast whose operand is a
`new`-allocated (unique/owning) pointer local.

## Why it is worth fixing

The reporter's real shape was a Win32 out-parameter buffer:

```cflat
u8* buffer = ...;
CounterItem* items = (CounterItem*)buffer;
PdhGetFormattedCounterArrayW(..., buffer);   // rejected
```

A reinterpreting cast reads as a VIEW, not a transfer - it produces a second name for the same
bytes, and every C-interop buffer idiom needs both names live at once. Because the cast consumes,
the ORDER of two adjacent, apparently independent lines decides whether the program compiles, and
the remedy (hoist the cast below the last use of the original) is invisible from the diagnostic.

## Fix direction

A pointer-to-pointer reinterpreting cast should BORROW: the result aliases the operand and the
operand stays live and stays the owner. Ownership transfer through a cast should require the
explicit `move` spelling (`(Item*)move buffer`), the same as everywhere else.

If borrowing-by-default is judged too loose, the fallback is an explicit non-consuming spelling
plus a diagnostic on the consuming one that names the remedy. Do NOT close this by only rewording
the message: the ordering sensitivity is the defect.

Adjacent: [[move-of-borrow-into-move-sink-parameter]],
[[string-pointer-param-slot-semantics-depend-on-argument-provenance]].

## Regression test

Extend `Test/test_move.cb` (or a related pointer test) with the repro above, asserting that
`buffer` is still usable after the cast and that `delete[] buffer` frees exactly once.
