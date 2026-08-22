# A lambda cannot capture a stack-local `stringbuilder`/`list` - the documented `process.onStdout`
# accumulator pattern is unusable

Filed 2026-08-21 from an external report (v0.11.0 issue 01). Reproduced on `39d4b38`.

## Repro

```cflat
import "process.cb";
import "string.cb";
extern int main() {
    stringbuilder sb;
    process p;
    p.exe = "/bin/echo";
    p.onStdout = (char* buf, int len) => { for (int i = 0; i < len; i++) sb.appendChar(buf[i]); };
    list<string> args; args.add("hi");
    p.run(args); p.WaitForExit();
    printf("%s", sb.data());
    return 0;
}
```

```
(11,4): bonded value cannot be stored in a struct field or through a pointer - bond lifetime
        would be untrackable
```

The diagnostic is accurate about the mechanism (the capture is stored into the closure's struct
field), but the consequence is that the natural way to consume a `(char*, int)` streaming callback
- accumulate into a local buffer - does not work at all. The reporter's workaround was to make the
accumulator a GLOBAL, which is worse in every respect (not reentrant, not thread-safe) and is what
the current rules push every user of `process.onStdout` toward.

## Why this is p2 and not a design ruling to close

`process.onStdout` is a documented API whose only useful shape is "collect the bytes somewhere".
If capture of a bonded local is never going to be allowed, then `process.cb` needs to offer the
accumulation itself (e.g. an opt-in `p.captureStdout` that fills a `string`/`stringbuilder` owned
by the `process`), so callers never need the capture. Today there is neither the capture nor the
built-in alternative.

## Fix direction (pick one)

1. **Allow the capture when the closure provably does not outlive the local.** A lambda assigned
   directly to a field of a local `process` that is itself a local in the same scope is the
   trackable case; the general case is not. Narrow and hard.
2. **Give `process` a built-in capture.** `p.captureStdout = true;` then read `p.stdoutText` after
   `WaitForExit()`. Cheap, closes the actual use case, no lifetime rules change. **Recommended.**
3. **Document it.** At minimum, `doc/` and the `process.cb` header comment should say the callback
   cannot capture a bonded local and show the supported idiom - today the example implies it works.

## Adjacent

- [[consolidate-named-variable-borrow-provenance]] - the bond/borrow provenance machinery this
  check reads.
