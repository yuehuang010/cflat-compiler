# '?:' arm temp passed through a function<> call is freed while its address is returned (p1, use-after-free)

Found by T234 (return keep rule), reproduced on master 3b77633d4 and on the T234 branch: the fix
there does not touch it, because the indirect call is answered "keep" at the return flush and the
temp is already freed before that point.

Repro (from scratch/repro_keep/e2/rv2/c2.cb, case r18):
```cflat
struct Temp { int pad = 0; int v = 0; Temp(int x) { v = x; } ~Temp() { v = -777; } };
struct PH { int* p = nullptr; int k = 0; };
int fb = 6;
PH mkPH(int* p) { PH r; r.p = p; r.k = 1; return r; }
function<PH(int*)> gfn = mkPH;
PH r18(int c) { return gfn(c > 0 ? &(new Temp(18))->v : &fb); }
// main: PH h = r18(1); *h.p reads the freed Temp.
```
Observed: -O0 reads 0, -O2 reads garbage; the temp's destructor has already run (destroys = 1).
Expected: the value 18, and the temp freed once after the statement that reads it (or never, if the
caller keeps it), never before `*h.p` is read.

Likely root: the ternary arm's flush (FlushOwnedTempsSince with keep = nullptr in the arm) frees the
arm temp before the indirect call passes it on; PreserveRetainedJoinArmTempsBeforeCall sees no
callee body (callee == nullptr, ParameterRetainsArgument answers true) but the arm temp is not a
join-arm slot in this shape. Verify with lldb on the arm flush before fixing.

Fix direction: keep an arm temp passed to an indirect call (function<>, function pointer) until the
statement ends, the same way a declared callee keeps it; add a leg in Test/test_collection_leaks.cb
beside the T234 legs (must fail on master: value 18 and the temp's destructor count).
