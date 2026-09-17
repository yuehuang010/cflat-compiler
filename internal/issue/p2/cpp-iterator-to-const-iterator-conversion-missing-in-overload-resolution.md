# `v.insert(v.begin(), x)` is refused: iterator -> const_iterator is not a conversion in overload resolution

Bucket: **p2** (idiomatic std code does not compile; behaviour is inconsistent with the
single-candidate path, which accepts the same argument).

## Summary

libc++ declares `vector<T>::insert` and `vector<T>::erase` over `const_iterator`. In C++,
`iterator` converts to `const_iterator`, so `v.insert(v.begin(), x)` is the normal spelling.

CFlat's overload resolution does not know that conversion, so:

- `v.insert(v.begin(), x)` - **refused**, "no overload of 'insert' matches" (7 candidates listed,
  all over `std.__wrap_iter<constintptr>`)
- `v.insert(v.cbegin(), x)` - compiles and runs correctly
- `v.erase(v.begin())` - compiles and runs correctly

The `erase` case proves the conversion is not the blocker in general: when a member has a single
candidate the argument is accepted, but as soon as the member is overloaded the same argument is
rejected. So the rule the user sees is "it depends on how many overloads the member happens to
have", which is not a rule anyone can predict.

## Repro

`scratch/bb4_iter4.cb`:

```cflat
import cpp "vector" cache;
extern int main()
{
    std.vector<int> v = default;
    v.push_back(1); v.push_back(2); v.push_back(3);
    int x = 9;
    v.insert(v.cbegin(), x);                // ok
    printf("A size=%d front=%d expect=4 9\n", (int)v.size(), v[0]);
    v.insert(v.begin(), x);                 // refused
    return 0;
}
```

Measured (macOS arm64, Release, master 2798eb1a), reproduced twice:

```
bb4_iter4.cb(9,4): no overload of 'insert' matches the given arguments.
  Call arguments (3):
    [0] std.vector<int> <this>
    [1] std.__wrap_iter<intptr> <unnamed>
    [2] int <unnamed>
  Candidates (7):
    insert(list<string>*, int, string)
    insert(list<string>*, int, move string)
    insert(std.vector<int>*, std.__wrap_iter<constintptr>, int)
    ...
```

`scratch/bb4_iter5.cb` shows `v.erase(v.begin())` compiling and producing `size=2 front=2`.

Two side observations from the same message, worth folding into the fix:
- the candidate list mixes in the CFlat core `list<string>::insert` overloads, which can never
  apply to a `std.vector<int>` receiver - noise that makes the real mismatch harder to see;
- `std.__wrap_iter<constintptr>` is not a spellable CFlat type; a user cannot write the type the
  compiler is asking for.

Related, same root area: the free `operator-(__wrap_iter, __wrap_iter)` used by
`v.end() - v.begin()` is not bound either (`no overload of 'operator-' matches`, candidates are
only the iterator-minus-integer forms). That one is the known "std free function templates not
bound" work already in flight, so it is not filed separately here.

## Fix direction

Teach the C++ argument-conversion ranking one implicit conversion: a class specialization
`std.__wrap_iter<T*>` converts to `std.__wrap_iter<const T*>` at zero cost (they are
layout-identical). The general rule is "a specialization converts to the same template with a
`const`-added pointer argument", which also fixes `reverse_iterator`, `map::iterator` and
user templates that follow the same pattern. Filtering the candidate list to members of the
receiver's own type before printing would fix the diagnostic noise independently.
