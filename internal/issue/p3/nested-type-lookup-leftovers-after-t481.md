# Nested type lookup leftovers after T481

T481 round 2 fixes only the caller-scope mismatch in nested generic member code. These separate lookup and duplicate-definition cases remain active. Each repro path is followed by its source inline.

## Enclosing aggregate nested type from an inner method

Master exits 1; clang++ -std=c++20 exits 2. Round 1 also exits 1.

Repro path: `scratch/t481/leftover_outer_inner.cb`

```cflat
namespace N {
    struct Mid { int v = 1; };
    struct S {
        struct Mid { int v = 2; };
        struct Inner { int read() { Mid m = default; return m.v; } };
    };
}
int main() { N.S.Inner i = default; return i.read(); }
```

## Injected nested name inside its own type body

Master exits 31; clang++ -std=c++20 exits 32. Round 1 also exits 31.

Repro path: `scratch/t481/leftover_self_new.cb`

```cflat
namespace N {
    struct Mid { int v = 1; };
    struct S {
        struct Mid {
            int v = 2;
            int read() { auto p = new Mid(); int r = 30 + p->v; delete p; return r; }
        };
    };
}
int main() { N.S.Mid value = default; return value.read(); }
```

## Outer nested type from a nested generic, without a namespace twin

Master and round 2 fail with `cannot find the type 'Mid'`; round 1 failed with `cannot find the type 'N.S.Mid'`; clang++ -std=c++20 exits 2.

Repro path: `scratch/t481/leftover_outer_generic.cb`

```cflat
namespace N {
    struct P {
        struct Mid { int v = 2; };
        struct G<T> { int read() { Mid m = default; return m.v; } };
    };
    struct S { int read() { P.G<int> g = default; return g.read(); } };
}
int main() { N.S value = default; return value.read(); }
```

## Nested generic lookup without a matching caller frame

These review-2 cells remain pre-existing. Each binds `N.Mid` (value 1) on master
and round 2; `clang++ -std=c++20` resolves the enclosing nested type (value 2).
The shared gap is that the outer lexical aggregate slot is unavailable to remap.

### Instantiated from a free function

Repro path: `scratch/t481/r3_free.cb`. Master and round 2 exit 1; clang exits 2.

```cflat
namespace N {
    struct Mid { int v = 1; };
    struct P {
        struct Mid { int v = 2; };
        struct G<T> { int f() { Mid m = default; return m.v; } };
    };
}
int read() { N.P.G<int> g = default; return g.f(); }
int main() { return read(); }
```

### Instantiated from a generic method

Repro path: `scratch/t481/r3_generic_method.cb`. Master and round 2 exit 1; clang exits 2.

```cflat
namespace N {
    struct Mid { int v = 1; };
    struct P {
        struct Mid { int v = 2; };
        struct G<T> { int f() { Mid m = default; return m.v; } };
    };
    struct S { int h<U>(U value) { P.G<int> g = default; return g.f(); } };
}
int main() { N.S s = default; return s.h(0); }
```

### Two-level nested generic uses outer `P.Mid`

Repro path: `scratch/t481/r3_two_level.cb`. Master and round 2 exit 1; clang exits 2.

```cflat
namespace N {
    struct Mid { int v = 1; };
    struct P {
        struct Mid { int v = 2; };
        struct Q { struct G<T> { int f() { Mid m = default; return m.v; } }; };
    };
}
int main() { N.P.Q.G<int> g = default; return g.f(); }
```

## Duplicate type definitions are not consistently rejected

Master and round 1 both silently use the first definition and exit 1; clang++ -std=c++20 rejects the second definition with `redefinition of 'Dup'`.

Repro path: `scratch/t481/leftover_duplicate.cb`

```cflat
struct Dup { int v = 1; };
struct Dup { int v = 2; };
int main() { Dup value = default; return value.v; }
```

- Generic OUTER struct (T481 r3 review): `A.P<X>.Q.G<T>.f` does `new Mid()` with `Mid` declared in `Q`, called from `Q.h`:
  master and T481 1, clang 2. The instantiated scope is mangled `A.P$int.Q`, which cannot be compared lexically with the
  parse-tree owner `A.P.Q` (arguments may contain dots); T481 keeps master's lookup there. Fix needs a per-segment demangle
  (TypeMangling.h DemangleType) or the registry's template names. Also: `using M = Mid;` inside `G<T>` fails on master and T481.
