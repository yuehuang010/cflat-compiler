# An owning struct temp produced inside a lambda body is never destructed (leak)

Filed 2026-08-04 by the round-1 review of `fix/temp-uniq-borrow`, while building the accept set
for the temp-field escape gate. **Pre-existing** - measured identical on master `6e9ab46` and on
the merged fix. Different root cause from the escape family, hence its own file.

Severity: **leak**, not a use-after-free. Nothing is freed early and nothing dangles; the
pointee is simply never released. P2 rather than P1: the program does not lie about a value.

## Repro

```cflat
int dtors = 0;
class Dt { int v = default; ~Dt() { dtors = dtors + 1; } };
struct Box<T> { T t = default; };
Box<unique Dt*> makeDt() { Box<unique Dt*> b = default; b.t = new Dt(); b.t->v = 70; return b; }
extern int main()
{
    Lambda<Dt*()> f = () => makeDt().t;
    Dt* p = f();
    printf("v=%d dtors=%d\n", p->v, dtors);
    return 0;
}
```

```
v=70 dtors=0
```

compile rc 0, run rc 0 on BOTH binaries. Contrast the same body written as a free function -
`Dt* f() { return makeDt().t; }` - which is REJECTED on the merged fix ("cannot store unique
field 'makeDt.t' of a temporary into the return value") and, before it, freed the pointee at the
end of the statement so the caller read freed memory.

## What the two behaviours tell you

The `Box<unique Dt*>` temp inside the lambda body is never registered as an owned struct temp,
or its flush never runs: `dtors` stays 0 even at process exit, so `Box__unique_Dtptr.dtorfull`
is not called at all. That also explains why the escape gate is silent here - the read carries
no `FromOwningTempField`/`OwningTempParent` provenance, because the member-access branch only
sets it when the parent temp IS registered.

So this is not "the guard misses a spelling". It is that a lambda body does not run the
statement-boundary owned-temp machinery the enclosing function would. Any owning value type
returned by call inside a lambda body should leak the same way; the `unique` field is only how
it became visible.

## Fix direction

Establish first whether `RegisterOwnedStructTemp` / `FlushOwnedStructTemps` run at all for a
lambda body's statement boundaries (compile with `--out-lli` and look for the `owntemp` alloca
and the `.dtorfull` call - on the repro above there is neither). If they do not, that is the
bug, and it is a lambda-codegen fix, not an ownership-rules fix. Fixing it will very likely turn
this repro into a use-after-free that the escape gate then rejects, so land both halves
together and expect the value leg to change from `dtors=0` to a rejection.

Check the neighbouring shapes before scoping: an owning-STRING temp in a lambda body, a
`.copy()` result, and a lambda that stores the temp into a captured variable.

Related: [[interface-issue-queue]]
