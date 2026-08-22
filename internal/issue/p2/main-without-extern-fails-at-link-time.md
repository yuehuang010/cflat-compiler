# `int main()` without `extern` compiles clean, then fails in the LINKER with `undefined symbol: _main`

Filed 2026-08-21 from an external report (v0.11.0 issue 11). Reproduced on `39d4b38`.

## Repro

```cflat
int main()
{
    printf("hello\n");
    return 0;
}
```

```
ld64.lld: error: undefined symbol: _main
>>> referenced by the entry point
Error: linking failed (exit 1):
Error: failed to emit executable '...'
```

The front end emits no diagnostic at all. Without `extern`, `main` is not exported, so the entry
point cannot find it - but the user is shown a raw linker error naming a mangled symbol they never
wrote, with no file or line.

This is a first-five-minutes mistake: every other language on the planet takes a bare
`int main()`, and the C-family shape of CFlat invites exactly this. The reporter hit it while
starting a new file.

## Fix direction

Pick one; (1) is the friendlier answer and (2) the more conservative:

1. **Export `main` implicitly.** A function named `main` at file scope with a `main`-compatible
   signature (`int main()` / `int main(int, char**)`) is the program entry point by definition -
   treat it as `extern` whether or not the keyword is written. Nothing else can legitimately want a
   non-exported `main`.
2. **Emit a front-end error.** At end-of-module, if a `main` with an entry-point signature exists
   but is not exported, `LogError` with the function's own location: "entry point 'main' must be
   declared extern". Cheap, precise, and points at the source line.

Either way the raw linker error should never be the user's first signal. If (2) is chosen, also
cover the case where NO `main` exists at all - check what that currently reports.

## Regression test

`Test/errors/err_main_not_extern.cb` is the natural home if (2) is chosen - but note the
`expect_error` harness runs a compile, and this error must fire at the end-of-module resolve for a
compile that is otherwise clean. Verify it fires under `--check` too, not only under `-o`.
