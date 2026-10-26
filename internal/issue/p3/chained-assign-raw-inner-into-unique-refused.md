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
- Still refused while the two-statement form compiles: a `??` arm, a `(PS*)0` arm, `owner = (raw = mkU())`,
  `return (raw = new PS())`.
- Pre-existing leaks in the TWO-statement form (master and T250 alike): `raw = mkU(); owner = raw;` 0 dtors
  (cases/mk_two.cb); `raw = (c ? new PS[3] : nullptr); owner = raw;` into `unique PS*` 1 of 3 dtors
  (cases/arr_tern_two.cb - should refuse an array into a scalar unique); `PS* other = new PS();
  raw = (c ? new PS() : other);` 1 dtor where 2 are expected (cases3/tern_owned_two_nouse.cb).

## Ruling 2026-10-15 (T250): two-statement form refused
`r.raw = new PS(); owner = r.raw;` is refused exactly like `unique PS* owner = r.raw;` and the chained form - assignment
and declaration agree. Steer: `owner = move r.raw;` or `owner = new PS();`.

## T443 (2026-10-23): ternary into unique + borrowed-global call landed; open below
Fixed: `owner = k ? raw : nullptr` (decl and assign) consumes the selected arm like if/else; `owner = pick()` is refused when
every return of the callee is a module global (parse-tree summary, order-independent, per generic instance).
- NEEDS RULING (part 2 of the 2026-10-21 ruling): a call returning a PARAMETER borrow stays accepted on assignment while the
  declaration refuses it. Refusing it breaks accept legs test_move.cb:2823 (`borrowed = borrowFirstResource(h, new Resource())`),
  :2869, :2882, :3049 (`m = mixedReturnResource(makeIdResource(51), true)`) and the alias-proof family. Change those legs, or keep
  the parameter-borrow call accepted on assignment?
- Gap (T443 Sol r4): the return summary counts returns in DISCARDED `if const` branches, so `N* pick<T>(N* p) { if const
  (sizeof(T)==4) { return g; } else { return p; } }` leaves pick<int> unflagged and `owner = pick<int>(nullptr)` compiles. Fix:
  collect live returns only, using the if-const evaluator per instance. Repros scratch/repro_keep/box1022/t443/ (generic_ifconst*.cb, ifconst_dead.cb).
- `j_field` residual (T266) unchanged.
