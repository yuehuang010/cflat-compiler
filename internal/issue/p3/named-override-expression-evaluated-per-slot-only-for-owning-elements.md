# The named override expression in an array value-init list runs N times for an OWNING element and once for a POD one

Filed 2026-08-09 by the review of `fix/splatseed`. One spelling, two evaluation counts, chosen by
a property of the element type the source does not mention.

Severity: a surprise, not a miscompile. No crash, no leak beyond the one already recorded in
`p2/brace-override-of-an-owning-field-leaks-the-constructed-value.md`.

## Repro

```cflat
int n = 0;
int g() { n = n + 1; return 100 + n; }
string mk() { string a = "he"; string b = "llo"; return a + b; }
struct Own { string s = mk(); int v = default; };   // owning element
struct Pod { int a = default; int v = default; };   // POD element
extern int main() {
    n = 0; Own scalar = { v = g() };  printf("scalar: n=%d v=%d\n", n, scalar.v);
    n = 0; Own[3] o = { v = g() };    printf("own:    n=%d %d %d %d\n", n, o[0].v, o[1].v, o[2].v);
    n = 0; Pod[3] p = { v = g() };    printf("pod:    n=%d %d %d %d\n", n, p[0].v, p[1].v, p[2].v);
    return 0;
}
```

Measured on `fix/splatseed`:

| cell | g() runs | values |
|------|----------|--------|
| `Own scalar = { v = g() }` | 1 | 101 |
| `Own[3] o = { v = g() }`   | **3** | 101 102 103 |
| `Pod[3] p = { v = g() }`   | 1 | 101 101 101 |

On `c7d5978` all three ran g() once and every slot got 101 - the array rows agreed with the
scalar row, because the whole element was built once and memcpy'd. Construction order is
first-to-last, verified with a counter over `E[4]`.

## Root cause

`MainListener::EmitOwningArrayValueInitSlots` re-runs `EmitFieldInitializer` inside the per-slot
walk, which re-emits the override's expression tree in each slot's body (and inside the runtime
loop, for N > `kMaxUnrolledArrayElements`). The POD arm still evaluates the list once into a seed
and memcpy's it.

Re-application per slot is REQUIRED for an owning override - `Own[3] o = { s = mk2() };` must not
hand three slots the same buffer, and post-fix it correctly gives three - so evaluating once is
not by itself the answer. The fix is to evaluate the override expression ONCE and copy the
resulting value into each slot through the owning-copy path, which would make the owning arm
agree with both the scalar spelling and the POD arm.

## Fix direction

In `EmitOwningArrayValueInitSlots`, hoist each `fieldInit()` value out of the walk: evaluate it
once before the walk into a temp, then per slot assign that temp through `CreateAssignment`
(which already deep-copies an owning value). The per-slot CONSTRUCTOR call must stay inside the
walk - that count moving 1 -> N is the deliberate convergence with `= default`, recorded in the
commit that created this file.

`Test/test_initializer_list.cb` pins the current split in four legs ("override expression runs
per slot" / "runs once"); they are freezes of measured behaviour, not endorsements, and must be
updated together with the fix.
