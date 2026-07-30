# A generic interface 'IFace<T>' is registered as an opaque struct and is unusable in most positions

Filed 2026-07-27 by an interface bug hunt against master `dcb9003`.

Severity: LLVM verifier failure reachable from plain source, plus false rejections with
nonsense diagnostics.

## Repro A - verifier failure on a parameter

```cflat
interface Container<T> { T Get(); void Set(T v); };
class Storage<T> : Container<T> { T data = default; T Get() { return data; } void Set(T v) { data = v; } };

extern int main()
{
    Storage<int> s = default;
    s.Set(42);
    Container<int> ci = s;
    printf("boxed arg: %d\n", sumInt(ci));
    return 0;
}
int sumInt(Container<int> c) { return c.Get(); }
```

Exit 1, expected `42`:

```
Module verification failed:
Call parameter type does not match function signature!
  %3 = load %__iface_fat_ptr, ptr %ci, align 8
 %Container__int = type opaque  %4 = call i32 @_sumInt_int_Container__int_(%__iface_fat_ptr %3)
storing unsized types is not allowed
  store %Container__int %c, ptr %c1, align 1
```

Same family:

- Parameter, function declared first: `Unknown identifier 'Get'.` (false rejection).
- `struct H { Container<int> c = default; };`: `Invalid InsertValueInst operands!`
- `list<Container<int>>`: `'Container__int' does not implement interface 'Container__int'`
  (nonsense diagnostic).
- `move Container<int> mk()` return type: same nonsense self-check.
- Forcing the instantiation with a file-scope global before the function does NOT help,
  so this is not an ordering-only issue.

Control: a LOCAL of generic-interface type works, and a generic class implementing a
NON-generic interface works everywhere.

## Root cause

`ForwardRefScanner::ScanGenericTypeUses`, the `tryPreDeclare` lambda at
`cflat/MainListener.h:1912-1920`:

```cpp
auto tryPreDeclare = [&](const std::string& baseName, CFlatParser::GenericTypeParametersContext* genericParams)
{
    ...
    compiler->CreateStructType(mangledName, {});                      // unconditional
    LLVMBackend::TypeAndValue returnType{ .TypeName = mangledName };
    compiler->CreateFunctionDeclaration(mangledName, returnType, {});
};
```

It fires for EVERY `Base<Args>` type specifier in the tree with no check against
`genericInterfaceTemplates`, so `Container__int` is entered into `dataStructures` as an
opaque named struct (`CreateStructType`'s empty-field branch, LLVMBackend.h:13897).
`InstantiateGenericInterface` (MainListener.h:3522-3569) later also enters it in
`interfaceTable`, so the name lives in BOTH maps.

`GetType` (LLVMBackend.h:15676, `bool isInterface = interfaceTable.count(...)`) checks
`interfaceTable` first, which is why a local resolves correctly. But any signature,
field or element type materialized before `ProcessPendingInstantiations` runs the
interface instantiation gets the opaque struct, and the assignment/boxing paths that
consult `dataStructures` produce the `'X' does not implement interface 'X'` self-check.

## Fix direction

In `tryPreDeclare`, skip the `CreateStructType` / `CreateFunctionDeclaration`
pre-declaration when `genericInterfaceTemplates.count(baseName)` - an interface
instantiation has no struct shell and no default ctor. Route it through
`QueueGenericInstantiation`, which already distinguishes the interface case. Then
ensure pending INTERFACE instantiations are drained before function signatures and
struct layouts are materialized, so `interfaceTable` is authoritative at
signature-build time.

As a backstop, `LogError` if a name ever lands in both `dataStructures` and
`interfaceTable` - that state is always a bug and currently fails far downstream.

## Variance

Identical at `-O0`, `-g`, `-O2`.

## Test plan - BUILD THE TEST FIRST (added 2026-07-29)

This issue is a whole-feature hole, not a single shape, so the accept set has to be pinned
before the fix is written. Work in this order and do not reorder it.

**Step 1 - write a comprehensive STANDALONE test file, before touching the compiler.**

- Location: `scratch/gi/test_generic_interface.cb`. It MUST live in `scratch/`, never in
  `Test/`, while the fix is in progress: `test.bat` / `test.sh` glob `Test/*.*`, so a
  half-red file there breaks the suite for everyone. `scratch/` is gitignored and excluded
  from the globs.
- It is a normal runnable program: `extern int main()` returning 0 on success, with one
  `bool test*()` function per shape family and an assertion helper in the style of
  `Test/test_interface.cb`. Compile and run it as
  `x64/Release/cflat scratch/gi/test_generic_interface.cb -i Test/library -o scratch/gi/gi.exe`
  (also exercise `--run`).
- Every assertion must check a VALUE, not merely that the program compiled. The whole
  failure mode of this family is silent miscompilation into an opaque struct, so
  "it links" proves nothing.
- Shapes to cover, at minimum. The first six are the reported failures; the last three are
  the controls that currently PASS and must not regress:

  | # | Shape | Status on master |
  |---|---|---|
  | 1 | `IFace<int>` as a function PARAMETER, function defined after `main` | verifier failure |
  | 2 | Same, function DECLARED before use | `Unknown identifier 'Get'` |
  | 3 | `IFace<int>` as a STRUCT FIELD (`struct H { Container<int> c = default; };`) | `Invalid InsertValueInst operands!` |
  | 4 | `IFace<int>` as a generic type ARGUMENT (`list<Container<int>>`) | `'X' does not implement interface 'X'` |
  | 5 | `IFace<int>` as a RETURN type, both plain and `move` | same nonsense self-check |
  | 6 | Forced instantiation by a file-scope global before use | does not help; still fails |
  | 7 | LOCAL of generic-interface type | PASSES - control |
  | 8 | Generic class implementing a NON-generic interface, all positions | PASSES - control |
  | 9 | Non-generic interface in all the same positions | PASSES - control |

  Extend beyond the table where it is cheap: more than one type argument (`Pair<int,float>`),
  a non-primitive type argument (`Container<Point>`), a pointer type argument, two distinct
  instantiations of the same template live in one program (`Container<int>` and
  `Container<float>` - this is where a shared-mangled-name bug would show), a class
  implementing TWO generic interfaces, and an interface-to-interface `as` downcast between
  generic instantiations.
- Also record, in a comment block at the top of the file, the exact master behaviour of each
  leg (verifier text / diagnostic / pass). That is the non-vacuity evidence: after the fix,
  every leg must pass, and before the fix, the legs listed as failing must actually fail.
  A leg that passes on BOTH binaries is testing nothing and must be replaced.

**Step 2 - fix the compiler**, per the fix direction above, verifying against the standalone
file the whole way. The fix is not done until every leg of the standalone file passes AND the
current host's full suite is green (`./test.sh Release` on macOS, `test.bat` on Windows).

Watch the accept-set polarity, which is the recurring lesson of this queue: the backstop
`LogError` for "a name landed in both `dataStructures` and `interfaceTable`" must only fire on
a state that is provably that bug. If it can fire on a legal program it is a false rejection,
which is worse than the miscompile it guards.

**Step 3 - merge the standalone test into the existing suite, only once step 2 is green.**

- Positive shapes fold into `Test/test_interface.cb` as one `bool testGenericInterface*()`
  function per family, called from the existing `extern int main()` there. Do NOT add a new
  file under `Test/` - the repo convention is to extend a related file.
- Any shape that is REJECTED by design after the fix (if any) goes to `Test/errors/` as an
  `expect_error` leg. Pin the message substring only, never a path.
- Delete `scratch/gi/` after the merge; the merged assertions are the durable artifact.
- Then delete this issue file and its row in [[interface-issue-queue]] in the same change.
