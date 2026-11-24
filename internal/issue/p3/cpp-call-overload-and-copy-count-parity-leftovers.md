# C++ call parity leftovers (pre-existing; residue after CALLPAR, fix box 16)

Measured 2026-11-19 against clang++ -std=c++20. Counts are ctor/copy/move/dtor/live digits of cpptw::Twin
(Test/library/cpp_interop_twin.h), read after the scope closes. CALLPAR fixed: `?:` default typed from the
other arm over competing overloads, bare `default` over two class overloads (refused, clang ambiguous), ADL to
namespace-scope C++ functions and templates, template-only sets deferring `c ? default : local`. Already
matching clang on master and dropped: a global `?:` leaf, a by-value parameter's field leaf, a bool
field/global condition.

Header for the repros below:

```cpp
#include "cpp_interop_lastuse.h"
namespace cp
{
    struct Host
    {
        int mix(cpptw::Twin a) { return 10 + a.value(); }
        template <class T> int mix(T a) { return 20 + a.value(); }
    };
    using FP = int (*)(int);
    inline int runfp(FP&& f) { return f(4); }
}
```

1. Member set holding a non-template and a member template, `?:` argument: one extra move per arm. The set
   always routes through the clang wrapper (deduceBesideSpecializations), which receives the joined slot as
   `T&&` and moves it on. Deferring the `?:` into the wrapper (as template-only sets now do) reaches the
   non-template by-value binder instead and refuses ("cannot pass C++ class ... by value").
   `cp.Host h = default; cpptw.Twin x = cpptw.Twin(7); h.mix(false ? default : x)`: 17 11130, clang 17 11020;
   `h.mix(true ? default : x)`: 10 20130, clang 10 20020. (T418TemplateSingle/Competing legs are this shape.)

2. ADL beside a visible CFlat overload of the same arity keeps CFlat-only lookup, so a worse CFlat overload
   wins: `int take(int z) { return z; }` at file scope, then `cpptw.Twin x = cpptw.Twin(7); take(x)` returns
   1 (Twin -> bool -> int) with 10010; clang picks cpptw::take by ADL, 7 11020. A fix must merge the ADL
   candidates into the visible set and let clang rank both, or refuse the mixed set.

3. (Out of CALLPAR scope, numbers from the 2026-10-22 filing, not re-measured.) Ctor `S(A&... rest, int a, int b)` (non-final reference pack): 1111 vs clang 1011 (T416 keeps the
   conservative slot rule; see fix-issue-lessons digest T416).

4. (Out of CALLPAR scope, 2026-10-22 numbers.) By-value class temp through the free wrapper moves once more than clang (1010020 vs 1000010).

Not this issue: a CFlat `Lambda<int(int)>` local into a non-template `FP&&` parameter (`cp.runfp(f)`, with or
without `move f`; clang 5) is refused as "this 'function<>' value may store captured state" - the same refusal
a by-value `FP` parameter gives, so it belongs to cpp-capturing-closure-to-std-function.md. `std.function&&`,
a class `Fn&&`, a named function and an inline lambda into `FP&&` all match clang.
