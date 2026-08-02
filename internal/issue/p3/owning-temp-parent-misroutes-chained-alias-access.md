# `OwningTempParent` is wrongly TRUE at depth >= 2 under an `alias` container return

Filed 2026-08-01 by the round-3 adversarial review of `fix/uniq-family`, the commit that split the
two-owner temp-source diagnostic into a call-result message and a container-element message.
**Not a correctness regression**: no program's accept/reject status changed, and no new double free
is reachable. It is a WRONG DIAGNOSTIC on a narrow shape - the message states a mechanism that is
false and names a remedy that aborts.

Severity: wrong diagnostic, no correctness change. Filed at P1 only because it sits in the
`unique` field-store family and the next person to touch that routing must see it; re-rank freely.

## Root cause - diagnosed, one line

`cflat/MainListener.h:19725-19728`:

```cpp
bool parentOwnsTemp = parentIsOwningTemp && !structVar.TypeAndValue.IsAlias;
namedVar.OwningTempParent = parentOwnsTemp;
```

The `alias`-ness of the ORIGINAL return does not survive a member-access hop. At the second hop
`structVar` is the intermediate FIELD, whose `TypeAndValue.IsAlias` is false, so an `alias`
container return looks like an owning temp. Compare `FromOwningTempField` two lines below, which
propagates explicitly (`parentIsOwningTemp || structVar.FromOwningTempField`). `OwningTempParent`
has no such propagation.

Depth 1 (`l.get(0).t`) routes CORRECTLY. Only depth >= 2 misroutes.

## Repro

```cflat
import "list.cb";
class Node { int v = 55; };
struct Inner { unique Node* t = default; };
struct Outer { Inner inner = default; };
struct Holder { unique Node* slot = nullptr; };
extern int main()
{
    list<Outer> l;
    Outer o = default; o.inner.t = new Node(); l.add(move o);
    Holder c = default;
    c.slot = l.get(0).inner.t;
    return 0;
}
```

Emits the CALL-RESULT wording: "cannot store unique field 't' of a temporary ... the temporary's
synthesized destructor frees it at the end of this statement ... bind the whole call result to a
local first and move the field out of that local instead."

Both claims are false - the **list** owns the pointee, and nothing is freed at end of statement.
Following the named remedy aborts: `alias Outer e = l.get(0); c.slot = move e.inner.t;` compiles
and exits **134** on both the pre-fix binary (`cf5e909`) and the fix commit.

The correct message for this shape is the container-element one, whose remedy (drop `unique` from
the destination field if it only borrows) does work.

## Why it is not a regression

The pre-fix binary rejects the same program with `... Use 'move t' ...`, which is an equally dead
remedy. The fix changed WHICH wrong message appears, not whether the program compiles.

## Fix direction

Propagate the alias origin across hops, mirroring the sibling flag - e.g.
`parentOwnsTemp && !structVar.FromOwningTempField`, or carry an explicit alias-origin bit
alongside `FromOwningTempField`. Verify at depth 1, 2 and 3, and with both an owning and an
`alias` container return, since the two must route to different messages.

Wants a regression leg pinning the CONTAINER wording on the depth-2 chained shape; the existing
`tempElementFieldToField` leg only covers depth 1.

Related: [[unique-field-to-field-array-element-receiver]],
[[temp-unique-field-into-borrow-slot-use-after-free]], [[interface-issue-queue]]
