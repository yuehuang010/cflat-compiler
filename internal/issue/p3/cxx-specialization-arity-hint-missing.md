Bucket: **p3** (misleading diagnostic; no wrong values).

# A C++ specialization over a namespace-qualified argument prints its RAW `$` mangling

Per the invertible-mangling ruling every user-facing surface demangles. `std.unique_ptr<int>`
does; `std.unique_ptr<cppi.Tracked>` does NOT - it reaches diagnostics as
`std.unique_ptr$cppi.Tracked`.

## Repro (identical on master 137a4bb0 and on fix/cpp-vbool-proxy - NOT a regression)

```cflat
import cpp "cpp_interop_basic.h";
int takeint(int x) { return x; }
extern int main()
{
    std.unique_ptr<cppi.Tracked> p = cppi.make_tracked_ptr(1);
    return takeint(p);
}
```

```
  Call arguments (1):
    [0] std.unique_ptr$cppi.Tracked <unnamed>
```

The same file with `std.unique_ptr<int>` prints `std.unique_ptr<int>`, so the failure depends on
the ARGUMENT being namespace-qualified, not on the base template.

## Root cause direction (not confirmed)

`DemangleType` splits a C++ foreign identity using an arity hint recorded by
`LLVMBackend::RememberCxxMangledArity` (called from `RequestCxxForeignType`). Without a hint the
parser produces two reversible candidates (`std.unique_ptr` alone, and a speculative 1-argument
parse) and gives up, returning the raw name. So the hint is apparently not recorded for this
identity - find which path builds it and record the count there, as
`RequestCxxForeignType` does.

Note: fix/cpp-vbool-proxy already extended `RememberCxxMangledArity` to record every NESTED
template-id in a spelling, which fixed the `std.vector<bool<std.allocator<bool>>>` comma-as-
nesting mis-render. That is a different hole from this one.

## Why it is filed rather than fixed in place

The identity is produced by a path that has to be located first, and the fix has its own
accept-set (which foreign identities get a hint, and whether a missing hint should fall back to
the single speculative parse instead of the raw name).
