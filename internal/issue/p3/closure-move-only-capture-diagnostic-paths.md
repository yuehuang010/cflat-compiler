# Move-only closure capture diagnostics differ by C++ conversion path

For a closure that captures `unique<T>` by reference, the direct explicit constructor path reports
that the closure has a move-only capture. The named closure assignment and deduced-callable paths
fall through to the generic by-reference-capture diagnostic instead.

Repro shape:

```cflat
unique<CapturedBox> owned = new CapturedBox();
Lambda<int(int)> f = (int x) => { return x + owned->v; };
std.function<int(int)> g = move f;
```

This reports `a closure that captures by reference cannot be passed to C++ ... capture a pointer
instead`. Replacing the final line with
`std.function<int(int)> g = std.function<int(int)>(move f);` reports the more precise
`a closure capturing the move-only 'owned' cannot be passed ...` message. Passing `move f` to a
deduced C++ callable such as `cppi.apply_copy(move f, 5)` also takes the generic message.

The shared `CxxClosureArgumentRefusal` check recognizes the move-only case by looking up capture
names in the current live-variable frames and checking their type. The named/moved closure paths
retain the by-reference/bond state but do not reliably resolve the original unique capture there,
so they fall through to the generic refusal. Changing only the message would claim move-only for
other by-reference captures too.

Fix direction: preserve enough capture provenance to identify move-only captures after a named
closure is moved or passed through a deduced-callable path, then use the same move-only diagnostic
as the explicit `std.function<...>(move f)` path.

## Separate performance note: owning array capture copy is unrolled

Capture-site deep-copy code emits one copy call per array element while generating IR. Large fixed
arrays therefore produce proportionally large IR and compile-time work. The clone and destruction
helpers already use `EmitFixedArrayElementWalk`; consider using that helper for capture-site copies
as a separate optimization.
