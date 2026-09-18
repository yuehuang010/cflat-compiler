p3

# Assigning into a live nontrivial C++ object rejects every RHS that is not a call or a name

## Summary

The nontrivial-C++-assignment path in `MainListener_Expressions.cpp` decides the right-hand side
is a temporary only when it is a postfix expression with an argument list (a call). Anything else
that is not a bare identifier is refused, including an operator result and a parenthesized name.

## Repro

```cflat
import cpp "string" cache;
extern int main()
{
    std.string a = std.string("hi");
    std.string b = std.string("ho");
    std.string t = std.string("");
    t = a + b;              // and: t = (a);
    return (int)t.size();
}
```

```text
p_assign.cb(2,110): cannot assign to C++ class 'std.string' from this expression;
the source must be a 'std.string' variable, optionally written 'move <variable>'
```

Measured on master 884a0c08 and on the free-operator-template branch: `t = (a);` fails with the
identical message on both, so the rule is about the RHS SHAPE and is independent of operators.
`t = std.to_string(42);` (a call) succeeds on both, and the DECLARATION form
`std.string t = a + b;` succeeds on the branch - only assignment into a live object is refused.

## Root cause

```cpp
const bool rhsTemporary = rhsPostfix != nullptr
    && !rhsPostfix->argumentExpressionList().empty();
```

Only that arm arms `pendingCxxSretDest_` and assigns from the scratch slot. Broadening the
predicate alone is not enough: measured, an operator result does not land in
`pendingCxxSretDest_` from this path (the declaration path has a separate temporary-then-move
fallback for exactly that reason), so the branch falls through to the same refusal.

## Fix direction

Give the assignment path the declaration path's fallback: when the RHS produced a value of the
destination's type with its own storage, MOVE-assign from that storage rather than byte-copying
it into a second slot - a byte copy plus a second registered owned temp would double-free.

## Acceptance

`t = a + b` into a live nontrivial C++ object runs the class's assignment operator exactly once,
destroys the old value exactly once, and leaves the operator's temporary destroyed exactly once
(instrumented counts, as `Test/test_cpp_interop.cb`'s lifetime section does). Trivially-copyable
classes keep their current generic path - `cppfop.Box<int>` assignment already works.
