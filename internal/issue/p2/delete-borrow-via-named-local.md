# Borrowing container fed an owning local THROUGH a helper: still a silent use-after-free

## NARROWED 2026-09-01: the depth-1 helper shape is CLOSED (commit 1406cec, branch claude/dbnl-helper-gap)

The filed repro (Bag.put) is now rejected at the call site via a depth-1 callee summary
reusing the temp-unique-field family's MemoryOutlivesCall walk: a bare-pointer parameter
provably reaching a borrowing-container element sink whose receiver outlives the helper
frame rejects a PROVEN owning named local argument (same predicate as the add-site rule),
unknown-accepts, end-of-module resolve for late-emitted callees. Regression:
Test/errors/err_owning_local_via_helper.cb; accept-set in Test/test_list_ownership.cb.
REMAINING OPEN (this file now covers only these): depth-2 forwarding (put -> put2 -> add)
and indirect calls through function<T> - both accept, both need more than a direct-callee
walk. The approach-B and same-function-check prohibitions below still stand for THOSE.

Residual of the add-site rule that landed on `fix/add-owning-local-needs-move`. The direct
spelling (`l.add(p)` with `p` a live owning named local) is now a hard error naming
`add(move p)`; this file holds the one shape the rule deliberately does not reach, and it is
the accepted gap the ruling named when it was written, not a regression.

## Repro

```cflat
import "list.cb";
class B { int v = 0; double pad[8] = default; };
class Bag { list<B*> items; void put(B* p) { items.add(p); } };
extern int main() {
    Bag bag;
    int i = 0;
    while (i < 4) { B* n = new B(); n->v = i * 10; bag.put(n); i++; }
    printf("first=%d\n", bag.items.get(0)->v);   // prints garbage
    return 0;
}
```

Measured on the branch build, Release: compiles clean, `first=-1765856369` (the freed block's
reused bytes). The same program with `bag.items.add(n)` written inline is rejected.

## Root cause

The add-site rule is decidable exactly because it asks a LOCAL question: is this argument a live
owning named local of THIS frame? Inside `Bag.put` the argument is `p`, a borrowed by-value
parameter, so the rule accepts - correctly, since `put` cannot know whether its caller kept an
owner. The caller's `n` dies one line later and nothing at either site can see both facts.

This is the container-element flavour of the general provenance gap; the field flavour was
closed out on 2026-09-03 - see the temp-unique-field digest entry in
`internal/fix-issue-lessons.md`.

## Fix direction

Needs caller-visible parameter provenance, i.e. some summary on `put` saying "this parameter is
STORED into a borrowing container that outlives the call", propagated to the call site so the
caller's owning-local test can run there. That is an interprocedural summary pass, not a local
guard - do not retry it as a same-function check (the reverted approach B and the maintainer
ruling on false coverage are recorded in the digest in `internal/fix-issue-lessons.md`).

Until then the remedy is the same one the direct spelling names: hand the object over
(`bag.put(move n)`, once `put` declares `move B* p`), or give the container an element policy
(`list<unique B*>`).

## Related

- `internal/plan/unique-ownership.md` - the `alias` design.
- The landed add-site rule and its accept-set: `LLVMBackend::IsBorrowingContainerElementSink` /
  `RejectOwningLocalIntoBorrowingContainer`, regressions in
  `Test/errors/err_delete_borrowed_owned_element.cb` and `Test/test_list_ownership.cb`.
