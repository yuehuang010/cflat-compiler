# `(const E)[]` view variable cannot be reassigned

Filed 2026-10-19 from the c16 report; verified on master 076a9dd31 (scratch/cview/reseat.cb, gitignored).

```cflat
int[2] a = {1, 2}; int[2] b = {3, 4};
(const int)[] cv = a;
cv = b;            // error: cannot assign to 'cv': it is declared const
(const int)* cp = &a[0];
cp = &b[0];        // accepted
```
R11 whole-word rule: the parenthesized form constrains only the element, so `(const int)[]` is a reseatable view
like `(const int)*`. Same over-refusal for a `(const int)[]` struct field ("field 'v' is const and can only be set
during construction"). Cause per c16: a view's ConstMask bit 0 is read as both the element and the variable itself
(IsConstObject / ApplyWholeWordConst depth for IsArrayView). Fix direction: separate the view's own level from its
element level the way the pointer path does; `const int[] v` (whole word) stays fully const. Acceptance: the repro
compiles and returns 7; `cv[0] = 1` still refused; `const int[] w = a; w = b;` still refused.
