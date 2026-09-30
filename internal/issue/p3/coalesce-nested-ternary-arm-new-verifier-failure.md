# [P3] Coalesce containing nested ternary arm new fails verification

Found 2026-09-29 in E2 round-1 review; pre-existing on master.

Summary: A nested plain ternary in a null-coalescing RHS can generate invalid LLVM IR.

Repro:

```cflat
struct Temp { int v = 0; Temp(int x) { v = x; } };
Temp* tnull = nullptr;
int look(Temp* p) { return p == nullptr ? -1 : p->v; }
int probe(int c) { return look(tnull ?? (c > 0 ? new Temp(3) : tnull)); }
extern int main() { return probe(1) == 3 ? 0 : 1; }
```

Review found the verifier failure on both master and the E2 branch: `Instruction does not dominate
all uses`.

Root cause pointer from review: the inner ternary arm's conditional slot is keyed to the outer arm
block, which does not dominate the outer join or return; the outer flush therefore skips it.

Fix direction: make the nested ternary arm slot dominate the outer coalesce join and ensure its
selected pointer is freed exactly once on both the selected and null paths.
