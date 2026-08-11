# q04: Diagnostics wording and attribution

10 items. The compiler detects the problem correctly but tells the user the wrong thing: wrong
blame, mangled internal names, or a message from a guard the user did not trip.

## Shared root cause

Two recurring shapes:

- **Wrong blame.** A single emission site serves several causes and hardcodes one of them, or a
  replay picks the first failing candidate rather than the intended one.
- **Internal names leak.** Error formatting runs AFTER mangling and reads internal fields
  (mangled type name, companion basename) instead of the spelled source name and canonical path.

## Members

- `p2/incomplete-layout-message-blames-c-interop` - shell can also come from a forward reference
  or a swallowed error.
- `p2/overload-replay-blames-wrong-candidate` - replay picks the first name-failing candidate.
- `p3/generic-function-call-diagnostics-are-misleading` - error path runs post-mangling, reports
  phantom candidates.
- `p3/mangled-generic-name-leaks-into-diagnostics`
- `p3/as-cast-unbound-pointer-shape-generic-message` - storage-keyed lookup finds nothing for a
  GEP-derived pointer source.
- `p3/unique-on-closure-arg-message-denies-the-pointer` - generic "requires pointer" instead of
  the declarator's accurate message.
- `p3/simd-array-error-wording-differs-from-plain-arrays`
- `p3/interface-collision-message-prefix-still-basename` - formatter reads the basename field, not
  the canonical path field.
- `p3/failed-expect-error-type-poisons-its-name` - forward-ref scanner registers a struct shell
  inside an `expect_error` block and never unregisters on the swallowed error.
- `ui/warn-program-run-differing-only-by-move` - a legal but never-starting `run()` overload needs
  a diagnostic that does not exist.

## Fix direction

Batchable, zero semantic risk, and a good single delegated pass at the sonnet tier. Two mechanical
sub-tasks:

1. Route every diagnostic through a formatter that takes the SPELLED source name and canonical
   path; add the spelled name to the carrier struct where it is missing.
2. Give each multi-cause emission site the actual cause as a parameter instead of a hardcoded
   string.

`p3/failed-expect-error-type-poisons-its-name` and `ui/warn-program-run-differing-only-by-move` are
real behaviour changes, not wording - do those two separately from the batch.

Note the `--init` serializer rule: any new field added to `TypeAndValue` / `StructData` to carry a
spelled name MUST be added to the `LLVMBackend.cpp` cache round-trip in the same change, or the
`Test/errors/` suite goes vacuous on a warm cache.
