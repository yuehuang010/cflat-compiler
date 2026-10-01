# C++ specialization imported under a different define keeps the primary layout silently

Found 2026-09-30 by the W5 review (pre-existing on master; the review probe def.cb was not kept -
rebuild it from the repro below).

## Repro
Primary template `S<T>` in `a.h`; explicit specialization `S<long>` in `b.h`, imported with a
different define (`import cpp "b.h" define "FOO";`). CFlat code using `S<long>` gets the primary
layout (correct per the "same-define groups only" rule), but functions declared in `b.h` were
compiled against the specialized layout - an ABI mismatch with no diagnostic.

## Expected
A diagnostic at the use site when a template instantiation's layout differs between import groups
that the program links together, or a documented rule that prevents the mismatch.

## Direction
Needs a ruling: diagnose (cheap: compare the specialization set per group at request time) vs
merge define groups for specializations.
