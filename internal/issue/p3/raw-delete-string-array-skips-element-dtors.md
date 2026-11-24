# p3: raw delete[_] of an owning-element array skips element destructors (R21 / N-COPY residue)

Filed at R21 landing (2026-10-27); re-measured at N-COPY (2026-11-19). Since N-COPY a `T*` subscript element of an
owning type is a live object: an unproven read copies and a store drops the old element. `delete[n]` destroys the
elements; `delete[_]` is the documented raw free ("frees memory only, no destructor calls", doc/LANGUAGE.md), so an
owned element still alive at a `delete[_]` leaks. allocs/frees/live, N-COPY exe:

- p02: `string* g; g = new string[2]; g[0] = mk(); string t = g[0]; delete[_] g;` -> 3/2/1 (`delete[2] g` -> clean).
- p04: the same with `g` set by a called setter -> 3/2/1.
- Struct twin (new at N-COPY, master 2/2/0 by an adopt + no-dtor balance): `SO* g` global, `SO t = g[0]` then
  `delete[_] g` -> 3/2/1; `delete[2] g` -> clean.
- Raw-new LOCAL `string* p = new string[2]; p[0] = mk(); ... delete[_] p;` -> 3/1/2 (unchanged: the raw-heap string
  slot keeps its borrow arm, and `delete[n]` of a local `string*` is refused by Test/errors/err_delete_string_array.cb).
- Raw-new LOCAL struct (`SO* p = new SO[2]`) with `delete[_]` -> 3/1/2; `delete[2]` -> clean.

Count-known `delete[_]` running element destructors (the run brief's direction) was implemented and measured, then
reverted: it cleans every raw-new LOCAL cell above, but Test/test_cpp_interop.cb:20498 (`delete[_] onRaw; // frees, runs
no dtor`, C++ class array, read-only) pins the opposite contract, and a native-only variant would split the rule by
CFlat vs C++ element type (bridge transparency direction). Global / field / returned pointers carry no count at the
delete, so p02 and p04 stay leaks under either reading.

Needs a ruling: (a) `delete[_]` keeps the no-destructor contract and these are user leaks (close as by-design, maybe a
diagnostic when the operand is known to hold owned elements), or (b) `delete[_]` runs element destructors when the
count is known (doc change + the cpp_interop leg rewritten), or (c) refuse `delete[_]` of a destructor-bearing element
array whose count is known.

## Repro

Prints allocs/frees/live per case. N-COPY exe: str 3/2/1, so 3/2/1 (master: str 3/2/1, so 2/2/0). Both clean with
`delete[2]`.

```cflat
import "diagnostic/heap_audit.cb";
u64 gA = 0; u64 gF = 0;
void cA(void* p, u64 n) { gA++; cflat_heap_audit_alloc(p, n); }
void cF(void* p) { gF++; cflat_heap_audit_free(p); }
string mk() { return "abcdefghijklmnopqrstuvwxyz0123456789abcdefghij".copy(); }
struct SO { string name = default; };
string* g = nullptr;
SO* gs = nullptr;
int strCase() { g = new string[2]; g[0] = mk(); string t = g[0]; int r = t.length(); delete[_] g; return r; }
int soCase() { gs = new SO[2]; gs[0].name = mk(); SO t = gs[0]; int r = t.name.length(); delete[_] gs; return r; }
void run(function<int()> body)
{
    u64 before = HeapAudit.reportLeaks(); gA = 0; gF = 0;
    __alloc_audit_hook = cA; __free_audit_hook = cF;
    int v = body();
    __alloc_audit_hook = cflat_heap_audit_alloc; __free_audit_hook = cflat_heap_audit_free;
    printf("v=%d allocs=%llu frees=%llu live=%llu\n", v, gA, gF, HeapAudit.reportLeaks() - before);
}
extern int main() { run(strCase); run(soCase); return 0; }
```
