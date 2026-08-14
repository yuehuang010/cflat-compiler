# A conditional raw-array assignment leaves a non-dominating element count at cleanup

Filed 2026-08-13 during the integrated Q01-Q15 review.

Severity: invalid LLVM IR for ordinary control flow. The Release compiler reaches module
verification and rejects a valid program.

## Repro

```cflat
class Elem { int x = default; ~Elem() { x = 0; } };

int main()
{
    unique Elem* values = nullptr;
    int n = 3;
    if (n > 0)
    {
        values = new Elem[n];
    }
    return 0;
}
```

Probe: `scratch/review_q13_conditional_raw_array.cb`.

Observed on the 2026-08-13 Release build:

```text
Module verification failed:
Instruction does not dominate all uses!
  %3 = sext i32 %2 to i64
  %13 = icmp slt i64 %12, %3
```

## Root cause

`NamedVariable::RawArrayLength` is an `llvm::Value*`. `SetVariableRawNewArray` records the SSA
count produced by the most recently walked assignment. `EmitOwningPtrCleanup` later consumes that
value at scope exit.

For an assignment inside a branch, the count is defined only in that branch and does not dominate
the cleanup block. The metadata is also AST-walk state rather than runtime state: with different
`new T[n]` assignments in two arms, the pointer selected at runtime can be paired with the count
from whichever arm the compiler visited last. Hoisting only the SSA value would fix dominance but
not that pointer/count mismatch.

The same cleanup loop walks elements from zero upward. Explicit `delete[n]` walks from `n - 1`
downward. The two destruction paths should not encode separate element-order rules.

## Fix direction

Treat an owning raw-array binding as runtime state:

- Add companion count storage whose lifetime matches the pointer storage.
- Update pointer and count together on initialization, reassignment, move, and nulling.
- Load the active count in `EmitOwningPtrCleanup`; never retain a branch-local count SSA value in
  `NamedVariable` beyond the lowering site.
- Extract one counted-array destruction helper and use it for both scope cleanup and `delete[n]`,
  including the same null handling and element order.

Extend an existing related test file; do not add a new test file. Cover a conditional assignment,
two arms with different counts, a move followed by source cleanup, and destructor order.

