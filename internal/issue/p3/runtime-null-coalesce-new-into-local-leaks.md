# `T* q = g ?? new T(x);` with `g` null only at runtime leaks the fallback

Bucket: p3 (bounded leak, never an early free). b3a6e009 hands the fallback to `q` only when the
left arm is provably null (JoinArmIsProvablyNull: every store to the slot is nullptr).

## Repro

`int runtimeCoalesce(T* g) { T* q = g ?? new T(30); return q->v; }` called with nullptr: one new,
zero dtors. Probe: scratch/argleak_review/conditional_new_review.cb (`runtimeCoalesce`).

## Fix direction

Conditional ownership for a runtime `??` join: an entry slot holding the fallback allocation,
null when the LHS was taken, freed at `q`'s scope exit. Must stay borrowed when `g` is taken.
