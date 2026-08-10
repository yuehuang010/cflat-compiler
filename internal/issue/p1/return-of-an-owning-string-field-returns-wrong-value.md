# `return b.s` of a local struct's owning string field returns the wrong value

Filed 2026-08-09 from the fix/strread review. Pre-existing: measured identical on `bfb5943` and
on fix/strread (`6b6e3ec`). The STRING sibling of
[[return-of-an-owning-struct-field-copies-instead-of-consuming]] - same return position, but the
symptom here is a SILENT wrong value (rc 0), not an abort, so nothing flags it at runtime.

Severity: wrong code, silent.

## Repro

```cflat
struct B { string s; };
string f() {
    B b;
    b.s = "ab" + "cd";
    return b.s;          // caller reads back an empty/garbage string
}
extern int main() {
    string y = f();
    printf("y=%d\n", y == "abcd" ? 1 : 0);   // prints y=0 on both binaries, rc 0
    return 0;
}
```

Reviewer probe `rev_ret_*` in the strread round measured `y=0`, rc 0, on both binaries.

## Root cause (hypothesis, verify)

The return path for a string FIELD source borrows the field's `{ptr,len}` while the frame's
struct destructor tears the buffer down before/as the caller adopts it - the returned pair points
at freed memory that happens to read as not-equal. The fix/strread return arm deliberately gates
on a fixed-array ELEMENT source and does not touch field sources; the field spelling needs its
own deep-copy (or consume) leg in the return path, mirroring how fix/strread's element arm and
the local-return string machinery (clearReturnedStringBorrowBit) already work.

## Fix direction

In the return-position string handling (MainListener_Statements.cpp, beside the fix/strread
element arm), recognise a string FIELD source of a dying local struct and deep-copy it before the
frame teardown, or consume the field. Watch the alias-return exclusion the element arm already
has. Add a value-asserting leg beside the aelsr_ return legs in Test/test_move.cb.
