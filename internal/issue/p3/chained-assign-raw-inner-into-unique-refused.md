Bucket: p3 (usable-surface gap) + one silent leak (see item 2)

# Chained assignment - remaining parameter ownership gap

Still open:

2. Parameter destination leak (master has the same behavior):
   `void f(PS* raw, bool c) { PS* other = new PS(); raw = (c ? new PS() : other); }`
   With c true this frees `other` but not the fresh block (2 births, 1 dtor). The plain
   `void f(PS* raw) { raw = new PS(); }` also leaks (1 birth, 0 dtors). This is not chain-specific:
   argument slots carry no owner bit, and the statement store excludes stack arguments from adoption
   (`FindStackArgumentByStorage`), so there is nothing to adopt the fresh block into. Fixing it needs
   an owner bit on argument slots, or a refusal of a fresh-object store into a borrowed parameter.
   This remains a design call and is out of scope for the chain fix.
   Same family, parameter middle: `unique PS* a = nullptr; PS* e = nullptr; a = prm = e = new PS();`
   is refused as a chain (`prm` is a borrowed parameter), while `e = new PS(); prm = e; a = prm;` is
   accepted (births 2, dtors 2). The chain stays refused until argument slots carry an owner bit.
   Same family, raw field / global middle (split form leaks on master and the branch): with c true,
   `PS* outer = nullptr; h.raw = (c ? new PS() : nullptr); outer = h.raw;` (h a local struct with
   `PS* raw`) and `g = (c ? new PS() : nullptr); outer = g;` (g a global `PS*`) each leave 1 birth,
   0 dtors. The chain forms `outer = h.raw = (...)` and `outer = g = (...)` are balanced (1 birth,
   1 dtor): the chain hands the fresh object to the raw outer, the split copy does not. Minimal repro:
   ```
   int births = 0; int dtors = 0;
   struct PS { int value = 7; PS() { births = births + 1; } ~PS() { dtors = dtors + 1; } };
   struct Holder { PS* raw = nullptr; };
   PS* g = nullptr;
   int main(int argc, char** argv)
   {
       bool c = argc > 1;
       { Holder h; PS* outer = nullptr; h.raw = (c ? new PS() : nullptr); outer = h.raw; }
       printf("field births=%d dtors=%d\n", births, dtors);   // run with an argument: 1 / 0
       { PS* outer = nullptr; g = (c ? new PS() : nullptr); outer = g; }
       g = nullptr;
       printf("global births=%d dtors=%d\n", births, dtors); // 2 / 0 after both blocks
       return 0;
   }
   ```
   Owner bit on raw field/global slots is the same design call as above; not fixed by the chain work.
