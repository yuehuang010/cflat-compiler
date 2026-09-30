Bucket: p3 (ownership temporaries; left by E2, fix/coalesce-arm-temp-gaps, 2026-09-29)

# Return keep rule treats a callee defined later in the file as unknown and leaks the arm temp

E2 made the return flush keep a `?:` / `??` arm temporary whose address may reach a pointer-bearing
return value. Reachability uses ParameterMayReachReturn on the callee's body; a callee with no body
(extern, indirect call) falls back to "may reach". A callee DEFINED LATER in the same file has no
emitted body yet when the caller's return is flushed, so it takes the fallback too and the temp leaks
where master freed it (1/1 -> 1/0, -O0 and -O2). Leak only, never an early free.

Repro (probes scratch/repro_keep/e2/rv2/):
```cflat
struct Temp { int v; }
int fb = 0;
int* secondLate(int* a, int* b);
int* f(int c) { return secondLate(c > 0 ? &(new Temp(20))->v : &fb, &fb); }
int* secondLate(int* a, int* b) { return b; }
```
Same with a struct-returning callee defined later (`noLate(...)`).

Fix direction: decide reachability from the parse tree of the later definition (the ForwardRefScanner
already sees it), or defer the keep/free decision for such returns to the end of the module.
