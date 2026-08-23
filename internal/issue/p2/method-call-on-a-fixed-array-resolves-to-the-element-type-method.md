# `arr.length()` on a fixed array of a struct type resolves to the ELEMENT's method and fails the LLVM verifier

Filed 2026-08-22 from an external report (cflat v0.11.0, quant-backtester project, issue 17).
Reproduced on `master` at the time of filing, macOS arm64 Release.

Severity: internal compiler error surfaced as "module verification failed". No miscompile - nothing
is emitted - but the diagnostic is an LLVM verifier dump instead of a source-level error, and the
underlying overload resolution is admitting a receiver it must reject.

## Repro

```cflat
import "string.cb";
string[3] g_names = { "AAPL", "MSFT", "NVDA" };
int[3]    g_nums  = { 1, 2, 3 };
extern int main()
{
    printf("ints: %d\n", g_nums.length());      // clean front-end error (correct)
    string[3] local = { "A", "B", "C" };
    printf("local: %d\n", local.length());      // verifier failure
    printf("global: %d\n", g_names.length());   // verifier failure
    return 0;
}
```

`int[3]` behaves correctly:

```
17_...cb(13,25): no overload of 'length' matches the given arguments.
  Call arguments (0):
  Candidates (3):
    _length_i32_string_(string self)
    _length_i32_string_viewPtr_(string_view* string_view__)
    _length_i32_stringbuilderPtr_(stringbuilder* stringbuilder__)
```

Comment that line out and the `string[3]` calls reach codegen:

```
Module verification failed:
Call parameter type does not match function signature!
  %0 = load [3 x %string], ptr %local, align 8
 %string = type { ptr, i32 }  %1 = call i32 @_length_i32_string_([3 x %string] %0)
Call parameter type does not match function signature!
  %2 = load [3 x %string], ptr @g_names, align 8
 %string = type { ptr, i32 }  %3 = call i32 @_length_i32_string_([3 x %string] %2)

Error: module verification failed.
Compilation failed.
```

Both a local and a global fixed array reproduce; the storage class is not the discriminator.

## Root cause

Overload resolution for a method call on a fixed-array receiver matches on the array's ELEMENT
type name (`string`) and ignores the array shape, so `string[3].length()` binds
`_length_i32_string_(string self)` and passes the whole `[3 x %string]` aggregate as the `self`
argument. `int[3]` escapes only because `int` has no method named `length` to bind to - the same
bad match would happen for any primitive with a same-named method in scope. The bug is therefore
not "strings are special"; it is that the fixed-array-ness of the receiver is dropped before the
candidate set is filtered.

## Fix direction

Two parts, and the first is required regardless of whether the second is done:

1. **Reject the receiver.** A fixed-array receiver must not match an overload whose `self` is the
   element type. Filter on the receiver's array shape (`ConstArraySize` / `ConstInnerDimensions`
   on `TypeAndValue`) before candidate matching, and produce the same "no overload of 'length'
   matches" diagnostic `int[3]` already gets. That alone turns a verifier dump into a real error.
2. **Optionally, give fixed arrays a length accessor.** The reporter's actual need: `string[30]`
   as a compile-time universe with `.length()`. The size is a compile-time constant, so
   `arr.length()` can fold to that constant, in the same place `sizeof`-style compile-time queries
   are handled. This is a language-surface decision (the maintainer may prefer `sizeof(arr)/
   sizeof(arr[0])`, or a `T[]` view, or nothing at all) - do NOT bundle it with part 1 without
   ratifying it. Part 1 is a straight bug fix; part 2 is a feature.

## Acceptance

- `local.length()` / `g_names.length()` on a `string[3]` produce a source-level "no overload"
  error identical in shape to the `int[3]` one - never a verifier dump - OR, if part 2 is
  ratified, both fold to the constant `3` and the program runs.
- No LLVM module verification failure from any method call on a fixed-array receiver.
- Add the repro to an existing error test (`Test/errors/err_*.cb` via `expect_error`) or, if part 2
  lands, to an existing array test file. Do not create a new test file.
- Full suite green: `./test.sh Release` on macOS/Linux, `test.bat` on Windows.
