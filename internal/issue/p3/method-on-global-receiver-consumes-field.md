# p3: a method called on a GLOBAL receiver implicitly consumes the global's unique field (NEEDS RULING)

Pre-existing; found by the T382 review (2026-10-21). Repro: scratch/repro_keep/box1021/t382_rev/gap_method_on_global.cb.

```cflat
struct HH { unique PS* p = default; unique PS* take() { unique PS* o = nullptr; o = p; return o; } };
HH gH = default;
// main: gH.p = new PS(5); unique PS* t = gH.take();   // consumes gH.p - accepted on master and after T382
```

The global/static storage ruling refuses `o = gH.p;` in a function (implicit consume out of storage that outlives the function).
Through a method the receiver's storage is unknown inside the body, so `o = p;` (implicit this) is accepted.

## Ruling needed

(A) leave it: `this` is a borrowed receiver whose owner decides; a method consuming its own field is ordinary (Rust: `&mut self` + take
needs `Option::take` / `mem::replace` - i.e. Rust REFUSES a plain move out of `&mut self`); or (B) refuse an implicit consume of a
field of `this` (require `move p` / `_ = move` style explicit transfer) - a broad tightening of every implicit-this consume; or (C)
refuse only at the call site when the receiver is global/static and the callee summary consumes a receiver field.
Main-session recommendation: (C) - narrow, matches the storage ruling at the point where the storage is known.
