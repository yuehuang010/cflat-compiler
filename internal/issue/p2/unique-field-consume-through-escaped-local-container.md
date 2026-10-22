# p2: a unique field consumed through a raw pointer to a local container that a call redirected to a global

Pre-existing (master exits 55 too); found by the T382 review r5 (2026-10-21).

## Repro

Repros: scratch/repro_keep/box1021/t382_rev/r5_local_escape_addr.cb and r5_local_twoptr_call.cb.

```cflat
A a; A* pa = &a; pa->b = &lu; setB(&a); o = pa->b->p;   // setB: x->b = &gU
```

This compiles and consumes the global's field: `gU.p` is nulled and exit is 55. Variants that behave the same:

- `setB(pa)`
- `stash(&a); touch()`
- `gA = &a; touch()`
- `setBB(&pa)`
- the address escaping before the store
- two pointers `pa`/`pb` with `setB(pb)`

The direct form `a.b = &lu; setB(&a); o = a.b->p` is already refused.

## Root cause

At MainListener_Expressions.cpp ~1479-1495 (after T382), a base that is a load of `pa` holding `&a` is traced with no escape scan. The escape walk only runs when the base is the local itself, and the backward scan only kills stores to the same address. A call that receives `&a` or `pa` is never seen.

## Fix direction

Do one of these:

- run the same escape walk on the local that `pa` points to, following loads of `pa`'s slot;
- or treat any call between the traced field store and the read as a kill.

`raw_local_target`, which has no call in between, must stay accepted.
