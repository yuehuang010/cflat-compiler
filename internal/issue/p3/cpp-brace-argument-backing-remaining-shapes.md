# Brace-list backing: optional constructor parameter case remains

E1b fixes the function-template `std::initializer_list<T>` caller-frame backing,
the converting class-element backing for `RequestCxxBraceFunction`, and BY-VALUE and
`const A&...` constructor template packs reached by an argument beside a brace list.
The remaining optional backing shape is:

- A class-typed constructor parameter is only backed when every fitting constructor
  takes the same class at that position and that class declares exactly one
  non-template `initializer_list<E>` constructor. A list-ctor template (not listed
  by the extractor) can still leave the thunk-built list.

Related, not backing: a non-template function with an array-reference parameter
(`const long (&)[2]`) does not accept a brace argument at all; the copying path
refuses `vector<double>` from `{floatVar, 2}` although clang accepts it (CFlat call
conversion of the thunk's per-element parameters).

## Class-prvalue elements in deduced initializer lists (E1b round 2)

`keep({E(1), E(2)})` constructs each temporary and then copies it into the backing array. The measured constructor/destructor delta is 4/4 on the branch versus clang's 2/2; the returned value matches (12). Master refuses this shape. Repro: `struct E { int v; inline static int made = 0, gone = 0; E(int x) : v(x) { ++made; }
E(const E& o) : v(o.v) { ++made; } ~E() { ++gone; } };`, `template <class T> const T* keep(std::initializer_list<T> l)
{ return l.begin(); }`, `long rde(const E* p) { return p[0].v * 10 + p[1].v; }`; CFlat
`rde(keep({E(1), E(2)}))` = 12, made/gone deltas 4/4 (clang 2/2).

Root cause: the backing-array lowering materializes prvalue list elements before initializing their slots, rather than constructing directly into the slots. Fix direction: elide the intermediate class temporary while preserving full-expression lifetime and destructor counts.

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
- `const A&...` with two pack arguments (`PkM<long>({2, 3}, i, l)`, int + long): clang 2, cflat fails
  with "no overload of '__cflat_tpl_...' matches" - leaks the internal wrapper name (master same).
- `A*...` beside a `const void*` overload: refused "argument 2 ... cannot be ranked", clang 2
  (master same).
Direction: mirror reference packs as reference parameters in the selector wrapper (alias params),
and map a multi-parameter pattern by substituting only the pack's own parameter while the others
go through the normal template-parameter rename.
