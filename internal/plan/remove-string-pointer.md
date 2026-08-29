# Remove `string*` from the user-facing language (breaking change - scope first)

Ruling 2026-08-28 (maintainer): `string*` should not exist. `string` is already a
reference-like handle (a pointer under the hood), so a pointer to it is redundant. This is a
breaking change and must be SCOPED before any implementation. This plan replaces
`internal/issue/p2/string-pointer-param-slot-semantics-depend-on-argument-provenance.md`
(deleted; its records are folded in below).

## Why removal also closes the filed bugs

The deleted issue's two misbehaviours exist only because a `string*` parameter slot has two
caller-dependent meanings the callee cannot see:

1. `void fill(string* h, string s) { h[0] = s; }` with `string* h = new string[2]` - the slot
   must BORROW; the deep copy the parameter path performs is orphaned (+16 leaked bytes
   measured, nothing frees elements past 0 since `delete[_]` frees only the buffer).
2. The same `fill(arr, a)` with `string[4] arr` decayed - the slot must DEEP-COPY into the
   caller's LIVE owners; this half is correct today and must stay correct.
3. Read side, same provenance gap: `string* p = arr; string q = p[0];` loses the deep copy
   the direct `arr[0]` spelling gets (`indep=0`, rc 134 under `--run`) -
   `IsOwningArrayStringElementRead` admits only the two-index fixed-array GEP.

With no `string*`, each caller spells intent: `string[]` view = caller's live storage;
owning heap arrays of string get a container or `string[]`-producing allocation. The
ambiguity has no slot to live in.

## Lessons that must survive (from the deleted issue)

- Round 3 of `fix/rawheap` tried a NEGATIVE provenance gate (admit unless known fixed-array)
  and produced use-after-free at every unenumerated binding (joins, re-assignments). The gate
  polarity is ratified POSITIVE / unknown-accepts. Any interim guard work keeps that polarity.
- The plan-level shape the q06 bucket ratified still applies to whatever remains: one "root
  provenance" walk (owned / borrowed / unknown) instead of per-flag propagation; serialize
  with `internal/issue/p3/consolidate-named-variable-borrow-provenance.md`.

## Scoping inventory (measured 2026-08-28)

`grep 'string\*|string \*'` over `.cb` corpus:

- `cflat/core`: 2 hits, both `bond string* this` method receivers in `core/string.cb`
  (`view`, `span`). Receiver `this` slots are compiler-managed - decide whether the ban is
  user-surface-only (receivers exempt) or total (receivers respelled).
- `Test`: 28 hits (test_move bucket legs, rawheap legs, etc.) - these are the accept-set to
  re-spell or retire with the feature.
- `example`: 0 hits.

## Questions the scoping pass must answer

- Exact ban surface: declarations (`string* p`), parameters, fields, `new string[n]`
  (currently produces `string*`), `&stringVar`, generic instantiations (`list<string*>`),
  C interop boundaries (`char*` untouched).
- Replacement for each banned spelling: `string[]` view, containers (`list<string>`), or a
  by-ref parameter mechanism if one exists by then.
- What `new string[n]` returns after the change.
- Diagnostic wording and migration story (error message must name the replacement).
- Whether `string_view*` / other handle-type pointers follow the same rule (same redundancy
  argument) or stay.
- Interaction with `bond`/`this` receiver spelling in core.

## Sequencing

After `consolidate-named-variable-borrow-provenance` (same functions, same guards), and with
its own fix-issue round once the scope above is ratified by the maintainer.
