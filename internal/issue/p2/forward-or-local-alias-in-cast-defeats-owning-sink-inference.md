# A cast spelled through a FORWARD or FUNCTION-LOCAL `using` alias still misses owning-sink inference

Filed 2026-08-10 by `fix/aliascast`, which closed every other spelling of
`p1/alias-spelled-redundant-cast-defeats-owning-sink-inference.md` (that file was deleted by the
same commit). These are the two cells it left open, deliberately: both need the alias to be
INVISIBLE to the ForwardRefScanner at the moment it scans the function.

Severity: double free (abort; measured rc 133 as a native exe - macOS malloc's double-free
guard - and rc 134 under `--run`) plus a missing caller-side `use of moved variable`
rejection. Narrow: the alias must be declared AFTER the function, or inside the function body.

## Repro

`scratch/ac_c6_alias_after_fn.cb` - the alias is declared after the callee:

```cflat
struct Res  { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
void f(UBox p) { UBox o = (UB)p; }        // aborts: double free
using UB = UBox;                          // ... because this comes later
extern int main() { UBox a = umk(5); f(a); return 0; }
```

`scratch/ac_c19_local_alias_decl.cb` - the alias is declared in the body:

```cflat
void f(UBox p) { using UBL = UBox; UBox o = (UBL)p; }   // aborts: double free
```

Measured rc 133 (native exe) on BOTH the merge base and `fix/aliascast`. Move the `using` above the
function and the same program is rc 0 on `fix/aliascast` (`ac_c1_alias_cast_only.cb`).

## Root cause

`AllWrapperTypesName` now canonicalizes both the recorded cast SPELLING and the parameter's
`TypeName` through `CanonicalWrapperTypeName` (namespace resolution, alias fold, generic mangling),
which is what fixed every other spelling. But the CALLER-side half of the sink lives in the
ForwardRefScanner: the main pass re-runs the inference at `ParseFunctionDefinition`, and by then the
alias is registered, but the function symbol was already declared by the scanner, so the main pass's
flags never reach a call site. The scanner walks the file in source order, so:

- a `using` BELOW the function is not yet in `typeAliases` (`RegisterTypeAlias` runs at the
  declaration's own scan), and
- a FUNCTION-LOCAL `using` is a scope-stack entry the scanner never pushes.

`CanonicalWrapperTypeName` therefore returns the spelling unchanged, the intersection misses, and no
sink is inferred. That is the CONSERVATIVE direction (a missed sink, never a false one) and matches
today's behaviour exactly - the callee's semantic half still consumes correctly; only the caller is
not told.

## Fix direction

Either (a) give the ForwardRefScanner a `using`-only pre-pass so every file-scope alias is
registered before any function body is scanned - this closes the forward cell but not the
function-local one - or (b) stop the caller-side decision depending on the scanner at all: let the
main pass UPDATE an already-declared function symbol's parameter sink flags, and make the call site
read the updated flags. (b) is the general fix and also closes any future scanner/main-pass
divergence, but it needs an answer for a call site compiled BEFORE the callee's definition.

Accept-set to hold: a TYPE-CHANGING cast must keep failing the intersection (`wcp_typechanging_*`
in `Test/test_move.cb`, `okTypeChanging` in
`Test/errors/err_wrapper_consume_source_use_after_move.cb`), and every `wcp_alias_*`,
`wcp_generic_*` and `wcp_ns_*` leg in `Test/test_move.cb` must stay green.
