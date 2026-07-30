# A type whose declaration fails inside `expect_error` stays registered and poisons its own name

Filed 2026-07-30, found by the adversarial review of
[[interface-issue-queue]] (landed design records) (round 2). **Pre-existing on both binaries** and
independent of generics.

Severity: **false rejection, contained to the file that declares it.** No wrong value.

## Repro

`scratch/r2/h1b_control_nongeneric.cb` - no generics anywhere:

```cflat
struct Item { int v = 9; };
namespace A
{
    expect_error("nullable '?' is not allowed on primitive type 'int'") {
        struct Item { int? bad = 0; };
    }
    int f() { Item i = default; return i.v; }      // wanted: the global Item
}
```

```
h1b_control_nongeneric.cb(8,22): nullable '?' is not allowed on primitive type 'int'
PASS: expected error received
h1b_control_nongeneric.cb(10,14): type 'A.Item' has an incomplete layout (a field type C interop
could not import); it can only be used through a pointer
```

Identical on `15809e0` and on the type-argument fix.

The `expect_error` block does its job - the expected diagnostic fires and is swallowed - but the
struct SHELL for `A.Item` was already registered by the forward-ref scan, and its layout never gets
filled because the field that would fill it is the thing that failed. From that point on, the name
`Item` inside `namespace A` resolves to that dead shell, so unrelated later code in the same
namespace cannot use the global `Item` any more.

The message is also wrong twice over: the file imports no C, and the real cause ("a declaration in an
`expect_error` block failed, so this type has no body") is not mentioned. Same reused
incomplete-layout diagnostic recorded under T5 of [[interface-issue-queue]] (landed design records).

## Root cause direction

`ForwardRefScanner::ScanStructOrClassDefinition` registers the shell for every struct it walks,
including those inside an `expect_error` block, and the main pass abandons the body on the expected
error without unregistering. Two candidate repairs:

- Roll back the types a failing `expect_error` block registered when its error is swallowed
  (symmetrical with how the block already swallows the diagnostic), or
- keep the registration but mark the type POISONED, and have name resolution skip a poisoned
  candidate so the enclosing scope keeps resolving as if the block were not there.

The second is closer to how `certain == false` is already used for the generic template key space (a
declaration inside an `expect_error` block must not claim or veto a name outside the block) and would
extend that convention to ordinary types.

## Consequence for the type-argument fix

`generic-type-arguments-not-key-space-resolved` resolves a bare generic type ARGUMENT through the
enclosing-namespace chain, so `Box<Item>` inside `namespace A` now reaches `A.Item` and lands on this
defect exactly where a plain `Item` local already did (`scratch/rev5/h1_expect_error_type.cb`, PRE
`inA=9` -> now the same incomplete-layout family of error, reported from inside the template body as
`Unknown identifier 'v'.`). The generic form has converged onto the non-generic answer, which is the
ratification standard used throughout this queue; it cannot be repaired from the argument accept set,
because `dataStructures` holds the dead shell and is authoritative for the main pass. This issue owns
the repair.

Note the diagnostic gets WORSE through the generic route: the error surfaces at the template body's
field access rather than naming the type, so a user sees `Unknown identifier 'v'.` with no mention of
`A.Item` at all. Whatever fix lands here should also make the generic route name the type.

Related: [[interface-issue-queue]] (landed design records)
