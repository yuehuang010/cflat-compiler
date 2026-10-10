# `auto` copy of an interface value or a closure variable loses its methods / callability

Found by T62 review 1 (pre-existing, with or without `static`).

## Repro (scratch/repro_keep/t62_rev/rev_t62_if.cb, rev_t62_li.cb)
```cflat
IGet ig = &impl;  auto r = ig;  r.get();   // error: no overload of 'get' (r deduced as Impl*, not IGet)
auto lam = [](int x) -> int { return x + 1; };
auto f = lam;  f(1);                       // error: the function 'f' is not known
```
Expected: `auto` deduces the source's declared type - r is an IGet (dispatches get), f is a copy of
the closure and is callable (closure value-capture ruling 2026-09-30: copying a closure copies its
captures).

## Fix direction
`auto` deduction from an identifier should take the variable's declared type (interface fat
pointer / closure type), not the underlying concrete pointer, and register a callable for closures.
