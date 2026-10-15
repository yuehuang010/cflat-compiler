Bucket: p3 (usable-surface gap, conservative refusal) + one silent leak (see item 2)

# Chained `owner = <inner> = new PS()` - remaining gaps after T250

T203 (raw local) and T250 (ternary-new, parenthesised, declaration inner) now match the two-statement
form. Still open:

1. Unique-field inner leaks silently: `owner = u.p = new PS();` (u: struct with `unique PS* p`)
   compiles, one object leaked (dtors=0). The two-statement `u.p = new PS(); owner = u.p;` is correct
   (dtors=1). Read of `u.p` after the chain is not diagnosed in either form (null deref at run time).
   Root cause: the unique-field '=' returns the OLD field value as its Primary (the value is
   stored before the reset returns), and the NamedVariable copy drops IsOwning, so the outer unique
   '=' takes the stale value. A reload of the field pointer was tried and reverted: the outer then
   emits `get(raw)` on the raw object pointer (wrong code). Fix direction: make the outer treat the
   inner's field as a unique-field move source (as IsUniqueFieldRead does for `owner = u.p`).

2. Two-statement field-raw inner leaks: `r.raw = new PS(); owner = r.raw;` (r: plain struct with
   `PS* raw`) compiles with dtors=0 (the source is an unproven raw value, so owner becomes an alias
   and nothing frees it). Chained form `owner = r.raw = new PS()` is REFUSED on purpose
   ("cannot reset unique<PS> 'owner' from borrowed value 'r'"), matching `unique PS* owner = r.raw;`.
   Decide: refuse the two-statement assignment too (consistent with the declaration form), or
   accept with a consume of the field. Not changed by T250.

3. Chained three deep `a = b = c = new PS()` stays refused ("cannot reset unique<PS> 'a' from
   borrowed value 'b'"). The two-statement `c = new PS(); b = c; a = b;` compiles (dtors=1). Kept
   as is: the middle `b = c` is a raw alias, and extending the slot through a raw alias is not done.

Global inner `owner = gRaw = new PS()` stays refused (a global is never a consumable slot).

Repro (item 1):
    unique PS* owner = nullptr; U u; owner = u.p = new PS();   // dtors=0, expected 1

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
