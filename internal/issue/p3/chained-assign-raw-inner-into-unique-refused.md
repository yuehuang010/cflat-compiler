Bucket: p3 (usable-surface gap, conservative refusal) + one silent leak (see item 2)

**RULED 2026-10-21:** A ternary into a unique behaves exactly like if/else - accepted in declaration AND assignment, the selected arm is consumed (later read refused maybe-moved); borrow-returning calls (`a = pick()` of a global) are refused on assignment like the declaration. Probes scratch/rulings1021/{d,a}_*.cb. See fix-issue-lessons.md digest.

# Chained `owner = <inner> = new PS()` - remaining gaps after T250

T203 (raw local) and T250 (ternary-new, parenthesised, declaration inner) now match the two-statement
form. Still open:

3. Chained three deep `a = b = c = new PS()` stays refused ("cannot reset unique<PS> 'a' from
   borrowed value 'b'"). The two-statement `c = new PS(); b = c; a = b;` compiles (dtors=1). Kept
   as is: the middle `b = c` is a raw alias, and extending the slot through a raw alias is not done.

Global inner `owner = gRaw = new PS()` stays refused (a global is never a consumable slot).

## T250 review 1 (repros scratch/repro_keep/t250_rev/)
- Still refused while the two-statement form compiles: a `??` arm and a `(PS*)0` arm (three attempts, 2026-11-08).
- `return (raw = (c ? new PS() : other))` into `unique PS*` is refused ("cannot return borrowed value 'raw'"),
  while `raw = (c ? new PS() : other); return raw;` compiles (2026-11-08).
- Parameter destination leak (master the same): `void f(PS* raw, bool c) { PS* other = new PS(); raw = (c ? new PS() : other); }`
  with c true frees `other` but not the fresh block (3 births, 2 dtors). Argument slots carry no owner bit, so the
  join is not adopted there.
