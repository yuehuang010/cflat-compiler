# std::move (rvalue-reference C++ call) refused as an initializer / assignment / return source

Ruling (maintainer, 2026-10-01): KEEP `std::move`. The `move` keyword is CFlat's own (Rust-style,
tracked by the ownership analysis); `std.move(x)` is the C++ operation with C++ semantics. Both stay.

## Repro

```cflat
import cpp {"memory", "utility", "string"};
extern int main()
{
    std.unique_ptr<int> p = std.make_unique<int>(7);
    std.unique_ptr<int> q = std.move(p);   // refused today
    std.string s = std.string("hello");
    std.string t = std.move(s);            // refused today
    return p.get() == nullptr && *q == 7 && t == "hello" ? 0 : 1;
}
```

Observed: `cannot initialize C++ class 'std.unique_ptr<int>' from this expression; use
'std.unique_ptr<int>(args)', '= default', a 'std.unique_ptr<int>' lvalue, or 'move <...> lvalue'`.
Already works: `v.push_back(std.move(p))` - the argument path binds `T&&`, `p` is left null.

## Fix direction

1. A C++ call whose result is an rvalue reference `T&&` to a C++ class (std::move, and by return type
   alone boost::move, eastl::move, MoveTemp, ...) is a move source in declaration init, assignment
   and `return`: move-construct / `operator=(T&&)` from the referent, the same machinery
   `move <T> lvalue` uses. Key on the return type, never on the name. The 3-argument algorithm
   `std::move(first, last, dest)` returns an iterator and is unaffected.
2. C++ semantics for the source: valid but unspecified, still readable (`unique_ptr` null). No
   CFlat "use of moved variable" diagnostic for `std.move` - that stays the `move` keyword's.
3. Better diagnostics (special-cased), each with a specific message:
   - `std.move` applied to a CFlat (non-C++) value, e.g. a CFlat owning struct or `unique<T>`:
     refuse, suggest `move x`.
   - the init refusal above, until fixed / for a non-class `T&&`: name the rvalue-reference result
     instead of the generic "cannot initialize from this expression".

Disabled case: test_libs/std/std_11_94_utility_move.cb (enable when 1-2 land; add the string leg).
Error tests for item 3 go in Test/errors/.

On landing: copy the ruling (first paragraph + fix-direction items 1-3) into the landed design-record digest at the bottom of internal/fix-issue-lessons.md before deleting this file.
