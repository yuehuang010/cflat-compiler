# A `string` element of a fixed array double-frees in BOTH store spellings

Filed 2026-08-09 by `fix/arrslot` for the ASSIGNMENT spelling, then merged here by the review of
`fix/bracown`, which measured the BRACE spelling as the same defect and filed a duplicate. This
file supersedes the former `internal/issue/p2/string-element-of-a-fixed-array-double-frees-on-assign.md`,
which was deleted; severity is the higher of the two (p1). `string` is deliberately excluded from
the owning-struct transfer arms (it carries a runtime owned bit and has its own deep-copy-on-borrow
machinery), so both the assignment arm from `fix/arrslot` and the brace-element decision from
`fix/bracown` skip it. Measured, that machinery does NOT cover a fixed-array element destination in
either spelling.

Severity: double free (abort).

## Repro

```cflat
// (a) BRACE spelling
extern int main()
{
    string t = "aa" + "bb";
    string[2] dst = { t };
    printf("eq=%d\n", dst[0] == "aabb" ? 1 : 0);
    return 0;
}
```

-> compiles 0, prints `eq=1`, then **rc 133**.

```cflat
// (b) ASSIGNMENT spelling - the same store written as `=`
extern int main()
{
    string t = "aa" + "bb";
    string[2] dst;
    dst[0] = t;
    printf("eq=%d\n", dst[0] == "aabb" ? 1 : 0);
    return 0;
}
```

-> compiles 0, prints `eq=1`, then **rc 133**.

When the assignment spelling overwrites an already-set element (`dst[0] = "ol" + "d";` first),
`leaks --atExit` additionally reports 1 leak / 16 bytes - the old element, never dropped.

Both measured identical on `2f5a91a` (master, pre-`fix/bracown`) and on `fix/bracown`, and the
assignment spelling was measured identical on `7beb979` and on `fix/arrslot`, so neither store is
a regression of either fix - both fixes leave `string` exactly where they found it.

The neighbouring shapes that DO work, measured on the same binaries:

- a string LITERAL element - `string[2] dst = { "aa", "bb" };`, or `dst[0] = "x";` - rc 0 in both
  spellings, because neither side owns a buffer. That is why the cell is easy to miss.
- a string FIELD inside an owning struct - `struct SBox { string s; }; SBox[2] dst = { a };` -
  rc 133 before `fix/bracown`, rc 0 after (it goes through the owning-struct copy). The assignment
  spelling of the same shape was fixed by `fix/arrslot` and is covered by the `aels_stringowner_*`
  legs in `Test/test_move.cb`; the brace spelling is covered by `abri_stringowner_*`. Only a bare
  `string` element is left, in either spelling.
- the whole-local decl-init `string u = t;` - rc 0 (the `srcBorrowsOwnedString` deep-copy branch).

## Root cause

The fixed-array element destination is a TWO-index GEP over `[N x string]`, so it is neither
`destIsStructField` (a 2-index GEP over a STRUCT) nor `destIsLocalOwningVar` (alloca/global), and
the two `string` arms in `MainListener_Expressions.cpp` that would apply are gated on exactly those
two shapes - the field arm (`RejectOwningValueCopyIntoField` / the owned-string deep copy) and the
"destruct the old value of an owning-string LOCAL" arm. Part 6's container-slot arm DOES handle
`slotElemType == "string"`, but only for a SINGLE-index GEP, and it deliberately does not drop-old.
So the store is a plain bit copy of the `{ptr,len,owned}` aggregate: the source keeps its owned bit
set and the element gets a copy of it, and both free the buffer.

## Fix direction

Give the fixed-array element destination the same pair of `string` arms the whole-local destination
has, at BOTH sites - the assignment arm in `MainListener_Expressions.cpp` (`destIsFixedArrayElem`)
and `EmitPositionalFixedArrayIntoSlot` (which currently returns early for `elemTypeName == "string"`
in `ConsumeOwningBraceElementSource`). For the assignment site that means destruct the old element
first (the `string` dtor is null/owned-bit guarded, so a never-assigned or borrowed slot is a safe
no-op) and then deep-copy an owned named source (`EmitOwnedStringDeepCopy`) exactly as the field arm
does; the brace site CONSTRUCTS its slot, so it needs the deep copy only. Do NOT route `string` through
`ClassifyOwningAssignSource` instead: the 2026-08-09 `fix/owncopy` session measured that preempting
the dedicated string branch keeps every suite green while silently adding leaks.
