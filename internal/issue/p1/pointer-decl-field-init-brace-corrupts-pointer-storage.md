# A field-init brace list on a POINTER declaration writes through the pointer's own storage

Filed 2026-08-02, found by review while auditing the global-struct-positional-init family's
comments for accuracy (`S* p {a=1};` was cited as an example of a guard it does not actually
reach). Different root cause from that family, and the `=` spelling below is genuinely
pre-existing - but the BARE spelling's value is NOT identical before and after `af68158`: see
"Both spellings, measured separately" below before trusting either as a fixed baseline.

Severity: silent miscompile / memory-unsafe. The declared pointer ends up holding a nonsense
address built from the field-init VALUES (or, pre-`af68158`, an `undef` pointer for the bare
spelling - see below), not a real object address; dereferencing either is undefined behaviour
with no diagnostic anywhere in the pipeline.

## Repro

Primary repro - the `=` spelling, genuinely identical on `58d5d27` and `af68158`:

```cflat
struct S { int a; int b; };
extern int main(){ S* p = {a=1}; printf("p=%p\n", (void*)p); return 0; }
```

-> compiles rc 0, runs rc 0, prints `p=0x1` on BOTH binaries.

`--out-lli` shows why: the whole statement lowers to

```
%0 = alloca ptr        ; p's own storage - space for ONE POINTER, not an S
call ... @llvm.memset...(%0, 0, 8, ...)   ; zeroed
store i32 1, ptr %0    ; field "a" written at offset 0 of P'S OWN SLOT
%1 = load ptr, ptr %0  ; read back as if it were the pointer value itself
```

(effectively - the optimized IR folds it to the constant `inttoptr (i64 1 to ptr)` directly, but
the effect is the same: `p` becomes address `0x1`, not a null pointer, not an address of any
real `S`, and not the uninitialized garbage a plain `S* p;` would leave.) A 3-field struct or a
field with a wider type would spray further into `p`'s 8 bytes; a struct whose FIRST field is
`i64`-sized would overwrite the entire pointer with that field's value.

### Both spellings, measured separately - the bare spelling is NOT a stable baseline

A first draft of this file claimed "identical on both [58d5d27 and 26d1fe2]" using the BARE
spelling as its repro. That claim was wrong - checked by review, corrected here. Measured
directly, 3 runs each, against a fresh `58d5d27` build and the `af68158` state of
`fix/global-positional`:

```
S* p = {a=1};   PRE p=0x1               POST p=0x1               identical - genuinely pre-existing
S* p   {a=1};   PRE p=0x1f7808100 (x3)  POST p=0x1 (x3)          CHANGED by af68158
```

PRE's `--out-lli` for the bare spelling is `call printf(..., ptr undef)` - `fix/global-positional`
had not yet been applied, so the bare spelling's brace list was discarded entirely (the bare-
brace bug that fix closes for the STRUCT/CONTAINER/PRIMITIVE cases); the resulting `p` is
`undef`, stable at `0x1f7808100` across repeated runs on this build but not a value the language
promises. POST, `af68158` routes the bare spelling through the SAME `EmitFieldInitializer` path
the `=` spelling already used, so it now hits this same pointer-corruption bug and produces the
identical deterministic `0x1`.

**Read this correctly: `af68158` is not a new defect class here, and this file is not filed
against that commit.** Making the two spellings agree is the fix's entire point, and an
`undef` pointer is not a "working" baseline to preserve - it is simply a different flavour of
wrong. But the VALUE did change for the bare spelling, so it is recorded in
`fix/global-positional`'s own design record (see `internal/issue/interface-issue-queue.md`) as
one of that fix's side effects, and repeated here so this issue file's repro is not read as
understating what changed.

## Root cause

`EmitFieldInitializer` (`MainListener.h`, ~line 16650) is handed `structPtr` and does
`CreateStructGEP(sd.StructType, structPtr, fieldIdx, ...)` unconditionally - it assumes
`structPtr` is the address of an ACTUAL `S` instance. For a pointer-typed declaration
(`typeAndValue.Pointer == true`), the caller passes `alloc`, which is the address of the
POINTER VARIABLE's own storage (8 bytes holding an `S*`), not an `S`. Nothing gates
`EmitFieldInitializer` on `!typeAndValue.Pointer` before calling it from the local
scalar-declarator path (`MainListener.h` ~line 10098, `else if (!TryEmitContainerInitializer(...))
EmitFieldInitializer(alloc, typeAndValue.TypeName, localInitList);` - the guard added by
`fix/global-positional` for the SIBLING primitive-typed case explicitly excludes pointers from
its own reject, per that fix's design record, because `GetDataStructure(typeAndValue.TypeName)`
looks up by the POINTEE name and finds `S`, so the new guard's `StructType == nullptr` test is
false and it falls through to the pre-existing `EmitFieldInitializer` call untouched).

## Fix direction

Not diagnosed to a specific plan. The direct fix is a `!typeAndValue.Pointer` (or equivalent)
gate before the `EmitFieldInitializer` call in the local scalar-declarator path (and its
`new T{...}` / named-argument callers, if they can reach a pointer-typed target the same way -
not checked), rejecting `S* p {a=1};` with a message naming the real problem ("cannot apply a
struct field initializer to a POINTER declaration; allocate an 'S' first: 'S* p = new S(); p->a
= 1;'"). Sweep for other `EmitFieldInitializer` call sites that might hand it a pointer-to-
pointer `structPtr` the same way before assuming this is the only one.
