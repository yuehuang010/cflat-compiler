# `list<int[]>` fails with a diagnostic pointing into list.cb instead of at the argument

Found 2026-09-02 while checking whether the `T[]` noalias contract survives generics (it
does: element reads through `Box<int[]>` fields and plain `int[]` fields keep `!alias.scope`;
`(T[], T[])` tuples work). Diagnostic-quality issue only; the rejection itself is correct.

## Repro

```cflat
import "list.cb";
int main() { list<int[]> l = default; return 0; }
```

Output: `probe.cb(44,52): cannot find the type 'int[]'` - line 44 is list.cb's
`(T*)calloc(...)`, reported against the USER file name.

## Root cause

`T*` with `T = int[]` forms `int[]*` (pointer-to-array-view), an invalid type by the
language rule (doc/LANGUAGE.md "T[]* ... are not valid types"). The body of `list` (and any
core container spelling `T*` / `sizeof(T)`) trips it during instantiation, and the error
surfaces at the substituted line with the wrong file attribution.

## Fix direction

At instantiation, when a substituted `T*` would form `T[]*`, report at the generic ARGUMENT
site: "'int[]' cannot be a type argument of 'list': the body forms 'int[]*' (pointer to
array view), which is not a valid type". Keep user generics that never form `T*`
(`Box<int[]>`) working - they do today and preserve the view's alias scope. Do NOT block
`T[]` as a generic argument in general.
