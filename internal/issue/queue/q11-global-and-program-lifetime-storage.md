# q11: Global, program-lifetime, and static storage

4 items. Ownership machinery is written against `AllocaInst` locals. Storage that is not a local -
a `GlobalVariable`, program-lifetime storage, a static local - falls out of the analysis or is
re-entered a second time in a state the analysis never anticipated.

## Shared root cause

Storage-class blindness. Guards, taint recording, and codegen tails test for or assume an alloca,
so global and static destinations skip the path entirely; and nothing re-initializes
program-lifetime storage between entries, so any "consume once" rule silently becomes "works the
first run, breaks the second".

## Members

- `p2/move-out-of-program-lifetime-storage-crashes-on-reuse` - storage is not re-initialized
  between calls, so a second `move` double-frees the zeroed value.
- `p3/implicit-consume-of-a-global-owner-loses-the-value-on-the-second-run` - assignment from a
  global owner now takes the transfer arm, nulling the global on first use.
- `p2/global-pointer-destination-does-not-propagate-borrow-taint` - taint recording is gated on
  `AllocaInst` storage, excluding `GlobalVariable` destinations.
- `p3/static-local-missing-origin-slot-and-dwarf` - static-local codegen returns early from the
  `GlobalVariable` path, skipping the origin-slot and DWARF tails.

## BLOCKED 2026-08-11: needs a design discussion, not a fix round

The maintainer's position, recorded verbatim in substance: a global has very little compile-time
lifetime safeguard BECAUSE its lifetime cannot easily be proven, and therefore a destructor cannot
easily be run for it. That is the crux - it is upstream of all four filed items, and it is not a
question a fix agent can answer from the issue files.

Do NOT start this bucket until that discussion has happened. The three options already on the
table, none chosen:

1. Implicit consume from a global/program-lifetime owner is an ERROR; explicit `move` is allowed
   and re-initializes the storage on entry. Breaking, makes the cost visible.
2. Reading a global owner COPIES rather than transfers. Non-breaking, matches borrow-by-default,
   but silently pays a copy the user did not write.
3. Keep transfer semantics, re-initialize on entry so reuse is defined. Least disruptive, keeps
   the surprise that reading a global empties it.

Whatever is chosen has to answer the destructor question too, not just the consume question -
that is why the maintainer flagged it as the harder one.

Note `p3/static-local-missing-origin-slot-and-dwarf` is NOT blocked on this: it is a plain codegen
early-return with no semantic content, and can be fixed independently at any time.

## Fix direction (applies once the ruling above exists)

1. Audit for `isa<AllocaInst>` / alloca-shaped assumptions in the ownership paths and replace with
   a storage-class query that admits globals and statics. `p2/global-pointer-destination-does-not-propagate-borrow-taint` and
   `p3/static-local-missing-origin-slot-and-dwarf` are both instances of an early return on that test.
2. Decide the semantics for consuming from storage whose lifetime outlives the function: the two
   consume issues are the same question asked of a global and of program-lifetime storage, and
   they should get the SAME answer (most likely: reject the implicit consume, require an explicit
   `move` that also re-initializes).
3. `p2/move-out-of-program-lifetime-storage-crashes-on-reuse` needs re-initialization on entry, not
   just a guard.

Reasonably self-contained; can run alongside the q01 chain if the consume-arm edits are kept out
of the shared assignment code that q05 will rewrite. If they are not, sequence after q05.
