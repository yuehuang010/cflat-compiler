# q08: `for-in` loop variable

4 items. The range-for loop variable is a half-borrow: the compile-time record says owning, the
runtime borrow bit is cleared, and the rebind store per iteration does neither an owner's drop-old
nor a borrower's no-op.

## Shared root cause

The loop variable was implemented as a special binding that reuses the owning-local machinery with
the borrow bit cleared at runtime. Nothing reconciles the two views, so:

- rebinding it leaks the old value (owner path skipped);
- writing through its source slot frees the buffer it points at (borrow path assumed);
- returning it does not deep-copy (compile-time record says owning).

## Members

- `p2/assigning-to-a-for-in-loop-variable-leaks-on-the-next-rebind`
- `p2/for-in-loop-variable-dangles-when-its-own-slot-is-overwritten`
- `p2/returning-a-for-in-loop-variable-dangles`
- `p3/for-in-over-a-t-view-does-not-compile` - array-view type falls through to the user
  `count()`/`get()` overload leg instead of the view leg. Different symptom, same lowering.

## RULING 2026-08-11: the loop variable is a BORROW of the element; assignment writes through

Decided by the maintainer. Apply this one answer to BOTH the compile-time record and the runtime
borrow bit - the bug is that they disagree, so any fix that changes only one side re-creates it.

Consequences, all three of which are the filed symptoms:

- `for (x in list) x = v;` MUTATES the container element (write-through). It is not an error, and
  it is not a rebind of a local - so the "leaks on the next rebind" symptom disappears because
  there is no owning local to leak.
- Returning the loop variable DEEP-COPIES at the return. The compile-time record must stop
  claiming the variable is owning, which is what currently suppresses the copy.
- Overwriting the loop variable's own source slot becomes a diagnosable ESCAPE, since the borrow's
  referent is being destroyed while the borrow is live.

Do NOT implement this as an owning copy per iteration - that was considered and rejected: it taxes
every loop over an owning container and breaks mutation-through-loop.

## Fix direction

Self-contained bucket: it touches range-for lowering and little else, so it is safe to run in
parallel with the q01/q05 ownership chain. Fix `p3/for-in-over-a-t-view-does-not-compile` at the same time since it
is in the same lowering function.

Regression cases go into the existing range-for test file, not a new one.
