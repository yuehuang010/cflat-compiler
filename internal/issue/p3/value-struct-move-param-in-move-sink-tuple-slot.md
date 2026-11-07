# Value-struct `move` parameter in a move-sink tuple slot is destroyed twice

## Summary

A declared `move` parameter of an owning value-struct type, read into a tuple slot whose callee takes the tuple by
`move`, is destroyed twice: once by the callee's sink and once by the caller's parameter cleanup. The scalar sink and
the explicit `move h` slot are correct.

## Minimal repro

```cflat
import "unique.cb";
int dt = 0;
struct D { int v = 0; ~D() { dt = dt + 1; } };
struct HU { unique<D> u = default; };
int kmv(move (HU, int) t) { return t.item_1; }
int f(move HU h) { return kmv((h, 1)) + dt; }
int main() { int a = 0; { HU h = default; h.u = new D(); a = f(move h); } return a * 10 + dt; }
```

Observed (master c98047df, compiled with -o): exit 133 (double free). Expected: 21, the same as the scalar
`int kmvs(move HU x)` / `kmvs(h)` and the explicit `kmv((move h, 1))`.

## Fix direction

The tuple-safety landing (box 10) gives a `move` param in a slot one ownership rule for unique<T> (transferred once);
apply the same move-sink transfer (clear the source) for value structs with destructors. Found by the tupsafe
round-9 review (box 10, 2026-11-03).
