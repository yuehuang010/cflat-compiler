# `using` type alias in struct/class scope, selectable by `if const` over the type parameters

## Summary

A generic struct that stores either `T` (interface value) or `T*` (pointee pointer) has to
spell every field, constructor, accessor and mutator twice under `if const (is_interface(T))`.
`cflat/core/unique.cb` carries nine such arms. A member-scope alias that resolves once per
instantiation would collapse the arms that differ only in the spelled type.

Filed 2026-09-02 (maintainer request during the `[unique]` attribute prototype). Needs a
ruling on the surface before build.

## Minimal repro (all three fail today)

```cflat
struct Box<T>
{
    using P = T*;                                            // (1) parse error: 'using' not allowed here
    P _p = default;
};

using PtrOf<T> = T*;                                         // (2) parse error: alias is not generic

struct Box2<T>
{
    if const (is_interface(T)) { using P = T; }              // (3) parse error at 'using'
    else                       { using P = T*; }
    P _p = default;
    P get() { return _p; }
};
```

Probes: scratch/attr/al1.cb, al2.cb, al3.cb on the branch. Grammar: `using` is an
`aliasDeclaration` admitted at file scope and function scope only (CFlat.g4 ~737); the
struct-body rule (CFlat.g4 ~878) does not list it. LANGUAGE.md "`using` Aliases" documents the
file/function forms and that `*` / `[N]` suffixes fold onto the use site.

## Proposed spelling

Plain `using` inside a struct or class body, visible from that point to the end of the body,
with `if const` permitted around it exactly as around a field:

```cflat
[unique]
struct unique<T>
{
    if const (is_interface(T)) { using P = T; }
    else                       { using P = T*; }
    P _p = default;
    unique(move P p) { _p = move p; }
    P get()          { return _p; }
    bool valid()     { return _p != nullptr; }
    move P release() { P p = move _p; _p = nullptr; return move p; }
    void reset(move P p) { P old = move _p; _p = move p; if (old != nullptr) delete old; }
};
```

Semantics: the alias is resolved per instantiation by the scanner (it depends on `T`), scoped to
the aggregate body (methods included), shadows nothing outside, and folds `*` / `[N]` suffixes
like the file-scope form. `sizeof`, `is_pointer(P)`, `is_interface(P)` answer for the resolved
type.

## What it saves in unique.cb

Six of nine arms collapse: field, constructor, `get`, `valid`, `operator!`, `release`,
`reset`. Three stay because they differ in semantics, not spelling: the destructor's
`unique<Foo*>` rejection, `operator->` (pointer-only; an interface value already dispatches),
and any site where a fat interface value and a pointer need different `move` handling.

## Alternatives considered

- Generic file-scope alias `using PtrOf<T> = T*;` - still needs `if const` per use, does not
  reach the interface case, and adds a second generic mechanism.
- Compiler-side uniformity: let an `is_interface(T)` value accept `T*`-shaped member syntax in
  the wrapper. Helps core only, not user code, and hides the representation choice.
- Leave as is: the arms are correct today; the cost is duplication and review surface in core.

## Acceptance

- The three repro forms compile; the struct-scope alias resolves per instantiation for both an
  interface `T` and a struct `T`.
- `cflat/core/unique.cb` rewritten on the alias with the arm count reduced as listed above;
  `Test/test_move.cb` unique legs and every `Test/errors/err_unique_*.cb` unchanged and green.
- Both `ParseDeclarationSpecifiers` copies see the alias (scanner registers it before it walks
  fields; main pass resolves it identically).
- Alias survives the `--init` cache round trip (it is derived from the instantiation, so it must
  be recomputed or serialized with StructData).
- A `using` at struct scope that names an unknown type or a second pointer level beyond the
  documented maximum reports the same LogError the file-scope form does.
