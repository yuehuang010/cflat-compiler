# `&v` on an `S[]` view local yields the slot address typed as an element pointer

Pre-existing on master; found by the T387 Opus review round 5 (2026-10-21).

## Repro

`scratch/repro_keep/box1021/t387_rev5/a10_slot_alias_noloop.cb`:

```cflat
class S { void* q = nullptr; int id = 0; ~S() {} };
void sink(move S[] p) {}
int main(int argc, char** argv) { S[] owner = new S[3]; owner[0].id = 99; S[] w = owner; S[] v = new S[1];
    auto p = &v; p->q = (void*)&w[0]; if (v[0].id != 99) return 9; sink(move v); return 0; }
```

Master and T387: compile rc 0; ASan reports a double free. `auto p = &v` has type `S*` but holds the address of the view SLOT
(not `&v[0]`), so `p->q = ...` overwrites the view pointer itself and `v` now points into the borrowed `owner` block.

## Notes

`S[]* p = &v` is refused ("pointer to array-view 'S[]*' is not a valid type"), so the explicit spelling is closed; the `auto` deduction
and `(void*)&v` / C-function forms are not. T387's loop rebind scan relies on a view local having no second name.

## Fix direction

Decide what `&v` means on a view: either the element address (`&v[0]`, matching how `T[]` decays to `T*`) or a refusal like the
explicit `S[]*` spelling. Either way `auto p = &v` must not produce an `S*` that points at the slot.
