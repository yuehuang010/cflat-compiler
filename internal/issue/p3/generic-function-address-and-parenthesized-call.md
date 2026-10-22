# p3: parenthesized generic call still fails

`(id)(3)` is refused; clang calls `id<int>(3)` by deducing from the argument.
Root cause hint for whoever picks it up: `(id)(3)` parses as a cast-expression `(typeName) castExpr`; the cast route reports "cannot find the type 'id'" instead of falling back to a parenthesized-primary call when the name is a function (T386 stopped after 3 attempts).

Misleading text (T386 review, master too): `g(&id)`, `c ? &id : &id` and `void* p = &id` with a generic `id` are refused
(correct - clang refuses) but say "Unable to get an Address-of an object without a Storage."; should name the generic-function problem.
