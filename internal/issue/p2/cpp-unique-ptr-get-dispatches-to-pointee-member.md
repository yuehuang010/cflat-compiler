# `p.get()` on std::unique_ptr<M> calls M::get through the pointer when M has a get() member

Found by the ST8 review (2026-10-01), pre-existing on master 1cfe8a3b. Repro kept in
scratch/repro_keep/st8_preexisting/unique_null_collision.cb (rv.h: struct M has `int get()`).

```cflat
import cpp {"rv.h", "memory"};
extern int main() { std.unique_ptr<rv.M> a = default; return a.get() == nullptr ? 0 : 1; }
```

Observed: compiles, the program crashes (139): `a.get()` resolves to `M::get` through the smart
pointer forwarding (operator-> / `.` forwarding) and dereferences null.
Expected (clang): `a.get()` is unique_ptr::get(), a member of the receiver's own class, which always
wins over forwarding to the pointee; returns nullptr. Per the operator-dot ruling, `.` forwards
through operator-> only when the receiver class has no such member.
