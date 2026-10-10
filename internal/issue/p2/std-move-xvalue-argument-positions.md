# A C++ `T&&` class result (std.move(x)) passed as an ARGUMENT is not an xvalue

Found while fixing std-utility-move-cannot-bind (2026-10-01): that fix made a C++ call returning
`T&&` to a C++ class an xvalue at declaration init (typed and `auto`), assignment and `return`.
The argument doors still see the result as a borrowed `T*`.

## Repro (in-repo shape: scratch-free, `cppi.xv_move` in Test/library/cpp_interop_basic.h)

```cflat
import cpp {"vector", "utility"};
import cpp "library/cpp_interop_basic.h";
import cpp "library/cpp_interop_basic.cpp";
int by_value(cppi.Tracked t) { return t.value(); }
extern int main()
{
    cppi.Tracked a = cppi.Tracked(7);
    cppi.Tracked b = cppi.Tracked(cppi.xv_move(a));   // silent COPY: a stays 7 (C++ moves, a == -1)
    int v = by_value(cppi.xv_move(a));                 // "no overload of 'by_value' matches"
    std.vector<cppi.Tracked> vec = default;
    vec.emplace_back(cppi.xv_move(a));                 // template deduces Tracked*: clang refuses
    cppi.Tracked c = cppi.xv_move(cppi.xv_move(a));    // inner result deduces Tracked*&&
    return 0;
}
```

Measured on the post-fix binary (same as master for every row):
- `T(std.move(x))` / `new T(std.move(x))`: compiles, copy-constructs (copy=1, move=0); clang++
  move-constructs. Silent wrong overload - the worst row.
- by-value parameter, C++ (`cbyval(Res)`) or CFlat callee: "no overload ... matches".
- function template parameter (`emplace_back`, `tbyval(T)`, `tfwd(T&&)`, nested `std.move`):
  the argument is spelled `T*`, clang rejects the instantiation.
- Works already: non-template `T&&` and `const T&` parameters (`push_back(std.move(p))`,
  overload `f(const T&)` / `f(T&&)` picks `T&&`).
- Accepted on master and must keep working: a `T*` parameter (C++ or CFlat) bound from the
  `T&&` result (not valid C++; keep or rule).

## Fix direction

`move *p` (explicit move of an unnamed lvalue) already works in every argument door; its
NamedVariable shape (Storage = referent, IsExplicitMove, IsRvalue) is the target representation.
Normalizing in `ParseCallArgument` (MainListener_PostfixExpression.cpp) would reach the
overload-scorer and template doors at once but changes the `T*` parameter row above - needs a
ruling on that row first, and the ctor-arg door (`ForeignCxxConstructArgs` loop in
TryDeclareForeignCxxLocal) has its own argument assembly.

## Ruling (maintainer, 2026-10-02 00:10)

Follows the N48 ruling (std::move and the CFlat `move` keyword both stay; std.move mimics C++). So the open row above is decided: a `T*` parameter bound from a `T&&` C++ result is refused like clang refuses it, and every argument door treats the result as an xvalue (move-construct, by-value params, template deduction spells `T`, not `T*`).

## T67 review 1 (2026-10-07)
- `std.move(x)` passed to a C++ `int&&` parameter is refused, while `move x` works (master same).
- (T67 review 2) `move m2` into a C++ `int*&&` materializes a temporary instead of binding m2's slot;
  value right, address differs from clang's std::move (master same).
