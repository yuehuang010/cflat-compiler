# `(string)` cast: identity cast refused, global initializer not folded

## Summary

Two gaps in the explicit `(string)` cast, both on master 81fecd7a:

1. Identity cast refused:
   ```cflat
   extern int main() { string s = "cat"; string t = (string)s; printf("%d\n", (int)t.length()); return 0; }
   ```
   Observed: "no overload of 'operator string' matches the given arguments". Expected (C++ `static_cast<T>(t)` of a T):
   a no-op borrow, prints 3. Same for `string(s)`.
2. Global initializer:
   ```cflat
   string g = (string)"abc";
   extern int main() { printf("%d\n", (int)g.length()); return 0; }
   ```
   Observed: "global variable initializer must be a compile-time constant". `string g = "abc";` folds and prints 3;
   the cast of a literal must fold the same way. Same for `string g = string("abc");`.

## Fix direction

1: treat a cast to the operand's own type as identity before the conversion-operator lookup. 2: the global constant
folder should look through a `(string)` / `string(...)` of a literal. Found by the strctor review (box 10, 2026-11-03).
