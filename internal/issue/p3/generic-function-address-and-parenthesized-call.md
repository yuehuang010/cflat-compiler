# p3: generic function address and parenthesized-call forms still fail

`r2.f1(&id)` (address of a generic function into a C++ callback parameter) is refused on master, although clang binds it. After T373 the refusal text says "without a function-pointer destination", which is wrong because the destination is present. `(id)(3)` is also refused.

## Misleading texts (T380 review, 2026-10-21, master too)

- `g(id)` with a generic `g<U>(U f)`: "the function 'g' is not known" (clang: cannot deduce).
- `sizeof(id)`: "cannot find the type 'id'" - should say a generic function name has no size / is not a type.
