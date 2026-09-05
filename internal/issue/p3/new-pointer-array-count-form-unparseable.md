# `new int*[2]` does not parse

Bucket: grammar; needs a look at CFlat.g4 (no parser predicates allowed). Filed 2026-09-04
during the q10 merge verification, master 073f5948.

## Summary

`int*[] pv = new int*[2];` fails with `unexpected '[' here; expected {... 'new' ...}` at the
`[`: the `new T[n]` form does not accept a pointer element type. The workaround is a fixed
array `int*[2] cells; int*[] pv = cells;`. `new int[2]` and `new Foo*` parse. Note the raw
heap-array area is ON HOLD (2026-09-04 ruling); this is only the parse of the spelling, but
check with the maintainer before touching `new` grammar.
