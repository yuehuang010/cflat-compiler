# HResult `?.`/`?->` chaining leaks the intermediate COM pointer

Created: 2026-07-13 (found while triaging the `--heap-audit` macOS sweep of `Test/*.cb`)

NOT platform-specific. A `[winrt]` class is a pure codegen feature - the compiler
lowers it to a plain struct (vtable ptr + refcount + fields) and emits
QueryInterface/AddRef/Release itself; `com.cb` imports only `guid.cb` and calls no
Win32/ole32 API, so `test_com` builds and runs on any target. The leak below is in
that emitted code and leaks identically on Windows. It surfaced on macOS only because
the macOS sweep was the first time this test was ever run under `--heap-audit`.

## Summary

The `?.`/`?->` "HResult chaining" sugar (`cflat/core/com.cb` `HResult<T>` +
the compiler-side lowering in `MainListener.h` around `winrtSlotHResultType_`)
unwraps an `HResult<Pointer>`'s `.value` to dispatch a further call on it (e.g.
`m->Make(false)?.Value()`), but nothing in the chain ever releases that
intermediate pointer. When the produced object is refcounted (a `[winrt]`
class, released via `->Release()`), and the caller only chains through it to
read a value rather than binding it to a local, the object is allocated and
then never released - a genuine, permanent leak, not a false positive.

Reproduced in `Test/test_com.cb`'s `testChaining()`:

```cflat
HResult<i32> ok = m->Make(false)?.Value();
```

`Make(false)` internally does `r.succeed(new Counter(7)); return r;` - the
`Counter` object (16 bytes: vtable ptr + refcount + one i32 field) is
allocated, `?.Value()` reads `this.total` off it, and the `Counter*` itself
is discarded with the chain expression - there is no local holding it, so
`->Release()` is never called and the object leaks.

Contrast with `testMultiMethodAndParamForwarding()` in the same file, where
the equivalent object IS captured in a local (`Counter* k = new Counter(10);`)
and explicitly released at the end (`k->Release();`, line 170) - proving the
convention in this codebase is "the creator/caller must Release what
`Make()`-style factories hand back," and the `?.` chaining sugar simply gives
the caller no way to do that for the intermediate pointer.

## Repro

`cflat Test/test_com.cb -i Test/library --heap-audit -g -o repro && ./repro`
reports:

```
*** cflat heap-audit: LEAK ptr=... size=16 ***
      #0 operator new
      #1 _Make_HResult__Counterptr_MakerPtrbool_   (Maker::Make, `new Counter(7)`)
      #2 __winrt_Maker_Make                        (compiler-generated vtable slot wrapper)
      #3 main                                      (testChaining, inlined)
```

## Root cause

By design/necessity, refcounted `[winrt]` objects are not tracked by cflat's
ownership system (no `move`/auto-delete - lifetime is managed by hand via
`AddRef`/`Release`, mirroring real COM). The `?.`/`?->` chain lowering
(`MainListener.h`, "HResult `?.`/`?->` chaining" comments around the
`winrtSlotHResultType_` map) unwraps `.value` purely to dispatch the next
call; it has no notion of "this was a temporary owned pointer, release it
after the chain completes." So any chain of the shape
`obj->Factory(...)?.SomeMethod()` where `Factory` returns `HResult<Pointer>`
will leak the produced object UNLESS the caller breaks the chain to bind the
pointer to a local and `Release()` it itself.

This is at minimum a test gap (`testChaining()` should capture and release
the `Counter*` it manufactures), but it also flags a real, permanent design
gap in the `?.`/`?->` sugar: today there is no syntax to chain through a
`Make()`-style call AND release the intermediate object - every future user
of this pattern will leak unless they avoid the chaining sugar entirely for
pointer-returning factories.

## Fix direction

1. Minimal: `Test/test_com.cb`'s `testChaining()` should not chain through
   `Make()` when it needs to discard the pointer - bind it
   (`HResult<Counter*> mc = m->Make(false); ...; mc.value->Release();`) like
   `testMultiMethodAndParamForwarding()` already does.
2. Larger (language gap, needs an internal/plan write-up, not a quick fix):
   decide whether the `?.`/`?->` HResult chain sugar should support an
   opt-in "release the intermediate pointer after the chain" mode, or
   whether it should simply be documented as unsafe to use with
   pointer-typed `HResult<T>` results without also holding a released local -
   i.e. a caveat in `doc/LANGUAGE.md`'s COM/WinRT section (if any) warning
   that `?.` chaining through a factory method leaks the produced object
   unless captured and released separately.
