# Copying one fixed array into another emits an invalid bitcast

Filed 2026-07-29 while fixing `global-primitive-array-boxed-into-interface`. PRE-EXISTING
and unrelated to that fix: identical on `df32dd8` and on the fix commit. NOT
interface-related - filed here because `internal/issue/` is the only queue.

Severity: hard compile failure with NO source diagnostic. The only output is an LLVM
module-verifier dump, which names an internal value (`%arrptr`) and no source location.
Nothing miscompiles, so this is a diagnostic/feature gap rather than a correctness hole.

## Repro

```cflat
extern int main()
{
    int[3] a;
    a[0] = 1;
    int[3] b = a;
    printf("%d\n", b[0]);
    return 0;
}
```

Both binaries, `-o` and `--check`, exit 1 with:

```
Module verification failed:
Invalid bitcast
  %0 = bitcast ptr %arrptr to [3 x i32]
```

The class-element spelling is not special: this is about the fixed-array VALUE copy, not
about the element type.

## Root cause

Not diagnosed. The decl-init path evidently treats the RHS as a value of the declared
`[3 x i32]` type and casts the decayed `ptr` to the array type, instead of emitting an
element-wise copy or a memcpy of the extent. The brace-init spelling (`int[3] b = {1,2,3}`)
and the view spelling (`int[] b = a`) both work, so only the array-to-array VALUE copy is
affected.

## Fix direction

Decide the language question first: is `int[3] b = a;` meant to be a copy at all? If yes,
lower it as a memcpy of `sizeof(T) * N` (the seed-element loop already used for brace-init
element fill is the nearest existing machinery). If no, reject it with a `LogError` naming
the array types - per the repo rule an unsupported construct must produce a diagnostic,
never an LLVM verifier dump.
