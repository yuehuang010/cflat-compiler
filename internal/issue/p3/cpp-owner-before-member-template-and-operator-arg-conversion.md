# `shared_ptr::owner_before` is not callable, and `map<weak_ptr,...>[shared_ptr]` refuses the implicit conversion

Two separate refusals found together; both have working spellings.

1. `a.owner_before(b)` on `std.shared_ptr<int>` / `std.weak_ptr<int>` fails with `Undefined variable owner_before.`
   (`Unknown identifier 'owner_before'.` on a weak_ptr). `owner_before` is a member function template
   (`template<class U> bool owner_before(const shared_ptr<U>&) const`, plus a weak_ptr overload). Workaround:
   `std.owner_less<std.shared_ptr<int>> ol = default; ol(a, b)` works.
2. `std.map<std.weak_ptr<int>, int, std.owner_less<std.weak_ptr<int>>>` `om[a] = 10` with `a` a
   `std.shared_ptr<int>` fails: `no overload of 'operator[]' matches the given arguments ... [1]
   std.shared_ptr<int>; Candidates: operator[](map*, std.weak_ptr<int>) / (map*, std.weak_ptr<int>*)`. The same
   conversion in copy-init (`std.weak_ptr<int> w = a;`) works. Workaround: build a `weak_ptr` local first.

## Repro

```cflat
import cpp {"memory", "map"};
extern int main()
{
    std.shared_ptr<int> a = std.make_shared<int>(1); std.shared_ptr<int> b = std.make_shared<int>(2);
    bool x = a.owner_before(b);                       // expected: compiles
    std.map<std.weak_ptr<int>, int, std.owner_less<std.weak_ptr<int>>> om = default;
    om[a] = 10;                                       // expected: compiles (converting ctor weak_ptr(shared_ptr<Y>))
    return 0;
}
```

Case: test_libs/std_full/std_full_11_929_smart_ptr_member_templates.cb (3 legs).

## Root cause

1. `owner_before` was extracted as a C++ member-function template, but the postfix walk visited its
   bare name before it had established the receiver. It sent the name through ordinary identifier
   lookup and reported it undefined. The call path now defers registered member-template names and
   recovers a named C++ receiver from the member-access path. libc++ does not expose a separate
   `weak_ptr.owner_before` registry entry, so its call uses the matching `shared_ptr` template entry
   with the actual weak_ptr receiver; clang resolves the overload.
2. Map indexing now reaches constructor-conversion ranking, but is still rejected. The bound C++
   candidates expose the `const weak_ptr&` and pointer-shaped `weak_ptr*` ABI forms. Clang prefers the
   latter for the temporary produced by `weak_ptr(shared_ptr)`, while CFlat's conversion ranking
   keeps the const-reference candidate and reports that it cannot adapt the argument to the pointer
   shape. Three focused attempts did not resolve this mismatch; it remains active under the stop rule.

## Remaining fix direction

Correctly preserve/rank the C++ reference binding for a temporary created by a non-explicit
converting constructor, then materialize that temporary for the selected `operator[]` candidate.
Do not remove the DISABLED marker until the map leg passes.

Found by: test_libs/std_full/std_full_11_memory_smart_ptrs (2026-10-02).
