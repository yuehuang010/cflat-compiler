# A ternary of raw-array allocations loses count and ownership

Filed 2026-08-13 during review of the raw-array runtime-count fix.

Severity: silent resource leak for ordinary control flow. The selected allocation is neither
element-destructed nor freed at the receiving unique local's scope exit.

## Repro

```cflat
int dtors = 0;
class Elem { ~Elem() { dtors++; } };

int run(int choose)
{
    unique Elem* values = nullptr;
    values = choose != 0 ? new Elem[2] : new Elem[4];
    return 0;
}

extern int main()
{
    run(1);
    int first = dtors;
    dtors = 0;
    run(0);
    printf("true_dtors=%d false_dtors=%d\n", first, dtors);
    return first == 2 && dtors == 4 ? 0 : 1;
}
```

Probe: `scratch/rac_24_ternary_raw_array_join.cb`.

Measured on Release before and after the local raw-array runtime-state fix: compile rc 0, run rc 1,
`true_dtors=0 false_dtors=0`; the probe expects 2 and 4.

## Root cause

Each `new Elem[n]` arm has an expression-local count and owned-allocation provenance, but the
conditional-expression join returns only the selected pointer SSA value. It has no value-keyed
count channel pairing the selected pointer with the selected count, and the join's ownership
classification does not make the existing unique destination release it. A single ambient count
or AST-walk flag would repeat the original pointer/count mismatch across the two arms.

## Fix direction

Add value-keyed raw-array result provenance for joins, carrying both ownership and count. The join
must select the count with the same condition as the pointer, then let declaration/assignment store
that selected count in the receiving binding's runtime state. Cover both arms with different runtime
counts, nested joins, null arms, reassignment and declaration initialization, and resource-owning
elements. Do not infer a count from whichever arm was lowered last.
