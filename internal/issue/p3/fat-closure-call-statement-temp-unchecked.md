# p3: Lambda<> (fat closure) calls skip the unprovable-callee statement-temp check (R7 residue)

Filed at R7 / T521 landing (2026-10-27). The T521 refusal ("cannot pass an owning statement temporary to a callee whose
retention cannot be proven") runs only for thin function pointers (`IsThinFnPtr()`). A `Lambda<void(int*)>` call is not
checked:

```cflat
void inv(Lambda<void(int*)> k) { k(&(new Obj(5))->v); }   // k stores its argument
```

Master and the T521 fix both compile it; the temp leaks (dtors 0, leak 1), no use-after-free. The 2026-10-27 ruling
names `function<>`, C callees and function pointers; whether `Lambda<>` falls under the same fail-closed rule (or gets
the body-proof T521 uses for known targets) needs a one-line maintainer confirmation before work.

RULED 2026-10-29 (main session, from precedent; maintainer may veto): YES - `Lambda<>` is an unprovable callee like `function<>`, a C callee or a thin function pointer; the
T521 fail-closed refusal applies with the same text, and the leaking probe becomes an err_ leg. Unparked.

Also noted by the T521 review (design, by-design today): a `function<>` whose complete target set provably KEEPS the
pointer (every target stores it) is refused (fail closed); master accepted and destroyed once after the caller's delete.
Repros: scratch/repro_keep/t521_rev/ (f6, Lambda probe).
