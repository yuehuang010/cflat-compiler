# A `unique` type-argument RETURN is not treated as owned by the caller (leak)

Filed 2026-07-22. Found while reviewing the generic-interface explicit type-arg fix
(the commit that made `VerifyInterfaceMethodContract` compare sink-ness for both
params and returns).

## Summary

The parameter-consumption path is sink-aware, but the return-consumption path is not.
A generic method whose RETURN type is a substituted `unique X*` type argument gets
`ReturnType.IsUniqueTypeArg = true` with `IsMove` left clear (see
`MainListener.h:2993`: `if (substUniqueArg && !declType.IsAlias) declType.IsUniqueTypeArg = true;`).
The caller-side "did this call return an owned value" decision only tests `IsMove`:

- `LLVMBackend.h:10565` - `lastCallReturnsOwned = rt.IsMove && (...)` - `IsUniqueTypeArg` absent.

For contrast, param consumption DOES honor sink-ness:

- `LLVMBackend.h:10184` and `:10198` - both test `IsMove || IsUniqueTypeArg`.

So a call to such a method returns a heap object the caller never frees -> leak.

## Why it matters

This is the RETURN-side counterpart of the param sink-consumption the container work
relies on. The interface CONTRACT check now agrees that a `unique` type-arg return
"owns like move" (params and returns are symmetric after the generic-iface fix), but
CODEGEN at `:10565` does not honor that premise, so a fully-conforming
`IGetter<unique X*>` with a `T get()` returning the owned value leaks.

## Repro (direction - not yet reduced to a minimal expect_error/run test)

```cflat
class R { int id = default; };
interface IGetter<T> { T get(); };
class G : IGetter<unique R*>
{
    unique R* _v = default;
    unique R* get() { return move _v; }   // transfers ownership out
};
extern int main()
{
    G g = default;
    g.get();          // owned R* returned; caller never frees it -> leak
    return 0;
}
```

The generic-iface fix's new test (`Test/test_interface.cb`
`testGenericInterfaceExplicitPtrArg`) covers the PARAM sink path
(`IGSetter<T>::set(T value)`) with a dtor-count free-once assertion, but has no
`T`-returning method, so this return path is uncovered.

## Root cause

`lastCallReturnsOwned` (`LLVMBackend.h:10565`) classifies an owned return by
`rt.IsMove` alone. It must also treat `rt.IsUniqueTypeArg` as owned, mirroring the
param-consumption disjunction at `:10184`/`:10198`.

## Fix direction

Make `:10565` sink-aware: `rt.IsMove || rt.IsUniqueTypeArg` (guarding the same
existing side-conditions). This is NOT a blind one-liner: it changes return-ownership
classification for EVERY `IsUniqueTypeArg` return site, not just interface methods, so
it needs its own regression test (an `IGetter<unique X*>`-style return with a
dtor-count free-exactly-once assertion, plus a check that no double-free is introduced
where an owned return was already being freed via some other path) and a full
`test.sh` + `example_mac.sh` pass. The `--init` serializer already round-trips
`IsUniqueTypeArg` as `"unt"` (`LLVMBackend.cpp`), so no cache change is needed.

## Related

- The generic-interface explicit type-arg fix that surfaced this (the same commit made
  the interface CONTRACT check sink-aware for params and returns; this is the missing
  CODEGEN half on the return side).
- `internal/plan/unique-ownership.md` - the ownership ruling this return path should honor.
