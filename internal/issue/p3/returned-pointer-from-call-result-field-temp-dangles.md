# p3: Pointer returned from a call can outlive its owning call temporary

Summary: A raw pointer read from an owning temporary's field can escape through a pointer-returning call and be stored beyond the full expression.

Repro:

```cflat
int* echo(int* p) { return p; }
int main() { int* p = echo(mk()->heap); return 0; }
```

Observed: the direct `int* p = mk()->heap` form is rejected by `err_raw_pointer_into_call_temp_field.cb`, but this indirect form is accepted. Master leaves the owner undestroyed; with T480 full-expression cleanup, the destructor runs after the initializer and `p` dangles. The pointer is not dereferenced in the probe. The by-value twin `int* p = echo(mk2().heap);` (and a callee storing the pointer in a global) already dangles the same way on master; clang with unique_ptr gives the same single destructor.

Root cause: the field-pointer ownership provenance is not carried through a pointer-returning call to the destination binding check.

Fix direction: preserve the owning-temp field origin across pointer-returning calls and reject an escaping raw pointer binding or require an alias lifetime.
After T482 the same applies to `use2(mk()->heap + 1)` (use2 stores its argument in a global) and `int* p = id(mk()->heap) + 1`.
