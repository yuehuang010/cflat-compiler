# A LEGAL closure source as a fat `Lambda<>` FIELD default is stored unwidened and crashes

Filed 2026-08-07 during the `fix/fat-default` round (found by its Phase A, scope widened by its
round-1 review). Pre-existing on `28ef745` and unchanged by the provenance gates - this is the
WIDENING half of the field-default story, not the provenance half.

Severity: memory-unsafe accept. Compiles with no diagnostic; the call through the field crashes.

## Repro - both legal source kinds, FIELD site only

```cflat
import "function.cb";
int addOne(int x) { return x + 1; }
struct FNM { Lambda<int(int)> f = addOne; };
extern int main() { FNM s = default; printf("%d\n", s.f(30)); return 0; }
```
-> compile rc 0 (with `-o`), run rc 139 (SIGSEGV).

```cflat
import "function.cb";
int addOne(int x) { return x + 1; }
function<int(int)> gthin = addOne;
struct TF { Lambda<int(int)> f = gthin; };
extern int main() { TF s = default; printf("%d\n", s.f(30)); return 0; }
```
-> compile rc 0, run rc 139. The thin `function<>` VALUE source crashes the same way - the bug is
not specific to named functions.

The PARAMETER-default site is confirmed NOT affected for either source kind: a named-fn or
thin-value `Lambda<>` parameter default forwards through the wrapper's inner call and widens
correctly via the existing call-site path (measured correct values 21 / 61).

## Root cause

`ParseFieldDefaultInitializer` (`MainListener_Expressions.cpp` ~5834) stores the bare code
pointer straight into the two-word fat closure struct with no `{code, null}` wrap, so the runtime
closure-call ABI mismatches. `GenerateDefaultParamOverloads` does not have the bug because it
forwards through `LowerByValueArg` / `WidenToClosureFatChecked` at the call site.

## Fix direction

Widen at the field-default site: route a legal bare/thin source through the same
`WidenBareOrThinToClosureFat` machinery the call-site path uses, then store the fat value.
Accept-set first per the skill: named fn, thin `function<>` value, `Lambda<>` value (works
today - must stay byte-identical), `nullptr` (works today), lambda literal (fails module
verification today, see `internal/issue/p2/lambda-literal-param-default-invalid-ir.md` - must not
change unmeasured). The provenance gate (`CheckFatClosureAssignProvenance`, landed
`fix/fat-default`) sits in the same function and must keep firing for proven-data sources.

## Test coverage

None. Value legs belong next to the closure legs in an existing `Test/` file once the widening
lands; the reject legs for the provenance half are already in
`Test/errors/err_data_pointer_to_closure_param.cb`.

Related: [[interface-issue-queue]] - `fix/fat-default` landed record has the full measurement.
