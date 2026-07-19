# Interface value ownership: provenance beyond the scope

Promoted 2026-07-18 from `internal/issue/list-interface-element-copy-imperfect.md` (deleted;
this file supersedes it). Promoted because the container symptom is not fixable in the
container - it needs a language-level answer to what an interface value owns.

## Problem

An interface value is a fat pointer `{ vtable*, data* }` (`__iface_fat_ptr`,
`LLVMBackend.h:7386`). Boxing never copies the object - `CoerceArgToInterface`
(`LLVMBackend.h:8087-8129`) stores the address of whatever storage already exists. So the data
pointer may point at a stack alloca or a heap block, and both are legal and both have static
type `IShape`:

```cflat
Triangle t;  IShape st = t;                 // data ptr -> STACK alloca
Circle* hc = new Circle(); IShape s = hc;   // data ptr -> HEAP block; boxing moves ownership
delete s;                                   // ...and delete through the interface is correct
```

`Test/test_interface.cb:95-106` exercises both.

**At scope level this is decidable.** The compiler sees the boxing site, knows whether the
source was a local or a `new`, and the existing scalar `delete ifaceVar;` path
(`MainListener.h:12522-12525` -> `DeleteInterfaceValue`, `LLVMBackend.h:9613-9645`) already
does the right thing: extract the data pointer, dispatch the vtable dtor slot
(`InterfaceDtorSlotIndex`, `LLVMBackend.h:9571`), free.

**Inside `list<T>` it is not.** Once the value is stored in a container, the boxing site is
gone; only the fat pointer survives, and it records NOTHING about provenance. The container
cannot decide whether to free. Both answers are wrong:

- Do not free -> heap-boxed elements leak.
- Free -> stack-boxed elements pass a stack address to `operator delete`. Heap corruption,
  strictly worse.

Today `list<T>` does the first, by accident rather than decision: every ownership branch keys
off `is_pointer(T)`, a text check for a trailing `*` (`MainListener.h:15591-15611`), which is
false for `"IShape"`. It then falls into the value-type branches, which ALSO no-op, because
`.~()` resolves through `GetOrCreateFullDestructor` (`LLVMBackend.h:2913-2915`) and the array
delete gates on `IsDataStructure` (`MainListener.h:12582`) - both search `dataStructures`, and
interfaces live in `interfaceTable`. The list frees its fat-pointer buffer and nothing else.

## Evidence

CONFIRMED 2026-07-18 (`scratch/iface_leak.cb`, `-o` build; HeapAudit needs a linked exe):

```cflat
HeapAudit.enable();
{
    list<IShape> shapes;
    int i = 0;
    while (i < 3) { Circle* c = new Circle(); c.r = 2; IShape s = c; shapes.add(s); i++; }
}
printf("leaks=%u\n", HeapAudit.reportLeaks());   // leaks=3
```

Three `*** cflat heap-audit: LEAK ***` traces, each pointing at the `new Circle()`.

Independently corroborated: the interface-fields spike hit the same leak and scoped it out -
`internal/plan/interface-fields-feasibility.md:138-141` records "ONE 64-byte leak... a
`list<IFace>` with one `add` and no interface fields anywhere leaks the same 64 bytes." Two
separate investigations reached this from different directions.

## Why the copy behaviour is NOT the bug

`list.cb`'s `else result.add(_data[i].copy())` branch resolves into the bitwise fallback
(`LLVMBackend.h:13930-13944`), which for an interface argument reloads the fat struct by value -
duplicating both pointers, cloning nothing. Downstream of non-ownership that is arguably
CORRECT: two lists of non-owning references aliasing the same objects is what non-owning means.
Do not fix the copy in isolation. It becomes a question only if the answer below is "owning",
at which point a copy needs a real clone and therefore a vtable copy slot every implementor
fills.

## The decision this plan is waiting on

How does an interface value carry ownership, given the boxing site is not available at the use
site? Candidates, none costed yet:

1. **Provenance bit in the fat pointer.** Widens every interface value, or steals a low bit of
   the data pointer. Purely dynamic - a container can then always do the right thing, with no
   type-system change and no new syntax. Cost is paid by every interface value everywhere,
   including the many that never enter a container.
2. **Owning vs borrowed interface types.** An owning interface type distinct from a borrowed
   one, so `list<unique IShape>` is a different instantiation from `list<IShape>` and
   `is_pointer`-style dispatch has something real to test. Fits the existing `unique` field
   work (`internal/plan/field-ownership-unique.md`) and keeps the cost on the declarations that
   opt in. Largest surface: parse, mangling, ForwardRefScanner, overload resolution, and the
   `--init` serializer round-trip.
3. **Restrict containers to explicit pointers.** Require `list<IShape*>`, where `is_pointer` is
   already true and the existing owned-pointer path is already correct, and make bare
   `list<IShape>` a diagnostic. Cheapest by far and immediately stops the silent leak; costs
   expressiveness and needs a decision on what breaks.

Option 3 is worth considering as an interim regardless of which of 1/2 wins - it converts a
silent leak into a compile error, which is the property that matters most today.

## Open questions

- Does the same hole exist in `dictionary<K,V>` / `hashset<T>` with interface elements? Not
  checked - the mechanism (`is_pointer` false, `dataStructures` miss) is shared, so assume yes
  until verified.
- Interface fields (`internal/plan/interface-fields-feasibility.md` F1) hit an adjacent version
  of this: field-store ownership rules there are shape-based, not flag-based, and silently
  leaked until fixed. Whatever provenance answer wins should cover both.
- Does `move` through an interface already imply anything about ownership that could be reused?

## Stages

Not staged yet - blocked on the decision above. Do not start implementation.

## Related

- `internal/plan/field-ownership-unique.md` - the `unique` model, and option 2's natural home
- `internal/plan/interface-fields-feasibility.md` - independent sighting; shape-based ownership
  rules leaking through interfaces
- `Test/test_interface.cb:95-106` - both boxing forms
- `Test/test_collection_leaks.cb` - where a regression test goes
