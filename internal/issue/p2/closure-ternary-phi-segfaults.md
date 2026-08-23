# Ternary over two closure values segfaults at runtime

## Summary
A ternary whose arms are two closure (`Lambda<T(...)>` / `function<>`) values loaded from struct
fields compiles but the merged value is corrupt: calling it crashes (exit 139). Pre-existing on
master (reproduced on the master binary with no bonded-closure analysis involved), found while
reviewing the bonded-closure field-extraction fix.

## Repro
```
import "list.cb";
struct Holder { Lambda<int()> f = default; };
int main()
{
    list<int> xs = { 1, 2, 3 };
    Holder h = default; Holder k = default;
    h.f = () => xs.count();
    k.f = () => xs.count() + 1;
    Lambda<int()> joined = true ? h.f : k.f;   // local, non-escaping
    return joined();                          // segfault, exit 139
}
```
(Probe kept at scratch/ce_pos_ternary.cb in the closure-extract worktree; re-create under scratch/.)

## Root cause (hypothesis)
The closure is a fat pointer (fn ptr + env); the ternary merge builds a PHI over only one of the
two components, or over a pointer to a destructed temp, so the merged value's env (or fn ptr)
is stale. Inspect the .ll for the `select`/`phi` emitted for the ternary.

## Fix direction
Merge both closure components (or the whole aggregate) through the ternary, the same way the
`?.` chain carries its produced-temp identity across the merge. Add a positive leg to
Test/test_function_ptr.cb (ternary between two closures, both arms exercised) and, if the
closure needs its owner kept alive, a matching dtor-count leg.
