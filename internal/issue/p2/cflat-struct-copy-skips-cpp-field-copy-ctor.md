# Copying a CFlat struct that holds a C++ class field skips the field's copy constructor, then destroys it twice

Found 2026-09-16 by the C++-interop bug bash round 3 (macOS arm64, Release, worktree at master 2798eb1a).
Sibling of `internal/issue/p2/cpp-class-copy-from-field-skips-copy-ctor.md` (copy OUT of a field);
this is the ENCLOSING struct's copy, and it leaves the construct/destroy counts unbalanced.

## Summary

NOT STARTED: same family as internal/issue/p2/cpp-class-copy-from-field-skips-copy-ctor.md (CFlat-side copy of a C++ class), which is HELD by maintainer ruling for a later CFlat-interop plan; needs the same ruling.

`Box b = a;` where `Box` is a CFlat struct with a foreign C++ class field copies the field
bitwise: the class's copy constructor never runs, yet both `a` and `b` run the field destructor at
scope exit. One construction, two destructions - a resource the class owns is released twice.

The existing shallow-copy guard only fires when the class has a POINTER or VIEW field (measured: a
class holding `int* p` is correctly rejected with "it has a non-trivial destructor and a
shallow-copied pointer/view field"). A class that owns a non-pointer handle - a file descriptor, a
registry slot, an index into a pool - passes the guard and double-releases.

## Repro (compile 0, run 0, verified twice on fresh compiles)

`scratch/bb3_life.h`:

```cpp
namespace bb3l {
inline int& slots() { static int s = 0; return s; }
inline int n_slots() { return slots(); }
struct Handle {
    int id;
    Handle() : id(7) { slots()++; ctors()++; }
    Handle(const Handle& o) : id(o.id) { slots()++; copies()++; }
    ~Handle() { slots()--; dtors()++; }
};
}
```

`scratch/bb3_handle.cb`:

```cflat
import cpp "bb3_life.h";
extern int printf(const char* f, ...);
struct Box { int tag = default; bb3l.Handle h = default; };
extern int main()
{
    bb3l.reset();
    { Box a = default;
      Box b = a;
      printf("in scope slots=%d ctor=%d copy=%d dtor=%d\n",
             bb3l.n_slots(), bb3l.n_ctor(), bb3l.n_copy(), bb3l.n_dtor()); }
    printf("after slots=%d (want 0) ctor=%d copy=%d dtor=%d\n",
           bb3l.n_slots(), bb3l.n_ctor(), bb3l.n_copy(), bb3l.n_dtor());
    return 0;
}
```

Measured:

```
in scope slots=1 ctor=1 copy=0 dtor=0        <-- C++ requires copy=1, slots=2
after slots=-1 (want 0) ctor=1 copy=0 dtor=2 <-- the same slot released twice
```

The same shape with a plain counted class (`scratch/bb3_wrapcopy.cb`) gives `ctor=1 copy=0 dtor=2`.

## Related symptom - the std.string field spelling fails with a confusing message

`scratch/bb3_wrapstr.cb`, `struct Box { int tag = default; std.string s = default; };` then
`Box b = a;`:

```
bb3_wrapstr.cb(14,12): no overload of 'copy' matches the given arguments.
  Call arguments (1):
    [0] std.string <unnamed>
  Candidates (7):
    copy(string) / copy(string, IAllocator) / copy(list<string>*) / ... / copy(Box)
```

The synthesized struct copy recursed into the field and looked the field copy up in the CFlat
`copy` overload set (core library candidates, plus `copy(Box)` itself), instead of using the C++
copy constructor. So the same root cause is visible in three forms: silent bitwise copy (Handle),
correct rejection only when a pointer field happens to be present (Owner), and an unrelated-looking
overload error (std.string).

## Fix direction

When the synthesized CFlat struct copy walks a field whose type is a foreign C++ class, emit the
class's copy constructor (`EmitCxxCopyOrMoveConstruct`) for that field rather than a bitwise field
copy or a CFlat `copy()` lookup. If the class has no accessible copy constructor, reject the
enclosing struct's copy the way the deleted-copy-ctor case is already rejected at a C++ call site.
The pointer/view heuristic should stop being the only trigger: any C++ field with a nontrivial copy
constructor or destructor qualifies.

Acceptance: the Handle repro prints `copy=1` and `after slots=0`, and the std.string form compiles
and prints both strings.

Suggested bucket: p2 (silent double release on a clean compile; upgrade to p1 if the std.string form
is considered common, since that one is a double free once the overload lookup is fixed).
