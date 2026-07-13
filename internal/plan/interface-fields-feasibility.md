# Interface fields + is/null/upcast: feasibility spike

Status: SPIKE COMPLETE - working prototype, all four features proven end-to-end.
Date: 2026-07-13
Gates for the spike build: `test.bat` (Release) ALL PASSED, `test_lsp.bat` 204/204,
`example.bat` 89 passed / 0 failed / 24 skipped. No existing test was modified.

Feeds: `internal/plan/ui-interface-refactor.md` (Phase 1).

## VERDICT

| # | Feature | Verdict |
|---|---------|---------|
| F1 | Fields on interfaces, as true lvalues (read, write, NESTED struct write, owned `string`) | **FEASIBLE** - the proposed byte-offset vtable slot works exactly as designed, including the nested case I was asked to doubt |
| F2 | `x is <Interface>` | **FEASIBLE** - trivial (OR-chain of typedesc compares) |
| F3 | `ifaceValue == / != nullptr` | **FEASIBLE** - did NOT work before (the fat struct reached `CreateOperation` as an aggregate); now lowers to a data-pointer compare |
| F4 | Implicit derived-iface -> parent-iface upcast | **FEASIBLE-WITH-CAVEATS** - works for assignment, call arg, and `list<IParent>.add`; the caveat is cost + the two places overload resolution had to learn about it. Return-position upcast is UNVERIFIED (see gaps) |

The prototype's proof program is `spike_iface.cb` (in the session scratchpad, not the repo,
per instructions). Its full output is in the section "Proof output" below.

## The vtable layout I settled on

```
index 0                : typedesc (unique per-class i8 global)   -- unchanged
index 1 .. M           : method0 .. methodM-1                     -- unchanged
index 1+M .. 1+M+F-1   : fieldOff0 .. fieldOffF-1                 -- NEW
index 1+M+F            : fullDtor                                 -- MOVED (was 1+M)
```

A field slot holds `inttoptr(<byte offset of that field in THIS implementor>)`, computed at
vtable-build time from `module->getDataLayout().getStructLayout(structTy)->getElementOffset(i)`.
There is no thunk, no indirect call: `ifaceVal.title` lowers to

```
vtable = extractvalue fat, 0 ; data = extractvalue fat, 1
off    = ptrtoint (load vtable[1+M+k])
addr   = getelementptr i8, data, off
addr   = getelementptr <fieldTy>, addr, 0     ; re-GEP at the field type (see "Surprises")
```

`addr` is a plain lvalue, so reads, writes, nested-struct writes (`b.style.gap = 1`) and
`&`-style uses all fall out of the existing struct-field machinery for free.

### Every place that indexes the vtable (complete list)

| What | Where | Prototype status |
|------|-------|------------------|
| typedesc, slot 0 | `MainListener.h` `LoadTypeDescFromInterface` (~:7954) | unchanged |
| method slot `idx+1` | `LLVMBackend.h` `CallInterfaceMethod` (~:8862) | unchanged (methods still precede fields) |
| dtor slot | `LLVMBackend.h` `DeleteInterfaceValue` | **CHANGED** - now calls the new `InterfaceDtorSlotIndex()`, the single source of truth |
| field slot (new) | `LLVMBackend.h` `EmitInterfaceFieldAddress` | new |
| vtable construction | `LLVMBackend.h` `GetOrCreateVTable` + `GetOrCreateProgramVTable` | both gained `AppendInterfaceFieldOffsetSlots()` |

The dtor-slot move is the regression the parent agent feared most. It is covered:
`Test/test_interface.cb` test 15/16 (dtor-count assertions) still pass, and the spike program
deletes three objects through interfaces and counts exactly 3 destructor calls.

## Files/functions changed in the prototype

All changes are small and additive. Line numbers are post-change.

**`cflat/CFlat.g4`**
- `interfaceDefinition` body: `(interfaceMethod | interfaceField)*`; new rule
  `interfaceField : declarationSpecifiers directDeclarator ';'`. No ambiguity - `directDeclarator`
  cannot consume `(`, so `int kind();` is unambiguously a method and `int kind;` a field.

**`cflat/LLVMBackend.h`**
- `interfaceFields` map (next to `interfaceTable`) + `GetInterfaceFields` / `InterfaceFieldIndex` /
  `InterfaceFieldCount`.
- `CreateInterfaceDefinition(...)` gained a `fields` parameter; parents' fields are flattened
  first, exactly like methods.
- `AppendInterfaceFieldOffsetSlots()` - emits the offset slots, and is where the
  "implementor must have a field of the same name and type" validation lives (LogError).
- `InterfaceDtorSlotIndex()`, `EmitInterfaceFieldAddress()`.
- `RebuildInterfaceFatValue()` / `ReboxInterfaceIfNeeded()` - the F4 upcast (typedesc if-chain),
  with a null-vtable guard so upcasting a failed-cast value stays null instead of faulting.
- Overload resolution: derived-iface arg now matches a parent-iface param (scored 1 = implicit,
  so an exact same-interface overload still wins).
- Arg emission in `CreateOverloadedFunctionCall` and `CallInterfaceMethod`: interface->interface
  args go through `ReboxInterfaceIfNeeded`.
- `NamedVariable::IsInterfaceField` flag (see "Surprises").

**`cflat/MainListener.h`**
- `ScanInterfaceDefinition`, `ParseInterfaceDefinition`, `InstantiateGenericInterface`: parse the
  field list (`ParseInterfaceFields`). Note both `ParseDeclarationSpecifiers` copies are used as-is;
  no type-parsing change was needed, so the two copies did not have to be touched.
- Member access on an interface receiver (`case CFlatParser::Identifier`, the
  `interfaceVar.TypeAndValue.IsInterface` branch): a name that is an interface FIELD now resolves
  to an lvalue; anything else still degrades to a method name as before.
- `GenerateIsCheck`: interface target -> OR-chain (F2).
- `LowerInterfaceNullCompare` called from `ParseEqualityExpression` (F3).
- Decl-init and assignment paths: rebox on a derived->parent interface assignment (F4).
- `destIsStructField` / `srcIsStructField` now also true for an interface-field access, so the
  existing owning-value field-store rules (destruct-before-overwrite, alias reject, by-value
  string deep-copy) apply to interface fields too.

**`cflat/LLVMBackend.cpp`**
- `interfaceFields.clear()` added to both reset paths (next to `interfaceTable.clear()`).

## Ownership: does the owned-`string` field work through an interface?

Yes, but ONLY after one non-obvious fix, and this is the finding I would most want carried into
the production plan.

The field-store ownership rules in the assignment path (`MainListener.h` ~:7191) detect "this
store targets a struct field" **structurally**: `destination` must be a GEP with 2 indices whose
source element type is a struct. An interface field's address is a *byte* GEP, so it failed that
test, and the "destruct the old owned value before overwriting" step was silently skipped ->
**every owning-string overwrite through an interface leaked**. Verified: two 3-byte leaks under
HeapAudit, where the identical raw `obj->title = ...` overwrite leaked nothing.

Fix: an explicit `NamedVariable::IsInterfaceField` flag ORed into `destIsStructField` /
`srcIsStructField`. After that, the interface path is byte-for-byte leak-clean under HeapAudit,
and it also inherits the alias-reject and string-deep-copy diagnostics for free.

Lesson for production: **do not rely on the GEP shape to classify a field store.** Any other place
that sniffs `getNumIndices() == 2` will have the same blind spot for interface fields.

## Proof output (`spike_iface.cb`, compiled with `-o` and run)

Two implementors with deliberately DIFFERENT layouts (`Button` starts with an `int pad`;
`Toggle` starts with the `string`), so a fixed offset could not possibly pass this.

```
read  : Go Flip                                        (expect Go Flip)
method: kind 1 2  clicks 3 9                           (expect 1 2 / 3 9)
bool  : iface 1 0  raw 1 0  pad 7                      (expect 1 0 / 1 0 / 7)
nested: iface 11 22  raw 11 22  width 100              (expect 11 22 / 11 22 / 100)
string: iface Go! Flipped  raw Go! Flipped             (expect Go! Flipped twice)
is    : label-is-IButton 0  button-is-IButton 1  label-is-IElement 1   (expect 0 1 1)
null  : hit!=null 1  miss!=null 0  miss==null 1        (expect 1 0 1)
upcast: assign kind 1 title Go!  arg len 3             (expect 1 Go! 3)
list  : kinds 6 count 3                                (expect 6 3)
dtors : 3                                              (expect 3)
```

Every value is the expected one. HeapAudit reports ONE 64-byte leak, which is the
`list<IElement>` backing buffer - **pre-existing**, reproduced on the unmodified feature set
(a `list<IFace>` with one `add` and no interface fields anywhere leaks the same 64 bytes).
With the list removed, the program is 100% leak-clean.

Diagnostics also work:
```
'Missing' does not implement interface field 'IE::string title'
'WrongTy' field 'title' has type 'int' but interface 'IE' declares it as 'string'
```

## Surprises

1. **`GetTypeFromStorage` reads a GEP's element type.** My first `EmitInterfaceFieldAddress`
   returned `getelementptr i8, data, off`, so every store through it thought the destination type
   was `i8` and tried to cast a `string` aggregate into it ("cannot cast an aggregate value").
   Fix: re-GEP at the field type with index 0 - address-identical, but now the storage type is
   recoverable. Cheap, but completely non-obvious.
2. **The field-store ownership rules are shape-based, not flag-based** (see the ownership section).
   This silently leaked and would have been very easy to ship unnoticed.
3. **`printf("%d", <bool expr>)` prints garbage today** - and this is PRE-EXISTING, not caused by
   this work: `printf("%d", s is ConcreteClass)` with the stock compiler prints e.g. `1421148161`.
   An `i1` is not being promoted for varargs. It cost me 20 minutes of chasing a phantom F1 bug.
   Worth a separate issue; it makes any bool-returning feature look broken in a test print.
4. **The core bitcode cache does not serialize interface fields.** `LLVMBackend.cpp` :4095-4112
   (write) and :4402-4425 (read) round-trip interface name/parents/METHODS only. The cached set is
   exactly runtime.cb's import closure (IAllocator, IList, IString, IJSON, ...). The UI framework's
   interfaces are NOT in that closure (they are user-imported), so **this does not block the UI
   refactor** - but if any interface inside the runtime closure ever gains a field, it will lose it
   on a cache hit. Must be fixed before shipping F1.

## Designs I rejected

- **Accessor-method thunks per field** (the plan's fallback). Rejected: it needs a getter AND a
  setter per field, cannot express `card.style.gap = 1` without a whole nested-lvalue protocol, and
  costs an indirect call per access. The offset slot dominates it on every axis.
- **Relying on the vtable prefix property for the F4 upcast** (reinterpret the derived fat pointer
  as the parent). This is what the compiler does TODAY for interface->interface args, and it is
  only accidentally correct: it already breaks for the SECOND parent of a multi-parent interface,
  and adding field slots breaks it outright (derived = `[td, pM, dM, pF, dF, dtor]`, parent expects
  `[td, pM, pF, dtor]`). I rebuild through the typedesc if-chain instead - correct for
  multi-inheritance too, at O(#implementors of the target).
  I also considered re-ordering slots per inheritance level (`[td, pM, pF, dM, dF, dtor]`) to
  RESTORE the prefix property and make upcasts free. It works for single inheritance and is a
  legitimate future optimization, but it does not fix multi-parent, so I did not take the
  complexity in the spike. If the 23-widget hierarchy stays single-parent, this is the obvious
  perf follow-up.

## What I could NOT get working / did not verify

- **Return-position upcast** (`IElement f() { IButton b = ...; return b; }`) is NOT wired. The
  three sites I hooked are decl-init, assignment, and call args. `CreateReturn`'s interface path
  only boxes a concrete pointer; a derived-interface return value would pass through unre-boxed and
  would read the wrong slots. Straightforward to add (same `ReboxInterfaceIfNeeded` call), but it
  is not in the prototype and it is on the UI plan's path (Phase 5 factories).
- **Field access on a NULL interface value segfaults** (null vtable -> the offset-slot load faults).
  This is consistent with today's behavior for a METHOD call on a null interface value, so it is
  not a regression, and F3 is exactly the guard that prevents it - but there is no `?.` support for
  interface values, so the UI code must null-check before every reach-through, as it does today.
- **Validation is lazy.** A missing/mismatched field is only reported when a vtable is actually
  built (i.e. at the first boxing site), so a class that declares `: IFace` but is never boxed goes
  unchecked, and the error points at the boxing line, not the class. For production this check
  should run eagerly at class-definition time and point at the class.
- **Programs (`program` construct) implementing an interface with fields** compile through the same
  new code path (`GetOrCreateProgramVTable`) but were not exercised.
- **LSP**: interface fields are not registered with the symbol index, so no hover/go-to-def on them.
  `test_lsp.bat` is green (nothing regressed), but the feature is invisible to the IDE.
- Generic interfaces with fields (`interface IBox<T> { T value; }`) parse and flow through
  `InstantiateGenericInterface`, but were not tested.

## Complexity estimate to productionize

| Feature | Estimate | Notes |
|---|---|---|
| F1 fields | **M** (~2-3 days) | The mechanism is done and proven. The remaining work is: eager class-time validation with a good message, the cache serialization, LSP symbol registration, return-position rebox, generic-interface fields, `Test/test_interface.cb` + `Test/errors/` coverage. |
| F2 `is IFace` | **S** (~2 hours) | Done as written; just needs tests. |
| F3 null compare | **S** (~2 hours) | Done as written; consider also allowing `if (ifaceVal)` truthiness. |
| F4 implicit upcast | **M** (~1-2 days) | Prototype covers assignment/arg/list. Needs return position, a decision on the prefix-ordering optimization, and care in overload resolution (I scored the upcast as IMPLICIT so exact overloads still win - that ranking deserves its own tests). |

Overall: F1-F4 as a Phase-1 landing is a solid **M/L, roughly a week**, and nothing in it is
blocked or architecturally risky. The UI refactor's central bet (interface fields as lvalues, so
JSX and `card.style.gap = 1` keep working unchanged) is **sound**.
