# Pointer-element array views (`T*[]`) are unimplemented, so `auto v = <T*[N]>` is rejected

Originally filed 2026-07-29 as a SILENT MISCOMPILE covering all `auto x = <fixed array>`
bindings. The non-pointer half was fixed on `fix/array-shape` (`auto` now deduces the array
view `T[]`). This file is RESTORED and narrowed to the surviving half: a POINTER-ELEMENT
fixed array, `T*[N]`.

Severity: FEATURE GAP, not a silent wrong value. The `auto` binding is now a clean,
source-located rejection; nothing miscompiles. Re-ranked P1 -> P2 on that basis.

## Current behaviour

```cflat
struct Foo { int k = 0; };
extern int main(){ Foo f; f.k=7; Foo*[2] a=default; a[0]=&f; auto v = a; return v[0].k; }
```

```
cannot deduce 'auto' from pointer-element fixed array 'Foo*[2]' - the array view 'Foo*[]'
is not supported. Bind one element ('Foo* p = a[i];') or index the array directly.
```

Before the reject this compiled and `v[0].k` read garbage. The interface consequence
(`auto s = gPtr; IShape t = s;` -> raw `Invalid bitcast ... to %__iface_fat_ptr` with no
source location) is closed by the same reject.

## Why it is a gap and not a bug - measured, do not re-derive

- **The EXPLICIT spelling does not work either.** `Foo*[] v = a;` is rejected on `4097959`
  AND on the fix branch by the raw-pointer-to-view gate (`MainListener.h:11220`), because an
  element-star `Pointer` is indistinguishable from a raw-pointer `Pointer` there.

- **`T*[]` COLLAPSES TO `T[]` at parse time, in BOTH `ParseDeclarationSpecifiers` copies**
  (scanner ~1414-1431, codegen ~4004-4023): the declarator star sets `Pointer`, then the
  empty brackets set `IsArrayView` + `Pointer`, and `ElemPointer` is never touched, so the
  two spellings become the same type. This is the load-bearing fact. Probe, which prints
  `r=99` on BOTH binaries - a `T*[]` parameter accepting a plain `int[3]` and indexing it as
  `int`:

  ```cflat
  int first(int*[] v){ return v[0]; }
  extern int main(){ int[3] a = default; a[0] = 99; printf("r=%d\n", first(a)); return 0; }
  ```

- **`new T*[n]` does not parse**, and there are ZERO uses of the `T*[]` spelling in `Test/`,
  `example/` or `core/`.

## The representation is NOT forced

The encoding `{Pointer, ElemPointer, IsArrayView}` is FREE today - nothing produces that
combination - and one element star is the whole requirement, consistent with the documented
2-level pointer cap. All four flags already round-trip in the `--init` serializer. So **no
new `TypeAndValue` field is needed and no serializer work is required**; the "this needs a
new field" hypothesis is dead, do not revive it.

## Fix direction - this is a FEATURE, size it accordingly

1. Stop the collapse in BOTH `ParseDeclarationSpecifiers` copies: when a declarator star is
   followed by empty brackets, encode `{Pointer, ElemPointer, IsArrayView}` rather than
   letting the view assignment overwrite the element star.
2. Then audit EVERY reader of `ElemPointer` and `IsArrayView` before shipping it. This is
   the expensive half and the reason it was not done inside the fix/array-shape commit:
   `internal/fix-issue-lessons.md` records that setting `Pointer` on the function-pointer
   parser branch silently disarmed the `unique`-field shape guard (`!f.Pointer ||
   f.ElemPointer`) and shipped a compiling free of a CODE address, caught only by the third
   review. A new flag combination on a widely-read type has exactly that blast radius.
3. Relax the raw-pointer-to-view gate at `MainListener.h:11220` so a genuine `T*[N]` source
   binds to a `T*[]` view while a raw `T*` still does not.
4. Only then remove the `auto` reject and let the deduction produce `T*[]`.

## Related

[[interface-issue-queue]] - the multi-dimensional axis of the same "which array shapes can
`auto` deduce" question is SETTLED (see the `fix/mdview` design record): `auto` over a
`T[N][M]` now rejects, alongside every `T[][]` spelling.
