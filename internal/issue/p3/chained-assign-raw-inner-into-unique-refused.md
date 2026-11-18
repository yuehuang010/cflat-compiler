Bucket: p3 (usable-surface gap, conservative refusal) + one silent leak (see item 2)

**RULED 2026-10-21:** A ternary into a unique behaves exactly like if/else - accepted in declaration AND assignment, the selected arm is consumed (later read refused maybe-moved); borrow-returning calls (`a = pick()` of a global) are refused on assignment like the declaration. Probes scratch/rulings1021/{d,a}_*.cb. See fix-issue-lessons.md digest.

# Chained `owner = <inner> = new PS()` - remaining gaps

Still open:

2. Parameter destination leak (master the same): `void f(PS* raw, bool c) { PS* other = new PS(); raw = (c ? new PS() : other); }`
   with c true frees `other` but not the fresh block (2026-11-10 probe: 2 births, 1 dtor). Not chain-specific:
   the plain `void f(PS* raw) { raw = new PS(); }` also leaks (births 1, dtors 0). Argument slots carry no owner
   bit, and the statement store excludes stack arguments from adoption (`FindStackArgumentByStorage`), so there is
   nothing to adopt the fresh block into. Fixing it needs an owner bit on argument slots, or a refusal of a
   fresh-object store into a borrowed parameter (a design call, not made here).

3. Sibling, still refused while the two-statement form compiles (2026-11-10):
   `unique PS* a = nullptr; PS* b = nullptr; PS* other = new PS(); a = b = (c ? other : nullptr);`
   The two-statement `b = (c ? other : nullptr); a = b;` compiles (births 1, dtors 1). The chained
   `?:` with an owning LOCAL arm and no fresh arm is not adopted: `chainedAssignmentJoinOwns` needs a fresh owner
   arm (or two arms with a named owner plus a fresh one). Same for `return (raw = (c ? other : nullptr))`, which is
   refused in both the chained and the two-statement form (not a gap).

Global inner `owner = gRaw = new PS()` stays refused (a global is never a consumable slot).

4. Middle is a global or implicit-this field (2026-11-10, round 2, OPEN): `a = gRaw = e = new PS();`, `a = b = e = new PS();`
   with `b` an implicit-this field or a same-named global. The chain is accepted; the two-statement form
   (`gRaw = e; a = gRaw;`) is refused. The middle's store never reaches the chain gate, and the outer '=' adopts the
   pending `new` temp (`srcIsOwnedForCoreUnique`), so no leg can pin it yet. Fix: read the non-local destination as
   the named variable `a = <name>;` does, then let the existing refusal fire.
