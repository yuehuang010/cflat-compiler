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
