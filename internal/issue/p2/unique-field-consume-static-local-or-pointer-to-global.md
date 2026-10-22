# p2: a unique field reached through a static local or a pointer to a global can be consumed into a local

Pre-existing; found by the T376 review r2 (2026-10-21). Violates the global/static storage ruling (implicit consume out of storage that
outlives the function is an error). Case bodies: scratch/repro_keep/box1021/t376_rev/rev_t376_opus_cases/r2g_* and r2h_* (`.body` is the
main() body; PS has `int v`, U has `unique PS* p`, gU is a global U).

```cflat
static U sU = default; unique PS* o = nullptr; sU.p = new PS(6); o = sU.p;   // master: consumes, sU.p nulled
U* pg = &gU;            unique PS* o = nullptr; pg->p = new PS(7); o = pg->p; // master: consumes gU.p
```

A direct global `o = gU.p;` is refused ("cannot consume owning value 'gU.p' out of storage that outlives this function"). After T376 the
declaration chain and statement chain forms agree with the two-statement form (all consume).

Fix direction: ImplicitOutlivingOwnerConsumeError (LLVMBackend_WinRT.cpp) recognizes only recorded global roots; add static locals, and
treat a field reached through a pointer whose provenance is unknown or global as outliving (conservative refusal, same text).
