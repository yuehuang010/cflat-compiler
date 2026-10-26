# p3: parenthesized generic call still fails

`(id)(3)` is refused; clang calls `id<int>(3)` by deducing from the argument.
Root cause hint for whoever picks it up: `(id)(3)` parses as a cast-expression `(typeName) castExpr`; the cast route reports "cannot find the type 'id'" instead of falling back to a parenthesized-primary call when the name is a function (T386 stopped after 3 attempts).
