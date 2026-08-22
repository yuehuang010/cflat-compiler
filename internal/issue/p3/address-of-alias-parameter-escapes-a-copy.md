# Taking `&` of an `alias` parameter yields the parameter binding, not the caller's object

Filed 2026-08-21 while documenting the sanctioned callback-context idiom (retired issue
`no-stdcall-callback-context-slot`). Measured on the current tree, macOS arm64, Release.

## Repro (compiles clean, SEGFAULTs at runtime)

```cflat
import "hashset.cb";

i64 asCallbackContext<T>(alias T value) { return (i64)&value; }
T* fromCallbackContext<T>(i64 context)  { return (T*)context; }

struct EnumCtx { hashset<u32> seen = default; int calls = default; };

extern stdcall i32 onItem(void* handle, i64 lparam)
{
    EnumCtx* ctx = fromCallbackContext<EnumCtx>(lparam);
    ctx.seen.add((u32)(i64)handle);
    ctx.calls = ctx.calls + 1;
    return 1;
}

extern int main()
{
    EnumCtx ctx;
    i64 lp = asCallbackContext<EnumCtx>(ctx);      // address of the PARAMETER, not of ctx
    for (int i = 1; i <= 3; i++) onItem((void*)(i64)i, lp);
    return 0;                                       // exit 139 (SIGSEGV)
}
```

Exit code 139. The pointer-parameter spelling of the same helper is fine:

```cflat
i64 asCallbackContext<T>(T* value) { return (i64)value; }   // called as asCallbackContext<EnumCtx>(&ctx)
```

which runs correctly (`calls=3 size=3`), as does writing `(i64)&ctx` at the call site directly.

## Root cause (hypothesis - not yet confirmed in the codebase)

`alias T` is the borrow-by-reference parameter form, so `&value` inside the callee should be the
caller's object. The measured behaviour is consistent with `&` binding to the callee's local
parameter slot instead - i.e. the alias is materialized into a stack slot and the address-of
takes that slot's address, which dies at return. Confirm against the `alias` parameter lowering
before editing anything.

## Why it matters

It is exactly the shape a library author reaches for when trying to wrap the callback-context
idiom in a helper, and it fails at RUNTIME with no diagnostic. Either `&` on an `alias`
parameter must yield the caller's address, or taking `&` of an `alias` parameter and letting it
escape the callee must be a compile error.

## Related

- `doc/C_INTEROP.md`, "Carrying context through a callback's `lParam` / `userdata`" - documents
  the direct `(i64)&ctx` / `(T*)slot` pair and explicitly says why no core helper wraps it.
- `Test/test_function_ptr.cb`, `testCallbackContextSlot` - the working idiom's regression legs.
