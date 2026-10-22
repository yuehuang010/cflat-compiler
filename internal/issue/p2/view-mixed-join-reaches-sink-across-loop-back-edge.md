# A mixed owned/borrowed view join reaches a move sink across a loop back edge

Found by the T377 r4 review. Pre-existing: master and T377 behave the same.

## Repro

`scratch/repro_keep/box1021/t377_rev/f26_loop_sink_then_rejoin.cb` (also `f23_loop_rejoin_bottom_then_arm.cb`):

```cflat
S[] u = new S[1];
for (int i = 0; i < 2; i++) { sink(move u); u = c ? new S[2] : w; }
```

This compiles on master and on the fix. At argc 1 (`c` false), the second iteration moves the borrowed `w` into the sink and double-frees.

## Root cause

The ownership analysis makes a single pass over the loop body. The mixed join at the bottom of the body only reaches the `move u` sink higher up on the next iteration, so that pass never sees the join at the sink.

T377 refuses the same join when it comes before the sink in straight-line code.

## Fix direction

The reviewer's proposal:

- Per loop frame, record the storages declared outside the loop that a move sink inside the body consumes. The hooks are the loop enter/leave in `LLVMBackend_OwnershipTemps.cpp` (~1219 / ~1225, after T377).
- A later maybe-borrowed join into one of those storages in the same body then gets the same `LogError` as the straight-line case.

To verify: the f23 and f26 repros are refused, and every T377 accept leg stays accepted.
