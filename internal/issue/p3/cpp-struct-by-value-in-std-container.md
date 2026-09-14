# `std.vector<Leaf>` of a `[cpp] struct` by value fails: "no matching function for call to '__construct_at'"

## Summary

A `[cpp] struct` used BY VALUE as the element of a standard container is refused at the
class-template type request (found 2026-09-14 during M10 timebox 2; scratch/m10/tb2/p8.cb
against scratch/m10/tb2/ms.h):

```
struct Leaf : ms.Module { int k = 0; Leaf(int k0) { k = k0; } override int forward(int x) { return x + k; } };
struct Net : ms.Module { std.vector<Leaf> children = default; ... };
// p8.cb(11,14): C++ type 'std::vector<__cflat_user::Leaf>' could not be parsed: no matching function for call to '__construct_at'
```

`std.vector<std.shared_ptr<Leaf>>` and `std.shared_ptr<Leaf>` work (M78 rows).

## Root cause (hypothesis, not yet traced)

The generated class deletes its copy constructor (borrow-by-default ruling) and keeps a move
constructor forwarding to `__cflat_move_Leaf`. The type request instantiates every member of
`std::vector<Leaf>` for the wrapper set, including the copy-taking overloads
(`push_back(const T&)`, copy ctor/assignment of the vector), which libc++ rejects for a
non-copyable element, and the whole request fails instead of skipping the uninstantiable
members. C++ itself allows `std::vector<MoveOnly>` as long as only the move-taking members
are used.

## Fix direction

- In the class-template request, instantiate members individually (or mark each wrapper
  with a SFINAE probe) so members that fail to instantiate for a move-only argument are
  dropped from the wrapper set instead of failing the request; the same rule already
  applies to any move-only C++ argument (`std::vector<std::unique_ptr<T>>` - check whether
  that works today; if it does, reuse its path).
- Then `push_back(Leaf(3))` must route the CFlat temporary through the generated move
  constructor (see the M75 return-adoption rows for the pattern).
- Tests: M78/M79 rows with `std.vector<Leaf>` push_back of temporaries, index access,
  `size()`, dtor counter after scope.
