# A move pointer parameter loses its raw-array element count

Filed 2026-08-13 during review of the raw-array runtime-count fix.

Severity: silent partial destruction and resource leak. A move parameter receiving `new T[n]`
destroys only element zero when the callee scope exits.

## Repro

```cflat
int dtors = 0;
class Elem { ~Elem() { dtors++; } };
void sink(move Elem* values) { }

extern int main()
{
    unique Elem* values = new Elem[3];
    sink(move values);
    printf("dtors=%d\n", dtors);
    return dtors == 3 ? 0 : 1;
}
```

Probe: `scratch/rac_25_move_pointer_param_count.cb`.

Measured on Release before and after the local raw-array runtime-state fix: compile rc 0, run rc 1,
`dtors=1`; the probe expects 3.

## Root cause

The pointer parameter ABI carries only the pointer. `ApplyMoveParamTransfer` clears the caller's
pointer and count state, but the callee's synthesized move-parameter slot has no corresponding count
argument or other runtime provenance. Its cleanup therefore uses scalar semantics.

## Fix direction

Design count propagation as part of the ownership-transfer ABI instead of another compiler-side
SSA field. All direct, indirect, generic, interface and synthesized call paths must agree on the
extra state, including null, scalar (`-1`), zero-length, different runtime counts, forwarding and
returning a move parameter, over-alignment, and warm-cache signatures. This is intentionally
deferred from the local-binding fix because a direct-call-only side channel would silently fail for
function pointers and separately compiled call boundaries.
