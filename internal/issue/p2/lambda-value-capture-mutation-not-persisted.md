# Mutating a value-captured primitive in a Lambda does not persist across calls

## Summary

`doc/LANGUAGE.md` (Value capture, ~line 750) says primitives are "copied into the closure at
lambda creation time" - i.e. the closure owns ONE copy. But a mutation of that copy does not
survive the call: every invocation observes the original snapshot again. A counter closure -
the canonical capturing-lambda idiom - silently returns the same value forever. No diagnostic.

Found 2026-09-16 dogfooding the systems surface (`program.onStdout` handlers, which the docs
advertise as "may capture state").

## Repro

`scratch/dogfood/sys/repro_4.cb`:

```c
extern int main()
{
    int n = 0;
    Lambda<int()> bump = () => { n = n + 1; return n; };
    int a = bump();
    int b = bump();
    int c = bump();
    printf("%d %d %d (expect 1 2 3)\n", a, b, c);
    return (a == 1 && b == 2 && c == 3) ? 0 : 1;
}
```

```
x64/Release/cflat scratch/dogfood/sys/repro_4.cb --run
1 1 1 (expect 1 2 3)      exit 1
```

## Observed

`1 1 1`. Each call re-seeds the captured `n` from the creation-time snapshot.

## Expected

Either `1 2 3` (the closure env is the single mutable copy, matching the documented "copied into
the closure" model and C++ `mutable` lambdas), or a compile error on assignment to a
value-captured variable. Silently discarding the write is the worst of the three.

## Notes

- Same behaviour through the `program.onStdout` path (`scratch/dogfood/sys/repro_3.cb`): the
  handler sees `n=1` on both lines and the enclosing frame still sees `n == 0`.
- Reference-captured values (structs, `stringbuilder`, `list<T>`) DO work - the same repro
  accumulating into a `stringbuilder` behaves correctly. Only the by-value primitive/pointer/
  `string` path is affected.

## Fix direction

Look at how the closure environment is materialised at the call site in `MainListener.h` /
`LLVMBackend.cpp`: the symptom is consistent with the captured slot being re-copied from the
outer variable (or from a constant initialiser) on each invocation instead of the lambda body
reading/writing the env slot in place. Decide the semantics first (mutable env vs. reject the
write) - this needs a ruling, since it changes what `doc/LANGUAGE.md` promises.

Main-session note 2026-09-16: repro confirmed on master 24b86c32 (prints 1 1 1). doc/LANGUAGE.md "Value capture" only says outer changes do not reach the copy; it is silent on whether writes to the copy persist across calls. Needs a ruling before the fix: (a) persist like C++ `mutable` (write the local back to env on exit / operate on env directly), or (b) reject assignment to a value-captured variable with a diagnostic. Either way the silent discard is the defect.
