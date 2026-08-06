# A closure type argument's '[]' suffix, and a closure pointer in a SIGNATURE position, are unvalidated

Filed 2026-08-05 out of the Phase A coverage matrix for
`lambda-pointer-as-generic-type-arg-bypasses-guard`. Both cells measured IDENTICAL on the pre-fix
binary (master `4c06cce`) and the post-fix binary. Low severity: neither is reachable to a real
store or call today, so nothing miscompiles - they are missing diagnostics on shapes that only
compile because they are never used.

The landed fix rejects a FAT `Lambda<T>*` as a generic type argument and supports a THIN
`function<T>*`. It deliberately did NOT touch the two neighbouring suffix/position cells below.

## Cell 1 - array-view '[]' on a closure generic type argument is not validated

```cflat
import "function.cb";
struct Box<T> { T item = default; };
extern int main() { Box<Lambda<int(int)>[]> b = default; printf("ok\n"); return 0; }
```
Compiles and runs on both binaries. The `function<int(int)>[]` spelling likewise.

The DECLARATOR guard rejects the fat case (`Test/errors/err_lambda_array_view.cb`, first leg:
"array-view '[]' is not supported on closure type 'Lambda<int(int)>'"), because an array-view is
a thin `ptr` repr and a fat closure is a by-value struct. As a generic type ARGUMENT the same
spelling is accepted, because `ResolveTypeArgEntry`'s `functionPointerSpecifier` branch returns
the encoded name before the `hasArrayView` suffix is applied - the exact structural twin of the
pointer bug that was fixed, one suffix over.

Not fixed with the pointer case because no probe could get the shape to a store or a subscript:
every use spelling tried failed earlier for an unrelated reason, so the rejection could not be
shown to stand in front of a real defect (the standing rule is that a site added to a reject must
be shown broken from the `--no-opt` IR, not from a probe value alone).

## Cell 2 - a fat closure POINTER inside a closure signature is accepted

```cflat
import "function.cb";
struct Box<T> { T item = default; };
extern int main() { Box<Lambda<int(Lambda<int(int)>*)>> b = default; printf("ok\n"); return 0; }
```
Compiles and runs on both binaries. The inner `Lambda<int(int)>*` is a signature COMPONENT,
resolved by `ResolveSigComponentCodegen` (and its scanner twin), which is a third site that never
consults the fat-closure-pointer guard - distinct from both the declarator guard in
`ParseDeclarationSpecifiers` and the type-argument funnel in `ResolveTypeArgEntry`.

The same signature written as a plain declarator IS rejected on both binaries:
```
int callIt(Lambda<int(int)>* p) { return (*p)(5); }
(2,11): pointer '*' is not supported on closure type 'Lambda<int(int)>'; ...
```

Not reachable to a call today: supplying a matching lambda literal
(`b.item = (Lambda<int(int)>* p) => (*p)(7);`) fails on both binaries with
`unknown type 'Lambda<int(int)>'` - a lambda literal cannot declare a closure-typed parameter at
all. So the accepted declaration is inert until that separate gap is closed.

## Fix direction

Cell 1: apply the closure-suffix validation at the same funnel the pointer case now uses - reject
`[]` on a FAT closure type argument with the existing array-view wording, keep it for the thin
spelling if a working lowering exists (verify from IR first; do not assume it mirrors the pointer
asymmetry).

Cell 2: give `ResolveSigComponentCodegen` the same fat-closure-pointer rejection, so all three
sites agree. Do it together with the lambda-literal-closure-parameter gap, or the diagnostic
guards a shape nobody can write.

Related: [[interface-issue-queue]]
