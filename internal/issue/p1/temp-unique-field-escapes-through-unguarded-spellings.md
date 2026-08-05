# A temp's `unique` field escapes through spellings no persist site sees

Filed 2026-08-04 by the round-1 review of `fix/temp-uniq-borrow`, which closed
[[temp-unique-field-into-borrow-slot-use-after-free]] for the five persist sites it enumerated
(assignment, decl-init, interface decl-init, return, brace-init) and for the parenthesized
spelling. Every shape below is what that enumeration did NOT reach. All are **pre-existing** -
each was measured identical on master `6e9ab46` and on the merged fix - so this is a residue
file, not a regression.

Severity: **silent use-after-free**. Each program compiles clean, runs, prints a plausible
value and exits 0. That is the same severity as the issue this splits off from, which is why
this is P1 and not P3: these produce wrong programs today.

## The shared root

An owning struct TEMP (`makeBox()`) is registered as an owned struct temp and destructed at the
end of the statement, freeing its `unique` field's pointee. A read of that field carries
`FromOwningTempField` + `OwningTempParent`, and the fix rejects it at the sites where the value
is stored into a slot that outlives the statement. The spellings below either LOSE that
provenance before reaching a persist site, or reach a store that is not one of the guarded
sites at all.

## Measurement method

`MallocScribble=1` on macOS makes this family self-diagnosing: a use-after-free read of the
`int` field returns **1431655765** (the 0x55 fill) instead of 70. Every pair below is
`<binary> -o` + run under that env var, never `--check`.

## The spellings, each with its measured pre/post pair

`Box<unique Node*> makeBox()` with a dtor-LESS `Node` throughout; `scratch/tub_*` in the fix
worktree holds each repro.

| # | Spelling | master `6e9ab46` | merged fix |
|---|---|---|---|
| 1 | `Node* raw = (Node*)makeBox().t;` (same-type C-style cast) | rc 0, `v=1431655765` | rc 0, `v=1431655765` |
| 2 | `Node* raw = makeBox().t ?? nullptr;` (`??` join) | rc 0, `v=1431655765` | rc 0, `v=1431655765` |
| 3 | `Node* p = c > 0 ? makeBox().t : makeBox().t;` (`?:` join) | rc 0, `v=1431655765` | rc 0, `v=1431655765` |
| 4 | `Node*[2] a = { makeBox().t, nullptr };` (array aggregate initializer) | rc 0, `v=1431655765` | rc 0, `v=1431655765` |
| 5 | `keep(makeBox().t)` where `void keep(Node* n) { g = n; }` | rc 0, `v=1431655765` | rc 0, `v=1431655765` |
| 5b | same with `void keepU(unique Node* n)` | rc 0, `v=1431655765` | rc 0, `v=1431655765` |
| 5c | same with `void keepM(move Node* n)` | rc 0, `v=1431655765` | rc 0, `v=1431655765` |
| 5d | `l.add(makeBox().t)` / `PlainSlot(makeBox().t)` (constructor arg) | rc 0, `v=1431655765` | rc 0, `v=1431655765` |

The parenthesized spelling (`(makeBox().t)`) belonged to this list and was CLOSED in the same
commit that files this - see the paren legs in `Test/errors/err_unique_borrow_into_field.cb`.

## Why each one misses

- **(1) cast.** `castExpression` is a different production from the parenthesized primary; it
  builds its own NamedVariable and does not travel the `lastParenExpr*` side channel the paren
  fix added. Deliberately not closed with the parens: a cast is the spelling a user reaches for
  to say "I mean this", and rejecting it needs its own accept set (there are legal same-type and
  widening casts off borrowed values throughout `core/`). Closing it is the narrowest next step
  and probably wants the same side-channel treatment.
- **(2) and (3) joins.** The phi carries none of the provenance; there is no NamedVariable to
  read by the time the join result reaches a persist site. Note `?:` was already recorded as
  out of scope by the fix's own coverage matrix; `??` was NOT, and is the same shape.
- **(4) array aggregate initializer.** Does NOT go through `EmitOneFieldInit` (that is the
  struct brace path), so the brace-init leg the fix added cannot see it. This is the closest of
  the four to the guarded sites and is the most likely to fall out of a single added call.
- **(5) call argument.** The store happens in the CALLEE, so a call site cannot in general tell
  a storing argument from a read-only one - and the read-only spelling
  (`int rd(Node* n) { return n->v; } rd(makeBox().t)`) must keep working. **Partly closable,
  though: a parameter declared `unique T*` or `move T*` states the ownership claim AT the call
  site**, so those two spellings (5b, 5c) are decidable there and are the honest first cut. A
  plain `Node*` parameter is the genuinely undecidable remainder.

## Fix direction

Do NOT widen a single predicate across all five - the polarity differs per spelling and the
accept sets do not overlap. Take them in this order, each with its own frozen accept set:

1. Array aggregate initializer (4) - a missing call at a store site the fix already models.
2. Cast (1) - side channel, mirroring the parenthesized fix, with a `core/`-wide sweep first.
3. `unique`/`move` parameters (5b, 5c) - decidable at the call site; leave plain `T*` open and
   say so in the message.
4. Joins (2, 3) - needs provenance to survive a phi, which is a real design step, not a call.

Related: [[interface-issue-queue]]
