# A string literal containing `{}` is typed `string` instead of `char*`, breaking overload resolution

Filed 2026-08-02, found while writing a regression leg whose test LABEL happened to contain `= {}`.
Unrelated to the fix that found it; identical on the pre- and post-fix binaries (measured below).

Severity: SILENT MISCOMPILE in the variadic case, false rejection in the overload case - one root
cause with two faces, and the miscompile is the worse one. Filed in `p2/` for the rejection it was
found through; re-rank if the variadic face is worked first.

## Repro A - silent miscompile, no diagnostic (the severe face)

```cflat
extern int main(){ printf("a = {} b\n"); return 0; }
```

Compiles rc 0, links, runs rc 0, and prints binary garbage - the `string` object's bytes are handed
to `printf` where a `char*` is expected. Measured identical on both binaries:

```
                     compile rc   run rc   stdout
master  7f41a15         0           0      \xf0\x60\x51\x01   (address bytes, not "a = { } b")
fix/ptr-fieldinit       0           0      \xf0\x60\x52\x01
```

The dedicated guard for exactly this mistake - `cannot pass 'string' to the variadic '...'`
(`LLVMBackend.h:18105`) - does NOT fire here, so nothing anywhere in the pipeline says a word.
`printf("plain\n")` in the same program prints correctly, so the literal's CONTENT is the only
variable.

## Repro B - false rejection (the face this was found through)

```cflat
import "test_helper.cb";
extern int main(){ int passed = 0; passed += Test("a = {} b", 1, 1); printf("%d\n", passed); return 0; }
```

```
err.cb(2,43): no overload of 'Test' matches the given arguments.
  Call arguments (3):
    [0] string <unnamed>
    ...
  Candidates (7):
    _Test_int_charPtrboolbool_(char* name, bool actual, bool expected)
    ...
```

Argument 0 is reported as `string`, so no `char* name` candidate matches. Removing the braces from
the literal - `"a = b"` or `"a * b"` - compiles and links. Both `"a = {} b"` and `"a {} b"` fail, so
the trigger is the brace pair, not the `=`.

Measured pre/post pair across the `fix/ptr-fieldinit` change (the diff does not touch literals):

```
"a = {} b"   PRE  no overload of 'Test' matches      POST  no overload of 'Test' matches
```

## Root cause

Not diagnosed. A brace pair inside a string literal appears to route it through the interpolation /
`string`-wrapping path, changing the literal's static type from `char*` to `string` before overload
resolution runs.

## Fix direction

Find what inspects literal CONTENT for braces and decide the type from it; a literal's type must not
depend on its characters unless the language actually has interpolation syntax there. If the
interpolation feature is intended, the diagnostic must name the literal, not the call.

Separately, work out why the `string`-into-variadic guard misses this value - the guard is the
existing safety net for repro A and it should have caught it. Whichever way the typing question is
settled, that miss is its own defect and is what makes the current behaviour silent.
