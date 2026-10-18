# unique<Wrap<S2>> field over a later-declared type instantiates the body ("sizeof incomplete")

T327 (2026-10-16) fixed the raw-pointer shapes (`Wrap<S2>*` field / param / local before `struct S2`: pointer-only pending
instances wait for the struct body). Still open, same on master:

```cflat
struct Wrap<T> { T v; int size() { return sizeof(T); } }
struct Holder { unique<Wrap<S2>> u; }
struct S2 { int a = 0; }
```
Refused "sizeof/alignof: type 'S2' is incomplete"; clang accepts a unique_ptr<Wrap<S2>> member. Likely path: the eager
type-argument queue at MainListener_Declarations.cpp ~629 plus core/unique.cb (destructor `delete _p` instantiates the
pointee). Fix direction: unique<T>'s instance (and its pointee) waits for T like a pointer-only record (T327
PendingWaitsOnStructBody). Also eager: pointer uses inside generic template bodies and in function-pointer signatures.
