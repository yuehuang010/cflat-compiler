# Closure / generics monomorphization gaps

Status: DONE (both gaps + dogfood; all gates green). Two string-normalization gaps in generic monomorphization
of closure/function-pointer types. Grammar already parses both (CFlat.g4 typeParameterEntry ->
typeSpecifier -> functionPointerSpecifier). The failures are purely in how type-name strings are
resolved/mangled in MainListener.h (both passes) + LLVMBackend.h.

## Terminology / existing machinery
- `Lambda<R(Args)>` = fat 16B owning closure, internal token `__closure_fat_ptr` (owning value type;
  dtor + copy via EnsureClosureLifetimeRegistered, LLVMBackend.h).
- `function<R(Args)>` = thin 8B bare C fn ptr, token `__c_fn_ptr`, non-owning POD.
- Signature rides on TypeAndValue: IsFunctionPointer + FuncPtrReturnTypeName/FuncPtrReturnPointer +
  FuncPtrParams. IsThinFnPtr() derives from TypeName == "__c_fn_ptr".
- User-code indirect call is [PFX-5] in MainListener.h (~13483): fires when
  namedVar.TypeAndValue.IsFunctionPointer -> CreateIndirectCall.
- Type-arg resolution: ResolveTypeArgEntry (codegen ~1757), ResolveForwardTypeArg (scanner ~1123).
- Both ParseDeclarationSpecifiers copies (scanner ~555, codegen ~1839) have the
  functionPointerSpecifier branch that stores FuncPtr* from raw getText().

## Gap (b) - generic type INSIDE a closure signature (Lambda<list<string>()>)   [FIX FIRST]
The functionPointerSpecifier branches store return/param types via raw getText() ("list<string>"),
never mangling/queueing, so the signature keeps "list<string>" and GetType misses.

Fix: resolve each signature-position typeSpecifier the same way ordinary type positions resolve:
apply activeTypeSubstitutions, and if it is a generic instantiation -> MangledGenericName +
QueueGenericInstantiation, storing the MANGLED base name (list__string). Non-generic types stay
byte-identical. Applied at: codegen ParseDeclSpec fnptr branch, scanner ParseDeclSpec fnptr branch,
BuildFuncPtrAliasType (both copies).

Shared helper: ResolveSigComponent{Codegen,Scanner}(typeSpecifier*, bool& ptr) -> base type name.

## Gap (a) - closure type AS a generic argument (list<Lambda<int(int)>>)   [FIX SECOND, option 2]
ResolveTypeArgEntry/ResolveForwardTypeArg have no functionPointerSpecifier branch; raw getText()
leaks parens/commas/angles into mangled names.

### Encoding (symbol-safe, collision-free, length-prefixed)
BuildEncodedName(isThin, ret, retPtr, params):
  prefix = isThin ? "__thinfn" : "__fatfn"
  + "_" + argc
  + for ret and each param: "_" + len(comp) + "_" + comp   where comp = MangleTypeArg(name + (ptr?"*":""))
Examples:
  1. Lambda<int(int)>            -> __fatfn_1_3_int_3_int
  2. function<void(UiTest*)>     -> __thinfn_1_4_void_9_UiTestptr
  3. Lambda<list<string>()> arg  -> __fatfn_0_12_list__string   (nested generic uses gap-(b) mangle)
Only [A-Za-z0-9_]; distinguishes fat/thin; length-prefixed => collision-free; deterministic.
Canonicalization: built from parsed typeSpecifier components (whitespace-insensitive); param names
ignored (grammar has none); using-aliases + substitutions resolved first via ResolveSigComponent.

### Registry (resolution-site)
encodedClosureTypes_ : encodedName -> TypeAndValue{ IsFunctionPointer, TypeName=__closure_fat_ptr|
__c_fn_ptr, FuncPtr* } (the CALL descriptor).
RegisterEncodedClosureType(encodedName, sig):
  - store sig; EnsureClosureLifetimeRegistered()
  - dataStructures[encodedName]: fat -> StructType = closure fat struct {i8*,i8*}, StructFields =
    closure's, Destructor = closure dtor (=> IsOwningValueType true, container frees envs);
    thin -> StructType = named { i8* } (8B POD), no Destructor.
  This lets GetType(encodedName) + pointer-wrap + array-size + element dtor all "just work" as a
  normal struct value type, so `T* _data`, `new T[]`, `_data[i].~()` in list.cb need no changes.

### Wiring
- ResolveTypeArgEntry + ResolveForwardTypeArg: functionPointerSpecifier arg -> EncodeClosure -> encoded.
- ParseDeclSpec generic-arg loops (both) + ScanUsingDeclaration generic loop: same, only for the
  functionPointerSpecifier case (non-closure args byte-identical).
- [PFX-5] call site: fire also when TypeName is a registered encoded closure; pull the descriptor
  from the registry (fat: call value directly; thin: ExtractValue{0} to bare ptr) for CreateIndirectCall.
- Arg matching (LLVMBackend ~11771) + assignment conversion: accept encoded-fat where
  __closure_fat_ptr is expected and vice-versa (layout identical); thin bare-ptr <-> {i8*} wrap.

### Thin representation decision
Thin `function<>` generic args are POD (per the design). dataStructures[encoded-thin] is a named
`{ i8* }` struct (8B, no dtor). A bare C fn ptr / thin function<> value is wrapped into that struct
at the call arg boundary (LowerByValueArg) and unwrapped (ExtractValue field 0) at the invoke site
([PFX-5]). Descriptor stays thin (TypeName __c_fn_ptr) so CreateIndirectCall uses the thin path.
Fat `Lambda<>` args reuse the closure fat struct verbatim (layout-identical), so no wrap/unwrap.

### Latent bug fixed en route
`move` closure args never unregistered the owned-closure temp (only `move string` did), so ANY
`move Lambda<...>` / `move function<...>` parameter double-freed the env (crash). Fixed in
CreateOverloadedFunctionCall alongside the string case. list.add(move T) relies on this.

### Scoped-out boundaries (documented, not silently wrong)
1. Bare `Lambda` / `function` (no signature) as a generic arg -> hard error ("bare 'function'/
   'Lambda' has no signature; write 'function<R(Args)>' ..."). Fires during the codegen pre-pass
   (ScanAndQueueGenericTypeUses -> ResolveTypeArgEntry), BEFORE expect_error arms, so it is not
   harness-testable via expect_error; verified manually. No err_*.cb added for it.
2. A closure-typed LAMBDA PARAMETER (`(Lambda<int(int)> g) => ...`) is a PRE-EXISTING limitation
   ("unknown type 'Lambda<int(int)>'") independent of these gaps - it fails even with no generics.
   So `list<Lambda<int(Lambda<int(int)>)>>` type-checks/encodes fine, but you cannot write the
   lambda body that takes a closure parameter. Out of scope here (orthogonal lambda-param feature).
   Nested closures in RETURN position and in signature POSITIONS (not as a lambda's own param) do
   compose recursively and work.

## Regression tests
Extend Test/test_function_ptr.cb (existing lambda/function test) with:
  (b) Lambda<list<string>()> local+field+param, returns built list<string>, caller iterates, leak-clean.
  (a) fat: list<Lambda<int(int)>> - 3 capturing lambdas, call via get(i)(x), reassign (old env freed
      once), list destruct frees all envs; --heap-audit clean.
  (a) thin: list<function<int(int)>> named functions, call through.
Error tests under Test/errors/ for any rejected nesting boundary.

## Dogfood (LAST)
core/ui_test.cb reconcileStress: Lambda<string()> expectedKeys (CSV workaround) -> Lambda<list<string>()>.
Update caller example/ui/gallery/gallery_app.cb + docs (doc/UI.md, internal/plan/ui-test-framework.md
T2 deviation 3 addendum "FIXED - now list<string>"). Rebuild release (core file -> deployed copy).
Optional: UiCase wrapper -> direct list<function<void(UiTest*)>> if trivially clean.

## Gates (all GREEN)
1. cmake_build.bat release - clean
2. test.bat Release - all pass (test_function_ptr 51/51 incl. 4 new closure-generics cases)
3. test_lsp.bat - 199/0
4. example.bat Release - 87/0/23 (gallery self-test + leak-clean)
5. cflat.exe --init - exit 0

## Files touched
- cflat/LLVMBackend.h: encodedClosureTypes_ registry; RegisterEncodedClosureType / IsEncodedClosureType
  / GetEncodedClosureType; GetType/dtor "just work" via the dataStructures alias; overload matcher
  accepts encoded closures; LowerByValueArg thin-wrap; move-arg UnregisterOwnedClosureTemp fix.
- cflat/MainListener.h: BuildEncodedClosureName (static); codegen ResolveSigComponentCodegen +
  EncodeClosureCodegen + EncodeClosureFromSig; scanner ResolveSigComponentScanner + EncodeClosureScanner;
  gap-b wiring in both ParseDeclarationSpecifiers fnptr branches + both BuildFuncPtrAliasType;
  gap-a wiring in ResolveTypeArgEntry, ResolveForwardTypeArg, both scanner generic-arg loops,
  ScanUsingDeclaration; [PFX-5] recognizes encoded closures; lambda-arg seeds expected sig from registry.
- Test/test_function_ptr.cb: 4 regression tests (gap b sig-return local/param/field; gap a fat list
  with reassign; gap a thin list; alias unification).
- core/ui_test.cb + example/ui/gallery/gallery_app.cb + doc/UI.md + internal/plan/ui-test-framework.md:
  reconcileStress migrated Lambda<string()> CSV -> Lambda<list<string>()>.
