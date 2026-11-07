# Root global member used before its declaration

A function in a namespace cannot access a root-level global declared later in the translation unit:

```cflat
namespace A { int read() { return p->v; } }
struct T { int v = default; };
T* p = nullptr;
extern int main() { return 0; }
```

The compiler reports `Undefined variable p.` The equivalent probe is
`scratch/qualorder/root_global_member.cb`. Root globals remain source ordered; namespace-global
member access through later declarations is covered by the separate QUALORDER2 fix.
