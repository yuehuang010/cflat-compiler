# Return of a C++ reference-result ternary remains refused

Found 2026-09-23 while fixing the reference-return-at-return issue (macOS arm64, Release). Pre-existing.

## Summary

The return cell `C* choose(bool c) { return c ? ref_a() : ref_b(); }` is refused. The pre-fix
diagnostic was "cannot return this value: its type does not match the return type of function
'choose'". Joining the arm addresses changes the diagnostic to the alias-escape message, but the
return remains refused. This cell is blocked on the `&ref()` return ruling in
`cpp-address-of-reference-result-return-refused.md`; do not classify it in this issue.

## Repro

`scratch/tr_return.cb` (header: two static cells returned by `C&`).

## Root cause (measured)

The `?:` lowering loads each alias arm and joins VALUES, so the join is not an alias value and
`CxxReferenceResultAsPointer` (which requires `IsAliasValue(src.Storage)`) has no address to bind.
For a return sink, the address join is classified as an escaping alias and refused.

## Fix direction

Deferred until the `&ref()` return ruling decides whether the selected reference result may escape.
