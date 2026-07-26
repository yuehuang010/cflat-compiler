# `?.` on an interface local segfaults instead of short-circuiting

Found 2026-07-26 while writing tests for the unique-interface move work (the plan wanted a
"`?.` on a maybe-moved interface local" leg). NOT move-related: the crash reproduces on a
plain null interface local with no `move` anywhere.

## Repro

```cflat
interface INq { int get(); };
class NqImpl : INq
{
    int v = 5;
    NqImpl() { }
    int get() { return v; }
};

extern int main()
{
    INq a = nullptr;
    int x = a?.get();
    printf("x=%d\n", x);
    return 0;
}
```

Observed: segfault (exit 139), no output. Expected: the `?.` short-circuits on the null
receiver and `x` gets the default/null-path value, as it does for thin-pointer receivers.
The non-null case (`INq a = new NqImpl();`) also needs checking once the null path works.

## Root cause (hypothesis, not fully diagnosed)

The null-conditional lowering appears to have no fat-pointer branch: an interface receiver
is a `{vtable, data}` struct value, so the null test either never fires or tests the wrong
thing, and dispatch proceeds through a null vtable/data pointer. The deref sites gate their
MOVE diagnostics on `nullConditionalPending` (e.g. the interface member-access arm in
`cflat/MainListener.h` ~16686), so the parse-side plumbing knows about `?.` on interfaces -
the gap is in the emitted null-check/branch, which likely only handles `isPointerTy()`
receivers. Start from wherever `nullConditionalPending` drives the runtime null test for
thin pointers and add the fat-value case (extract data slot `{1u}`, compare, branch).

## Fix direction

Lower `receiver?.member` on an interface exactly like the thin-pointer form: extract the
data slot, branch on null, and merge the default value on the null path. Then add test legs:
null receiver short-circuits, non-null receiver dispatches, and a maybe-moved
`unique <interface>` receiver via `?.` compiles WITHOUT the cross-block moved-deref
diagnostic (the deref sites already skip the Deref event under `nullConditionalPending`) -
that last leg is the one the unique-interface test pass had to skip
(see internal/plan/unique-interface-move-readable-null.md).
