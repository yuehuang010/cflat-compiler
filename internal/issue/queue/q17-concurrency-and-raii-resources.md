# q17: Concurrency and RAII resources

3 items. Resources with a lifetime the compiler cannot verify (a thread, a pool) currently have no
destructor at all, and one wait loop can never terminate.

## Shared root cause

Two of the three are DELIBERATE deferrals, not defects: giving `Thread` and the pools a destructor
requires a precondition the compiler cannot check (quiescence for pools; no bitwise copy and no
by-design detach for `Thread`). They are grouped here because any RAII decision must settle both
together - a destructor on one and not the other leaves the same footgun.

## Members

- `p3/thread-cannot-go-raii` - `Thread` is bitwise-copied in `ThreadPool.resize` and detached by
  design in channel pipes, both of which block a destructor. DESIGN, not a bug.
- `p3/pools-no-destructor-shutdown-ordering` - pool teardown has a quiescence precondition the
  compiler cannot verify, so the destructor is deliberately omitted. DESIGN, not a bug.
- `p2/waitforexit-stop-token-hangs-on-unstarted-program` - `try_join` is called with a null thread
  handle and always returns false, so the wait loop never exits. A REAL bug and independent of the
  other two; fix it on its own.

## RULING 2026-08-11: both stay filed as design deferrals; the Thread direction is blocked on a language gap

Decided by the maintainer.

**Direction, if it is ever pursued:** make `Thread` non-bitwise-copyable (i.e. `ThreadPool.resize`
moves rather than bitwise-copies). Pool quiescence-as-a-typestate was NOT selected.

**Blocker, and this is the new finding: CFlat has no syntax for a deleted copy.** There is no way
to spell "this type may not be copied", so `Thread` cannot be made non-bitwise-copyable today. The
RAII question is therefore downstream of a missing LANGUAGE FEATURE, not merely of a design choice
- which is why both items stay filed rather than becoming work.

Anyone picking this up must add the deleted-copy spelling first. That is a language-design task in
its own right (grammar, both `ParseDeclarationSpecifiers` copies, diagnostics at every copy site,
and an interaction with the existing move/borrow inference) and should not be smuggled in as part
of a `Thread` fix. Candidate for its own issue file if it is ever scheduled.

**Status: neither item is a bug.** Nothing is unsafe today, because neither type has a destructor
that could run at the wrong time. Do not add one to either.

## Fix direction

- Fix `p2/waitforexit-stop-token-hangs-on-unstarted-program` immediately and separately: guard the
  null handle and return "already exited" for a program that was never started.
- For the two design items, the question to answer first is whether `Thread` can stop being
  bitwise-copyable (make `ThreadPool.resize` move instead) and whether pool quiescence can be made
  a typestate rather than a runtime precondition. Only after that does a destructor become
  expressible. Do not add a destructor to either without answering it - the deferral is recorded
  because a naive destructor was already judged unsafe.

Relevant context: the planned concurrent B-tree needs hand-over-hand locking, so raw mutex
acquire/release must remain legal. Do not propose an RAII-only lock story as part of this bucket.
