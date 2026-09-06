# `int*[2]` passed to an `int*[]` parameter is falsely rejected; the store door accepts it

Bucket: batch mode (one axis of one guard; freeze the pointer-element view corpus first). Filed
2026-09-05 by the review of fix/fixed-array-call-join (f6519ff8); pre-existing on master and
confirmed identical before and after that commit. A false REJECTION, not a miscompile.

## Repro

```
int takeP(int*[] v) { return *v[1]; }
extern int main() { int a = 1; int b = 2; int* cells[2]; cells[0] = &a; cells[1] = &b;
    int*[] pv = cells;          // accepted: store door carries ElemPointer from Pointer
    return takeP(cells); }      // rejected by the raw-pointer axis of the call door
```

Exact message on master f6519ff8 (measured, `--check`):

```
ptrview_param.cb(3,11): cannot pass a raw pointer 'T*' as array-view parameter 'v' ('T[]'): a view must span a whole allocation (it comes only from 'new T[n]' or another 'T[]'); the 'T[] -> T*' conversion is one-way
```

Two defects in one line: the source is a fixed array, not a raw pointer, and the template prints
the placeholder spellings `T*` / `T[]` instead of `int*` / `int*[]`, so the axis that tripped
cannot be read off the message.

The declarator / store door (edbd87e2, `ReshapeFixedArrayAsView` in cflat/LLVMBackend_Lookup.cpp)
reshapes the fixed array into an `int*[]` view with the element star carried into
`ElemPointer`. The call door's raw-pointer axis in `RejectArrayViewParamBinding`
(cflat/LLVMBackend_Overloads.cpp ~800-840) sees `Pointer` set on the argument and rejects it as
"a raw pointer bound to an array view" before the element compare runs. Passing `pv` (the view)
works, so the workaround is a local view.

## Fix direction

In the raw-pointer axis, treat a fixed array whose ELEMENT is a pointer (`ConstArraySize != 0`
and `Pointer`) as an array source, not a raw pointer: reshape first (same helper as the store
door) and let the element compare decide, so `int*[2]` binds and `int*[2]` into `long*[]` or
`int[]` still rejects. Accept-set to freeze: every pointer-element view in Test/, example/,
cflat/core/ (`--check` sweep), `T*[]` generic params fed `T*[N]`, `char*[N]` into `char*[]` and
`string[]` if that conversion exists, `void*[]`, and that a genuine `int*` into `int[]` (single
pointer, no array) keeps the raw-pointer rejection. Legs beside the `Cfq` legs in
Test/errors/err_arrayview_call_arg.cb (the reject rows) and in `runViewFixedArraySources`
(Test/test_core.cb) for the accept row.
