# C++ operator[] on a pointer element type yields the reference, not the element

Summary: `v[0]` on `std.vector<char*>` evaluates to the `char*&` reference (pointer to the
slot) instead of the `char*` element. For `std.vector<int>` the `int&` result decays to
the element as expected, so the bug is specific to pointer element types.

Repro: scratch/findings_m5/p3t.cb in the feature/cpp-interop worktree, or:

```c
// cflat-args: --cpp-assume-noexcept
import cpp "vector";
extern int main()
{
    std.vector<char*> v = default;
    char* s = "x";
    v.push_back(s);
    char* back = v[0];   // receives the slot address, not s
    return back == s ? 0 : 1;
}
```

Root cause: the index-expression result for a foreign `operator[]` returning `T&` is
surfaced as a pointer-shaped alias; the decay-to-element step treats `T*&` as a pointer
result already and skips the load.

Fix direction: at the index-expression result, treat `T*&` exactly like `T&`: load the
element when the expression is used as an rvalue, keep the address only when it is the
target of a store. Add the case to Test/test_c_interop.cb section M10.

Found by the M5 review fix pass, 2026-09-06.
