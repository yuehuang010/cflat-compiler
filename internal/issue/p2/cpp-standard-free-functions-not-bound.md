# Common C++ standard-library free functions are missing after `import cpp`

## Summary

Direct imports of standard headers do not expose several ordinary public free functions
that real C++ code relies on: `std.sort`, `std.make_shared`, `std.to_string`, and
`std.stoi`. The failure is a clear missing-member diagnostic, but the missing surface
forces CFlat users into handwritten algorithms or scratch C++ bridge headers. Found
2026-09-16 during the standard-library dogfood session.

## Repro

Independent minimal lines (each was probed after importing the corresponding header):

```cflat
import cpp "algorithm" cache;
std.vector<int> values = default;
std.sort(values.begin(), values.end(), (int a, int b) => { return a < b; });

import cpp "memory" cache;
std.shared_ptr<int> p = std.make_shared<int>(1);

import cpp "string" cache;
std.string text = std.to_string(1);
int value = std.stoi(text);
```

Observed diagnostics from the minimal probes:

```
word-count.cb(36,4): 'sort' is not a member of namespace 'std'.
shapes.cb(23,36): 'make_shared' is not a member of namespace 'std'.
text.cb(5,24): 'to_string' is not a member of namespace 'std'.
probe-stoi.cb(6,11): 'stoi' is not a member of namespace 'std'.
```

`std.get` and `std.make_pair` are available in other imported headers, so this is not a
general inability to call every free template. For `sort`, the dogfood workaround is a
CFlat selection sort. For `make_shared`, constructing a `shared_ptr` from `new` works in
the tested shape. For `to_string` and `stoi`, a scratch C++ bridge works.

## Root cause (hypothesis)

The system-header path is treated as a template catalog and the public declarations are
registered selectively. These functions are likely omitted by the catalog's declaration
or signature filters, or are left only in transitively included libc++ implementation
headers. The exact filter decision was not traced.

## Fix direction

Expose the public free-function overloads and supported function templates from each
standard-header catalog, including their explicit type/non-type argument shapes and
deferred dependent signatures. Add fixture coverage for `sort`, `make_shared`,
`to_string`, and `stoi`; do not require users to know which internal libc++ header owns a
public declaration.

Verified 2026-09-16: pre-existing, not caused by the same-day system-header in-scope skip in
LLVMBackend_CInterop.cpp (probe `std.sort` / `std.to_string` fails identically with that line
removed). `std.get<1>(...)` and `std.make_shared<T>(...)` work because an explicit template
argument list routes them through the function-template request path; a plain call by name
needs the function to be in the bound surface, and direct standard headers are never a surface.
Fix direction stays: resolve an unknown `std.name(args)` call against the request group by
issuing a function request (the wrapper path) instead of requiring a surface entry.
