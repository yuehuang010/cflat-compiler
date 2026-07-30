# An array of `function<T>` silently truncates the enclosing function body

Filed 2026-07-29 while fixing `iface-thin-function-param-no-lowering`. PRE-EXISTING and
unrelated to interfaces or to that fix: identical on `83caa7f` and on the fix branch.

Severity: SILENT MISCOMPILE that reaches the verifier clean. `--check` reports PASS, IR
emission exits 0, and the program traps at runtime (exit 133, SIGTRAP on an `unreachable`).
Most of `main` is simply not emitted. Nothing is reported at any point.

## Repro

```cflat
import "function.cb";
int triple(int x) { return x * 3; }
int quad(int x) { return x * 4; }
extern int main()
{
    function<int(int)>[2] fns;
    fns[0] = triple;
    fns[1] = quad;
    printf("%d %d\n", fns[0](2), fns[1](2));
    return 0;
}
```

```
cflat repro.cb --check     -> "PASS: repro.cb / Checked 1 file(s), 0 failed."   exit 0
cflat repro.cb -l out.ll   -> no diagnostic                                      exit 0
cflat repro.cb --run       -> no output                                          exit 133
```

The whole emitted body:

```llvm
define i32 @main() #0 {
entry:
  %0 = call i32 @_triple_int_int_(i32 2)
  unreachable
}
```

The array declaration, the second element's invoke, the `printf` and the `return` are all
gone, and the first element's invoke was collapsed into a direct call to `triple` whose
result is discarded. The trailing `unreachable` is what raises SIGTRAP.

## Notes

Narrowed:

- A plain `function<int(int)>` variable, a 1-element array, and a 2-element array that is
  assigned but never invoked all work (exit 0). So neither the array type nor closure
  invocation is the trigger on its own.
- ONE element invoked works (`fns[0](2)` alone, exit 0, prints 6). TWO element invokes is
  the trigger, whether in one expression (`printf("%d %d\n", fns[0](2), fns[1](2))`) or in
  separate statements (`int a = fns[0](2); int b = fns[1](2);`) - both exit 133.
- So the failure is in emitting the SECOND element invoke: the first is emitted (and
  constant-folded to a direct call), then the block is terminated and the rest of the body
  is dropped.

## Fix direction

Not investigated. Start by finding who terminates the block: something emits `unreachable`
and stops walking the statement list while processing `function<T>[N]`, rather than
reporting an unsupported shape. Whatever the outcome, per the repo rule an unsupported
construct must produce a `LogError`, never a truncated body: if arrays of closures are not
meant to be supported, reject the declaration outright.
