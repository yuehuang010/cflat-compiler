Bucket: p2 (wrong value, no diagnostic)

# `vector.push_back(local)` of a `[cpp] struct` lvalue compiles and inserts a wrong element

Found 2026-09-23 while closing p3/cpp-struct-by-value-in-std-container (its `__construct_at`
refusal no longer reproduces on 23ccd008: `std.vector<Leaf>` of a `[cpp] struct` works for
`push_back(Leaf(3))`, `push_back(move l)`, `emplace_back`, `size()`, virtual calls through the
element, deque/list, and the `std.vector<std.unique_ptr<T>>` control - all measured green).

What is wrong: the generated C++ class for a `[cpp] struct` has a DELETED copy constructor
(borrow-by-default ruling) and a move constructor forwarding to `__cflat_move_<T>`, yet
passing a named lvalue WITHOUT `move` to `push_back` compiles with no diagnostic and the
inserted element is not the value of `local`:

```cflat
import cpp "vector";  import cpp "bc_fixture.h";   // ms.Module: a base with virtual int forward(int)
[cpp] struct CopyCheck : ms.Module { int k = 0; CopyCheck(int k0) { k = k0; }
                                     override int forward(int x) { return x + k; } };
extern int main() {
    std.vector<CopyCheck> values = default;
    CopyCheck local = CopyCheck(4);
    values.push_back(local);              // compiles; no diagnostic
    if (local.k != 4) return 51;          // passes
    if (values[0].forward(1) != 5) return 52;   // FAILS: exit 52
    return 0; }
```

Repro: scratch/bc/bc_copy.cb + scratch/bc/bc_fixture.h (main checkout scratch, from the
codex worktree). Not yet traced: which push_back overload the call binds (`const T&` should be
uninstantiable with the deleted copy; `T&&` must not accept an lvalue) and what gets stored.

Fix direction: per the borrow-by-default ruling an lvalue `[cpp] struct` into a by-value / sink
C++ parameter is refused with the existing "copy constructor is deleted ... write 'move
<variable>'" family of diagnostics (measure which one the plain-C++-class case prints and
reuse it); `push_back(move local)` and a temporary keep working. Acceptance: err test with the
exact message, plus value legs for the temporary/move forms in Test/test_cpp_interop_bridge.cb
(next free block 2060-2079).
