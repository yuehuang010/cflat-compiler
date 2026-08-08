# Measured residues of union member destruction

Filed 2026-08-08 as the recorded residue of `fix/union-closure` (the union closure member-call
crash). Residues 1-3 are deliberate trade-offs taken to remove memory-UNSAFE behaviour; each is a
LEAK. Residue 4 is a PRE-EXISTING wrong free that this branch neither creates nor removes - it is
recorded here because it was recorded nowhere, and because it is the reason residues 1-3 exist.

Severity: leak for 1-3, memory-unsafe for 4 (pre-existing, unchanged by this branch).

The rule the branch actually ships (`LLVMBackend::GetOrCreateFullDestructor`,
`cflat/LLVMBackend_CodegenHelpers.cpp`): a union destructs at most its FIRST field, and only when
that is the sole destructible arm. Everything below follows from that rule.

## Residue 1 - a union with TWO destructible arms destructs neither

```cflat
int dcJ = 0;
struct OJ { int t = default; ~OJ() { dcJ = dcJ + 1; } };
union UJ { OJ a = default; OJ b; };
void go() { UJ u = default; u.a.t = 1; printf("t=%d\n", u.a.t); }
extern int main() { go(); printf("dtors=%d\n", dcJ); return 0; }
```
-> compiles rc 0, prints `dtors=0`. Expected: `dtors=1`.
Merge-base `d885513`: does NOT compile ("Invalid indices for GEP pointer type!", a raw verifier
dump with no `file(line,col)`), so this is a leak on a program that previously did not build.

Root cause: a union arm is destructed through `self` (all arms alias at offset 0), which is exact
for ONE destructible arm. With two, both arms name the same bytes and nothing at that site knows
which is live, so running either destructor could free the other's contents (that is residue 4).
The code suppresses member destruction in that case rather than free arbitrarily.

Fix direction: DIAGNOSE it instead of suppressing - "a union may have at most one member with a
destructor". Two placements were considered and neither is free:

- In `GetOrCreateFullDestructor`, where `work` is exact, there is no source location and
  `LogError` throws mid-emission of a synthesized function.
- In `ParseStructDefinition`'s union arm, the location is exact but the destructible-member
  predicate is not: calling `GetOrCreateFullDestructor` there can memoize a wrapper before every
  member type is registered, and a predicate that over-answers is a FALSE REJECTION.

Record-then-resolve (record `{unionName, file, line, col}` at the definition, decide at end of
compile when the tables are complete) is the shape that fits the repo's existing pattern.

## Residue 2 - a destructible arm at index >= 1 is never destructed

```cflat
struct WN4 { int v = default; };
struct WBox4 { unique WN4* p = default; };
union WU4 { i64 i; WBox4 b; };          // destructible arm is index 1
```
-> the `WBox4` arm's `unique` pointer is never released; that memory leaks even when the `b` arm
is the live one.

Merge-base `d885513`: does NOT compile this union at all ("Invalid indices for GEP pointer type!" -
the per-field `CreateStructGEP` walks off the end of the single-slot union body). So the leak
exists only on programs the merge base refused.

Root cause / why it is deliberate: because the merge base never compiled these programs, emitting
a destructor for index >= 1 would be NEW REACHABILITY for the residue-4 wrong free, not parity.
Measured on the intermediate commit `97dc82c`, which did emit it:
`scratch/rev2_w4_idx1_unique.cb` runs rc **134** (`free` of the integer 12345) and
`scratch/rev2_w8_closure_env_poison_idx1.cb` runs rc **139** (wild indirect call through a poisoned
closure env). With the shipped rule both run rc **0**. A leak is acceptable where a wild free is
not, so index >= 1 leaks.

Cost of the rule, measured: `scratch/rev2_w4_idx1_unique.cb` and
`scratch/rev_b2_owning_struct_second.cb` (an owning struct as the SECOND union field) go from
`dtors=1` on `97dc82c` to `dtors=0`. Regression legs `ucm_dtor_arm_index1_suppressed`,
`ucm_wrongarm_unique_survives`, `ucm_wrongarm_closure_survives` and `ucm_wrongarm_no_wrong_free`
in `Test/test_function_ptr.cb` pin this; the whole file aborts rc 134 on `97dc82c`.

Fix direction: same live-arm knowledge residue 1 needs. Index 0 is not special for correctness -
it is special only because it is where the merge base already emitted, so keeping it is parity.

## Residue 3 - overwriting a union closure member leaks the previous env

```cflat
import "function.cb";
union UQ1 { Lambda<int(int)> f = default; int i; };
int go() { UQ1 u = default; int a = 1; int b = 2;
           u.f = (int x) => x + a; u.f = (int x) => x + b; return u.f(1); }
extern int main() { printf("%d\n", go()); return 0; }
```
-> compiles rc 0, prints `3` (correct), and `leaks --atExit` reports 1 leak / 32 bytes: the FIRST
lambda's env block. A single assignment (`scratch/uc_q2_single_arm_no_leak.cb`) is 0 leaks - the
scope-exit destructor frees that one.

Root cause: the closure-assignment path in `MainListener_Expressions.cpp` frees the destination's
old env before overwriting it, gated on the destination being a "known-initialized slot". A union
member is NOT one: every arm names the same bytes, so the current contents may belong to another
arm, and an ODD integer arm read as a closure env looks like an OWNED (tagged) block - the dtor
then loads a cleanup function pointer from `env - 8` and makes a wild indirect call. Measured
before the gate went in (`scratch/rev_h4_odd_env.cb`): compiles rc 0, runs rc 139, no output. The
gate therefore skips the free for a union member and accepts the leak.

Fix direction: this needs the live-arm knowledge residue 1 needs. A tagged/discriminated union is
the real answer; short of that, a narrower version could free only when the destination's previous
value is provably a closure (e.g. the same member was assigned a closure earlier in the same
block), which is a dataflow fact this site does not have.

## Residue 4 (PRE-EXISTING, unchanged) - a live wrong arm at index 0 is freed anyway

When the SOLE destructible arm is field 0 and the program last wrote through a DIFFERENT arm, the
scope-exit destructor runs field 0's destructor over those foreign bytes. This is a wrong free, not
a leak.

```cflat
struct N3 { int v = default; };
struct B3 { unique N3* p = default; };
union U3 { B3 b; i64 i; }               // destructible arm is index 0
// write u.i = 12345, leave scope -> free(12345)
```

Measured identically on the merge base and on this branch:

| probe | `d885513` | this branch |
|---|---|---|
| `scratch/rev2_w3_unique_wrapper.cb` | run rc 134 | run rc 134 |
| `scratch/rev2_w7_closure_env_poison.cb` | run rc 139 | run rc 139 |
| `scratch/rev2_wa_return_wrongarm.cb` | run rc 134 | run rc 134 |

So it is strict parity, not new exposure - which is precisely why field 0 keeps emitting: that
emission is also what makes an owning first member, a union inside a struct, an array of unions and
a union returned by value release at all (`scratch/rev_b1`, `rev_b3`, `rev_c3`, `rev_c4`,
`rev_c2c`, all `dtors` unchanged from the merge base).

One incidental improvement: the merge base typed the field-0 destruct with
`structTy->getElementType(0)`, i.e. the union's `[N x iM]` BODY ARRAY, and so ran the arm
destructor once per array element (twice for a 16-byte union with a closure arm). This branch types
it with the member's own type and runs it once. Same-or-fewer frees for every scalar arm; for an
owning fixed-array arm it now destructs all N elements instead of the merge base's truncated count,
matching the struct oracle (`union { OO[3] arr; i64 q; }` goes `dtors=2` -> `dtors=3`, while
`struct { OO[3] arr; }` is 3 on both - `scratch/rev3_o1_array_oracle.cb`). The counts only diverge
when the element type is smaller than the union body's `i64` stride, and no sub-8-byte type here
owns heap memory, so this adds no wrong free the merge base avoided.

Fix direction: a live-arm discriminant. Until then, destructible union arms are only safe when the
program never writes a different arm - the same precondition C imposes, but unenforced.

## Not in this file

The union default constructor now agrees with its STRUCT oracle in every containment measured
(local, function-static, `const`, `new`, struct field, nested union: initializer applied; global,
file-scope `static`, fixed-array element: initializer skipped). The three skipped containments are
type-agnostic and already tracked - see
[[fixed-array-default-skips-field-initializers]] and
[[global-struct-no-initializer-ignores-field-defaults]]. `struct SD { int i = 5; };` behaves
identically to `union UD { int i = 5; ... };` in all six.
