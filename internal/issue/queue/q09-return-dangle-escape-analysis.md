# q09: Return-dangle and escape analysis

3 items. A value that points into a dying frame escapes - by return, or by a store into storage
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

## Members - three different decisions, not one

- `p1/return-dangle-missed-when-slot-has-extra-user` - **NOT fixable by policy.** The check rejects
  only when every user of the returned local's slot is recognized; any unrecognized user (a method
  dispatch through the slot, a call argument, a GEP with a non-load user, a memcpy, a null store)
  is accept evidence and stops the walk. So `measure(r); return r;` is caught and
  `printf("%d\n", r.area()); return r;` is not - an IR-shape accident invisible in the source.
  The durable fix is front-end provenance: record at the BINDING site that an interface local was
  initialized from frame storage, carried on the `NamedVariable` rather than recovered from
  finished IR. BLOCKED on `p3/interface-boxing-keyed-on-source-binding` (filed in q02), which is
  the prerequisite for observing every assignment site. Note a source-level "tainted binding"
  property was already rejected for the original issue because a missed assignment site produces a
  false rejection. Leave filed; do not patch.

- `p2/alias-string-return-of-frame-local-element-dangles` - **always-wrong shape, safe to reject.**
  `alias string f() { string[2] dst; ...; return dst[0]; }` is an unconditional dangle (rc 133).
  An `alias` return is a borrow by design, but here the borrowed storage is the function's own
  frame. Reject in the `alias` return path when the returned string borrows storage that
  `PointsIntoStackFrame`. Check the field spelling (`return b.s;`) and the whole-local spelling in
  the same pass.

- `p2/lambda-ref-capture-into-program-lifetime-storage-dangles` - **targeted reject, kind-aware.**
  The bond/borrow checker validates capture lifetime against the CAPTURING scope, not the storage
  duration of what the closure is finally stored into. Reject at the store into program-lifetime
  storage (file-scope global, or a `NamedVariable` with `IsStaticLocal`) when the closure env holds
  a by-reference capture of a non-static local. `NamedVariable::LambdaCaptureNames` already has the
  capture list; what is missing is the by-reference-vs-by-value distinction surviving to the store
  site. A by-VALUE capture is the common spelling and MUST stay accepted - the check has to be
  capture-kind-aware, not capture-count-aware.

## Fix direction

Do the two always-wrong rejections (`alias`-string return, ref-capture-into-global) as one change;
they share the "reject at the escape site, not at the source" shape and neither touches the accept
polarity. Per the fix-issue rule, build and freeze the ACCEPT-SET as value legs BEFORE writing
either guard - by-value captures, `alias` returns of non-frame storage.

Leave `p1/return-dangle-missed-when-slot-has-extra-user` filed until q02 lands.

## Adjacent

q02 (`p3/interface-boxing-keyed-on-source-binding` is the prerequisite for the p1 item), q06 (same
governing rule), q11 (`static`-local storage duration).
