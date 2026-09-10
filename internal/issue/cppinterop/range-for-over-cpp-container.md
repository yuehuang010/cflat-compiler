# `for (T x in c)` does not work over a C++ container

Found 2026-09-09 by the std header coverage spike ([`std-header-coverage-spike.md`](std-header-coverage-spike.md), gap 6).

## Repro

```cflat
import cpp "vector";
extern int main() { std.vector<int> v = default; v.push_back(1); for (int x in v) { } return 0; }
```

    no overload of 'count' matches the given arguments.
      Call arguments (1):
        [0] std.vector<int> <this>

## Root cause

The CFlat range-for protocol requires the collection to expose `count()`. `std.vector` spells the
same thing `size()` and offers `operator[]`; nothing adapts that pair, and nothing uses
`begin`/`end` either. Every registered C++ container is affected, not just `vector` - the indexable
ones (`vector`, `deque`, `array`, `span`, `valarray`) could work through `size`/`operator[]` alone,
the node-based ones (`map`, `set`, `list`, `unordered_*`) need iterators.

## Fix direction

Teach the range-for lowering a second shape for imported C++ classes: `size()` + `operator[]` for
the indexable containers, and `begin()`/`end()` + `operator*`/`operator++`/`operator!=` for the
rest. Decide whether both land at once or the indexable shape ships first - `auto it = m.find(1)`
already works, so iterator objects themselves are reachable.

Acceptance: the repro compiles and runs to exit 0; a `std.vector<int>` and a `std.map<int,int>`
loop are asserted in `Test/test_cpp_interop.cb`.
