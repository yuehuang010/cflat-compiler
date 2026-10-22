# p3: adjacent nested-template and generic-alias gaps

## Adjacent (same review, pre-existing, lower value)

- A nested template that points to itself (`struct Node<T> { Node<T>* next; }` inside Outer) is refused with "cannot find the type".
- Diagnostics spell a type nested in a generic outer as `Outer<int.In>` (should be `Outer<int>.In`).
- `using II = Inner<short>;` inside Outer is refused with "not a generic type" (clang accepts).
- `sizeof(Outer.Mid)` inside a namespace and bare-name `Inner<int>()` construction from `Outer.Mid` still fail (master too; T384).
- `Inner<int>()` from a sibling method when Outer.Inner<T> is declared AFTER Mid is refused "incomplete here" (master: unknown type; clang 4; T384).
