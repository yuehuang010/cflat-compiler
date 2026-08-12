# q09: Return-dangle and escape analysis

1 active item remains, and it is now ACTIONABLE - its q02 prerequisite landed in `b911ccc`.
Two always-wrong escapes are fixed below. A value that points into a dying frame escapes - by return, or by a store into storage
that outlives the frame - with no diagnostic.

> **Corrected 2026-08-11.** An earlier draft of this file proposed inverting the analysis to
> fail-closed ("unknown rejects"). That is WRONG and contradicts a ratified ruling. See the
> governing rule below before proposing anything in this area.

## Governing rule (ratified, do not re-litigate)

**Unknown ACCEPTS.** A false rejection is a blocker; a missed dangle is today's behaviour. This
polarity was arrived at after THREE abandoned attempts that all rejected legal programs, and it is
the same rule that governs q06. Any proposal that widens an unknown-shape case into a rejection is
re-running an experiment the repo has already run and recorded.

What IS allowed under the rule: rejecting a shape that is ALWAYS wrong - one where no correct
program has that spelling. That rejects nothing legal and does not touch the polarity.

## Members - one item, now actionable

- `p1/return-dangle-missed-when-slot-has-extra-user` - **not fixable by WIDENING the IR walk.** The check rejects
  only when every user of the returned local's slot is recognized; any unrecognized user (a method
  dispatch through the slot, a call argument, a GEP with a non-load user, a memcpy, a null store)
  is accept evidence and stops the walk. So `measure(r); return r;` is caught and
  `printf("%d\n", r.area()); return r;` is not - an IR-shape accident invisible in the source.
  The durable fix is front-end provenance: record at the BINDING site that an interface local was
  initialized from frame storage, carried on the `NamedVariable` rather than recovered from
  finished IR. **NO LONGER BLOCKED (corrected 2026-08-11):** the prerequisite
  `p3/interface-boxing-keyed-on-source-binding` LANDED in `b911ccc` (q02) and its file is deleted.
  This item was skipped on a stale blocker and is now actionable. Note a source-level "tainted binding"
  property was already rejected for the original issue because a missed assignment site produces a
  false rejection - the provenance must observe every assignment site.

- The two p2 members below are fixed. Their checks reject only the always-wrong escape shapes and
  preserve the accepted legs (non-frame alias strings, by-value captures, and static captures):
  - `p2/alias-string-return-of-frame-local-element-dangles` - reject an `alias string` return
    that borrows a local, including an element, field, or whole local.
  - `p2/lambda-ref-capture-into-program-lifetime-storage-dangles` - reject storing a lambda with
    a by-reference capture of a non-static local into a global or static local; by-value captures
    remain accepted.

## Fix direction

q02 HAS landed, so `p1/return-dangle-missed-when-slot-has-extra-user` is AUTHORIZED: build the
front-end provenance (frame-storage origin recorded on the `NamedVariable` at the binding site)
rather than widening the IR-shape whitelist. The polarity rule is unchanged - unknown ACCEPTS, and
the provenance must observe EVERY assignment site or it reintroduces the false rejection that
killed the source-level "tainted binding" proposal.

## Adjacent

q02 (`p3/interface-boxing-keyed-on-source-binding` is the prerequisite for the p1 item), q06 (same
governing rule), q11 (`static`-local storage duration).
