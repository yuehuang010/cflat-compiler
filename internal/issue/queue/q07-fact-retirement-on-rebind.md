# q07: Facts not retired (or wrongly retired) on rebind

5 items. Ownership and boxing proofs are computed once, at declaration, and then either never
re-checked after the variable is rebound, or retired unconditionally on any store regardless of
whether that store is reachable.

## Shared root cause

The analysis is flow-INSENSITIVE where it needs to be flow-sensitive, in both directions:

- retirement fires on any `=` without asking whether the branch is reachable
  (`p2/conditional-store-retires-borrow-facts-unconditionally`);
- proofs fold to a single bool at declaration and are never revisited
  (`p3/boxed-join-proof...`, `p2/null-interface-proof...`);
- a rebind reads the STALE pre-transfer fact (`p3/implied-move-store-boxed-spelling...`);
- a suppression flag set at bind time stays set after the body rebinds
  (`p3/rebound-lambda-capture-leaks-its-new-value`).

## Members

- `p2/conditional-store-retires-borrow-facts-unconditionally`
- `p3/boxed-join-proof-never-retires-a-rebound-arm`
- `p3/implied-move-store-boxed-spelling-false-rejects` - `MarkPointerRebound` reads a stale fact
  that only the boxed-delete guard consults.
- `p3/rebound-lambda-capture-leaks-its-new-value` - capture-unpack marks the local
  `IsAliasBorrow` to suppress the destructor; wrong once the body rebinds it.
- `p2/null-interface-proof-false-rejects-heap-initialized-field` - the proof does not treat a
  `= new Impl()` field default as an implementation assignment.

## Fix direction

Give proofs an explicit lifetime instead of a bool: record the fact WITH the binding generation it
was proved against, and invalidate on rebind by comparing generations. That covers stale reads and
never-retired proofs with the same mechanism.

For `p2/conditional-store-retires-borrow-facts-unconditionally`, retirement needs branch
reachability - do not widen it to "retire on any store" as a stopgap, since that turns a
false-accept into a false-reject across the whole borrow system.

Note the guard-polarity lesson in `internal/fix-issue-lessons.md` before touching these: two of
these five are false-REJECTS and three are false-ACCEPTS, and a single-sided fix flips the wrong
ones.

## Adjacent

q02 (the join arms these proofs run over), q01 (ledger retirement is the same shape).
