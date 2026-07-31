# '?.' call arguments still evaluate eagerly on the HResult/COM chain path

Narrowed 2026-07-31 from the P1 `null-conditional-args-eval-order` (found 2026-07-26 during
opus review of the whole-chain '?.' short-circuit work).

The POINTER-guarded '?.' spellings - struct method, struct extension (UFCS) function,
interface contract method, interface extension function - are FIXED: the receiver null test
is now emitted before the argument list, so a null receiver skips the argument expressions'
side effects entirely. Regression legs live in
`Test/test_basic.cb::testNullConditionalArgOrder`.

What remains is the OTHER meaning of '?.': on an `HResult<T*>` receiver, '?.' means
"propagate the failure code", not "null-check the pointer" - `nullConditionalPending` is
deliberately cleared for it (`MainListener.h`, the `// the HResult path supersedes
null-ptr` line). Its lowering builds `chain.ok` / `chain.fail` blocks at [PFX-7], which is
AFTER [PFX-6] has already evaluated the argument list into `arguments`. So when the source
HResult failed, the call is skipped but its argument expressions have already run.

The same holds for a plain `[winrt]` COM slot call written with '?.': `winrtSlot` takes
precedence over the pointer guard at [PFX-7], and the new guard at [PFX-nc-struct]
explicitly excludes it (`winrtSlot == nullptr`), so no pointer test is emitted. That
exclusion is deliberate - the COM path owns its own lowering and must not be double-guarded
- and it reproduces master's behaviour exactly.

## Repro (Windows only - needs a winmd / header COM import)

```cflat
// h is an HResult<IThing*>; sideEffect() runs even when h.failed()
auto r = h?.Method(sideEffect(5));
```

## Why P3

COM/winrt only, therefore Windows only. Not reachable from any `.cb` in `Test/`,
`example/` or `cflat/core/` on a non-Windows host, so it could not be reproduced or
verified while the pointer spellings were fixed on macOS.

## Fix direction

Same shape as the landed fix: hoist the HResult `chain.failed` branch out of [PFX-7] to
before the [PFX-6] argument loop, and evaluate the arguments inside `chain.ok`. Watch
dominance - `argVals` is consumed by `EmitWinrtSlotCall` inside `chain.ok`, and the
`hresultChainReleasePtr` release must stay on the ok path only.
