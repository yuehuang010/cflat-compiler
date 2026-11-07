# Later struct and interface types in qualified function signatures

A signature using a later qualified struct or interface type fails even if the
namespace itself is already declared:

```cflat
namespace B {}
namespace A { B.T* f(B.T* x) { return x; } }
namespace B { struct T { int x = 0; }; }
extern int main() { return 0; }
```

The scanner reports `cannot find the type 'B.T'`. Replacing the struct with an
interface and `B.T*` with `B.I*` fails the same way. Register type shells before
scanning dependent signatures. A by-value local of a later `B.T` also fails
because its layout is incomplete; aggregate layout ordering needs separate work.

Late namespace functions, globals, enums and enum signatures are order-independent
as of QUALORDER (namespace publication plus pre-registered globals and enums).

Member access through a later namespace global of a later struct type
(`B.p->v`, `B.s.v` inside A, global and struct both declared after the use) fails on the
same layout-order cause; the message is now "Unknown identifier 'v'" (was "Undefined
variable B"). Repros scratch/repro_keep/qualorder/ (main checkout).

A bare file-scope global used before its declaration
(`int f() { return x; } int x = 9;`) is rejected "Undefined variable x": root globals are
not pre-registered (bare functions and, since QUALORDER, namespace globals are).
