# Container `.copy()` is broken for every element type except primitives and strings

> RESOLVED 2026-07-17 (see the SHIPPED block below) - verified green (test.bat 46/0, example 90/0/27,
> test_collection_leaks 128/128 leak-clean). ONE RESIDUAL GAP remains untracked-elsewhere:
> `list<IShape>.copy()` (interface elements) now compiles but does an imperfect copy when called; no
> caller exists. Keep this file until that residual is either fixed or split into its own issue.

Found 2026-07-16 during the `unique` code review; scope widened 2026-07-16 after investigation.
Pre-existing, unrelated to `unique`. All claims below confirmed by repro.

Supersedes the narrower `list-copy-empties-source-for-owning-elements` framing: the bug is wider
than "owning struct elements", hits three containers, and has two distinct failure modes.

## Summary

| container | element | behavior | severity |
|---|---|---|---|
| `list<T>` | primitive | correct | - |
| `list<T>` | `string` | correct (deep-copies) | - |
| `list<T>` | **`T*`** | **source element NULLED** | silent data loss |
| `list<T>` | **struct with owning field** | **source element MOVED OUT** | silent data loss |
| `dictionary<K,V>` | **struct with owning field** | **both dicts own one buffer** | **CRASH (heap corruption)** |
| `hashset<T>` | same shape as dictionary (untested) | suspected | suspected |

`.copy()` is documented as returning an independent copy. It does not.

## Repros (all in `scratch/`)

**`list<T*>` nulls the source** (`p_srcsurvive.cb`) - and the doc comment is WRONG:

```cflat
list<int*> lp = default; int* q = new int; *q = 3; lp.add(q);
list<int*> cp = lp.copy();
int* h = lp.get(0);        // -> nullptr
```
Output: `ptr   : src_nulled=1 count=1`

`core/list.cb`'s own comment says *"primitive and pointer elements are copied by value (a list of
pointers shares pointees)"*. **False.** Pointer elements are moved out. Do not trust that comment;
it misled the first investigation.

**`list<struct-with-owning-field>` empties the source** (`spike_listcopy2.cb`):

```cflat
struct SBox { string s = default; };
SBox b = default; b.s = "hel" + "lo";
list<SBox> a = default; a.add(move b);
list<SBox> c = a.copy();
// a[0].s len: 5 -> 0     c[0].s len: 5
```

**A `list<string>` test will NOT show any of this** - it takes the `is_string(T)` deep-copy branch and
looks correct. This already fooled one investigation. Use a pointer or struct element.

**`dictionary<K,V>.copy()` CRASHES** (`p_dict.cb`) - the worst of the family:

```
before copy: a[1].s len=5
after  copy: a[1].s len=5  c[1].s len=5     <-- source survives ...
exit=0xC0000374 (STATUS_HEAP_CORRUPTION)    <-- ... because BOTH own it
```

## Root cause

Two different mistakes with the same origin - `copy()` cannot ask "does T have a copy()?", so each
container hardcodes an `is_string(T)` special case and guesses for everything else.

`core/list.cb` (`copy()` and `copy(IAllocator)` - both):
```cflat
if const (is_string(T)) result.add(_data[i].copy());   // correct
else                    result.add(_data[i]);          // add() is `void add(move T value)` -> MOVES
```

`core/dictionary.cb:280-283` uses a plain store instead:
```cflat
result._values[i] = _values[i];    // shallow: source survives, both own the buffer -> double-free
```

`core/hashset.cb:109` has the `is_string(T)` shape too.

## BLOCKED: the fix needs a small compiler change first

The natural fix - route the element copy through overload resolution instead of `is_string(T)` -
**does not compile today**. Implemented and reverted, with evidence:

```cflat
if const (is_primitive(T))    result.add(_data[i]);
else if const (is_pointer(T)) result.add(_data[i]);
else                          result.add(_data[i].copy());
```

This FIXES the reported bugs and keeps int/ptr/string/POD-struct working. But **an enum is neither
`is_primitive` nor `is_pointer`**, so it falls to `.copy()`, which does not resolve for enums -
and the instantiation emits `copy()` whether or not it is called. So merely declaring `list<MyEnum>`
anywhere fails the build with `unknown function '_data'`. That is a far worse regression than the
bug. Verified: `list<Color>` passes at baseline, fails under the change, *without ever calling
`.copy()`*.

The compile-time predicate set is exactly `{is_pointer, is_primitive, is_string}` (`MainListener.h`,
`kIntrinsics`). There is no `is_enum`, `is_struct`, `has_copy`, `typeof` or `nameof`. A generic free
`T copy(T)` in core would inject a global copy overload competing with `string`/`list`/synth copies
in every program importing `list.cb` - a landmine, not a fix.

## SHIPPED 2026-07-17 (all gates green: test.bat 46/0, test_lsp 206/0, example 90/0/27, test_collection_leaks 128/128 leak-clean)

Landed via the bitwise-fallback enabling fix (below) plus the three-way element copy in list/dictionary/hashset. Two deviations from the literal spec, both safety-driven and verified:

1. **`list<T*>` / `dict<K,V*>` pointer elements MOVE, not share.** The issue framed the source-nulling as "silent data loss" to fix; in fact the container OWNS its raw pointers (dtor deletes them), so a shared-pointee copy double-frees at teardown (HeapAudit-confirmed). Move is the single-owner-clean answer, consistent with the owned-model for raw `T*` (decided earlier, not reverted). The false doc comment was corrected to say so. A true independent copy of `list<T*>` is not expressible under the owned model.
2. **`list<IShape>.copy()` (interface elements) now COMPILES but produces an imperfect interface copy when called.** It must compile (copy() is monomorphized on instantiation - this was the `list<IElement>` LSP failure). Nothing invokes it and the old plain-add was equally ill-defined, so no exercised path regressed. RESIDUAL GAP - not yet tracked separately; if a real caller appears, decide between a proper interface clone and an explicit rejection.

Also fixed two latent pre-existing bugs the change exposed: enum-backed receiver method dispatch (`enumVal.copy()`), and `ResetForReanalysis` not clearing `memberwiseCopyCache_` (LSP reanalysis regression).

### DECISION (2026-07-17): Option 2 - bitwise fallback at the copy choke point

The copy choke point (`LLVMBackend.h`, ~`:13599`) falls back to bitwise for non-struct types.
Precedent: `.~()` on an enum already gracefully no-ops (`p_enumdtor.cb`); `copy()` having no such
fallback is the oversight this fixes. Also makes `int.copy()` work. Localized - does NOT change
`is_primitive`'s meaning repo-wide. Chosen over Option 1.

### Two candidate enabling fixes - PICK ONE (this is the decision this issue is waiting on)

1. **`is_primitive` resolves through the enum backing table** before testing `kPrimitiveTypes`
   (`MainListener.h`, ~`:15217`). `GetEnumBackingType()` already exists as a public getter
   (`LLVMBackend.h`, ~`:15463`); `is_primitive` just does not consult it. ~2 lines. Changes the
   meaning of `is_primitive` for enums repo-wide - check every use first.
2. **The copy choke point falls back to bitwise for non-struct types** (`LLVMBackend.h`, ~`:13599`).
   Precedent: `.~()` on an enum already **gracefully no-ops** (verified, `p_enumdtor.cb`); `.copy()`
   has no such fallback. This makes copy consistent with destruction, and would also make
   `int.copy()` work. Arguably the more principled of the two - the asymmetry between `~()` and
   `copy()` looks like an oversight rather than a decision.

Once either lands, the `list.cb` change above is a clean core-only fix, and **the same shape fixes
`dictionary.cb` and `hashset.cb`**.

## Fix direction, in order

1. Land the enabling fix (decision above).
2. `core/list.cb` - both `copy()` overloads, as shown.
3. `core/dictionary.cb:280-283` - route through element copy; this is the crash.
4. `core/hashset.cb:109` - same shape; verify first, it is untested.
5. **Correct `core/list.cb`'s doc comment** - it currently states the opposite of the truth for
   pointer elements.

## Interaction with `unique`

An element type owning a `unique` pointer has no synthesized memberwise copy by design
(`TypeOwnsUniquePointer` bails; the choke point reports *"cannot copy 'X': its field ... is
'unique'"*). After the fix, `list<Box>.copy()` should report that error rather than silently moving -
which is correct and desirable.

**Do not read a green `list<Box>.copy()` today as evidence the `unique` copy guard held.** It passes
only because the source is silently emptied, so no second owner exists to double-free. Untestable
until the blocker above is resolved.

## Related

- `internal/plan/field-ownership-unique.md` - the review that surfaced this.
