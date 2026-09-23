Bucket: p2 (wrong binding: silent infinite recursion)

# `b.get()` inside a CFlat free function named `get` resolves to the free function itself

Found 2026-09-23 (fix/cpp-reference-return-at-return) while building the fixture. Inside a
CFlat free function `int get(cppt.Box<int> b) { return b.get(); }` the member call `b.get()`
binds to the enclosing free function `get` (recursing until stack overflow) instead of the
C++ member `Box<int>::get`. Renaming the free function makes the member call bind correctly.
Not measured for CFlat struct methods of the same name; check both.

Fix direction: a postfix member call `expr.name(...)` on a C++ class (or any struct) receiver
must look up `name` in the receiver's member set before any enclosing-scope free-function
lookup; the free-function fallback is only for free operators / ADL-style C++ free functions
taking the receiver as first argument, and must never pick the function currently being
compiled by name alone. Acceptance: the shape above returns the member's value; an err test is
not needed.
