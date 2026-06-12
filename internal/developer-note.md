# Developer notes - standing design decisions

Cross-cutting design intent that is NOT derivable from the code and must survive
refactors. A "clean-up" that violates one of these is a regression, not an improvement.
Language *usage* belongs in `doc/LANGUAGE.md`, not here.

## `char*` / `string` coercion is asymmetric on purpose

`char*` -> `string` is implicit everywhere (call args, `==`, `+`, decl-init); it wraps the
pointer in a NON-OWNING `string` (`strlen` for `_len`, no allocation). `string` -> `char*`
is NEVER implicit - callers must spell `.data()`. Do NOT add an implicit `string` -> `char*`
decay, however symmetric it looks: the outbound direction hands off a borrow whose lifetime
matters (the buffer is owned by the `string`), and the place it matters most is extern C
calls, where a silent decay would hide the hand-off. The inbound wrap is cheap and safe (a
borrow that the language already tracks as non-owning via `IsOwningString == false`); the
outbound decay is not, so it stays explicit. Related invariants, all verified 2026-06-11:

- A `string` wrapped from `char*` is non-owning and is never freed by its own scope exit.
  Moving it into a `list<string>` / `dictionary` is safe because the `move string`
  parameter path in `CreateOverloadedFunctionCall` heap-copies a non-owning argument at the
  call site, so the container owns its copy (the container dtor never frees the borrow).
- `char* == char*` is pointer identity (a raw C type); keep it. Content comparison goes
  through the `string` `operator==` (`memcmp` over `_len`), reached by wrapping one side.
- `(string)charPtr` is allowed (non-allocating borrow); `(string)primitive` is banned
  (hidden allocation - use `value.toString()`). One principle: a `string` cast is legal iff
  it does not allocate. The usage rules live in `doc/LANGUAGE.md` (strings section).

## No type meta system - types are strings

A CFlat type is a plain string (`TypeAndValue.TypeName`) plus a few flat flags
(`Pointer`, `ElemPointer`, `ConstArraySize`, `IsArrayView`, `IsSimd`, `IsNullable`).
This is intentional - keep it simple. Do NOT introduce a structured type-descriptor /
reflection layer, however tempting a refactor looks. Where structure is needed, derive
it at the point of use and fold it into the existing flat flags.

## `using` type aliases - string-only resolution

Consequences of the above for `using X = T` (`typeAliases` in `LLVMBackend.h`):

- Storage stays `unordered_map<string, string>`. The value is the literal RHS string,
  suffixes included ("void*", "float[3]"). Array sizes are constant-folded at
  registration and baked into the string - an alias never holds a live parse node.
  String-shaped storage is also why the JSON ser/deser needs no schema change
  (cross-file aliases round-trip as the literal string).
- Structure is re-derived at the resolution site by peeling suffixes off the resolved
  string (`PeelAliasArrayDims` then `PeelAliasPointerStars` - brackets are outermost,
  e.g. `int*[3]`) and OR-ing onto the flat flags. **Three sites must stay in sync**:
  both `ParseDeclarationSpecifiers` copies (ForwardRefScanner + MainListener) and
  `GetType` (`LLVMBackend.h`). Missing one produces a silent wrong type, not an error.
- Alias pointer depth combines with the declarator's stars; the `Pointer` +
  `ElemPointer` model caps at 2 levels - 3+ is a hard `LogError`, never silent
  truncation. Array dims travel on the carrier fields `AliasArraySize` /
  `AliasInnerDims`; the per-declarator finalization re-applies them as a fallback
  (the size lives on the *type*, shared by all declarators). `Vec3* p` fires the
  `T[N]*` ban explicitly off `AliasArraySize` - the declarator-level check misses it.
- **Intentional asymmetry** (looks like a bug, is not): a bare-identifier RHS that is
  not a known type falls into the *namespace*-alias branch, because a namespace and a
  forward-referenced type name are syntactically identical and erroring there would
  break legitimate forward-referenced namespace aliases. Only a structured RHS
  (generic / pointer / array - forms a namespace cannot take) errors on no-resolve.
- Two-pass rule: edit BOTH `ScanUsingDeclaration` (forward-ref, opportunistic, no
  diagnostics) and `ParseUsingDeclaration` (codegen, authoritative, emits every error).
- Generic RHS instantiation must go through `QueueGenericInstantiation` - a bare
  `CreateStructType` + `CreateFunctionDeclaration` pre-declare leaves an incomplete
  struct layout when the alias is the only mention of the instantiation.
