Bucket: p3 (usable-surface gap) + one silent leak (see item 2)

# Chained `owner = <inner> = new PS()` - remaining gap

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

3. Owner/borrow ternary arm mismatch (same refusal on master):
   `unique PS* a = nullptr; PS* other = new PS(); PS* b = nullptr; PS* bw = other;`
   `a = b = (c ? other : bw);` is refused, while `b = (c ? other : bw); a = b;` is accepted.
   The same holds with X a parameter, a global or a field. The owner/borrow X-arm family remains
   outside the nullptr-arm chain fix.
   Declaration form, same mismatch: `unique PS* a = b = other;` is refused while
   `b = other; unique PS* a = b;` is accepted.
