# `std.vector<std.shared_ptr<user.C>>` in a file-scope FUNCTION SIGNATURE still fails to resolve

Found 2026-09-17 while fixing internal/issue/p2/cpp-nested-std-specialization-over-a-user-class-cannot-be-named.md (fix/cpp-nested-std-over-user-class, macOS arm64, Release). Pre-existing on master (failed before that fix too, with a different message).

## Summary

After the owner-group union fix, `std.vector<std.shared_ptr<nest.Cell>> v = default;` as a LOCAL resolves and runs, and `std.vector<std.vector<...>>` and bare `std.shared_ptr<nest.Cell>` signatures work. But the same doubly nested spelling used as a parameter or return type of a file-scope CFlat function (the fix worktree's scratch/leg_L9d.cb and leg_L9f.cb) still fails: `-v` shows the OUTER specialization resolving in a <vector>+user-header group (libc++ reaches shared_ptr transitively there), and the INNER request then inherits that <memory>-less group and fails one level down - the same defect shape as the fixed one, one level deeper, reached through the ForwardRefScanner signature pass rather than the declaration path.

## Fix direction

Request the inner components before the outer at the ForwardRefScanner entry (signature pre-pass), so the inner owner group is known when the outer is unioned; unioning cxxTemplateOwnerGroup_ per nested base was tried in the fix run and changed nothing measurable.

Suggested bucket: p2.

## Related pre-existing gaps (review of fix/cpp-nested-std-over-user-class, 2026-09-17)

- The refusal wording for this case is misleading: it names the type as unresolvable while the same spelling binds fine as a local.
- `std.vector<std.vector<nest.Cell*>>` (a POINTER to a user class at inner depth) is refused before and after the fix.
- The nested-argument diagnostic is not catchable by `expect_error` in either form; only the depth-1 wording is pinned by Test/errors/err_cpp_nested_missing.cb.
