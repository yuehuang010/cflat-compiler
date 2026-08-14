# Centralize namespace-aware registry lookup and registration

Filed 2026-08-13 during the integrated Q01-Q15 review.

## Summary

The Q12 fixes introduced `ScopedNameCandidates`, which is the right common primitive, but each
registry still implements its own lookup loop and resolution policy. Type aliases, closure aliases,
mangling aliases, generic-base aliases, interfaces, data structures, and function-pointer struct
candidates can therefore drift on shadowing, root qualification, alias chaining, and tail-name
matching.

One conspicuous remaining exception is `FuncPtrStructCandidates`: it scans every data-structure key
and admits any dotted key whose tail matches the spelling. Other named-type lookup walks the active
namespace outward. The function-pointer path then needs recorded resolved keys to narrow ambiguities
created by its own broader search.

## Simplification direction

Create typed helpers for the two repeated operations:

- register under the current scoped key;
- find the first visible entry by outward namespace walk, with an explicit `forceRoot` option.

Keep broad searches as separately named operations used only when ambiguity is intentional. In
particular, make function-pointer component resolution ask the normal scoped type resolver first;
use an all-tail candidate scan only as a conservative fallback for legacy or incomplete metadata.

Acceptance criteria:

- Shadowing rules are shared by aliases, interfaces, structs, and closure aliases.
- A caller cannot accidentally implement a fresh tail-name search with an ad hoc loop.
- Existing Q12 namespace-collision tests and Q15 function-pointer tests remain unchanged and pass.
- Add namespace-shadowing coverage to an existing related test file.

