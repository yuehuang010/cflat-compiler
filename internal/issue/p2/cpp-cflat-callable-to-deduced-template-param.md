# A CFlat callable cannot be passed to a deduced C++ callable parameter

Summary: a CFlat native callable - a `function<...>` value, a lambda, or a bare function name - passed
where a C++ template deduces the callable type is refused with "argument type that cannot be spelled
in C++". Wrapping it first in `std.function<...>(free_fn)` works (that spelling is what the enabled
legs in test_libs/std use), so the gap is the bridge from CFlat callables to a C++ template
parameter, not the std API. Per the bridge-transparency direction the use site should not need the
`std.function` wrapper.

Repro:
```cflat
import cpp {"set", "thread"};
extern void work() { }
extern int main()
{
    std.set<int> s = default;
    s.insert(4);
    function<bool(int)> is_four = (int v) => { return v == 4; };
    int n = std.erase_if(s, is_four);      // C++ free function 'std.erase_if' has an argument type that cannot be spelled in C++
    std.jthread t = std.jthread(work);     // C++ class 'std.jthread' a constructor argument type cannot be spelled in C++
    t.join();
    return n == 1 ? 0 : 1;
}
```

Expected: the CFlat callable binds as the deduced callable (erase_if returns 1, the thread runs).
Works today: `std.erase_if(s, std.function<bool(int)>(is_four_fn))`, `std.jthread(std.function<void()>(work))`.

Disabled cases: test_libs/std/std_20_93_erase_if.cb, test_libs/std/std_20_97_jthread_stop_token.cb.
Related: p2/std-cflat-lambda-to-std-function.md (a lambda cannot initialize `std::function` either).
