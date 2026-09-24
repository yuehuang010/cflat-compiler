# std.set<[cpp] struct>.insert(temporary | move l) is refused

Found 2026-09-23 while fixing cpp-container-sink-gaps (macOS arm64, Release). It never worked on
master: the pre-fix binary SIGSEGVs, and it now gives the raw "no overload of 'insert' matches
the given arguments." Plain clang++ accepts the equivalent C++.

## Repro

```cflat
import cpp "set";
import cpp "../scratch/sg_key.h";          // sgk::Base: deleted copy, virtual, operator<
[cpp] struct SK : sgk.Base { int k = 0; SK(int value) { k = value; tag = value; } };
extern int main() { std.set<SK> s = default; s.insert(SK(3)); return (int)s.size(); }  // expect 1
```

Same result for `SK l = SK(5); s.insert(move l);`. Scratch corpus files (in the
fix/cpp-container-sink-gaps worktree): sg_s17, sg_s18 and sg_s19 in scratch/. The plain C++
move-only key sgk.Key works: insert(temporary) returns 1 and insert(move l) returns 14.

sg_key.h (scratch is untracked):
`namespace sgk { class Base { public: Base() = default; Base(const Base&) = delete; Base(Base&&) = default;
virtual ~Base() = default; int tag = 0; bool operator<(const Base& o) const { return tag < o.tag; }
virtual int forward(int x) { return x; } }; }`

## Observation (lldb, Debug)

`std.set$SK.insert(value_type&&)` is refused as "cannot be instantiated for these template arguments".
Its ErrorReachScan walk reaches two refused functions:
- `std::__tree<SK,...>::__assign_value`, which is isInvalidDecl.
- the poisoned `allocator_traits<allocator<__tree_node<SK, void*>>>::construct(..., const SK&)`.
  This is the copy path, and it is correctly refused.
So the rvalue overload is refused because of callees that only the copy path needs. The walk
edge that connects them was not found.
The [cpp] struct's generated class has a move ctor, but move-assign and copy-assign are deleted.
`__assign_value` needs one of them, and sgk.Key (which has move-assign) does not hit this.

## Fix direction

Find which call in the insert(&&) instantiation reaches __assign_value or the const& construct.
It may be a discarded if-constexpr branch or an unevaluated operand that the body visitor counts
as a call. Exclude such operands from Reaches.
