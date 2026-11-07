# C++ assignment result as a value: by-design B9 rulings

## Summary

The B9 array-element result case (`C b = (arr[1] = 10)`) is fixed: the selected by-value result is built in place in `b`, so copy=1, move=0, expression dtor=0, end dtor=1, as clang. Only the out-of-scope rulings below remain; they are by design or need a ruling, not active defects.

## Prior out-of-scope rulings

- B9 `a = (mkR(4)).get()`, discarded `-=`, move-only discarded `+=`, by-value parameter elision, brace-list element order, global destination, trivial by-value `operator=(int)`, native designated field initialization, and a CFlat lambda returning a local.
- B18 ternary and parenthesized-call rows from the former entry were measured as matching clang and are no longer active here.
