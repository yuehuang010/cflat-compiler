# p2: a `?:` arm naming a GENERIC function binds a null function value silently

Found by the T373 review (2026-10-21). Reproduces on master after T375 (b0047db06).

## Repro (scratch/repro_keep/box1021/t373_rev/v09_ternary.cb)
```cflat
int gen<T>(T* p) { return *p + 100; }
int plain(int* p) { return 7; }
extern int main(int argc, char** argv)
{ function<int(int*)> f = argc > 0 ? gen : plain; if (f == nullptr) return 240; int v = 1; return f(&v); }
```
Compiles rc 0 and returns 240: `f` is null. Calling it traps. clang++ -std=c++20 rejects the analogue (the conditional
operand `gen` is an unresolved overload set with no target type). A ternary of two CONCRETE functions (`plain : plain2`) binds
correctly (7). T373 (in flight) adds deduction for a bare generic name at a function-type destination, but the ternary arms do
not receive the destination signature, so the generic arm resolves to an empty value.

## Fix direction
Either pass the destination function type into both `?:` arms so a generic arm deduces like a bare name (CFlat-friendly), or
refuse a generic function name as a `?:` arm with a clear error (clang behavior). Never bind null silently. Needs a ruling only if
the deduce option is preferred over clang's refusal; the refusal is safe to land without one.

## More shapes (T380 review r2, 2026-10-21, all reproduce on master)

- `function<int(int)> f = c ? id : idc; return f(5);` (generic + concrete arm) compiles, run-time crash rc 133.
- `f = c ? idc : id;` (assignment, function<> LHS) crashes the COMPILER (rc 139).
- `function<int(int)>[2] fs = { id, idc };` run-time crash rc 139; the view form `function<int(int)>[] fs = { id, idc };` rc 133.
  Concrete elements, or a function<> variable as the element, are correct. The element path needs the same generic-name deduction
  against the element's function type (T373) that declarations get.
