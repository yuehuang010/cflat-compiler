# `unique` on a local array view names the type `Y[][]` (brackets doubled)

Found 2026-10-15 while probing `unique Y[]` (scratch/probe_uarr/a.cb). Diagnostic text only; the refusal itself is correct.

## Repro

```cflat
struct Y { int v = 0; ~Y() { } };
int main()
{
    unique Y[] arr = new Y[3];
    return 0;
}
```

Prints `'unique' on 'Y[][]': array views are not supported - a view does not own its buffer`. Expected `'Y[]'`.
The field form (`struct Holder { unique Y[] arr = default; };`) names `'Holder.arr'` and is fine.

## Root cause

cflat/MainListener_Declarations.cpp ~1458: the format string is `"'unique' on '{}[]': ..."` and the argument is
`SpellType(..., declType)`, which already spells a view as `Y[]`, so the literal `[]` doubles it.

## Fix direction

Drop the literal `[]` from the format string (`'{}'`). Check the sibling at ~398 (`entry->getText()`) spells the same
shape the same way. Extend an existing err_ leg on unique views to match the `'Y[]'` text; the locale catalog regenerates.
