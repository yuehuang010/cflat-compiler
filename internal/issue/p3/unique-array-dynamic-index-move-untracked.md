# P3: dynamic-index moves of array fields are untracked

A move through a runtime index such as `move boxes[i].item` does not identify one constant move path. Keep its behavior identical to master: add no `[*]` family and do not refuse later reads based on that move.

## Repro

```cflat
struct P { int value = 17; };
struct Box { unique P* item = nullptr; };
void consume(move P* p) { delete p; }

extern int main()
{
    Box[2] boxes = default;
    boxes[0].item = new P();
    int i = 0;
    consume(boxes[i].item);
    return boxes[0].item->value;
}
```

This compiles with master. If `i` selected the moved element, the later constant-index read can dereference null at runtime. That is the existing behavior and remains intentionally unfixed.

## Root cause

The compiler does not retain a stable runtime index identity for a move. Treating the index as a wildcard aliases distinct elements and rejects valid drain/rehash loops that move one different element on each iteration.

## Fix direction

Keep exact paths for compile-time constant indices. Do not track dynamic-index field moves or use them to reject later reads. Do not add a runtime check or wildcard family here.

- Pre-existing (T281 round 3): a GLOBAL array is untracked: `Box[2] a` global, `consume(a[0].p)`, then `a[0].p->v` runs 139 (scratch/rev_t281_sol_tmp/global.cb; rc 139 on master and fix).
- Pre-existing (T281 round 3): a LOCAL const index is not folded: `const int K=1`, `consume(a[K].p)`, then the read `a[1].p->v` runs 139 (scratch/rev_t281_sol_tmp/const_index.cb; rc 139 on master and fix).
- Pre-existing (T281 round 4): a moved slot read as a CALL ARGUMENT is not checked: `read(a[0].p) == 0` (scratch/rev_t281_sol_tmp/r2b_call_deref.cb) and the field twin `read(s.p) == 0` (scratch/rev_t281_sol_tmp/r2d_field_call.cb) run 139 on master and fix.
