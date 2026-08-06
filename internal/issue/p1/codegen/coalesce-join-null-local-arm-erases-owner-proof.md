# A `??` join whose LHS arm is a null-valued LOCAL erases the owner proof (raw AND boxed)

Filed 2026-08-05 by `fix/ptrcopy`, which closed the `?:` join member of
`pointer-copy-propagates-no-ownership-fact` and measured this one while building that member's
accept set. Exit 134, no diagnostic, identical on `d93c359` and on the merged `fix/ptrcopy`.

## Root cause

Both the interface-boxing per-arm ledger and the pointer-join proof `fix/ptrcopy` added apply the
same BOTH-ARMS rule: every non-null arm must prove another owner, or the fact is dropped. A `null
LITERAL` arm is skipped as neutral (it owns nothing, so deleting it is a no-op and it cannot be the
sole owner). A LOCAL that merely HOLDS null is not a literal - it resolves to a live binding that
proves nothing - so it fails the rule and the whole join is accepted.

The rule is right; the arm classification is too narrow. `Ci* n = nullptr;` is statically known to
own nothing at that point in exactly the way the literal is, and treating it as a blocking arm
turns a provable join into an unprovable one.

## Repros

Common prelude:

```cflat
int dtorCount = 0;
interface IS { int area(); };
class Ci : IS { int r = 7; int area() { return r; } ~Ci() { dtorCount = dtorCount + 1; } };
```

### (a) The RAW spelling - exit 134 on both binaries

```cflat
Ci* c = new Ci();
Ci* n = nullptr;
Ci* b = n ?? c;    // accepted; b == c on every path
delete b;          // `c` frees it again at scope exit
```

### (b) The BOXED spelling - exit 134 on both binaries

```cflat
Ci* c = new Ci();
Ci* n = nullptr;
IS s = n ?? c;
delete s;
```

**The two spellings AGREE here**, which is why this is not an asymmetry `fix/ptrcopy` introduced:
the boxed path had the same hole before that change and still has it. Its `?:` twins are both
correctly rejected on both binaries (`IS s = idb(true) ? c : nullptr; delete s;` rejects, and the
raw `Ci* b = idb(true) ? c : nullptr; delete b;` rejects after `fix/ptrcopy`), so the defect is
specific to a null-valued LOCAL arm, not to the `??` spelling as such.

## Fix direction

In `BoxInterfaceJoinArms`'s arm loop and in `JoinArmsKeepOwner` (`MainListener.h`), treat an arm
that resolves to a binding PROVABLY holding null the way a `ConstantPointerNull` arm is treated -
skipped as neutral rather than counted as a non-proving arm. Both sites share the classification,
so change them together or the raw and boxed spellings diverge.

**Polarity caution.** "Provably null" must be a positive proof about the arm at that program point,
not "we could not tell what it holds" - widening it to the latter would make a join of two
unresolvable arms provable and re-open the false-rejection direction. The accept legs this must not
break are `join_owner_new_*` and `join_two_new_*` in `Test/test_move.cb`.

## Related

[[interface-issue-queue]] - the `fix/ptrcopy` landed design record states the BOTH-ARMS rule and
why a null LITERAL arm is neutral.
