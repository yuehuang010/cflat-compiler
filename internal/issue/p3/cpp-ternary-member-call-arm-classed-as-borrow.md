Bucket: p3 (usable-surface gap, no wrong values)

# A `?:` mixing an operator temporary with a by-value member-call arm is refused as a borrow

Found 2026-09-23 (fix/cpp-assign-temporary) while probing assignment into a live nontrivial C++
object. Two neighbouring shapes stay refused, both measured before and after that fix:

1. `t = c ? a + b : a.twice();` (operator temporary vs BY-VALUE member call returning the same
   class) -> refused as mixed ownership because the member-call arm is classed as a borrow. Two
   member-call arms work, two operator arms work. Site: the ternary arm ownership classification,
   not the assignment path.
2. A C++ FIELD brace-initialised from a `?:` of two temporaries
   (`CppAssignHost h = { c ? cppas.Life(1) : cppas.Life(2) };`) -> "cannot initialize C++ class".
   Site: EmitForeignCxxValueIntoSlot.

`t = c ? a + b : b` (temporary vs variable) is refused as mixed ownership on purpose (2026-09-03
ruling); not part of this issue.

Repro base: Test/library/cpp_interop_assign.h (`cppas.Life`, counted), section 3320-3339 of
Test/test_cpp_interop.cb. Acceptance: both shapes compile, the selected temporary is
move-assigned / constructed in place and destroyed exactly once, counts asserted.
