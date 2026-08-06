# The "incomplete layout" message blames C interop for three unrelated causes

Filed 2026-07-30. Raised out of the diagnostic-quality bucket and given its own file because
it has stopped being cosmetic: **two ratification records in [[interface-issue-queue]] (landed design records)
cite a C-interop cause on files containing no C interop**, and establishing that they did not
actually involve C cost the reviewer two extra probes on work that was otherwise settled.

Severity: **misleading diagnostic on a common failure.** No wrong value. It sends a user
looking in entirely the wrong place, and it has already cost review time on this queue twice.

## The message

```
type '...' has an incomplete layout (a field type C interop could not import);
it can only be used through a pointer
```

Exactly ONE emission site: `cflat/LLVMBackend.h:13942`, in `CreateLocalVariable`. Its trigger
is purely STRUCTURAL - an opaque shell reached a by-value local. It says nothing about how the
shell got there.

## Three unrelated causes funnel into it

| Cause | Named by the wording? |
|---|---|
| A genuinely abandoned C-imported record | **Yes** - the only one it describes |
| A shell whose definition comes LATER in the file (use-before-declaration) | No |
| A shell whose definition was abandoned by an error swallowed inside an `expect_error` block | No |

So the wording is right about the MECHANISM (an opaque shell reached a by-value use) and wrong
about the CAUSE in two cases out of three. Use-before-declaration is the common one, and it
fails identically at global scope on both binaries - it is not generics-specific.

### A FOURTH cause was removed 2026-08-06; these four repros are untouched

`fix/generic-shell` (landed record in [[interface-issue-queue]]) gated the forward-ref scan's
opaque shell on the name actually naming a generic template, so an UNDECLARED generic
(`ZZZ<int> z;`) now reports `unknown type 'ZZZ__i32'` instead of funnelling into this message.

The P1 that fixed it claimed to be "the CAUSE of two of the three causes" here and to remove
"most of this P2's reach". Measured against the b18ae7f and post-fix binaries, that claim is
FALSE - it was a fourth, unlisted funnel, and all four repros below are byte-identical on both:

| Repro | b18ae7f and post-fix, identical |
|---|---|
| generic use-before-declaration | `type 'P2Box__i32' has an incomplete layout ...` |
| NON-generic use-before-declaration | `type 'P2S' has an incomplete layout ...` |
| namespace-local generic use-before-declaration | `type 'P2N.P2NBox__i32' has an incomplete layout ...` |
| failed `expect_error` declaration ([[failed-expect-error-type-poisons-its-name]]) | `type 'A.Item' has an incomplete layout ...` |

Severity is unchanged. What DID change for this file's fix direction: one fewer cause to
attribute, and one existing test (`Test/errors/err_if_const_generic_interface_dead_branch.cb`) has
moved off this message, so the `Test coverage` note below is now short by that file.

## Where it has already misled

- The layer-1 **T5** tightening record in [[interface-issue-queue]] (landed design records): a bare generic name
  used before a same-named namespace-local template is declared now reports this message. The
  file imports no C.
- The layer-2 **forward-reference** tightening in the same file, where the non-generic control
  says `incomplete layout` while the subject says `Unknown identifier 'v'` - two different
  sites, and the C-interop wording actively obscured that they fail by different mechanisms.
- [[failed-expect-error-type-poisons-its-name]] is the third cause, reached through a shell
  that never gets a body.

## Fix direction

Cheap and self-contained; it needs no analysis change, only information that is already
available at the emission site.

1. **Stop asserting a cause the site cannot know.** Lead with the mechanism: `type '...' is
   incomplete here (its layout is not available at this point); it can only be used through a
   pointer`.
2. **Attribute when the shell's provenance IS known.** The C-import path can tag its shells at
   creation, so the existing C-interop sentence survives for the one cause it is true of.
3. **When the definition exists LATER in the same file, say so** - that turns the most common
   cause into an actionable message ("declared at line N, below this use").

Check whether the same wording is reused anywhere else before editing; the site count above is
from a grep and should be re-confirmed, since a second site would change part 2.

## Test coverage

None specific. The message text is currently pinned indirectly by
`Test/errors/err_namespaced_generic_iface_bare_single_ns.cb` and the layer-1 T5 witnesses - so
**re-wording it will break existing `expect_error` substrings**. Sweep `Test/errors/` for the
substring and update those legs in the same change, per the working note that an `expect_error`
match is a plain `.find()` (`LLVMBackend.h:1201`).

Related: [[interface-issue-queue]] (landed design records), [[failed-expect-error-type-poisons-its-name]],
[[interface-issue-queue]]
