# p3: pointer-subscript reads of owning elements still adopt outside R21's string rule

Filed at R21 landing (2026-10-27). Both reproduce on master and on the R21 fix (exit 133, double free).

1. User generic over string: `struct Box<T> { T* gq = default; int read() { T t = gq[0]; ... } }` instantiated as
   `Box<string>`. R21 keeps the old bit-move for implicit-this field reads inside generic instances with T = string, to
   protect core container `_data[i]` / `_buf[slot]` reads (forcing it off: list<string> sort 15 allocs / 9 frees). The
   compiler records no "field type came from a template parameter" fact, so the two cannot be told apart yet.
2. Global `S*` subscript where S is a struct owning a string: `S t = g[0];` adopts and double-frees. R21's predicate
   (IsDeclaredPointerSubscriptString) is string-only.

Fix direction: generalize the R21 copy rule to every owning element type (copy on an unproven slot); give core
containers an explicit move-out spelling (or a provenance fact) instead of the generic-instance exemption.
