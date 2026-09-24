# Members refused during the suppressed header harvest carry no clang cause; stored cause is never shown

Found 2026-09-23 by the round-3 review of fix/cpp-container-sink-gaps (macOS arm64, Release).
That branch stores clang's diagnostic text for a failed instantiation as the refusal "cause"
(header cache v91, `CxxDiagnosticBlamesCopyOf` in LLVMBackend_Lookup.cpp) and keys the
deleted-copy blame on it. Gaps left open:

## M1: harvest-time refusals have no cause

Diagnostics are suppressed while clang harvests the header (`CxxIncrementalGroup.cpp` ~172-194),
so members refused there store no cause and real copy sinks lose the precise message:

- `ConstOnly<Key>.add(l)` (add copies): now "member 'add' ... cannot be instantiated ... (clang
  reported an error inside the body it generated)". Round 2 of the branch said "cannot copy C++
  class 'cplv.Key' into parameter 't' of 'add'". Pre-branch crashed (139).
- `std.set<Key>.insert(l)`: prints "parameter '__v' of 'insert' takes ownership of the value;
  pass 'move <arg>' or a temporary value". True (`insert(move l)` binds) but does not say why
  the lvalue failed.

Engine limitation, confirmed: turning suppression off sets clang's error flag, the interpreter
drops the module and module generation dereferences null - the same mechanism as the set/map
crash the branch fixed. Direction: capture the harvest diagnostics into the cause, then reset
clang's error state before code generation.

## L1: the stored cause is never printed

It often holds the useful line: `vector.assign(n, l)` -> clang's "deleted operator '='";
`map.insert(pair lvalue)` -> `is_constructible<pair<const Key,int>, pair<Key,int>&>`
requirement. Printing its first error line after the CFlat refusal would restore the clarity
lost for the assign / map-insert legs of Test/errors/err_cpp_struct_lvalue_sink.cb.

## L2: per-function cause text has no size cap

Only the overall text is capped at 64 KB. Bridge fixture measurement: 235 entries, 219 KB of
34.8 MB cache JSON (0.63%), largest entry 5.8 KB / 26 lines. Add the same cap per entry.

## L3: class-name matching cannot tell which object was copied

`Reg<nb::Key>::other(u)` copies a member of type `nb::Key` but the blame names parameter 'u'.
Right class, wrong parameter, no wrong fix suggested. Needs clang's source location of the copy,
not the class name.

## Acceptance

- M1 repros print a cause-carrying message; L1 prints clang's first error line; L2 cap in the
  serializer with a history line; L3 optional.
- Existing sink err tests keep passing cold and warm with `--error-on-cpp-reparse`.
