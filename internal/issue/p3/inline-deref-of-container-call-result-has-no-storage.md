# `(*ls.get(0))(2)` - inline deref of a call result - is rejected for want of Storage

Filed 2026-08-05 by `fix/genfn-lowering`'s coverage matrix. **Pre-existing**, measured IDENTICAL
on master `8c5a860` and on the post-fix binary.

Severity: false rejection with a LOCATED but unhelpful diagnostic. Nothing miscompiles, and a
one-line rewrite works, so this is P3.

## Repro

```cflat
import "function.cb";
import "list.cb";
int triple(int x) { return x * 3; }
extern int main()
{
    function<int(int)> g = triple;
    list<function<int(int)>*> ls;
    ls.add(&g);
    printf("R=%d\n", (*ls.get(0))(2));   // <-- rejected
    return 0;
}
```

Both binaries: `(9,22): Unable to dereference an object without a Storage.` exit 1.

Binding the element to a named local first compiles and prints `R=6` on both binaries:

```cflat
function<int(int)>* e0 = ls.get(0);
printf("R=%d\n", (*e0)(2));
```

That two-line form is the spelling `Test/test_function_ptr.cb` already covers
(`tgp_list_0` / `tgp_list_1`), which is why the container of callable POINTERS is otherwise known
to work.

## Root cause direction - not diagnosed

The deref path wants an addressable `Storage`; a call RESULT is a value in `Primary` with no
alloca behind it, so `*<call>` has nothing to load from. The message states the compiler's
internal precondition rather than the user's problem, and the element type is incidental - check
whether `*someCall()` on a plain `int*`-returning call is rejected the same way before scoping
this as a container issue.

## Fix direction

Either materialize a temporary alloca for a deref of a value-only operand, or reword the
diagnostic to name the construct and suggest the named-local rewrite. The wording fix is the
cheap floor.

Related: [[interface-issue-queue]]
