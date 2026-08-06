# A data pointer as a FAT `Lambda<>` field default or parameter default compiles clean and crashes

Filed 2026-08-06 by round-3 review of `fix/assign-gate`. Pre-existing on the base binary
(`68c78fc`), so NOT a regression from the thin assignment gate - the fat twin is rejected by the
accidental struct-storage message at the plain `=`, decl-init, brace field-init, and array
brace-init spellings, but the two DEFAULT-VALUE sites gate only `IsThinFnPtr()` and have no fat
branch, so the fat twin sails through there.

Severity: memory-unsafe accept. Compiles with no diagnostic, then calls a DATA address as code.

## Repro - both spellings, measured on `68c78fc` and on `fix/assign-gate` (identical)

```cflat
import "function.cb";
int q = 3;
void* gvp = &q;
struct D { Lambda<int(int)> f = gvp; };
extern int main() { D d = default; printf("%d\n", d.f(1)); return 0; }
```
-> compile exit 0, run exit 139 (SIGSEGV).

```cflat
import "function.cb";
int q = 3;
void* gvp = &q;
int fatData(Lambda<int(int)> cb = gvp) { return cb(1); }
extern int main() { printf("%d\n", fatData()); return 0; }
```
-> compile exit 0, run exit 138 (SIGBUS).

## Root cause

`ParseFieldDefaultInitializer` (`MainListener_Expressions.cpp` ~5847) and
`GenerateDefaultParamOverloads` (`MainListener_Statements.cpp` ~2312) carry the THIN gate
(`CheckThinFnPtrAssignProvenance`) but no `else` branch calling `WidenToClosureFatChecked` for a
fat destination - which is exactly what `EmitOneFieldInit` does have, and why its fat twin IS
caught. The five other gated sites reject the fat twin only via the pre-existing accidental
"cannot store a pointer value into struct storage" message.

## Fix direction

Add the `WidenToClosureFatChecked` else-branch at both sites, mirroring `EmitOneFieldInit`.
Small and mechanical per the round-3 reviewer, but needs its own accept-set pass first per the
skill: named-fn default, lambda-literal default (NOTE: currently emits invalid IR - see
`internal/issue/p2/lambda-literal-param-default-invalid-ir.md`, same emitter), `nullptr` default,
`Lambda<>`-value default, explicit-cast escape hatch.

## Test coverage

None for these two spellings. Legs belong in `Test/errors/err_data_pointer_to_closure_param.cb`
next to the 11 thin-gate legs.

Related: [[interface-issue-queue]] - see the `fix/assign-gate` landed record's fat-twin note.
