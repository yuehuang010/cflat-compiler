# A pointer to a generic struct instance over a later-declared type instantiates the body ("sizeof incomplete")

Found by the T301 Opus review (2026-10-16), pre-existing on master (with or without a unique field).

```cflat
struct Wrap<T> { T v; int size() { return sizeof(T); } }
struct Holder { Wrap<S2>* w = nullptr; }
struct S2 { int a = 0; }
extern int main() { Holder h = default; return 0; }
```
Refused "sizeof/alignof: type 'S2' is incomplete". clang accepts the C++ twin: a pointer to `Wrap<S2>` does not
instantiate the class body or its member functions until used. Fix direction: a pointer field to a generic instance should
register the instance shell only; instantiate fields/methods on first by-value use or member access (or defer method
bodies to the end of the file).
