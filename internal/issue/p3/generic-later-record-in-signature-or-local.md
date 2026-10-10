# Generic body: a record declared later is unusable in a signature or as a by-value local

Inside a generic template, `Payload f()` written before `struct Payload` fails "cannot find the type 'Payload'",
and a by-value body local of a later record fails "... incomplete here". Field / alias positions DO accept a later
record (T37 r3: provisional alias scan + RefreshProvisionalAggregateAliases). The member-type spelling and the
plain spelling report the same error. Found by T37 r3 (2026-10-05); fails on master identically.

## Fix direction

Let the scanner's provisional pass for generic templates (the T37 r3 provisionalAliasNs_ mechanism) also cover
signature components (ResolveSigComponentScanner) and defer by-value local completeness until every shell is
registered.
