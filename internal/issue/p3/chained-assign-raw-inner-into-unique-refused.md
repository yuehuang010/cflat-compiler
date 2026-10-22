Bucket: p3 (usable-surface gap, conservative refusal) + one silent leak (see item 2)

# Chained `owner = <inner> = new PS()` - remaining gaps after T250

T203 (raw local) and T250 (ternary-new, parenthesised, declaration inner) now match the two-statement
form. Still open:

3. Chained three deep `a = b = c = new PS()` stays refused ("cannot reset unique<PS> 'a' from
   borrowed value 'b'"). The two-statement `c = new PS(); b = c; a = b;` compiles (dtors=1). Kept
   as is: the middle `b = c` is a raw alias, and extending the slot through a raw alias is not done.

Global inner `owner = gRaw = new PS()` stays refused (a global is never a consumable slot).

## T250 review 1 (repros scratch/repro_keep/t250_rev/)
- Still refused while the two-statement form compiles: a `??` arm, a `(PS*)0` arm, `owner = (raw = mkU())`,
  `return (raw = new PS())`.
- Pre-existing leaks in the TWO-statement form (master and T250 alike): `raw = mkU(); owner = raw;` 0 dtors
  (cases/mk_two.cb); `raw = (c ? new PS[3] : nullptr); owner = raw;` into `unique PS*` 1 of 3 dtors
  (cases/arr_tern_two.cb - should refuse an array into a scalar unique); `PS* other = new PS();
  raw = (c ? new PS() : other);` 1 dtor where 2 are expected (cases3/tern_owned_two_nouse.cb).

## Ruling 2026-10-15 (T250): two-statement form refused
`r.raw = new PS(); owner = r.raw;` is refused exactly like `unique PS* owner = r.raw;` and the chained form - assignment
and declaration agree. Steer: `owner = move r.raw;` or `owner = new PS();`.

## T266 (2026-10-15): two-statement `owner = r.raw` refused (direct, implicit-this, global, ternary arm)
Still accepted on master and after T266 while the DECLARATION refuses them (assignment = declaration ruling):
`owner = pick()` where pick returns a borrowed global (but a plain-field accessor returning a raw pointer is adopted by
design - test_move plain_field_borrow_adopt_value has the same shape), and `owner = k ? raw_local : nullptr`; also
`j_field` residual. NEEDS RULING: must assignment match the declaration for call results that return a borrow?
Repros: scratch/repro_keep/t266_rev/.
