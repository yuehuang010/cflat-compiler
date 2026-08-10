# A GLOBAL pointer destination does not propagate the borrow taint, so `delete` through it is accepted

P2, PRE-EXISTING. Measured identical on `34cf07e` and on the `fix/assigntaint` branch head: rc 134
(abort under `--run`), one `dtor` printed, no diagnostic. Filed 2026-08-10 by `fix/assigntaint`,
which closed the LOCAL spelling of the same hole.

## Repro (rc 134 on both binaries)

```cflat
int dtorCount = 0;
class Ci { int r = 7; ~Ci() { dtorCount = dtorCount + 1; } };
Ci* g = nullptr;
void f(Ci* p) { g = p; delete g; }
extern int main() { Ci* a = new Ci(); f(a); printf("dtor=%d\n", dtorCount); return 0; }
```

The local spelling (`Ci* d = nullptr; d = p; delete d;`) IS rejected -
`cannot delete 'd' - it aliases borrowed parameter 'p'`.

## Root cause

`ParseAssignmentExpression`'s pointer store tail (`MainListener_Expressions.cpp`) - the whole block
holding `MarkPointerRebound`, `SetJoinKeepsOwner`, and `RecordAssignBorrow` - is gated on
`llvm::isa<llvm::AllocaInst>(namedVar.Storage)`, so a `GlobalVariable` destination records nothing.

## Fix direction

Not simply "widen the gate to globals". A global binding is program-wide and the walk is not
control-flow aware, so a borrow recorded by one function would be read by a `delete` in any later
function - the over-recording that is merely a false rejection for a local becomes a false rejection
spanning the whole program, with no scope exit to retire it. Any fix needs a global-specific
retirement story (or a same-function restriction) before the recording is armed.
