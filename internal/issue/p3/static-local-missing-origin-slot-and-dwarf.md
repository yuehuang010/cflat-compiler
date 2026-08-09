# A static local has no ownership-origin slot and no DWARF entry

Filed 2026-08-09 by the review of the fix that gave `static` locals their own storage
(module global + run-once initializer). Both gaps are deliberate omissions in that fix's
`CreateLocalVariable` static branch, recorded rather than papered over.

Severity: **P3, tooling only.** No wrong value, no unsafety - `--sanitize=ownership` still
traps, and `-g` still debugs everything else.

## Repro

```cflat
struct N { int v = default; };
int f(int flag)
{
    static N* sp = new N();
    N* lp = new N();
    sp->v = sp->v + 1;
    lp->v = lp->v + 1;
    int r = sp->v + lp->v;
    delete lp;
    return r;
}
extern int main() { printf("%d\n", f(1)); return 0; }
```

`cflat sl_23_origin.cb -g -l out.ll` emits exactly one variable record for the two locals:

```
!13 = !DILocalVariable(name: "lp", scope: !4, file: !3, line: 5, type: !14)
```

There is no `DILocalVariable` and no `DIGlobalVariableExpression` for `sp`, so a debugger
cannot see the static at all. Under `--sanitize=ownership` a trap on a moved-from static
reports its origin as `0,0` ("no origin recorded") instead of the move site; the trap
helper `___cflat_own_trap_void_i32i32i32i32_` reads `originLine == 0` and takes its
generic-null-deref arm.

The origin report IS reachable at run time, and only through a static: the move has to
happen on an EARLIER CALL, which no per-function analysis can see. Deref textually before
the move, so the compile-time check passes, then call twice:

```cflat
struct N { int v = default; };
int consume(move N* q) { int r = q->v; delete q; return r; }
int f()
{
    static N* p = new N();
    int r = p->v;          // fine on call 1; on call 2 `p` is the zeroed static
    consume(move p);
    return r;
}
extern int main() { printf("call1=%d\n", f()); printf("call2=%d\n", f()); return 0; }
```

`cflat sl_23_origin.cb --sanitize=ownership -o x && ./x` prints `call1=0` then traps:

```
ownership violation: null owned pointer dereferenced at 7:12 (moved or freed on an earlier path)
```

That is the generic arm - the trap names the DEREF site but not the move site, because the
static's storage has no origin entry. The equivalent plain-local shape cannot be built:
re-initialization each call means a plain local is never moved-from on entry, and every
single-call shape is rejected at compile time first.

## Root cause

`LLVMBackend_VariablesAndIR.cpp` `CreateLocalVariable`: the `static` branch returns the new
`GlobalVariable` before the two alloca-shaped tails run -

- `if (typeValue.Pointer) CreateOwnOriginSlot(alloc);` - the origin ledger is keyed on the
  storage value, so a global storage simply has no entry.
- the `diBuilder->createAutoVariable` + `insertDeclare` block - `insertDeclare` on a global
  would be wrong (that is `createGlobalVariableExpression`'s job), so it was skipped.

## Fix direction

- Origin slot: `CreateOwnOriginSlot` allocates an entry-block i64 alloca. For a static the
  slot must have the same lifetime as the variable, i.e. a second internal global beside it,
  otherwise a move recorded in call N is invisible in call N+1. Keying stays by storage
  value, so only the allocation site changes.
- DWARF: emit `diBuilder->createGlobalVariableExpression` with the enclosing subprogram as
  the scope (the DWARF shape for a C static local), attached to the `GlobalVariable`, rather
  than an auto-variable declare.
