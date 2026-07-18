# A `(T*)` cast defeats every `unique` borrow rule

> RESOLVED 2026-07-17 - fixed via Approach A (`NamedVariable::IsUniqueFieldAlias`), verified green
> (test.bat 46/0, example.bat 90/0/27), four `err_unique_cast_*` tests added. Migration unblocked
> (`internal/plan/field-ownership-unique.md`). Safe to delete on commit.

Found 2026-07-16 in the `unique` code review. Confirmed under ASAN, still open after the Stage 4/5
remediation.

## DECISION (2026-07-17): Option 1 - a cast PRESERVES borrow provenance

`(T*)b.p` stays a `unique`-field alias; all three rules (`delete`, `move`, store-into-field) fire.
This is the most-correct option. Chosen over reject (Option 3) deliberately.

**Gate before any edit:** verify the `srcIsOwningMove` interaction with a scratch repro FIRST -
`Storage`-severing is how ownership-transfer-through-a-cast is detected today, and preserving
provenance may collide with it. Stage 3 went 0-for-3 by coding from reading rather than running; do
not repeat that. Also settle what `(char*)b.p` (a genuine type change) means for the alias framing.

This is the last known blocker on migrating `core/` to `unique`
(`internal/plan/field-ownership-unique.md`, Migration).

## Summary

Stage 3 closed Trap B by marking a local bound from a `unique` field as borrowed, which routes
`delete`, `move` and store-into-field into rejections. **A cast bypasses all three**, because the
clause requires the RHS to have `Storage`, and a cast severs it.

The escaping spelling is not obscure. It is the identical code plus a cast - which is exactly what a
programmer reaches for when a compiler rejects a pointer assignment.

## Repro

```cflat
struct Res { int v = 0; };
struct Box { unique Res* p = nullptr; };

extern int main()
{
    Box b = default;
    b.p = new Res();
    Res* a = (Res*)b.p;   // cast: compiles clean
    delete a;             // AddressSanitizer: attempting double-free
    return 0;
}
```

Control - the identical code WITHOUT the cast is correctly rejected:

```
spike_cast_ctl.cb(7,4): cannot delete 'a' - it aliases unique field 'Box.p', whose synthesized
destructor already frees it, so this is a double-free. Use 'move b.p' to take ownership out of the
field (which nulls it), or let the field's destructor free it.
```

All three Stage 3 rules have the same trivially adjacent escape:

| Rule | un-cast | with `(Res*)` |
|---|---|---|
| `delete alias` | rejected | **compiles -> ASAN double-free** |
| `consume(move alias)` | rejected | **compiles** |
| `c.p = alias` | rejected | **compiles** |

## Why this is worse than it sounds

The `delete` diagnostic above actively steers the user toward `move` - and `move` is bypassed by the
same cast. So the compiler rejects the safe-looking spelling, recommends a fix, and silently accepts
both the workaround a user is most likely to try AND the fix it just recommended.

The same standard was applied to the direct field-to-field hole (`c.p = a.p`), which WAS treated as a
migration blocker and fixed in Stage 4, on the reasoning that a diagnostic which steers users into a
heap bug closes worse than no diagnostic at all. This is the same shape.

## Root cause

`MainListener.h`, the Trap B clause in `ParseDeclarationSpecifiers`' declaration-init path (search
`srcIsBorrowed` and `IsUniqueFieldRead`). The clause requires `rightNV.Storage != nullptr`. The
comment immediately below it documents that a cast severs `Storage` - that behavior is deliberate and
pre-existing, used by the `srcIsOwningMove` path to detect ownership transfer through a cast.

So the cast does not "sneak past" a check by accident; it lands in a path that was built assuming a
severed `Storage` means an ownership transfer, which is the opposite of what is happening here.

## The design question (UNDECIDED - this is why the issue exists)

**What should a cast mean for borrow tracking?** The options are not obviously separable, and the
existing `Storage`-severing behavior is load-bearing elsewhere:

1. **A cast preserves borrow provenance.** `(T*)b.p` stays a `unique`-field alias, and all three
   rules fire. Closes the hole. Risk: `Storage`-severing is how `srcIsOwningMove` recognizes an
   ownership transfer through a cast - preserving provenance may collide with it. Also unclear what
   should happen for a cast that genuinely changes type (`(char*)b.p`), where the "alias of Box.p"
   framing gets thin.
2. **A cast is an explicit escape hatch.** Document that casting off a `unique` field opts out of
   the ownership rules, deliberately - the C answer. Cheap and honest, but it means the feature's
   guarantees end at the first cast, and the rejection messages must stop recommending `move`
   without qualification.
3. **Reject casting a `unique` field.** `(T*)b.p` is an error; `move b.p` is the sanctioned way out.
   Closes the hole with no provenance machinery. Risk: may break legitimate reinterpretation, and
   `unique` is supposed to be additive - this would make a previously-legal cast illegal on any
   migrated type.

Option 3 is the most consistent with how the other hard cases were settled (D2 recursion, unions,
fixed arrays: "reject what the compiler cannot express unambiguously"), and it is reversible - an
error can become working code later without breaking anyone. Option 1 is the most correct if the
`srcIsOwningMove` interaction turns out to be tractable. **Not decided.**

## Before implementing whichever option wins

Verify the `srcIsOwningMove` interaction FIRST with a scratch repro. Several design items in this
feature have died on contact with the code because they were written from reading rather than
running it (see `internal/plan/field-ownership-unique.md`, Stage 3 - cancelled, 0 for 3).

Regression surface: ~254 ordinary (non-`unique`) scalar pointer fields across 144 structs, plus
every existing cast in `core/` and `example/`. `example.bat` baseline is 90 passed / 0 failed / 27
skipped.

## Related

- `internal/plan/field-ownership-unique.md` - the feature, the review findings, and Migration (blocked
  on this).
- `internal/plan/ownership-move-alias-discipline.md` - Trap B in its original form.
