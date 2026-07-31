# A `move` argument to a '?.' call leaks when the receiver is null

Filed 2026-07-31, introduced by `fix/nullcond-order` (the fix for the P1
`null-conditional-args-eval-order`). Found by the round-2 adversarial review of that fix.
ACCEPTED, not fixed - see "Why this is accepted" below.

## Repro

`scratch/nc_moveleak.cb`. The helper must CONSUME the value; a `sinkMove` that returns the
pointer instead shows no difference on either binary and does not exercise this at all.

```cflat
int allocs = 0;
int frees  = 0;
struct Res { int id = default; Res(int i) { id = i; allocs = allocs + 1; } ~Res() { frees = frees + 1; } };
struct A   { int v = default; int take(int a) { return v + a; } };

int sinkMove(move Res* r) { return r->id; }   // CONSUMES r

extern int main(int argc, char** argv)
{
    A av = A(); av.v = 10;
    A* live = &av;
    A* zed = argc > 99 ? live : nullptr;       // runtime-opaque null
    allocs = 0; frees = 0;
    Res* o1 = new Res(1);
    int r1 = zed?.take(sinkMove(move o1));
    printf("null recv : r=%d allocs=%d frees=%d\n", r1, allocs, frees);
    ...
}
```

Verbatim output, macOS arm64 Release:

```
master 4c2b2d3        null recv : r=0  allocs=1 frees=1
                      live recv : r=12 allocs=1 frees=1
fix/nullcond-order    null recv : r=0  allocs=1 frees=0     <- leaked
                      live recv : r=12 allocs=1 frees=1
```

Only the NULL path differs. The non-null leg is `frees=1` on both.

## Mechanism

The '?.' guard is now emitted before the argument list, so on a null receiver the argument
expression - including the `move` - never executes. Nothing takes ownership at runtime. But
`move o1` is a COMPILE-TIME fact: `o1` is statically marked moved from that point on, so
the enclosing scope emits no cleanup for it either. Neither the callee nor scope exit
frees, and the allocation leaks.

## Why this is accepted rather than fixed

- Move marking is a compile-time fact and "did the move run" is a runtime fact. They cannot
  be reconciled without emitting null-path cleanup for arguments that would have
  transferred ownership - a separate design question with its own blast radius.
- It is strictly better than the behaviour it replaces. Master runs ARBITRARY side effects
  on a null receiver, which is the bug being fixed; a bounded leak on a shape that must
  combine '?.', a null receiver and a `move` argument is the smaller harm.
- It is not memory-unsafe and not observable in-language: reading `o1` after the call is
  rejected identically on both binaries (`use of moved variable 'o1'`), so no program can
  see the difference except through an allocation counter.

## Fix direction

Emit cleanup on the null path for any argument that would have transferred ownership: at
[PFX-nc-struct] / [PFX-nc-iface] in `MainListener.h`, record the owning sources consumed by
the argument list and destruct them in the `nc_null` block before the merge. Two traps: the
sources are evaluated INSIDE `nc_access`, so the `nc_null` block cannot name them (the
cleanup has to hang off the pre-guard values, not the argument results); and the chain
merge is per-CHAIN, not per-link, so a multi-link chain has one `nc_null` for several
argument lists. Needs its own differential corpus sweep - it changes when destructors run.
