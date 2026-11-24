# Tuple leftovers after tuplit (2026-10-31) - residue after TUPLEFT (2026-11-05)

Repros: scratch/repro_keep/tuplit/ (main checkout).

Not reproduced on master 2026-11-05 (TUPLEFT): by-value tuple parameter dtor (r2s04 gd=1 at -O0/-O2/-g; ctor/dtor counts
balance for call literals, named tuples, lambda params, methods, generic params); pinned by the tdp_* legs in test_generics.

## Still open (pre-existing, master same)

- Assignment `t = idg((3,4))` and declaration both stay refused (non-literal tuple needs an exact type - ruled 2026-11-01 at tuplefts).

- Array brace initialization still leaks the owned string in a tuple call result. The master exe and this round both return 2 from the HeapAudit check; values pass. The array-element consumer stores the staged tuple without adopting or destroying its staged owner.
  Fix direction: make the array-list element consumer adopt the tuple staging value or destroy it after copying into the element.

```cb
import "test_helper.cb";
import "tuple.cb";
import "string.cb";
import "diagnostic/heap_audit.cb";
move string mk() { return toString(123); }
extern int main() {
    HeapAudit.enable();
    u64 b = HeapAudit.reportLeaks();
    {
        string s = toString(321);
        (int, string)[2] a = { (1, mk()), (2, move s) };
        if (a[0].item_0 != 1 || a[0].item_1 != "123"
            || a[1].item_0 != 2 || a[1].item_1 != "321") return 10;
    }
    return HeapAudit.reportLeaks() == b ? 0 : 2;
}
```
- (TUPLEAK review 2, pre-existing, master same) three tuple-with-string shapes still fail. With
  `string s = toString(5);`: `(int, string) f() { string s = toString(5); return (1, s); }` crashes at run time
  (the return adopts the staging slot that aliases the local's buffer, then the local is destroyed);
  `struct H { (int, string) p = default; }` with `H h = { p = (1, s) };` crashes the same way (brace-init field
  adopts the alias); and `(unique O*, string) t = c ? (new O(), "a") : (nullptr, "b");` is refused with
  "cannot cast an aggregate value". Expected: the first two copy `s` (s usable after, HeapAudit stable, like the
  declaration and assignment forms since TUPLEAK); the third compiles and selects one arm.
