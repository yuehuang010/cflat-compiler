# Brace-list backing: remaining shapes

E1b fixes the function-template `std::initializer_list<T>` caller-frame backing,
the converting class-element backing for `RequestCxxBraceFunction`, and BY-VALUE and
`const A&...` constructor template packs reached by an argument beside a brace list.

## Constructor template packs beside brace-list constructors still refused or wrong (E1b review 2)

Fixture shapes (clang++ -std=c++20 picks `which`):
```cpp
template <class T> struct PkA { int which; PkA(std::initializer_list<T>, const int&) : which(1) {}
    template <class... A> PkA(std::initializer_list<T>, A&&...) : which(2) {} };
template <class T> struct PkR { int which; PkR(std::initializer_list<T>, long) : which(1) {}
    template <class... A> PkR(std::initializer_list<T>, A&...) : which(2) {} };
template <class T> struct PkM { int which; PkM(std::initializer_list<T>, const int&, double) : which(1) {}
    template <class... A> PkM(std::initializer_list<T>, const A&...) : which(2) {} };
```
- `A&&...` (`PkA<long>({2, 3}, x)` / `({2, 3}, 5)`: clang 2 / 2) and `A&...` (`PkR<long>({2, 3}, x)`:
  clang 2; rvalue: clang 1) are refused ("passes an argument into a parameter pack ... cannot rank
  that call"): the generated selector wrapper cannot preserve reference category per element.
- A pattern naming a second template parameter (`const std::pair<U, A>&...` beside a non-template
  sibling) is refused for the same reason (fixture `cppcr.BrcPackPair`, clang 2).
- A non-type pack pattern (`template <std::size_t... N> X(il<T>, const std::array<int, N>&...)`
  beside `X(il<T>, W)` with W converting from `array<int, 2>`) is refused the same way (fixture
  `cppcr.BrcPackArr`, clang 2).
Direction: mirror reference packs as reference parameters in the selector wrapper (alias params),
and map a multi-parameter pattern by substituting only the pack's own parameter while the others
go through the normal template-parameter rename.

## Related claims closed by measurement

The `const long (&)[2]` constructor/function argument form and `std::vector<double>` from
`{floatVar, 2}` compile and return the clang value on the worktree, master, and clang. These were
not remaining backing bugs.

## Round 4 remaining call shapes

Round 4's cleanup-order change was reverted. These are fresh traces from the rebuilt r3 code in
this worktree, beside clang++ C++20. Each function-call body records `ev(r % 10)` after the call;
the S3 case stores `s.w` in `r` inside its scope, then calls `mark()` after scope exit.
Every successful executable calls `dump()` after that body.

| Repro and body | This CFlat exe | clang++ C++20 |
|---|---:|---:|
| `r3_nested_l`: `int r = bb::fr2({{1}, {2}}, bb::mark()); bb::ev(r % 10);` | `912650` | `129650` |
| `r3_conv_l`: `int r = bb::fr2({1, bb::E(2)}, bb::mark()); bb::ev(r % 10);` | `29186560` | `129650` |
| `r3_lval_l`: `bb::E e = bb::E(2); int r = bb::fr2({bb::E(1), e}, bb::mark()); bb::ev(r % 10);` | `219876550` | `2179650` |
| `r3_ovl`: `int r = bb::fr5({bb::E(1), bb::E(2)}, bb::mark()); bb::ev(r % 10);` | `1298865650` | `129650` |
| `r3_default`: `int r = bb::fr6({bb::E(1), bb::E(2)}, bb::mark()); bb::ev(r % 10);` | `1298865653` | `129653` |
| `r3_method`: `bb::M o = bb::M(); int r = o.m({bb::E(1), bb::E(2)}, bb::mark()); bb::ev(r % 10);` | `1298865650` | `129650` |
| `r3_elemcond`: `int c = 1; int r = bb::fr2({bb::E(1), c != 0 ? bb::E(2) : bb::E(3)}, bb::mark()); bb::ev(r % 10);` | `1298865650` | `129650` |
| `r3_inner`: `int r = bb::fr2({bb::E(1), bb::E(2)}, bb::fr2({bb::E(3)}, bb::mark())); bb::ev(r % 10);` | `1238977650` | `12397650` |
| `r3_ctor`: `{ bb::S3 s = bb::S3({bb::E(1), bb::E(2)}, bb::mark()); int r = s.w; bb::ev(r % 10); } bb::mark();` | `CE1` | `1296509` |

The CFlat conversion, lvalue, overload, defaulted-parameter, member, conditional-element, and inner-call
traces interleave construction: part of the brace-list backing is built before the later argument and
the rest after it. C++ argument evaluations are indeterminately sequenced, so no legal C++ order
produces that interleaving. Constructor/destructor events for temporary backing objects balance. In
the lvalue case, `e` is caller-owned and remains alive until after `dump()` in both programs. The
nested-brace case instead builds the list after `mark()`. The S3 CFlat probe is refused because its
by-value C++ class argument is not addressable.

The pinned master executable accepted only `r3_conv_l` (trace `29185660`) and refused the other eight
shapes. The current r3 code has the same conversion trace as master; the remaining rows record the
freshly measured worktree behavior rather than round 4's reversed cleanup order.

## Round 2 remaining shapes

- Named initializer-list local backing is not lifetime-extended. The fixture and exact reproducer
  below record clang trace `1292659` and CFlat trace `12886565979`. Extend the backing lifetime
  through the named local's scope and register cleanup at that scope's exit.
- Class and nested-brace initializer-list overload selection is still unsupported. The fixture and
  exact reproducer below show clang selecting the class overload for `g({E(1), E(2)})` (result
  `202`, trace `126522`) and the integer overload for `g({{1}, {2}})` (result `102`). Rank class
  element conversions and nested-list shape when choosing the overload.

### Shared C++ fixture

```cpp
#include <initializer_list>
#include <cstdio>
namespace bb {
inline long long trace = 0;
inline void ev(int digit) { trace = trace * 10 + digit; }
inline int mark() { ev(9); return 0; }
inline void dump() { std::printf("%lld\n", trace); }
struct E {
    int value;
    E(int v) : value(v) { ev(v); }
    E(const E& other) : value(other.value) { ev(7); }
    E(E&& other) : value(other.value) { ev(8); }
    ~E() { ev(value % 10 + 4); }
};
inline int fr(std::initializer_list<E> values) {
    int result = 0;
    for (const E& value : values) result = result * 10 + value.value;
    return result;
}
inline int fr2(std::initializer_list<E> values, int k) { return fr(values) * 10 + k; }
inline int fr5(std::initializer_list<E> values, int k) { return fr(values) * 10 + k; }
inline int fr5(int a, int k) { return a + k; }
inline int fr6(std::initializer_list<E> values, int k, int j = 3) {
    return fr(values) * 10 + k + j;
}
struct M { int m(std::initializer_list<E> values, int k) { return fr(values) * 10 + k; } };
struct S3 { int w; S3(std::initializer_list<E> values, int k) : w(fr(values) * 10 + k) {} };
inline int g(std::initializer_list<int> values) { return 100 + (int)values.size(); }
inline int g(std::initializer_list<E> values) { return 200 + (int)values.size(); }
}
```

`ev(d)` appends decimal digit `d`; constructors append their value, copy and move append `7`
and `8`, destruction appends `value % 10 + 4`, and `mark()` appends `9`. `fr` concatenates
the element values. These definitions make the ordering and overload results self-contained.
The r3 probes additionally declare `bb::E e = bb::E(2);` in `r3_lval_l` and `int c = 1;` in
`r3_elemcond`; the call bodies and their `ev(r % 10)` record are listed above.

### Named local lifetime repro

```cpp
int main() {
    {
        std::initializer_list<bb::E> values{bb::E(1), bb::E(2)};
        bb::mark();
        bb::ev(bb::fr(values) % 10);
    }
    bb::mark();
    bb::dump();
    return 0;
}
```

Clang keeps both elements alive through the local scope and prints trace `1292659`. CFlat
`std.initializer_list<bb.E> values = {bb.E(1), bb.E(2)}; bb.mark(); bb.ev(bb.fr(values) % 10);`
then `bb.mark()` prints `12886565979`.

### Overload selection repro

```cpp
int main() {
    int classResult = bb::g({bb::E(1), bb::E(2)});
    bb::ev(classResult % 10);
    bb::ev(classResult / 100);
    int nestedResult = bb::g({{1}, {2}});
    if (classResult != 202 || nestedResult != 102) return 1;
    bb::dump();
    return 0;
}
```

The class call selects `std::initializer_list<E>` and has trace `126522` after its two `ev`
calls. The nested call selects `std::initializer_list<int>` and returns `102`. CFlat currently
refuses the class overload pair and needs element conversion plus nested-list shape in ranking.
