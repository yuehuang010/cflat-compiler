# A `class` with no user-written constructor default-constructs to `undef`, not zero

Filed 2026-08-02, found (and re-measured per reviewer request) while investigating
the global-struct-positional-init family. Unrelated root cause to that family -
this fires with NO brace-list initializer at all - kept as its own item.

Severity: silent miscompile / undefined behaviour. Worse than "one field is
garbage": the emitted IR literally returns `undef` for the whole aggregate,
which is UB to consume (branch on it, compare it, spill it) even before any
field is read, not just "unspecified bits".

## Repro

Measured on the POST binary (`cflat/MainListener.h` at the state after both
rounds of the global-struct-positional-init fix; this path is untouched by
that fix, so the same result holds on `58d5d27`):

```cflat
class C { int a; int b; };
extern int main(){ C lc; printf("a=%d b=%d\n", lc.a, lc.b); return 0; }
```

-> compiles rc 0, runs rc 0, prints a DIFFERENT garbage pair each run, e.g.
`a=1 b=1866740320` then `a=1 b=1868345952`. `b` visibly varies; `a` reads `1`
on every run tried here, which looks deterministic but is undef - not to be
relied on.

The identical spelling with `struct` instead of `class` is correct:

```cflat
struct S { int a; int b; };
extern int main(){ S ls; printf("a=%d b=%d\n", ls.a, ls.b); return 0; }
```

-> prints `a=0 b=0` every time (correct zero-init).

Reproduces identically for `C lc = {};` (empty brace) and for a brace-list
initializer that names every field (`C lc = {a=1,b=2};` still leaves `b`
garbage) - so it is not specific to any one initializer spelling, only to the
`class` keyword with no user-written constructor.

## Root cause (IR-level evidence, not fully traced to a source line)

`--out-lli` on the no-init repro shows:

```
define i32 @main() #0 {
entry:
  %0 = call %C @_C_C__()
  %.fca.0.extract = extractvalue %C %0, 0
  %.fca.1.extract = extractvalue %C %0, 1
  call void (ptr, ...) @printf(ptr nonnull @2, i32 %.fca.0.extract, i32 %.fca.1.extract)
  ret i32 0
}

define internal %C @_C_C__() #0 {
entry:
  ret %C undef
}
```

`MainListener.h`'s local-scope no-initializer branch (~line 9520-9530) calls
`compiler->GetFunction(typeAndValue.TypeName)` and, if found, invokes it via
`CreateOverloadedFunctionCall` as the default constructor; `struct` types with
no constructor leave `GetFunction` empty and fall through to a real zero-init
alloca (`LogWarningContext` + `CreateLocalVariable`'s own zeroing), which is
why the `struct` twin above is correct. A plain `class` with no user-written
constructor apparently still registers a callable zero-arg constructor
(`_C_C__`), but its synthesized body is empty and never stores anything into
the return value, so LLVM's default "no terminator written" fallback (or
whatever emits the final `ret`) produces `ret %C undef` instead of a
zeroed/field-defaulted struct. Not traced further: where in `MainListener.h`
a class's implicit/no-op default constructor body is emitted, and why it
differs from the struct no-constructor path instead of sharing it.

## Fix direction

Not diagnosed to a specific plan. Two shapes to weigh:

1. When a class has no user-written constructor, do not register/call a
   synthesized one at all - fall through to the same zero-init alloca path
   `struct` already uses. Lowest risk if nothing else depends on `_Type_Type__`
   existing as a callable symbol (check generic instantiation and any codegen
   that assumes every class has a constructor function before changing this).
2. Give the synthesized no-op constructor a real body that stores a zero (or
   field-default, matching `GenerateDefaultValue`) aggregate before returning,
   so the existing call-based path becomes correct instead of bypassed.

Whichever direction, sweep for classes with no user-written constructor and a
value use of the default-constructed instance before any field is explicitly
set (i.e. every field-read that is not immediately preceded by an assignment)
in `core/*.cb`, `Test/*.cb`, and `example/*.cb` - this may already be a live
defect wherever code relies on "declare, then assign remaining fields".
