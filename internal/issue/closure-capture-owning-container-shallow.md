# Closure captures of another CAPTURING closure are still shallow (nested-closure gap)

Summary: an escaping closure that captures ANOTHER closure by value is still a SHALLOW copy of the
inner closure's fat pointer into the env - it aliases the inner closure's heap env instead of
cloning it. If the inner closure has its own captures (a non-empty heap env) and the outer closure
outlives the inner closure's defining scope, the captured inner env dangles (use-after-free).

This is the last remaining case. As of 2026-07-03, by-value captures of `string`, owning containers
(`list<T>`, `dictionary<K,V>`), and user structs with owning fields ARE deep-copied correctly into
the env (they behave exactly like `T x = src;` - deep copy via the type's own `copy()`, independent
lifetime, freed once by the closure's cleanup fn). Only nested closures remain shallow.

## Repro (inner closure with a heap env, captured by an escaping outer closure)

```cflat
import "list.cb";
Lambda<int()> gF = default;
extern int main()
{
    {
        int base = 100;
        Lambda<int(int)> adder = (int x) => x + base;   // inner closure OWNS a heap env
        gF = () => { return adder(5); };                // captures adder by value (SHALLOW)
    }   // adder destructed here - frees its env; gF's captured copy now dangles
    printf("%d\n", gF());   // --asan: heap-use-after-free reading adder's freed env
    return 1;
}
```

An inner closure that captures NOTHING (e.g. `(int x) => x + 10`) does NOT repro: its fat pointer
has an empty/null env, so a shallow copy has nothing to dangle. The bug needs the inner closure to
have real captures (a heap env).

## Root cause

Two layers, both deliberately skip `__closure_fat_ptr`:
1. Capture-store deep-copy (`isOwningCap` in the closure-literal emit, MainListener.h): explicitly
   returns false for `cap.TV.TypeName == "__closure_fat_ptr"`, so a nested closure takes the shallow
   load+store branch instead of a deep copy.
2. Capture MODE (`ci.ByReference` in `CollectLambdaCaptures`, MainListener.h): keys off
   `ClosureCaptureDeepCopyable`, which is NOT satisfied for the fat-ptr type, so a nested closure is
   not routed onto the value-deep-copy path either.

Nested closures were excluded on purpose: the closure fat type HAS its own env clone/dtor
(`EnsureClosureLifetimeRegistered` / `__closure_fat_ptr.copy` / `.dtor`), so deep-copying it means
calling that env-clone at capture-store AND marking the invoker's unpacked capture as the closure's
owner. But marking a `__closure_fat_ptr` capture `IsAliasBorrow` spuriously propagates the alias
flag to the closure's CALL RESULTS, which broke a lambda-capturing-lambda case in
Test/test_function_ptr.cb. That interaction must be resolved first.

## Fix direction (when revived)

- Resolve the `IsAliasBorrow`-propagates-to-call-result interaction (why marking the unpacked
  `__closure_fat_ptr` capture as a borrow leaks the flag onto the closure's call results). Only then
  is it safe to treat a nested closure like the other owning captures.
- Once that is fixed: admit `__closure_fat_ptr` in `isOwningCap`, deep-copy it at capture-store via
  the fat type's env clone (`__closure_fat_ptr.copy`, same call shape as `list.copy()`), and let the
  per-closure cleanup fn's CLONE/FREE paths (`GenerateClosureCaptureCleanup`) use that clone and the
  fat type's dtor. The generic per-field copy/dtor dispatch there already works for the container
  and struct cases; nested closures just need to stop being excluded.
- Regression: extend Test/test_function_ptr.cb (testCaptureOwningContainerEscapes) with the repro
  above - an escaping closure capturing a closure that itself captures a heap value - and assert the
  result + `--asan` clean.
