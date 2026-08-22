# `list<T>` whose element owns a `unique` leaks every element on destruction

Filed 2026-08-22, found while fixing [[lambda-captures-list-by-copy-instead-of-reference]] (the
new `list<UniqueCell>` regression leg is the first place in the suite that builds one).

## Repro

```cflat
import "list.cb";
struct Counter { int value = 0; };
struct UniqueCell { unique Counter* p = new Counter(); };
extern int main()
{
    {
        list<UniqueCell> cells = default;
        UniqueCell c1 = default; cells.add(c1);
        UniqueCell c2 = default; cells.add(c2);
        printf("c=%d\n", cells.count());
    }
    return 0;
}
```

`leaks --atExit` reports `4 leaks for 64 total leaked bytes` - the two `Counter` pointees and
the two backing slots. No lambda is involved, so this is independent of the capture fix; the
same program leaks identically before and after that commit. `Test/test_function_ptr.cb`'s
`ref capture list of unique-owning elems` leg carries the same 64 bytes into that binary
(1 leak / 32 bytes without it, 5 / 96 with it).

## Root cause (hypothesis - verify)

`list<T>`'s destructor drops the backing buffer but does not run each element's FULL destructor,
so an element that owns a `unique` never frees its pointee. `list<string>` does not leak (its
element destructor path is taken), which suggests the element-dtor loop is gated on a predicate
that a `unique`-owning element type fails - check the destructor `list<T>` synthesizes and the
`if const` guards around `_data[i]` teardown in `cflat/core/list.cb`.

## Fix direction

Run the element full-destructor for every owning element type, not just the string-shaped ones,
or reject `list<T>` of a `unique`-owning element at declaration if the container genuinely cannot
own it. Regression leg: the repro above under `leaks --atExit`, plus a destructor counter on the
element type asserting one destruction per element.
