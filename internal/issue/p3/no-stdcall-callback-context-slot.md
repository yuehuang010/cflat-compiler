# A `stdcall` callback has no sanctioned way to carry a context pointer

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 07).
Not independently reproduced; filed on the reporter's account, which is consistent with the
language rules (a `stdcall` function cannot capture, by design).

## Shape

```cflat
EnumWindows(_collectWindow, (i64)0);
```

`EnumWindows` takes a bare code address plus an `lParam`. A `stdcall` function cannot capture, and
there is no supported way to round-trip a CFlat object pointer through the `lParam` and back, so
the callback has to write to a FILE-SCOPE GLOBAL:

```cflat
hashset<u32> _visibleProcessIds;   // written by the callback
```

That is not thread-safe and breaks outright with two concurrent enumerations. The same shape
recurs across the entire Win32 callback surface (`EnumWindows`, `EnumChildWindows`,
`EnumThreadWindows`, `SetWindowLongPtr(GWLP_USERDATA)`, `EnumDisplayMonitors`, timer and hook
procs), so this is not one API's problem.

## Fix direction

What is needed is a sanctioned, documented round-trip for an opaque object pointer through an
integer-width callback parameter:

- a spelling that takes the address of a CFlat object as a `void*`/`i64` without consuming it or
  tripping the ownership analysis (related in kind to
  [[reinterpreting-cast-consumes-owning-pointer]]), and
- the reverse cast INSIDE the callback back to the typed pointer, borrowing.

The lifetime contract belongs in the docs, not in the analysis: the caller guarantees the object
outlives the enumeration. That is the same contract C++ callers work under, and trying to prove it
would run straight into the ratified "unknown ACCEPTS" rule for the guard family.

Check first whether the existing pointer-to-integer cast already round-trips correctly and this is
purely a documentation gap plus an ownership-analysis false positive - if so the fix is small and
this belongs a priority up.

## Regression test

`Test/test_windows.cb` is the natural home: enumerate with a context struct passed through
`lParam`, mutate it from the callback, assert the mutation is visible after the call, with no
file-scope global involved.
