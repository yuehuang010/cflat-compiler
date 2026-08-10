# An ALIAS-spelled redundant cast `(UB)p` defeats owning-sink inference and double-frees

Filed 2026-08-10 by `fix/wrapprov`, which closed every other wrapper spelling of
`p1/cast-wrapped-consume-source-defeats-owning-sink-inference.md` (both issue files of that family
were deleted by that commit). This is the one cell it left open, deliberately.

Severity: double free (abort, rc 134), plus a missing caller-side `use of moved variable`
rejection. Narrow: it needs a `using` alias in the cast spelling.

## Repro

`scratch/wp_x1_alias_cast.cb`:

```cflat
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
UBox umk(int n) { UBox b; b.item = new Res(); b.item->id = n; return b; }

using UB = UBox;
void f(UBox p) { UBox o = (UB)p; }                 // rc 134, one free in the callee then abort
extern int main() { UBox a = umk(5); f(a); printf("dtor=%d\n", dtor); return 0; }
```

Spelling the PARAMETER with the alias too (`void f(UB p) { UBox o = (UB)p; }`) fails the same way -
the parameter's recorded `TypeName` is the resolved `UBox`, so `"UB"` still misses. The one spelling
that works is an alias-declared parameter with a resolved cast (`void f(UB p) { UBox o = (UBox)p; }`,
rc 0 on `fix/wrapprov`).

Measured rc 134 / `dtor=1` on BOTH the merge base and `fix/wrapprov` - not a regression of that
fix, the same pre-existing hole under the one spelling its peel cannot reach. The un-aliased
`(UBox)p` twin beside it is rc 0 on `fix/wrapprov` (freed once, by the callee) and rc 134 before.

## Root cause

The fix has two halves and only the SEMANTIC one is alias-aware.

- Semantic: `MainListener::IsRedundantCastOfSource` compares `ResolveTypeAlias(src.TypeName)` with
  `ResolveTypeAlias(dest.TypeName)`, so the callee's consume arm sees straight through `(UB)p` and
  consumes correctly. That half is already right.
- Syntactic: `BareSourceText` records the peeled cast's TYPE SPELLING and
  `ApplyOwningSinkInferenceToBody` admits the peeled name only when every spelling equals the
  parameter's declared `TypeName` (`AllWrapperTypesName`). `"UB" != "UBox"`, so `p` is never made
  an owning sink and the CALLER is never told its argument was surrendered. Both sides then free.

The exact-spelling comparison is deliberate and is the conservative direction (a missed sink,
never a false one), but the collectors run in the ForwardRefScanner where no alias table is
resolvable for the operand.

## Fix direction

Resolve the recorded wrapper spellings through `ResolveTypeAlias` at the point
`ApplyOwningSinkInferenceToBody` compares them - the PARAMETER's type is in hand there, and the
alias table may be queryable at that point in the main pass even though it is not in the scanner.
If it is not, the alternative is to stop the syntactic half from being needed at all: hoist the
caller-side sink decision off the collected TEXT and onto the semantic consume decision the callee
already makes correctly.

Accept-set to hold: a TYPE-CHANGING cast must keep failing the intersection
(`wcp_typechanging_*` in `Test/test_move.cb`, `okTypeChanging` in
`Test/errors/err_wrapper_consume_source_use_after_move.cb`).

## Related

- `internal/fix-issue-lessons.md` - the `fix/wrapprov` digest entry, which carries the full
  eleven-cell coverage matrix and the other two out-of-scope cells (`(string)s`, `(UBox[])v`),
  both of which are hard errors on both binaries and so are NOT bugs.
