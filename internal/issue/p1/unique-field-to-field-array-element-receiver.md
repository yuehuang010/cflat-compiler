# `unique` field-to-field copy between ARRAY ELEMENTS is never diagnosed

Filed 2026-08-01 by the round-1 review of `fix/uniq-family`. **Pre-existing** - measured
identical on master `cf5e909` and on that branch, so it is neither caused nor worsened by the
field-store work that branch landed.

Severity: silent abort (exit 134), no diagnostic at all - the same class as the field-to-field
issues that branch closed, reached through a receiver shape none of its gates can see.

## Repros - both spellings, both binaries identical

### Written `unique` spelling

```cflat
int gfreed = 0;
struct Node { int v = default; ~Node() { gfreed = gfreed + 1; } };
struct Holder { unique Node* slot = nullptr; };
extern int main()
{
    Holder[2] arr = default;
    arr[0].slot = new Node(); arr[0].slot->v = 7;
    arr[1].slot = new Node(); arr[1].slot->v = 8;
    arr[0].slot = arr[1].slot;
    printf("v=%d freed=%d\n", arr[0].slot->v, gfreed);
    return 0;
}
```
```
v=8 freed=1
```
compile rc 0, run rc 134 on both binaries. (`freed=1` is the CORRECT reassignment free of the
old `arr[0].slot`; the abort is the two slots freeing the one surviving pointee at teardown.)

### Generic-substituted spelling

```cflat
struct Box<T> { T t = default; };
...
    Box<unique Node*>[2] arr = default;
    arr[0].t = new Node(); arr[0].t->v = 7;
    arr[1].t = new Node(); arr[1].t->v = 8;
    arr[0].t = arr[1].t;
```
```
v=8 freed=0
```
compile rc 0, run rc 134 on both binaries.

The NAMED-LOCAL equivalent of either spelling (`a.slot = b.slot`, `a.t = b.t`) is correctly
rejected on both binaries - so the ownership gates and the field-to-field reject are all fine.
Only the array-element receiver escapes them.

## Root cause direction - NOT verified past the receiver identity

The receiver-identity fields both gates consult name the CONTAINER, not the element:
`ParentVariableName` and `CallerName` for `arr[0].slot` and `arr[1].slot` are both `arr`.
With the same `FieldName` on both sides and the same caller name, `selfFieldAssign`
(`cflat/MainListener.h`, computed at the top of the field-store stack in
`ParseAssignmentExpression`) concludes self-assign and suppresses the whole guard stack -
the same failure mode as
[[interface-field-self-assign-false-positive]], reached through a different receiver kind.

That much is derived from the field assignments at `MainListener.h:19549` / `:20058` (the
array-element member-access branches both stamp `structVar.TypeAndValue.VariableName`, which is
the container). It has NOT been confirmed by instrumenting a run, and the index expressions are
constant here - whether a non-constant index behaves the same is unmeasured.

## Relationship to the other open receiver-identity issue

This shares a mechanism with [[interface-field-self-assign-false-positive]] and very likely
shares a fix: both need proof that two field lvalues denote DIFFERENT SLOTS, which neither a
variable name nor the receiver's storage can supply. That file records an attempted name-based
fix that false-rejected working programs, plus two witnesses and three approaches that will not
work. **Read it before attempting this one** - a fix that keys on names will false-reject
`arr[i].slot = arr[i].slot` for exactly the same reason.

Unlike the interface case, an array element does have a distinguishing signal available in
principle: the element GEP's index operand. Two CONSTANT indices that differ prove different
slots. A non-constant index proves nothing and must be accepted, per the polarity rule.

## Test coverage

None. Wants an `expect_error` leg once fixed, plus a must-keep-working leg for
`arr[i].slot = arr[i].slot` with a runtime `i`.

Related: [[interface-field-self-assign-false-positive]], [[interface-issue-queue]]
